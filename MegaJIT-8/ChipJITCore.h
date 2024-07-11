#include <fstream>
#include <random>
#include <array>
#include <vector>
#include <filesystem>

#include <udis86/udis86.h>

#include "ChipCore.h"
#include "ChipEmitter.h"
#include "ChipJITState.h"

extern ChipState s;
extern ChipJITState JIT;

class ChipJITCore : public ChipCore
{
public:
	uint16_t execute() override
	{
		if (!romLoaded || awaitingKeyPress()) [[unlikely]]
			return 0;

		auto map = JIT.blockMap[s.pc];

		if (!map.isValid) [[unlikely]]
			return compileBlock();

		auto& block = JIT.blocks[map.block];
		s.pc = block.endPC;
		return c.execute(block.cacheOffset);
	}

	inline void clearJITCache()
	{
		JIT.reset();
		c.clearCache();
	}

	inline void setSlowMode(bool enable)
	{
		instructionsPerBlock = enable ? 1 : BLOCK_MAX_INSTR;
		clearJITCache();
	}

	void dumpCode(const std::filesystem::path& path)
	{
		std::ofstream outFile(path, std::ios::out);
		if (!outFile) return;

		if (!udInitialized)
		{
			ud_init(&ud_obj);
			ud_set_mode(&ud_obj, 64);
			ud_set_syntax(&ud_obj, UD_SYN_INTEL);
			udInitialized = true;
		}

		for (const auto& block : JIT.blocks)
		{
			if (JIT.blockMap[block.startPC].isValid)
			{
				outFile << "JIT Block at PC: " << block.startPC << "-" << block.endPC << "\n--------------------------------\n";

				ud_set_input_buffer(&ud_obj, c.getCodePtr() + block.cacheOffset, block.cacheSize);

				while (ud_disassemble(&ud_obj)) 
					outFile << ud_insn_asm(&ud_obj) << "\n";

				outFile << "\n";
			}
		}
	}
private:
	ud_t ud_obj;
	bool udInitialized{ false };

private:
	ChipEmitter c{};
	static constexpr uint16_t BLOCK_MAX_INSTR = 64;

	uint16_t instructionsPerBlock { 1 };

	void initialize() override
	{
		s.reset();
		JIT.reset();
	}

	inline uint16_t compileBlock()
	{
		constexpr size_t CACHE_CLEAR_THRESHOLD = static_cast<size_t>(ChipEmitter::MAX_CACHE_SIZE * 0.8);

		if (c.getCodeSize() >= CACHE_CLEAR_THRESHOLD) [[unlikely]]
		{
			clearJITCache();
		}

		auto& map = JIT.blockMap[s.pc];
		map.isValid = true;

		if (map.block == -1) [[likely]]
		{
			map.block = JIT.blocks.size();
			JIT.blocks.push_back(JITBlock{ s.pc });
		}

		auto& block = JIT.blocks[map.block];
		block.cacheOffset = c.getCodeSize();

		analyzeBlock();
		emitBlock();
		c.emitEpilogue();

		block.endPC = s.pc;
		block.cacheSize = c.getCodeSize() - block.cacheOffset;

		return c.execute(block.cacheOffset);
	}

	bool isFlowNext(uint16_t pc)
	{
		const uint16_t opcode = (s.RAM[pc & 0xFFF] << 8) | s.RAM[(pc + 1) & 0xFFF];

		switch (opcode & 0xF000)
		{
		case 0x0000:
		{
			switch (opcode & 0x0FFF)
			{
			case 0x00EE:
				return true;
			default:
				return false;
			}
		}
		case 0x1000:
		case 0x2000:
		case 0xB000:
			return true;
		default:
			return false;
		}
	}

	void analyzeBlock()
	{
		uint16_t pc = s.pc;

		for (int i = 0; i < instructionsPerBlock; i++)
		{
			const uint16_t opcode = (s.RAM[pc] << 8) | s.RAM[pc + 1];
			const uint8_t xReg = ((opcode & 0x0F00) >> 8) & 0xF;
			const uint8_t yReg = ((opcode & 0x00F0) >> 4) & 0xF;

			pc += 2;

			switch (opcode & 0xF000)
			{
			case 0x0000:
				switch (opcode & 0x0FFF)
				{
				case 0x00EE:
					return;
				}
				break;
			case 0x1000:
			case 0x2000:
				return;

			case 0x3000:
			case 0x4000:
			case 0x5000:
			case 0xE000:
				c.VRegUsage[xReg]++;
				if (isFlowNext(pc)) return;
				break;
			case 0x6000:
			case 0x7000:
			case 0xC000:
				c.VRegUsage[xReg]++;
				break;
			case 0x8000:
				switch (opcode & 0x000F)
				{
				case 0x0000:
				case 0x0001:
				case 0x0002:
				case 0x0003:
					c.VRegUsage[xReg]++;
					c.VRegUsage[yReg]++;
					if (Quirks::VFReset) c.VRegUsage[0xF]++;
					break;
				case 0x0004:
				case 0x0005:
				case 0x0007:
					c.VRegUsage[xReg]++;
					c.VRegUsage[yReg]++;
					c.VRegUsage[0xF]++;
					break;

				case 0x0006:
				case 0x000E:
					c.VRegUsage[xReg]++;
					c.VRegUsage[0xF]++;
					break;
				}
				break;
			case 0x9000:
				c.VRegUsage[xReg]++;
				c.VRegUsage[yReg]++;
				if (isFlowNext(pc)) return;
				break;
			case 0xA000:
				c.IRegUsage++;
				break;
			case 0xB000:
				c.VRegUsage[(Quirks::Jumping ? xReg : 0)]++;
				break;
			case 0xD000:
				c.VRegUsage[xReg]++; 
				c.VRegUsage[yReg]++; 
				c.IRegUsage++;
				break;
			case 0xF000:
				switch (opcode & 0x00FF)
				{
				case 0x0007:
				case 0x0015:
				case 0x0018:
					c.VRegUsage[xReg]++;
					break;
				case 0x000A:
					return;
				case 0x001E:
				case 0x0029:
				case 0x0033:
					c.IRegUsage++;
					c.VRegUsage[xReg]++;
					break;
				case 0x0055:
				case 0x0065:
					c.IRegUsage++;

					for (int i = 0; i <= xReg; i++)
						c.VRegUsage[i]++;

					if ((opcode & 0x00FF) == 0x0055) return;
					break;
				}
				break;
			}
		}
	}

	void emitBlock()
	{
		c.allocateRegs();
		c.emitPrologue();

		bool condition { false };

		while (c.instructions < instructionsPerBlock || condition)
		{
			const uint16_t opcode = (s.RAM[s.pc] << 8) | s.RAM[s.pc + 1];

			const uint8_t xOperand = ((opcode & 0x0F00) >> 8) & 0xF;
			const uint8_t yOperand = ((opcode & 0x00F0) >> 4) & 0xF;
			const uint8_t value = opcode & 0x00FF;

			s.pc += 2;
			c.instructions++;

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
					c.emit00EE();
					return;
				}
				break;
			}
			case 0x1000:
				c.emit1NNN(opcode & 0xFFF);
				return;
			case 0x2000:
				c.emit2NNN(opcode & 0xFFF);
				return;
			case 0x3000:
				if (isFlowNext(s.pc))
				{
					c.emit3XNN<false>(xOperand, value);
					return;
				}
				else
				{
					c.emit3XNN<true>(xOperand, value);
					condition = true;
					continue;
				}
			case 0x4000:
				if (isFlowNext(s.pc))
				{
					c.emit4XNN<false>(xOperand, value);
					return;
				}
				else
				{
					c.emit4XNN<true>(xOperand, value);
					condition = true;
					continue;
				}
			case 0x5000:
				if (isFlowNext(s.pc))
				{
					c.emit5XY0<false>(xOperand, yOperand);
					return;
				}
				else
				{
					c.emit5XY0<true>(xOperand, yOperand);
					condition = true;
					continue;
				}
			case 0x6000:
				c.emit6XNN(xOperand, value);
				break;
			case 0x7000:
				c.emit7XNN(xOperand, value);
				break;
			case 0x8000:
				switch (opcode & 0x000F)
				{
				case 0x0000:
					c.emit8XY0(xOperand, yOperand);
					break;
				case 0x0001:
					c.emit8XY1(xOperand, yOperand);
					break;
				case 0x0002:
					c.emit8XY2(xOperand, yOperand);
					break;
				case 0x0003:
					c.emit8XY3(xOperand, yOperand);
					break;
				case 0x0004:
					c.emit8XY4(xOperand, yOperand);
					break;
				case 0x0005:
					c.emit8XY5(xOperand, yOperand);
					break;
				case 0x0006:
					c.emit8XY6(xOperand, yOperand);
					break;
				case 0x0007:
					c.emit8XY7(xOperand, yOperand);
					break;
				case 0x000E:
					c.emit8XYE(xOperand, yOperand);
					break;
				}
				break;
			case 0x9000:
				switch (opcode & 0x000F)
				{
				case 0x0000:
					if (isFlowNext(s.pc))
					{
						c.emit9XY0<false>(xOperand, yOperand);
						return;
					}
					else
					{
						c.emit9XY0<true>(xOperand, yOperand);
						condition = true;
						continue;
					}
				}
				break;
			case 0xA000:
				c.emitANNN(opcode & 0xFFF);
				break;
			case 0xB000:
				c.emitBNNN(opcode & 0xFFF, xOperand);
				return;
			case 0xC000:
				c.emitCXNN(xOperand, value);
				break;
			case 0xD000:
				c.emitDXYN(xOperand, yOperand, opcode & 0x000F);
				break;
			case 0xE000:
				switch (opcode & 0x00FF)
				{
				case 0x009E:
					if (isFlowNext(s.pc))
					{
						c.emitEX9E<false>(xOperand);
						return;
					}
					else
					{
						c.emitEX9E<true>(xOperand);
						condition = true;
						continue;
					}
				case 0x00A1:
					if (isFlowNext(s.pc))
					{
						c.emitEXA1<false>(xOperand);
						return;
					}
					else
					{
						c.emitEXA1<true>(xOperand);
						condition = true;
						continue;
					}
				}
				break;
			case 0xF000:
				switch (opcode & 0x00FF)
				{
				case 0x0007:
					c.emitFX07(xOperand);
					break;
				case 0x000A:
					c.emitFX0A(xOperand);
					return;
				case 0x001E:
					c.emitFX1E(xOperand);
					break;
				case 0x0015:
					c.emitFX15(xOperand);
					break;
				case 0x0018:
					c.emitFX18(xOperand);
					break;
				case 0x0029:
					c.emitFX29(xOperand);
					break;
				case 0x0033:
					c.emitFX33(xOperand);
					break;
				case 0x0055:
					c.emitFX55(xOperand);
					return; // ending the block on memory store, because self-modifying code can modify the current block.
				case 0x0065:
					c.emitFX65(xOperand); 
					break;
				}
				break;
			}

			if (condition)
			{
				c.emitLabel();
				condition = false;
			}
		}
	}
};