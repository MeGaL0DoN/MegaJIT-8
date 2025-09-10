#pragma once

#include <cstdlib>
#include <cstddef>
#include <bit>
#include <cassert>

#include "ChipState.h"
#include "ChipCore.h"
#include "utils.h"

static_assert(std::endian::native == std::endian::little, "This program requires a little-endian architecture!");

class ChipInterpretCore : public ChipCore
{
public:
	using ChipCore::ChipCore;

	#define CASE_16X16(base) case base: case base+16: case base+32: case base+48: case base+64: case base+80: case base+96: case base+112:\
							 case base+128: case base+144: case base+160: case base+176: case base+192: case base+208: case base+224: case base+240

	uint64_t execute() override
	{
		#define op ((opcode >> 4) & 0xF)

		#define nnn ((x << 8) | nn)
		#define nn (opcode >> 8)
		#define n (nn & 0xF)

		#define x (opcode & 0xF)
		#define regX s.V[x]
		#define regY s.V[opcode >> 12]

		#define SKIP() pc += 2

		uint64_t instrs { 0 };

		uint16_t pc { s.pc };
		uint8_t sp { s.sp };

		uint16_t opcode;

#define OP()												\
        std::memcpy(&opcode, &s.RAM[pc], sizeof(uint16_t)); \
        pc += 2;											\
        instrs++;											\
															\
        switch (op)											\
		{													\
        case 0x0:											\
            goto case0;										\
        case 0x1:											\
            goto case1;										\
        case 0x2:											\
            goto case2;										\
        case 0x3:											\
            goto case3;										\
        case 0x4:											\
            goto case4;										\
        case 0x5:											\
            goto case5;										\
        case 0x6:											\
            goto case6;										\
        case 0x7:											\
            goto case7;										\
        case 0x8:											\
            goto case8;										\
        case 0x9:											\
            goto case9;										\
        case 0xA:											\
            goto caseA;										\
        case 0xB:											\
            goto caseB;										\
        case 0xC:											\
            goto caseC;										\
        case 0xD:											\
            goto caseD;										\
        case 0xE:											\
            goto caseE;										\
        case 0xF:											\
            goto caseF;										\
        default:											\
			UNREACHABLE();									\
        }													\

#define DISPATCH() if (!executeFlag.load(std::memory_order_relaxed)) [[unlikely]] { goto end; } OP()
		
	OP();

	case0:
		if (nnn == 0x0EE) [[likely]]
		{
			pc = s.stack[--sp];
			DISPATCH();
		}
		else if (nnn == 0x0E0)
		{
			std::memset(s.screenBuffer.data(), 0, sizeof(s.screenBuffer));
			DISPATCH();
		}
		else [[unlikely]]
			goto invalid;

	case1:
		pc = nnn;
		DISPATCH();

	case2:
		s.stack[sp++] = pc;
		pc = nnn;
		DISPATCH();

	case3:
		if (regX == nn) SKIP();
		DISPATCH();

	case4:
		if (regX != nn) SKIP();
		DISPATCH();

	case5:
		if (n == 0x0)
		{
			if (regX == regY) SKIP();
			DISPATCH();
		}
		else [[unlikely]]
			goto invalid;

	case6:
		regX = nn;
		DISPATCH();

	case7:
		regX += nn;
		DISPATCH();

	case8:
		switch (nn)
		{
		CASE_16X16(0x0):
			regX = regY;
			DISPATCH();
		CASE_16X16(0x1):
			regX |= regY;
			if (s.quirks.vfReset) s.V[0xF] = 0;
			DISPATCH();
		CASE_16X16(0x2):
			regX &= regY;
			if (s.quirks.vfReset) s.V[0xF] = 0;
			DISPATCH();
		CASE_16X16(0x3):
			regX ^= regY;
			if (s.quirks.vfReset) s.V[0xF] = 0;
			DISPATCH();
		CASE_16X16(0x4):
			regX += regY;
			s.V[0xF] = regX < regY;
			DISPATCH();
		CASE_16X16(0x5):
		{
			const uint8_t flag = regX >= regY;
			regX -= regY;
			s.V[0xF] = flag;
			DISPATCH();
		}
		CASE_16X16(0x6):
		{
			if (!s.quirks.shifting) regX = regY;
			const uint8_t lsb = regX & 0x1;
			regX >>= 1;
			s.V[0xF] = lsb;
			DISPATCH();
		}
		CASE_16X16(0x7):
			regX = regY - regX;
			s.V[0xF] = regY >= regX;
			DISPATCH();
		CASE_16X16(0xE):
		{
			if (!s.quirks.shifting) regX = regY;
			const uint8_t msb = regX >> 7;
			regX <<= 1;
			s.V[0xF] = msb;
			DISPATCH();
		}
		default: [[unlikely]]
			goto invalid;
		}

	case9:
		if (n == 0x0)
		{
			if (regX != regY) SKIP();
			DISPATCH();
		}
		else [[unlikely]]
			goto invalid;

	caseA:
		s.I = nnn;
		DISPATCH();

	caseB:
		pc = nnn + (s.quirks.jumping ? regX : s.V[0]);
		DISPATCH();

	caseC:
		regX = static_cast<uint8_t>(rand() & nn);
		DISPATCH();

	caseD:
		dxyn(regX & (ChipState::SCR_WIDTH - 1), regY & (ChipState::SCR_HEIGHT - 1), n);
		DISPATCH();

	caseE:
		if (nn == 0x9E)
		{
			if (s.keys[regX & 0xF]) SKIP();
			DISPATCH();
		}
		else if (nn == 0xA1)
		{
			if (!s.keys[regX & 0xF]) SKIP();
			DISPATCH();
		}
		else [[unlikely]]
			goto invalid;

	caseF:
		switch (nn)
		{
		case 0x07:
			regX = s.delayTimer;
			DISPATCH();
		case 0x0A:
			pc -= 2;

			if (s.firstFX0ACall) [[unlikely]]
			{
				s.inputReg = static_cast<int8_t>(x);
				s.firstFX0ACall = false;
			}

			do
			{
				if (s.inputReg == -1) [[unlikely]]
				{
					pc += 2;
					s.firstFX0ACall = true;
					break;
				}
			} while (executeFlag.load(std::memory_order_relaxed));

			DISPATCH();
		case 0x1E:
			s.I = (s.I + regX) & 0xFFF;
			DISPATCH();
		case 0x15:
			s.delayTimer = regX;
			DISPATCH();
		case 0x18:
			s.soundTimer = regX;
			DISPATCH();
		case 0x29:
			s.I = (regX & 0xF) * 5;
			DISPATCH();
		case 0x33:
			s.RAM[s.I] = regX / 100;

			if (s.I == 0xFFF) [[unlikely]]
				goto fx33End;

			s.RAM[s.I + 1] = (regX / 10) % 10;

			if (s.I == 0xFFE) [[unlikely]]
				goto fx33End;

			s.RAM[s.I + 2] = regX % 10;
		fx33End:
			DISPATCH();
		case 0x55:
			if ((s.I + x) > 0xFFF) [[unlikely]]
				goto fx55End;

			for (int i = 0; i <= x; i++)
				s.RAM[s.I + i] = s.V[i];

			if (s.quirks.memoryIncrement)
				s.I = (s.I + x + 1) & 0xFFF;

		fx55End:
			DISPATCH();
		case 0x65:
			for (int i = 0; i <= x; i++)
				s.V[i] = s.RAM[s.I + i];

			if (s.quirks.memoryIncrement)
				s.I = (s.I + x + 1) & 0xFFF;

			DISPATCH();
		default: [[unlikely]]
			goto invalid;
		}

	invalid:
		pc -= 2; 
		assert(false);
		DISPATCH();

	end:
		s.pc = pc;
		s.sp = sp;

		return instrs;

		#undef BEGIN
		#undef DISPATCH
		#undef SKIP
		#undef op
		#undef n
		#undef nn
		#undef nnn
		#undef x
		#undef regX
		#undef regY
	}

private:
	FORCE_INLINE void dxyn(int x, size_t y, int n) const
	{
		s.V[0xF] = 0;

		if (s.quirks.clipping)
		{
			if (n == 1) [[likely]]
			{
				const uint64_t spriteMask { (static_cast<uint64_t>(s.RAM[s.I]) << 56) >> x };
				auto& ptr { s.screenBuffer[y] };

				if (ptr & spriteMask)
					s.V[0xF] = 1;

				ptr ^= spriteMask;
			}
			else
			{
				auto* ptr { &s.screenBuffer[y] };

				for (int i = 0; i < n; i++)
				{
					const uint64_t spriteMask { (static_cast<uint64_t>(s.RAM[s.I + i]) << 56) >> x };

					if (*ptr & spriteMask)
						s.V[0xF] = 1;

					*ptr ^= spriteMask;

					if (y == (ChipState::SCR_HEIGHT - 1))
						break;

					y++;
					ptr++;
				}
			}
		}
		else
		{
			if (n == 1) [[likely]]
			{
				const uint64_t spriteMask { std::rotr(static_cast<uint64_t>(s.RAM[s.I]) << 56, x) };

				if (s.screenBuffer[y] & spriteMask)
					s.V[0xF] = 1;

				s.screenBuffer[y] ^= spriteMask;
			}
			else
			{
				for (int i = 0; i < n; i++)
				{
					const uint64_t spriteMask { std::rotr(static_cast<uint64_t>(s.RAM[s.I + i]) << 56, x) };

					if (s.screenBuffer[y] & spriteMask)
						s.V[0xF] = 1;

					s.screenBuffer[y] ^= spriteMask;

					y = (y + 1) & (ChipState::SCR_HEIGHT - 1);
				}
			}
		}
	}
};