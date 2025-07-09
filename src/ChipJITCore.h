#include <fstream>
#include <random>
#include <array>
#include <vector>
#include <ranges>
#include <filesystem>
#include <queue>

#include <udis86.h>

#include "ChipCore.h"
#include "ChipEmitter.h"
#include "ChipJITState.h"
#include "macros.h"

class ChipJITCore : public ChipCore
{
public:
	friend ChipEmitter;

	ChipJITCore(ChipState& s) : ChipCore(s)
	{}

	FORCE_INLINE uint64_t execute() override
	{
		return c.execute(JIT.blockMap[s.pc]);
	}

	void clearJITCache()
	{
		JIT.reset();
		c.clearCache();
	}

	void setSlowMode(bool enable)
	{
		instructionsPerBlock = enable ? 1 : BLOCK_MAX_INSTR;
		clearJITCache();
	}

	void dumpCode(const std::filesystem::path& path)
	{
		std::ofstream outFile { path, std::ios::out };
		if (!outFile) return;

		if (!udInitialized)
		{
			ud_init(&ud);
			ud_set_mode(&ud, 64);
			ud_set_syntax(&ud, UD_SYN_INTEL);
			udInitialized = true;
		}

		ud_set_pc(&ud, 0);

		for (const auto& block : JIT.blocks)
		{
			const auto offset { JIT.blockMap[block.pc] };

			if (offset == 0)
				continue;

			outFile << "Block at PC: " << std::hex << "0x" << block.pc << "-0x" << block.pcRanges.back().second
					<< "\n-----------------------------------------\n";

			ud_set_input_buffer(&ud, c.getCodePtr() + offset, block.cacheSize);

			while (ud_disassemble(&ud))
				outFile << "0x" << std::hex << std::setw(8) << std::setfill('0') << ud_insn_off(&ud) << " | " << ud_insn_asm(&ud) << '\n';

			outFile << '\n';
		}
	}

private:
	ud_t ud{};
	bool udInitialized { false };

	ChipJITState JIT{};
	ChipEmitter c { *this };

	JITBlock* block { nullptr };
	std::queue<bool> blockFlagCalcList{};
	uint16_t flagOps { 0 };

	static constexpr size_t BLOCK_MAX_INSTR { 255 };
	size_t instructionsPerBlock { BLOCK_MAX_INSTR };

	void initialize() override
	{
		s.reset();
		clearJITCache();
	}

	inline uint64_t compileBlock()
	{
		constexpr size_t CACHE_CLEAR_THRESHOLD { static_cast<size_t>(ChipEmitter::MAX_CACHE_SIZE * 0.8) };

		if (c.getCodeSize() >= CACHE_CLEAR_THRESHOLD) [[unlikely]]
			clearJITCache();

		for (auto& b : JIT.blocks)
		{
			if (JIT.blockMap[b.pc] == 0)
			{
				block = &b;
				block->pc = s.pc;
				block->pcRanges.clear();
				break;
			}
		}

		if (block == nullptr)
		{
			JIT.blocks.emplace_back(s.pc);
			block = &JIT.blocks.back();
		}

		blockFlagCalcList = {};

		const auto offset { static_cast<uint32_t>(c.getCodeSize()) };
		JIT.blockMap[s.pc] = offset;

		c.reset();
		analyzeBlock(s.pc);
		c.allocateRegs();
		c.emitPrologue();

		c.instructions = 0;
		emitBlock(s.pc);
		block->cacheSize = static_cast<uint32_t>(c.getCodeSize() - offset);
		block = nullptr;

		return c.execute(offset);
	}

	inline bool isInlinableJump(uint16_t startPC, uint16_t pc, uint16_t nnn, uint16_t instrs) const
	{
		return (nnn < startPC || nnn >= pc) && instrs < instructionsPerBlock;
	}

	inline bool isInlinableSubroutine(uint16_t startPC, uint16_t pc, uint16_t nnn, uint16_t instrs) const
	{
		if (!(nnn < startPC || nnn >= pc))
			return false;

		bool branch { false };
		const uint16_t startNNN { nnn };
		
		while ((instrs < instructionsPerBlock || branch) && nnn < 0xFFF)
		{
			instrs++;

			const uint16_t opcode = (s.RAM[nnn] << 8) | s.RAM[nnn + 1];
			nnn += 2;

			switch (opcode & 0xF000)
			{
			case 0x0000:
				switch (opcode & 0xFFF)
				{
				case 0x00EE:
					// if (branch)
					// 	break;
					// return true;

					// TODO allow conditional returns later (fix current cycle counting first)
					return !branch;
				default:
					break;
				}
				break;
			case 0x1000:
				if (!branch)
					return isInlinableSubroutine(startNNN, nnn, opcode & 0xFFF, instrs);
				if (!isInlinableSubroutine(startNNN, nnn, opcode & 0xFFF, instrs))
					return false;
				break;
			case 0x2000:
				if (!isInlinableSubroutine(startNNN, nnn, opcode & 0xFFF, instrs))
					return false;
				break;
			case 0x3000:
			case 0x4000:
			case 0x5000:
			case 0x9000:
			case 0xE000:
				branch = true;
				continue;
			case 0xB000:
				return false;
			case 0xF000:
				switch (opcode & 0xFF)
				{
					case 0x000A:
					case 0x0033:
					case 0x0055:
						return false;
					default:
						break;
				}
				break;
			default:
				break;
			}
			
			branch = false;
		}

		return false;
	}

	inline bool isFlow(uint16_t pc) const
	{
		const uint16_t opcode = (s.RAM[pc] << 8) | s.RAM[pc + 1];

		switch (opcode & 0xF000)
		{
			case 0x0000:
				return opcode == 0x00E0;
			case 0x1000:
			case 0x2000:
			case 0xB000:
				return true;
			case 0xF000:
				return (opcode & 0xFF) == 0x0A;
			default:
				return false;
		}
	}

	inline uint16_t analyzeBlock(uint16_t pc)
	{
		const uint16_t startPC { pc };
		bool branch { false }, flow { false };

		const auto calculateAllFlags = [&]
		{
			while (flagOps > 0)
			{
				blockFlagCalcList.push(true);
				flagOps--;
			}
		};

		const auto handleFlow = [&]
		{
			if (branch)
				calculateAllFlags();
			else
				flow = true;
		};
		 
		const auto setFlagOpCalcVal = [&](bool val)
		{
			if (!val)
			{
				if (branch)
					return;

				c.VRegUsage[0xF] -= flagOps;

				while (flagOps > 0)
				{
					blockFlagCalcList.push(false);
					flagOps--;
				}
			}
			else
				calculateAllFlags();
		};

		while ((c.instructions < instructionsPerBlock || branch) && pc < 0xFFF)
		{
			c.instructions++;

			const uint16_t opcode = (s.RAM[pc] << 8) | s.RAM[pc + 1];
			pc += 2;

			const uint8_t x = ((opcode & 0x0F00) >> 8) & 0xF, y = ((opcode & 0x00F0) >> 4) & 0xF;
			const uint8_t n = opcode & 0xF;
			const uint8_t nn = opcode & 0xFF;
			const uint16_t nnn = opcode & 0xFFF;

			switch (opcode & 0xF000)
			{
			case 0x0000:
				switch (nnn)
				{
				case 0x00EE:
					handleFlow();
					break;
				}
				break;
			case 0x1000:
				if (isInlinableJump(startPC, pc, nnn, c.instructions))
				{
					if (!branch)
						return analyzeBlock(nnn);

					calculateAllFlags();
					analyzeBlock(nnn);
				}
				else
					handleFlow();
				break;
			case 0x2000:
				if (isInlinableSubroutine(startPC, pc, nnn, c.instructions))
				{
					if (branch)
						calculateAllFlags();

					analyzeBlock(nnn);
				}
				else
					handleFlow();
				break;

			case 0x3000:
			case 0x4000:
			case 0x5000:
			case 0xE000:
				c.VRegUsage[x]++;

				if (x == 0xF)
					setFlagOpCalcVal(true);

				if (!isFlow(pc))
					c.blockHasInstrSkips = true;

				branch = true;
				continue;
			case 0x6000:
			case 0xC000:
				c.VRegUsage[x]++;

				if (x == 0xF)
					setFlagOpCalcVal(false);
				break;
			case 0x7000:
				c.VRegUsage[x]++;
				break;
			case 0x8000:
				switch (n)
				{
				case 0x0:
					c.VRegUsage[x]++;
					c.VRegUsage[y]++;

					if ((x == 0xF) ^ (y == 0xF))
						setFlagOpCalcVal(y == 0xF);
					break;
				case 0x1:
				case 0x2:
				case 0x3:
					c.VRegUsage[x]++;
					c.VRegUsage[y]++;

					if (y == 0xF && x != 0xF)
						setFlagOpCalcVal(true);
					else if (s.quirks.vfReset)
						setFlagOpCalcVal(false);

					if (s.quirks.vfReset)
					{
						c.VRegUsage[0xF]++;
						flagOps++;
					}
					break;
				case 0x4:
				case 0x5:
				case 0x7:
					c.VRegUsage[x]++;
					c.VRegUsage[y]++;
					c.VRegUsage[0xF]++;
					setFlagOpCalcVal(y == 0xF && x != 0xF);
					flagOps++;
					break;
				case 0x0006:
				case 0x000E:
					c.VRegUsage[x]++;
					c.VRegUsage[0xF]++;

					if (!s.quirks.shifting && x != y)
					{
						c.VRegUsage[x]++;
						c.VRegUsage[y]++;
						setFlagOpCalcVal(y == 0xF);
					}
					else
						setFlagOpCalcVal(false);

					flagOps++;
					break;
				}
				break;
			case 0x9000:
				c.VRegUsage[x]++;
				c.VRegUsage[y]++;

				if (!isFlow(pc))
					c.blockHasInstrSkips = true;

				if (x == 0xF || y == 0xF)
					setFlagOpCalcVal(true);

				branch = true;
				continue;
			case 0xA000:
				c.IRegUsage++;
				break;
			case 0xB000:
				c.VRegUsage[(s.quirks.jumping ? x : 0)]++;
				handleFlow();
				break;
			case 0xD000:
				c.VRegUsage[x]++; 
				c.VRegUsage[y]++; 
				c.VRegUsage[0xF]++;
				c.IRegUsage++;
				setFlagOpCalcVal(x == 0xF || y == 0xF);
				flagOps++;
				break;
			case 0xF000:
				switch (nn)
				{
				case 0x07:
					c.VRegUsage[x]++;
					if (x == 0xF)
						setFlagOpCalcVal(false);
					break;
				case 0x15:
				case 0x18:
					c.VRegUsage[x]++;
					if (x == 0xF)
						setFlagOpCalcVal(true);
					break;
				case 0x0A:
					handleFlow();
					break;
				case 0x1E:
				case 0x29:
				case 0x33:
					c.IRegUsage++;
					c.VRegUsage[x]++;

					if (x == 0xF)
						setFlagOpCalcVal(true);

					break;
				case 0x55:
					c.IRegUsage += (s.quirks.memoryIncrement ? 2 : 1);

					for (int i = 0; i <= x; i++)
						c.VRegUsage[i]++;

					if (x == 0xF)
						setFlagOpCalcVal(true);

					break;

				case 0x65:
					c.IRegUsage += (s.quirks.memoryIncrement ? 2 : 1);

					for (int i = 0; i <= x; i++)
						c.VRegUsage[i]++;

					if (x == 0xF)
						setFlagOpCalcVal(false);

					break;
				}
				break;
			}

			if (flow)
				break;
	
			branch = false;
		}

		calculateAllFlags();
		return pc;
	}

	inline uint16_t emitBlock(uint16_t pc, std::vector<uint8_t*>* inlinedSubCondRetPtrs = nullptr)
	{
		const uint16_t startPC { pc };
		bool flow { false };
		uint8_t* branchEndPtr { nullptr };

#define BRANCH() \
		newBranchEndPtr = c.getCodeEndPtr(); \
		if (!isFlow(pc))\
			c.branchedInstrs++; \
		break \

		const auto popFlagCalc = [&]() -> bool
		{
			const bool val { blockFlagCalcList.front() };
			blockFlagCalcList.pop();
			return val;
		};
		const auto addPcRange = [&]()
		{
			const auto range { std::make_pair(startPC, static_cast<uint16_t>(pc - 1)) };

			if (std::ranges::find(block->pcRanges, range) == block->pcRanges.end())
				block->pcRanges.push_back(range);
		};
 
		while ((c.instructions < instructionsPerBlock || branchEndPtr) && pc < 0xFFF)
		{
			const uint16_t prevInstrCount { c.instructions };
			c.instructions++;

			bool inlinedBlock { false };
			uint8_t* newBranchEndPtr { nullptr };

			const uint16_t opcode = (s.RAM[pc] << 8) | s.RAM[pc + 1];
			pc += 2;

			const uint8_t x = ((opcode & 0x0F00) >> 8) & 0xF, y = ((opcode & 0x00F0) >> 4) & 0xF;
			const uint8_t n = opcode & 0xF;
			const uint8_t nn = opcode & 0xFF;
			const uint16_t nnn = opcode & 0xFFF;

			switch (opcode & 0xF000)
			{
			case 0x0000:
			{
				switch (opcode & 0x0FFF)
				{
				case 0x00E0:
					c.emit00E0();
					break;
				case 0x00EE:
					if (inlinedSubCondRetPtrs)
					{
						if (branchEndPtr)
						{
							c.emitJumpPlaceholder();
							inlinedSubCondRetPtrs->push_back(c.getCodeEndPtr());
						}
						else
						{
							addPcRange();
							return pc;
						}
					}
					else
					{
						c.emit00EE();
						flow = true;
					}
					break;
				default:
					c.emitIllegalOPHandler();
					break;
				}
				break;
			}
			case 0x1000:
				if (isInlinableJump(startPC, pc, nnn, c.instructions))
				{
					if (branchEndPtr)
					{
						const uint16_t prevBranchedInstrs { c.branchedInstrs };
						emitBlock(nnn);
						c.branchedInstrs -= (c.branchedInstrs - prevBranchedInstrs);
						c.branchedInstrs += (c.instructions - prevInstrCount);
						inlinedBlock = true;
					}
					else
					{
						addPcRange();
						return emitBlock(nnn);
					}
				}
				else
				{
					c.emit1NNN(nnn);
					flow = true;
				}
				break;
			case 0x2000:
				if (isInlinableSubroutine(startPC, pc, nnn, c.instructions))
				{
					std::vector<uint8_t*> condRetPtrs{};

					if (branchEndPtr)
					{
						const uint16_t prevBranchedInstrs { c.branchedInstrs };
						// TODO emit add!!
						emitBlock(nnn, &condRetPtrs);					
						c.branchedInstrs -= (c.branchedInstrs - prevBranchedInstrs);
						c.branchedInstrs += (c.instructions - prevInstrCount);
						inlinedBlock = true;
					}
					else
						emitBlock(nnn, &condRetPtrs);

					for (const auto ptr : condRetPtrs)
						c.patchBranchInstr(ptr, false);
				}
				else
				{
					c.emit2NNN(nnn, pc);
					flow = true;
				}
				break;
			case 0x3000:
				c.emit3XNN(x, nn, !isFlow(pc));
				BRANCH();
			case 0x4000:
				c.emit4XNN(x, nn, !isFlow(pc));
				BRANCH();
			case 0x5000:
				c.emit5XY0(x, y, !isFlow(pc));
				BRANCH();
			case 0x6000:
				c.emit6XNN(x, nn);
				break;
			case 0x7000:
				c.emit7XNN(x, nn);
				break;
			case 0x8000:
				switch (n)
				{
				case 0x0:
					c.emit8XY0(x, y);
					break;
				case 0x1:
					c.emit8XY1(x, y, s.quirks.vfReset ? popFlagCalc() : false);
					break;
				case 0x2:
					c.emit8XY2(x, y, s.quirks.vfReset ? popFlagCalc() : false);
					break;
				case 0x3:
					c.emit8XY3(x, y, s.quirks.vfReset ? popFlagCalc() : false);
					break;
				case 0x4:
					c.emit8XY4(x, y, popFlagCalc());
					break;
				case 0x5:
					c.emit8XY5(x, y, popFlagCalc());
					break;
				case 0x6:
					c.emit8XY6(x, y, popFlagCalc());
					break;
				case 0x7:
					c.emit8XY7(x, y, popFlagCalc());
					break;
				case 0xE:
					c.emit8XYE(x, y, popFlagCalc());
					break;
				default:
					c.emitIllegalOPHandler();
					break;
				}
				break;
			case 0x9000:
				switch (n)
				{
				case 0:
					c.emit9XY0(x, y, !isFlow(pc));
					BRANCH();
				default:
					c.emitIllegalOPHandler();
					break;
				}
				break;
			case 0xA000:
				c.emitANNN(nnn);
				break;
			case 0xB000:
				c.emitBNNN(nnn, x);
				flow = true;
				break;
			case 0xC000:
				c.emitCXNN(x, nn);
				break;
			case 0xD000:
				c.emitDXYN(x, y, n, popFlagCalc());
				break;
			case 0xE000:
				switch (nn)
				{
				case 0x009E:
					c.emitEX9E(x, !isFlow(pc));
					BRANCH();
				case 0x00A1:
					c.emitEXA1(x, !isFlow(pc));
					BRANCH();
				default:
					c.emitIllegalOPHandler();
					break;
				}
				break;
			case 0xF000:
				switch (nn)
				{
				case 0x07:
					c.emitFX07(x);
					break;
				case 0x0A:
					c.emitFX0A(x, pc);
					flow = true;
					break;
				case 0x1E:
					c.emitFX1E(x);
					break;
				case 0x15:
					c.emitFX15(x);
					break;
				case 0x18:
					c.emitFX18(x);
					break;
				case 0x29:
					c.emitFX29(x);
					break;
				case 0x33:
					c.emitFX33(x, pc);
					break;
				case 0x55:
					c.emitFX55(x, pc);
					break;
				case 0x65:
					c.emitFX65(x); 
					break;
				default:
					c.emitIllegalOPHandler();
					break;
				}
				break;
			}

			if (branchEndPtr)
			{
				if (flow)
				{
					c.emitEpilogue();
					c.branchedInstrs++;
				}

				c.patchBranchInstr(branchEndPtr, !(flow || inlinedBlock));
				flow = false;
			}
			else if (flow)
				break;

			branchEndPtr = newBranchEndPtr;
		}

		c.emitEpilogue(flow ? -1 : pc & 0xFFF);
		addPcRange();
		return pc;

#undef BRANCH
	}

	bool invalidateBlocks(uint16_t startAddr, uint16_t endAddr)
	{
		bool invalidated { false };

		for (const auto& block : JIT.blocks)
		{
			for (const auto& range : block.pcRanges)
			{
				if (range.first <= endAddr && range.second >= startAddr)
				{
					JIT.blockMap[block.pc] = 0;
					invalidated = true;
					break;
				}
			}
		}

		return invalidated;
	}

	void illegalOpcodeHandler()
	{
		assert(false);
	}
};