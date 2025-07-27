#pragma once

#include <fstream>
#include <random>
#include <array>
#include <vector>
#include <ranges>
#include <filesystem>
#include <algorithm>
#include <queue>
#include <bitset>

#include <Zydis/Zydis.h>

#include "ChipCore.h"
#include "ChipEmitter.h"
#include "ChipJITState.h"
#include "macros.h"

class ChipJITCore : public ChipCore
{
public:
	friend ChipEmitter;

	ChipJITCore(ChipState& s, std::atomic<bool>& executeFlag) : ChipCore(s), c(*this, executeFlag)
	{}

	FORCE_INLINE uint64_t execute() override
	{
		return c.execute();
	}

	void clearJITCache()
	{
		JIT.blocks.clear();
		std::fill_n(JIT.blockMap.begin(), ChipState::RAM_SIZE, c.getUncompiledPtr());
		std::memset(JIT.compiledRam.data(), 0, sizeof(ChipJITState::compiledRam));
		c.clearCache();
	}

	void setSlowMode(bool enable)
	{
		instructionsPerBlock = enable ? 1 : BLOCK_MAX_INSTR;
		clearJITCache();
	}

	void dumpCode(const std::filesystem::path& path) const
	{
		std::ofstream outFile { path, std::ios::out };
		if (!outFile) return;

		static ZydisDecoder decoder;
		static ZydisFormatter formatter;
		static bool decoderInitialized { false }, formatterInitialized { false };

		if (!decoderInitialized)
		{
			ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
			decoderInitialized = true;
		}
		if (!formatterInitialized)
		{
			ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
			ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_ADDR_PADDING_ABSOLUTE, ZYDIS_PADDING_DISABLED);
			formatterInitialized = true;
		}

		bool firstBlock { true };
		ZyanU64 runtimeAddress { 0 };

		for (const auto& block : JIT.blocks)
		{
			const auto buf { JIT.blockMap[block.pc] };

			if (buf == c.getUncompiledPtr())
				continue;

			if (!firstBlock)
				outFile << "\n\n";

			firstBlock = false;

			outFile << "Block at PC: " << std::hex << "0x" << block.pc// << "-0x" << block.pcRanges.back().second
					<< "\n-----------------------------------------";

			ZyanUSize offset { 0 };
			ZydisDecodedInstruction instruction;
			ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
			char textBuffer[256];

			while (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, buf + offset, block.cacheSize - offset, &instruction, operands)))
			{
				ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible, textBuffer, sizeof(textBuffer), runtimeAddress, nullptr);
				outFile << "\n0x" << std::hex << std::setw(8) << std::setfill('0') << runtimeAddress << " | " << textBuffer;

				offset += instruction.length;
				runtimeAddress += instruction.length;
			}
		}
	}

private:
	ChipJITState JIT{};
	ChipEmitter c;

	struct RetPoint
	{
		uint8_t* codePtr;
		uint16_t instrCount;
		uint16_t branchCount;
	};
	struct SubroutineInfo
	{
		bool conditional { false };
		bool requiresRuntimeStack { false };
		uint16_t startInstrCount { 0 };
		uint16_t startBranchCount { 0 };
		std::vector<RetPoint> condRetPoints{};
	};
	struct RegAllocFrameInfo
	{
		std::array<uint16_t, 16> regWeights{};
		std::bitset<16> modifiedRegs{};
		std::bitset<16> initialValUseRegs{};
		std::vector<RegAllocFrameInfo> childFrames{};
		bool conditionalFrame;

		std::array<RegAllocation, 16> allocation{};

		explicit RegAllocFrameInfo(bool cond) : conditionalFrame(cond)
		{}
	};

	JITBlock* block { nullptr };

	std::queue<bool> blockFlagCalcList{};
	uint16_t flagOps { 0 };

	std::vector<RegAllocFrameInfo> allocFrames{};

	static constexpr size_t BLOCK_MAX_INSTR { 256 };
	size_t instructionsPerBlock { BLOCK_MAX_INSTR };

	void initialize() override
	{
		s.reset();
		clearJITCache();
	}

	uint8_t* compileBlock(uint16_t pc)
	{
		constexpr size_t CACHE_CLEAR_THRESHOLD { static_cast<size_t>(ChipEmitter::MAX_CACHE_SIZE * 0.5) };

		if (c.getCodeSize() >= CACHE_CLEAR_THRESHOLD) [[unlikely]]
			clearJITCache();

		for (auto& b : JIT.blocks) // trying to reuse block element (if any was invalidated)
		{
			if (JIT.blockMap[b.pc] == c.getUncompiledPtr())
			{
				block = &b;
				block->pc = pc;
				std::memset(block->compiledRam.data(), 0, sizeof(JITBlock::compiledRam));
				break;
			}
		}

		if (block == nullptr)
			block = &JIT.blocks.emplace_back(pc);

		blockFlagCalcList = {};
		allocFrames.clear();

		const auto func { c.getCodeEndPtr() };
		JIT.blockMap[pc] = func;

		c.newBlock();
		auto& frame { allocFrames.emplace_back(false) };
		analyzeBlock(pc, &frame, allocFrames);
		allocateRegisters(allocFrames);

		c.instructions = 0;
		//c.allocatedRegs = allocFrames.front().allocation;
		c.emitLoadAllocRegs();
		emitBlock(pc, allocFrames.data() + 1);

		block->cacheSize = static_cast<uint32_t>(c.getCodeEndPtr() - func);
		block = nullptr;

		return func;
	}

	void allocateRegisters(std::vector<RegAllocFrameInfo>& frames) // TODO
	{
		constexpr int ALLOC_THRESHOLD { 2 };
		std::array<std::pair<uint8_t, uint16_t>, 16> usageMap{};

		for (auto& alloc : frames)
		{
			int cnt { 0 };

			for (int i = 0; i < 15; i++) // 16
			{
				if (alloc.regWeights[i] >= ALLOC_THRESHOLD)
					usageMap[cnt++] = { i, alloc.regWeights[i] };
			}

			std::ranges::fill(alloc.allocation, RegAllocation{ ChipEmitter::NOT_ALLOCATED });
			std::sort(usageMap.begin(), usageMap.begin() + cnt, [](const auto& a, const auto& b) { return a.second > b.second; });

			for (int i = 0; i < cnt && i < ChipEmitter::MAX_ALLOC_REGS; i++)
			{
				const auto reg { usageMap[i].first };
				alloc.allocation[reg] = RegAllocation{ static_cast<uint8_t>(i), alloc.initialValUseRegs[reg], alloc.modifiedRegs[reg] };
			}

			allocateRegisters(alloc.childFrames);
		}
	}

	inline bool subRequiresRuntimeStack(uint16_t startPC, uint16_t pc, uint16_t nnn, uint16_t instrs) const
	{
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
					if (branch)
					 	break;
					return false;
				default:
					break;
				}
				break;
			case 0x1000:
				if (!branch)
					return subRequiresRuntimeStack(startNNN, nnn, opcode & 0xFFF, instrs);
				if (subRequiresRuntimeStack(startNNN, nnn, opcode & 0xFFF, instrs))
					return true;
				break;
			case 0x2000:
				if (subRequiresRuntimeStack(startNNN, nnn, opcode & 0xFFF, instrs))
					return true;
				break;
			case 0x3000:
			case 0x4000:
			case 0x5000:
			case 0x9000:
			case 0xE000:
				branch = true;
				continue;
			case 0xB000:
				return true;
			case 0xF000:
				switch (opcode & 0xFF)
				{
					case 0x000A:
					case 0x0033:
					case 0x0055:
						return true;
					default:
						break;
				}
				break;
			default:
				break;
			}
			
			branch = false;
		}

		return true;
	}

	inline bool isFlow(uint16_t pc) const
	{
		const uint16_t opcode = (s.RAM[pc] << 8) | s.RAM[pc + 1];

		switch (opcode & 0xF000)
		{
			case 0x0000:
				return opcode == 0x00EE;
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
	inline bool isInlinableFlow(uint16_t startPC, uint16_t pc, uint16_t nnn) const
	{
		return (nnn < startPC || nnn >= pc) && c.instructions < instructionsPerBlock;
	}

	uint16_t analyzeBlock(uint16_t pc, RegAllocFrameInfo* alloc, std::vector<RegAllocFrameInfo>& allocFrames)
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
		const auto setFlagOpCalcVal = [&](bool val)
		{
			if (!val)
			{
				if (branch)
					return;

				alloc->regWeights[0xF] -= flagOps;

				while (flagOps > 0)
				{
					blockFlagCalcList.push(false);
					flagOps--;
				}
			}
			else
				calculateAllFlags();
		};

		const auto handleFlow = [&]
		{
			if (branch)
				calculateAllFlags();
			else
				flow = true;
		};

		const auto setInitialValUseReg = [&](uint8_t r)
		{
			if (!alloc->modifiedRegs[r])
				alloc->initialValUseRegs[r] = true;
		};
		const auto newRegAllocFrame = [&]()
		{
			alloc = &allocFrames.emplace_back(false);
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
				if (nnn == 0x0EE)
					handleFlow();
				break;
			case 0x1000:
				if (isInlinableFlow(startPC, pc, nnn))
				{
					if (!branch)
					{
						auto& frame { allocFrames.emplace_back(false) };
						return analyzeBlock(nnn, &frame, allocFrames);
					}

					calculateAllFlags();
					auto& frame { allocFrames.emplace_back(true) };
					analyzeBlock(nnn, &frame, allocFrames);
				}
				else
					handleFlow();
				break;
			case 0x2000:
				if (isInlinableFlow(startPC, pc, nnn))
				{
					if (branch)
					{
						calculateAllFlags();
						allocFrames.emplace_back(true);
					}
					else
						allocFrames.emplace_back(false);

					analyzeBlock(nnn, &allocFrames.back(), allocFrames.back().childFrames);
					newRegAllocFrame();
				}
				else
					handleFlow();
				break;

			case 0x3000:
			case 0x4000:
			case 0xE000:
				alloc->regWeights[x]++;
				setInitialValUseReg(x);

				if (x == 0xF)
					setFlagOpCalcVal(true);

				branch = true;
				continue;
			case 0x6000:
			case 0xC000:
				alloc->regWeights[x]++;
				alloc->modifiedRegs[x] = true;

				if (x == 0xF)
					setFlagOpCalcVal(false);
				break;
			case 0x7000:
				if (nn == 0)
					break;

				alloc->regWeights[x]++;
				setInitialValUseReg(x);
				alloc->modifiedRegs[x] = true;
				break;
			case 0x8000:
				switch (n)
				{
				case 0x0:
					if (x == y)
						break;

					alloc->regWeights[x] += 2;
					alloc->regWeights[y]++;
					setInitialValUseReg(y);
					alloc->modifiedRegs[x] = true;

					if (y == 0xF)
						setFlagOpCalcVal(true);
					break;
				case 0x1:
				case 0x2:
				case 0x3:
					alloc->regWeights[x] += 2;
					alloc->regWeights[y]++;;
					setInitialValUseReg(x);
					setInitialValUseReg(y);
					alloc->modifiedRegs[x] = true;

					if (y == 0xF && x != 0xF)
						setFlagOpCalcVal(true);
					else if (s.quirks.vfReset)
						setFlagOpCalcVal(false);

					if (s.quirks.vfReset)
					{
						alloc->regWeights[0xF]++;
						alloc->modifiedRegs[0xF] = true;
						flagOps++;
					}
					break;
				case 0x4:
				case 0x5:
				case 0x7:
					alloc->regWeights[x] += 2;
					alloc->regWeights[y]++;
					alloc->regWeights[0xF]++;
					setInitialValUseReg(x);
					setInitialValUseReg(y);
					alloc->modifiedRegs[x] = true;
					alloc->modifiedRegs[0xF] = true;

					setFlagOpCalcVal(y == 0xF && x != 0xF);
					flagOps++;
					break;
				case 0x0006:
				case 0x000E:
					alloc->regWeights[x]++;
					alloc->regWeights[0xF]++;

					if (!s.quirks.shifting && x != y)
					{
						setInitialValUseReg(y);
						alloc->regWeights[x]++;
						alloc->regWeights[y]++;
						setFlagOpCalcVal(y == 0xF);
					}
					else
					{
						setInitialValUseReg(x);
						setFlagOpCalcVal(false);
					}

					alloc->modifiedRegs[x] = true;
					alloc->modifiedRegs[0xF] = true;
					flagOps++;
					break;
				}
				break;
			case 0x5000:
			case 0x9000:
				alloc->regWeights[x] += 2;
				alloc->regWeights[y]++;
				setInitialValUseReg(x);
				setInitialValUseReg(y);

				if (x == 0xF || y == 0xF)
					setFlagOpCalcVal(true);

				branch = true;
				continue;
			case 0xA000:
				break;
			case 0xB000:
			{
				const auto reg { s.quirks.jumping ? x : 0 };
				alloc->regWeights[reg]++;
				setInitialValUseReg(reg);
				handleFlow();
				break;
			}
			case 0xD000:
				alloc->regWeights[x]++;
				alloc->regWeights[y]++;
				alloc->regWeights[0xF]++;
				setInitialValUseReg(x);
				setInitialValUseReg(y);
				alloc->modifiedRegs[0xF] = true;

				setFlagOpCalcVal(x == 0xF || y == 0xF);
				flagOps++;
				break;
			case 0xF000:
				switch (nn)
				{
				case 0x07:
					alloc->regWeights[x]++;
					alloc->modifiedRegs[x] = true;

					if (x == 0xF)
						setFlagOpCalcVal(false);
					break;
				case 0x15:
				case 0x18:
					alloc->regWeights[x]++;
					setInitialValUseReg(x);

					if (x == 0xF)
						setFlagOpCalcVal(true);
					break;
				case 0x0A:
					handleFlow();
					break;
				case 0x1E:
				case 0x29:
					alloc->regWeights[x]++;
					setInitialValUseReg(x);

					if (x == 0xF)
						setFlagOpCalcVal(true);

					break;
				case 0x33:
					alloc->regWeights[x]++;
					setInitialValUseReg(x);

					if (x == 0xF)
						setFlagOpCalcVal(true);

					// new reg allocation on mem stores
					newRegAllocFrame();
					break;
				case 0x55:
					for (int i = 0; i <= x; i++)
					{
						alloc->regWeights[i]++;
						setInitialValUseReg(i);
					}

					if (x == 0xF)
						setFlagOpCalcVal(true);

					newRegAllocFrame();
					break;
				case 0x65:
					for (int i = 0; i <= x; i++)
					{
						alloc->regWeights[i]++;
						alloc->modifiedRegs[i] = true;
					}

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

	// 	: main
	// loop
	//  v0 := 0
	//  if v0 == 0 then
	//   test
	//  v0 += 3
	// again
	//
	// : test
	// if v1 == 0 begin
	// 	v1 += 2
	// 	v1 -= 5
	// 	v1 -= 7
	// end
	// if v1 == 1 then
	// 	return
	// if v1 == 2 begin
	// 	vf += 200
	// 	vf += 200
	// 	vf += 200
	// 	vf += 200
	// 	vf += 200
	// 	vf += 200
	// 	return
	// end
	// return

	uint16_t emitBlock(uint16_t pc, RegAllocFrameInfo* alloc, SubroutineInfo* sub = nullptr, bool conditionalBlock = false)
	{
		const uint16_t startPC { pc };
		bool flow { false };
		uint8_t* branchEndPtr { nullptr }, *newBranchEndPtr { nullptr };

		const auto popFlagCalc = [&]() -> bool
		{
			const bool val { blockFlagCalcList.front() };
			blockFlagCalcList.pop();
			return val;
		};
		const auto newRegAllocFrame = [&]()
		{
			c.emitStoreAllocRegs();
			//alloc++;
			//c.allocatedRegs = alloc++->allocation;
			c.emitLoadAllocRegs();
		};
		const auto branch = [&](auto emitFunc)
		{
			if (isFlow(pc))
				emitFunc(false);
			else
			{
				emitFunc(true);
				c.branchedInstrs++;
			}

			newBranchEndPtr = c.getCodeEndPtr();
		};
 
		while ((c.instructions < instructionsPerBlock || branchEndPtr) && pc < 0xFFF)
		{
			const uint16_t prevInstrCount { c.instructions }, prevBranchCount { c.branchedInstrs };
			c.instructions++;
			bool inlinedBlock { false };

			JIT.compiledRam[pc >> 6] |= (1ull << (pc & 63));
			block->compiledRam[pc >> 6] |= (1ull << (pc & 63));

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
				switch (opcode & 0xFFF)
				{
				case 0x0E0:
					c.emit00E0();
					break;
				case 0x0EE:
					if (sub)
					{
						if (sub->requiresRuntimeStack)
							c.emit00EE(false);

						if (sub->conditional)
							c.emitInstrCountAdd((c.instructions - sub->startInstrCount) - (c.branchedInstrs - sub->startBranchCount));

						if (!branchEndPtr && !conditionalBlock)
							return pc;

						if (!sub->conditional)
							c.emitInstrCountAddPlaceholder(); // to later subtract the number of remaining instructions in the subroutine.

						sub->condRetPoints.emplace_back(c.emitJumpPlaceholder(), c.instructions, c.branchedInstrs);

						if (!branchEndPtr)
							return pc;

						c.branchedInstrs++;
						inlinedBlock = true;
					}
					else
					{
						c.emit00EE(true);
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
				if (isInlinableFlow(startPC, pc, nnn))
				{
					if (branchEndPtr)
					{
						const auto prevAllocRegs { c.allocatedRegs };
						newRegAllocFrame();
						emitBlock(nnn, alloc, sub, true);
						c.allocatedRegs = prevAllocRegs;
						c.branchedInstrs -= (c.branchedInstrs - prevBranchCount);
						c.branchedInstrs += (c.instructions - prevInstrCount);
						inlinedBlock = true;
					}
					else
					{
						newRegAllocFrame();
						return emitBlock(nnn, alloc, sub, conditionalBlock);
					}
				}
				else
				{
					c.emit1NNN(nnn);
					flow = true;
				}
				break;
			case 0x2000:
				if (isInlinableFlow(startPC, pc, nnn))
				{
					//const auto childFrames { alloc->childFrames.data() };
					newRegAllocFrame();

					SubroutineInfo newSub
					{
						branchEndPtr != nullptr, subRequiresRuntimeStack(startPC, pc, nnn, c.instructions),
						prevInstrCount, prevBranchCount
					};

					if (newSub.requiresRuntimeStack)
						c.emit2NNN(nnn, pc, false);

					if (branchEndPtr)
					{
						emitBlock(nnn, /*childFrames*/nullptr, &newSub);
						c.branchedInstrs -= (c.branchedInstrs - prevBranchCount);
						c.branchedInstrs += (c.instructions - prevInstrCount);
						inlinedBlock = true;
					}
					else
						emitBlock(nnn, /*childFrames*/nullptr, &newSub);

					newRegAllocFrame();

					for (const auto& p : newSub.condRetPoints)
					{
						c.patchBranchInstr(p.codePtr, false);

						if (!branchEndPtr)
						{
							const int32_t skippedInstrs = (c.instructions - p.instrCount) - (c.branchedInstrs - p.branchCount);
							// jmp opcode is 1 byte (0x9E) + 4 bytes jump displacement
							c.patchImm32(p.codePtr - sizeof(int32_t) - 1, -skippedInstrs); 
						}
					}
				}
				else
				{
					c.emit2NNN(nnn, pc, true);
					flow = true;
				}
				break;
			case 0x3000:
				branch([&](bool inc) { c.emit3XNN(x, nn, inc); });
				break;
			case 0x4000:
				branch([&](bool inc) { c.emit4XNN(x, nn, inc); });
				break;
			case 0x5000:
				branch([&](bool inc) { c.emit5XY0(x, y, inc); });
				break;
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
				if (n == 0x0)
				{
					branch([&](bool inc) { c.emit9XY0(x, y, inc); });
					break;
				}
				c.emitIllegalOPHandler();
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
				case 0x9E:
					branch([&](bool inc) { c.emitEX9E(x, inc); });
					break;
				case 0xA1:
					branch([&](bool inc) { c.emitEXA1(x, inc); });
					break;
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
					newRegAllocFrame();
					break;
				case 0x55:
					c.emitFX55(x, pc);
					newRegAllocFrame();
					break;
				case 0x65:
					c.emitFX65(x); 
					break;
				default:
					c.emitIllegalOPHandler();
					break;
				}
				break;
			default:
				c.emitIllegalOPHandler();
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

			branchEndPtr = std::exchange(newBranchEndPtr, nullptr);
		}

		c.emitEpilogue(flow ? -1 : pc & 0xFFF);
		return pc;

#undef BRANCH
	}

	void invalidateBlocks(uint64_t ind, uint64_t mask)
	{
		JIT.compiledRam[ind] &= ~mask;

		for (const auto& block : JIT.blocks)
		{
			if (block.compiledRam[ind] & mask)
				JIT.blockMap[block.pc] = c.getUncompiledPtr();
		}
	}

	void illegalOpcodeHandler()
	{
		assert(false);
	}
};