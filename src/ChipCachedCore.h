#pragma once

#include <random>

#include "ChipCachedState.h"
#include "ChipCore.h"
#include "macros.h"

class ChipCachedCore : public ChipCore
{
public:
	explicit ChipCachedCore(ChipState& s) : ChipCore(s)
	{}

	FORCE_INLINE uint64_t execute() override
	{
		const auto ind { cache.blockMap[s.pc] };
		return ind != 0 ? executeBlock(cache.blocks[ind - 1]) : compileBlock();
	}

	void clearCache()
	{
		cache.reset();
		buf.clear();
	}

	void setSlowMode(bool enable)
	{
		instructionsPerBlock = enable ? 1 : BLOCK_MAX_INSTR;
		clearCache();
	}

private:
	struct Instruction
	{
		uint8_t x;
		uint8_t y;
		uint8_t nn;
		uint16_t nnn;
	};
	struct CacheOp
	{
		void (ChipCachedCore::*func)(const Instruction&);
		Instruction instr;
	};

	ChipCachedState cache{};
	std::vector<CacheOp> buf{};

	std::default_random_engine eng { std::random_device{}() };
	std::uniform_int_distribution<> rng { 0, 255 };

	void initialize() override
	{
		s.reset();
		clearCache();
	}

	FORCE_INLINE uint64_t executeBlock(const CacheBlock& block)
	{
		s.pc = block.endPC;

		const auto* blockBuf { &buf[block.cacheOffset] };
		const auto instrCount { block.instrCount };

		for (int i = 0; i < instrCount; i++)
		{
			const auto& op { blockBuf[i] };
			(this->*op.func)(op.instr);
		}

		return instrCount;
	}

	FORCE_INLINE uint64_t compileBlock()
	{
		constexpr size_t CACHE_CLEAR_THRESHOLD { 16384 };

		if (buf.size() >= CACHE_CLEAR_THRESHOLD)
			clearCache();

		CacheBlock* block { nullptr };

		for (int i = 0; i < cache.blocks.size(); i++)
		{
			if (cache.blockMap[cache.blocks[i].pc] == 0)
			{
				block = &cache.blocks[i];
				block->pc = s.pc;
				cache.blockMap[s.pc] = i + 1;
				break;
			}
		}

		if (block == nullptr)
		{
			cache.blocks.emplace_back(s.pc);
			block = &cache.blocks.back();
			cache.blockMap[s.pc] = cache.blocks.size();
		}

		block->cacheOffset = buf.size();
		emitBlock(*block);
		block->endPC = s.pc;

		return executeBlock(*block);
	}

	static constexpr size_t BLOCK_MAX_INSTR { 255 };
	size_t instructionsPerBlock { BLOCK_MAX_INSTR };

	void emitBlock(CacheBlock& block)
	{
		block.instrCount = 0;

		while (block.instrCount < instructionsPerBlock)
		{
			const uint16_t opcode = (s.RAM[s.pc] << 8) | s.RAM[s.pc + 1];
			s.pc += 2;

			Instruction instr
			{
				static_cast<uint8_t>((opcode & 0x0F00) >> 8),
				static_cast<uint8_t>((opcode & 0x00F0) >> 4),
				static_cast<uint8_t>(opcode & 0xFF),
				static_cast<uint16_t>(opcode & 0xFFF), 
			};

			block.instrCount++;

			switch (opcode & 0xF000)
			{
			case 0x0000:
			{
				switch (instr.nnn)
				{
				case 0x00E0:
					buf.push_back({ &ChipCachedCore::op_00E0, instr});
					break;
				case 0x00EE:
					buf.push_back({ &ChipCachedCore::op_00EE, instr});
					return;
				default:
					buf.push_back({ &ChipCachedCore::op_INVALID, instr });
					return;
				}
				break;
			}
			case 0x1000:
				buf.push_back({&ChipCachedCore::op_1NNN, instr});
				return;
			case 0x2000:
				buf.push_back({&ChipCachedCore::op_2NNN, instr});
				return;
			case 0x3000:
				buf.push_back({&ChipCachedCore::op_3XNN, instr});
				return;
			case 0x4000:
				buf.push_back({&ChipCachedCore::op_4XNN, instr});
				return;
			case 0x5000:
				buf.push_back({&ChipCachedCore::op_5XY0, instr});
				return;
			case 0x6000:
				buf.push_back({&ChipCachedCore::op_6XNN, instr});
				break;
			case 0x7000:
				buf.push_back({&ChipCachedCore::op_7XNN, instr});
				break;
			case 0x8000:
				switch (opcode & 0xF)
				{
				case 0x0:
					buf.push_back({&ChipCachedCore::op_8XY0, instr});
					break;
				case 0x1:	
					if (s.quirks.vfReset)
						buf.push_back({&ChipCachedCore::op_8XY1<true>, instr});
					else
						buf.push_back({&ChipCachedCore::op_8XY1<false>, instr});
					break;
				case 0x2:
					if (s.quirks.vfReset)
						buf.push_back({&ChipCachedCore::op_8XY2<true>, instr});
					else
						buf.push_back({&ChipCachedCore::op_8XY2<false>, instr});
					break;
				case 0x3:
					if (s.quirks.vfReset)
						buf.push_back({&ChipCachedCore::op_8XY3<true>, instr});
					else
						buf.push_back({&ChipCachedCore::op_8XY3<false>, instr});
					break;
				case 0x4:
					buf.push_back({&ChipCachedCore::op_8XY4, instr});
					break;
				case 0x5:
					buf.push_back({&ChipCachedCore::op_8XY5, instr});
					break;
				case 0x6:
					if (s.quirks.shifting)
						buf.push_back({&ChipCachedCore::op_8XY6<true>, instr});
					else
						buf.push_back({&ChipCachedCore::op_8XY6<false>, instr});
					break;
				case 0x7:
					buf.push_back({&ChipCachedCore::op_8XY7, instr});
					break;
				case 0xE:
					if (s.quirks.shifting)
						buf.push_back({&ChipCachedCore::op_8XYE<true>, instr});
					else
						buf.push_back({&ChipCachedCore::op_8XYE<false>, instr});
					break;
				default:
					buf.push_back({ &ChipCachedCore::op_INVALID, instr });
					return;
				}
				break;
			case 0x9000:
				switch (opcode & 0xF)
				{
				case 0x0:
					buf.push_back({ &ChipCachedCore::op_9XY0, instr });
					return;
				default:
					buf.push_back({ &ChipCachedCore::op_INVALID, instr });
					return;
				}
			case 0xA000:
				buf.push_back({&ChipCachedCore::op_ANNN, instr});
				break;
			case 0xB000:
				if (s.quirks.jumping)
					buf.push_back({&ChipCachedCore::op_BNNN<true>, instr});
				else
					buf.push_back({&ChipCachedCore::op_BNNN<false>, instr});
				return;
			case 0xC000:
				buf.push_back({&ChipCachedCore::op_CXNN, instr});
				break;
			case 0xD000:
				if (s.quirks.clipping)
				{
					switch (opcode & 0xF)
					{
					case 0:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 0>, instr });
						break;
					case 1:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 1>, instr });
						break;
					case 2:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 2>, instr });
						break;
					case 3:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 3>, instr });
						break;
					case 4:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 4>, instr });
						break;
					case 5:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 5>, instr });
						break;
					case 6:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 6>, instr });
						break;
					case 7:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 7>, instr });
						break;
					case 8:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 8>, instr });
						break;
					case 9:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 9>, instr });
						break;
					case 10:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 10>, instr });
						break;
					case 11:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 11>, instr });
						break;
					case 12:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 12>, instr });
						break;
					case 13:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 13>, instr });
						break;
					case 14:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 14>, instr });
						break;
					case 15:
						buf.push_back({ &ChipCachedCore::op_DXYN<true, 15>, instr });
						break;
					}
				}
				else
				{
					switch (opcode & 0xF)
					{
					case 0:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 0>, instr });
						break;
					case 1:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 1>, instr });
						break;
					case 2:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 2>, instr });
						break;
					case 3:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 3>, instr });
						break;
					case 4:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 4>, instr });
						break;
					case 5:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 5>, instr });
						break;
					case 6:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 6>, instr });
						break;
					case 7:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 7>, instr });
						break;
					case 8:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 8>, instr });
						break;
					case 9:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 9>, instr });
						break;
					case 10:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 10>, instr });
						break;
					case 11:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 11>, instr });
						break;
					case 12:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 12>, instr });
						break;
					case 13:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 13>, instr });
						break;
					case 14:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 14>, instr });
						break;
					case 15:
						buf.push_back({ &ChipCachedCore::op_DXYN<false, 15>, instr });
						break;
					}
				}
				break;
			case 0xE000:
				switch (instr.nn)
				{
				case 0x009E:
					buf.push_back({ &ChipCachedCore::op_EX9E, instr });
					return;
				case 0x00A1:
					buf.push_back({ &ChipCachedCore::op_EXA1, instr });
					return;
				default:
					buf.push_back({ &ChipCachedCore::op_INVALID, instr });
					return;
				}
				break;
			case 0xF000:
				switch (instr.nn)
				{
				case 0x07:
					buf.push_back({ &ChipCachedCore::op_FX07, instr });
					break;
				case 0x0A:
					buf.push_back({ &ChipCachedCore::op_FX0A, instr });
					return;
				case 0x1E:
					buf.push_back({ &ChipCachedCore::op_FX1E, instr });
					break;
				case 0x15:
					buf.push_back({ &ChipCachedCore::op_FX15, instr });
					break;
				case 0x18:
					buf.push_back({ &ChipCachedCore::op_FX18, instr });
					break;
				case 0x29:
					buf.push_back({ &ChipCachedCore::op_FX29, instr });
					break;
					// Ending the block on memory store, because self-modifying code can modify the current block.
				case 0x33:
					buf.push_back({ &ChipCachedCore::op_FX33, instr });
					return;
				case 0x55:
					if (s.quirks.memoryIncrement)
						buf.push_back({ &ChipCachedCore::op_FX55<true>, instr });
					else
						buf.push_back({ &ChipCachedCore::op_FX55<false>, instr });
					return;
				case 0x65:
					if (s.quirks.memoryIncrement)
						buf.push_back({ &ChipCachedCore::op_FX65<true>, instr });
					else
						buf.push_back({ &ChipCachedCore::op_FX65<false>, instr });
					break;
				default:
					buf.push_back({ &ChipCachedCore::op_INVALID, instr });
					return;
				}
			default:
				buf.push_back({ &ChipCachedCore::op_INVALID, instr });
				return;
			}
		}
	}

	void invalidateBlocks(uint16_t startAddr, uint16_t endAddr)
	{
		for (const auto& block : cache.blocks)
		{
			if (block.pc <= endAddr && (block.endPC - 1) >= startAddr)
				cache.blockMap[block.pc] = 0;
		}
	}

	void op_INVALID(const Instruction& instr)
	{
		s.pc -= 2;
		assert(false);
	}

	void op_00E0(const Instruction& instr)
	{
		std::memset(s.screenBuffer.data(), 0, sizeof(s.screenBuffer));
	}
	void op_00EE(const Instruction& instr)
	{
		s.pc = s.stack[--s.sp];
	}
	void op_1NNN(const Instruction& instr)
	{
		s.pc = instr.nnn;
	}
	void op_2NNN(const Instruction& instr)
	{
		s.stack[s.sp++] = s.pc;
		s.pc = instr.nnn;
	}
	void op_3XNN(const Instruction& instr)
	{
		if (s.V[instr.x] == instr.nn)
			s.pc = (s.pc + 2) & 0xFFF;
	}
	void op_4XNN(const Instruction& instr)
	{
		if (s.V[instr.x] != instr.nn)
			s.pc = (s.pc + 2) & 0xFFF;
	}
	void op_5XY0(const Instruction& instr)
	{
		if (s.V[instr.x] == s.V[instr.y])
			s.pc = (s.pc + 2) & 0xFFF;
	}
	void op_6XNN(const Instruction& instr)
	{
		s.V[instr.x] = instr.nn;
	}
	void op_7XNN(const Instruction& instr)
	{
		s.V[instr.x] += instr.nn;
	}
	void op_8XY0(const Instruction& instr)
	{
		s.V[instr.x] = s.V[instr.y];
	}

	template<bool vfReset>
	void op_8XY1(const Instruction& instr)
	{
		s.V[instr.x] |= s.V[instr.y];
		if constexpr (vfReset) s.V[0xF] = 0;
	}
	template<bool vfReset>
	void op_8XY2(const Instruction& instr)
	{
		s.V[instr.x] &= s.V[instr.y];
		if constexpr (vfReset) s.V[0xF] = 0;
	}
	template<bool vfReset>
	void op_8XY3(const Instruction& instr)
	{
		s.V[instr.x] ^= s.V[instr.y];
		if constexpr (vfReset) s.V[0xF] = 0;
	}
	void op_8XY4(const Instruction& instr)
	{
		s.V[instr.x] += s.V[instr.y];
		s.V[0xF] = s.V[instr.x] < s.V[instr.y];
	}
	void op_8XY5(const Instruction& instr)
	{
		const uint8_t flag = s.V[instr.x] >= s.V[instr.y];
		s.V[instr.x] -= s.V[instr.y];
		s.V[0xF] = flag;
	}
	template<bool shifting>
	void op_8XY6(const Instruction& instr)
	{
		if constexpr (!shifting) s.V[instr.x] = s.V[instr.y];
		const uint8_t lsb = s.V[instr.x] & 0x1;
		s.V[instr.x] >>= 1;
		s.V[0xF] = lsb;
	}
	void op_8XY7(const Instruction& instr)
	{
		s.V[instr.x] = s.V[instr.y] - s.V[instr.x];
		s.V[0xF] = s.V[instr.y] >= s.V[instr.x];
	}
	template<bool shifting>
	void op_8XYE(const Instruction& instr)
	{
		if constexpr (!shifting) s.V[instr.x] = s.V[instr.y];
		const uint8_t msb = s.V[instr.x] >> 7;
		s.V[instr.x] <<= 1;
		s.V[0xF] = msb;
	}

	void op_9XY0(const Instruction& instr)
	{
		if (s.V[instr.x] != s.V[instr.y])
			s.pc = (s.pc + 2) & 0xFFF;
	}
	void op_ANNN(const Instruction& instr)
	{
		s.I = instr.nnn;
	}
	template <bool jumping>
	void op_BNNN(const Instruction& instr)
	{
		if constexpr (jumping) s.pc = s.V[instr.x] + instr.nnn;
		else s.pc = s.V[0] + instr.nnn;

		s.pc &= 0xFFF;
	}
	void op_CXNN(const Instruction& instr)
	{
		s.V[instr.x] = rng(eng) & instr.nn;
	}

	template <bool clipping, uint8_t N>
	void op_DXYN(const Instruction& instr)
	{
		if (N == 0)
		{
			s.V[0xF] = 0;
			return;
		}

		const uint8_t x = s.V[instr.x] & (ChipState::SCR_WIDTH - 1);
		uint8_t y = s.V[instr.y] & (ChipState::SCR_HEIGHT - 1);

		s.V[0xF] = 0;

		for (int i = 0; i < N; i++)
		{
			const auto sprite { static_cast<uint64_t>(s.RAM[s.I + i]) << 56 };
			uint64_t spriteMask;

			if constexpr (clipping)
				spriteMask = sprite >> x;
			else
				spriteMask = (sprite << (64 - x)) | (sprite >> x);

			if (s.screenBuffer[y] & spriteMask)
				s.V[0xF] = 1;

			s.screenBuffer[y] ^= spriteMask;

			if constexpr (N != 1)
			{
				if constexpr (clipping)
				{
					if (y == (ChipState::SCR_HEIGHT - 1))
						break;

					y++;
				}
				else
					y = (y + 1) & (ChipState::SCR_HEIGHT - 1);
			}
		}
	}

	void op_EX9E(const Instruction& instr)
	{
		if (s.keys[s.V[instr.x] & 0xF])
			s.pc = (s.pc + 2) & 0xFFF;
	}
	void op_EXA1(const Instruction& instr)
	{
		if (!s.keys[s.V[instr.x] & 0xF])
			s.pc = (s.pc + 2) & 0xFFF;
	}
	void op_FX07(const Instruction& instr)
	{
		s.V[instr.x] = s.delayTimer;
	}
	void op_FX0A(const Instruction& instr)
	{
		if (s.firstFX0ACall)
		{
			s.inputReg = static_cast<int8_t>(instr.x);
			s.firstFX0ACall = false;
		}
		else if (s.inputReg == -1)
		{
			s.firstFX0ACall = true;
			return;
		}

		s.pc -= 2;
	}
	void op_FX15(const Instruction& instr)
	{
		s.delayTimer = s.V[instr.x];
	}
	void op_FX18(const Instruction& instr)
	{
		s.soundTimer = s.V[instr.x];
	}
	void op_FX1E(const Instruction& instr)
	{
		s.I = (s.I + s.V[instr.x]) & 0xFFF;
	}
	void op_FX29(const Instruction& instr)
	{
		s.I = (s.V[instr.x] & 0xF) * 0x5;
	}
	void op_FX33(const Instruction& instr)
	{
		s.RAM[s.I] = s.V[instr.x] / 100;

		if (s.I != 0xFFF)
		{
			s.RAM[s.I + 1] = (s.V[instr.x] / 10) % 10;

			if (s.I != 0xFFE)
				s.RAM[s.I + 2] = s.V[instr.x] % 10;
		}

		invalidateBlocks(s.I, s.I + 2);
	}

	template<bool increment>
	void op_FX55(const Instruction& instr)
	{
		if ((s.I + instr.x) > 0xFFF)
			return;

		for (int i = 0; i <= instr.x; i++)
			s.RAM[s.I + i] = s.V[i];

		invalidateBlocks(s.I, s.I + instr.x);

		if constexpr (increment)
			s.I = (s.I + instr.x + 1) & 0xFFF;
	}
	template<bool increment>
	void op_FX65(const Instruction& instr)
	{
		for (int i = 0; i <= instr.x; i++)
			s.V[i] = s.RAM[s.I + i];

		if constexpr (increment)
			s.I = (s.I + instr.x + 1) & 0xFFF;
	}
};