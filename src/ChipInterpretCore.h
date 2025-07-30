#pragma once

#include <random>

#include "ChipState.h"
#include "ChipCore.h"
#include "macros.h"

class ChipInterpretCore : public ChipCore
{
public:
	std::array<uint64_t, 16> usage{};

	explicit ChipInterpretCore(ChipState& s) : ChipCore(s)
	{}

	FORCE_INLINE uint64_t execute() override
	{
		const uint16_t opcode = (s.RAM[s.pc] << 8) | s.RAM[s.pc + 1];
		s.pc += 2;

		#define nnn (opcode & 0xFFF)
		#define nn (opcode & 0xFF)
		#define n (opcode & 0xF)

		#define x ((opcode & 0x0F00) >> 8)
		#define regX s.V[x]
		#define regY s.V[(opcode & 0x00F0) >> 4]

		#define skipInstr() s.pc += 2

		switch (opcode >> 12)
		{
		case 0x0:
		{
			switch (nnn)
			{
			case 0x0E0: 
				std::memset(s.screenBuffer.data(), 0, sizeof(s.screenBuffer));
				break;
			case 0x0EE: 
				s.pc = s.stack[--s.sp];
				break;
			default:
				s.pc -= 2;
				assert(false);
				break;
			}
			break;
		}
		case 0x1:
			s.pc = nnn;
			break;  
		case 0x2:
			s.stack[s.sp++] = s.pc;
			s.pc = nnn;
			break;
		case 0x3:
			if (regX == nn) skipInstr();
			break;
		case 0x4:
			if (regX != nn) skipInstr();
			break;
		case 0x5:
			switch (n)
			{
			case 0x0:
				if (regX == regY) skipInstr();
				break;
			default:
				s.pc -= 2;
				assert(false);
				break;
			}
			break;
		case 0x6:
			regX = nn;
			break;
		case 0x7: 
			regX += nn;
			break;
		case 0x8:
			switch (n)
			{
			case 0x0:
				regX = regY;
				break;
			case 0x1:
				regX |= regY;
				if (s.quirks.vfReset) s.V[0xF] = 0;
				break;
			case 0x2:
				regX &= regY;
				if (s.quirks.vfReset) s.V[0xF] = 0;
				break;
			case 0x3:
				regX ^= regY;
				if (s.quirks.vfReset) s.V[0xF] = 0;
				break;
			case 0x4:
			{
				regX += regY;
				s.V[0xF] = regX < regY;
				break;
			}
			case 0x5:
			{
				const uint8_t flag = regX >= regY;
				regX -= regY;
				s.V[0xF] = flag;
				break;
			}
			case 0x6: 
			{
			    if (!s.quirks.shifting) regX = regY;
				const uint8_t lsb = regX & 0x1;
				regX >>= 1;
				s.V[0xF] = lsb;
				break;
			}
			case 0x7: 
				regX = regY - regX;
				s.V[0xF] = regY >= regX;
				break;
			case 0xE: 
			{
				if (!s.quirks.shifting) regX = regY;
				const uint8_t msb = regX >> 7;
				regX <<= 1;
				s.V[0xF] = msb;
				break;
			}
			default:
				s.pc -= 2;
				assert(false);
				break;
			}
			break;
		case 0x9:
			switch (n)
			{
			case 0x0:
				if (regX != regY) skipInstr();
				break;
			default:
				s.pc -= 2;
				assert(false);
				break;
			}
			break;
		case 0xA:
			s.I = nnn;
			break;
		case 0xB:
			if (s.quirks.jumping) s.pc = regX + nnn;
			else s.pc = s.V[0] + nnn;
			break;
		case 0xC:
			regX = rng(eng) & nn;
			break;
		case 0xD: 
			drawSprite(regX & (ChipState::SCR_WIDTH - 1), regY & (ChipState::SCR_HEIGHT - 1), n);
			break;
		case 0xE:
			switch (nn)
			{
			case 0x9E:
				if (s.keys[regX & 0xF]) skipInstr();
				break;
			case 0xA1:
				if (!s.keys[regX & 0xF]) skipInstr();
				break;
			default:
				s.pc -= 2;
				assert(false);
				break;
			}
			break;
		case 0xF:
			switch (nn)
			{
			case 0x07:
				regX = s.delayTimer;
				break;
			case 0x0A: 
				if (s.firstFX0ACall)
				{
					s.inputReg = static_cast<int8_t>(x);
					s.firstFX0ACall = false;
				}
				else if (s.inputReg == -1)
				{
					s.firstFX0ACall = true;
					break;
				}

				s.pc -= 2;
				break;
			case 0x1E:
				s.I = (s.I + regX) & 0xFFF;
				break;
			case 0x15:
				s.delayTimer = regX;
				break;
			case 0x18:
				s.soundTimer = regX;
				break;
			case 0x29:
				s.I = (regX & 0xF) * 0x5;
				break;
			case 0x33:
				s.RAM[s.I] = regX / 100;
				if (s.I == 0xFFF) break;
				s.RAM[s.I + 1] = (regX / 10) % 10;
				if (s.I == 0xFFE) break;
				s.RAM[s.I + 2] = regX % 10;
				break;
			case 0x55:
				if ((s.I + x) > 0xFFF)
					break;

				for (int i = 0; i <= x; i++)
					s.RAM[s.I + i] = s.V[i];

				if (s.quirks.memoryIncrement)
					s.I = (s.I + x + 1) & 0xFFF;
	
				break;
			case 0x65:
				for (int i = 0; i <= x; i++)
					s.V[i] = s.RAM[s.I + i];

				if (s.quirks.memoryIncrement)
					s.I = (s.I + x + 1) & 0xFFF;

				break;
			default:
				s.pc -= 2;
				assert(false);
				break;
			}
			break;
		default:
			s.pc -= 2;
			assert(false);
			break;
		}

		return 1;

		#undef n
		#undef nn
		#undef nnn
		#undef x
		#undef regX
		#undef regY
		#undef skipInstr
	}

private:
	std::default_random_engine eng { std::random_device{}() };
	std::uniform_int_distribution<> rng { 0, 255 };

	void drawSprite(uint8_t x, uint8_t y, uint8_t height) const
	{
		s.V[0xF] = 0;

		for (int i = 0; i < height; i++)
		{
			const auto sprite { static_cast<uint64_t>(s.RAM[s.I + i]) << 56 };
			uint64_t spriteMask;

			if (s.quirks.clipping)
				spriteMask = sprite >> x;
			else
				spriteMask = (sprite << (64 - x)) | (sprite >> x);

			if (s.screenBuffer[y] & spriteMask)
				s.V[0xF] = 1;

			s.screenBuffer[y] ^= spriteMask;

			if (s.quirks.clipping)
			{
				if (y == (ChipState::SCR_HEIGHT - 1))
					break;

				y++;
			}
			else
				y = (y + 1) & (ChipState::SCR_HEIGHT - 1);
		}
	}
};