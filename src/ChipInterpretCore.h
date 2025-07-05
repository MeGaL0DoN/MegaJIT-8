#include <random>

#include "ChipState.h"
#include "ChipCore.h"
#include "Quirks.h"
#include "macros.h"

extern ChipState s;

class ChipInterpretCore : public ChipCore
{
public:
	FORCE_INLINE void execute()
	{
		const uint16_t opcode = (s.RAM[s.pc & 0xFFF] << 8) | s.RAM[(s.pc + 1) & 0xFFF];
		s.pc += 2;

		#define nnn (opcode & 0x0FFF)
		#define nn (opcode & 0x00FF)
		#define n (opcode & 0x000F)

		#define x ((opcode & 0x0F00) >> 8)
		#define regX s.V[x]
		#define regY s.V[(opcode & 0x00F0) >> 4]

		#define skipInstr() s.pc += 2

		switch (opcode & 0xF000)
		{
		case 0x0000:
		{
			switch (nnn)
			{
			case 0x00E0: 
				std::memset(s.screenBuffer.data(), 0, sizeof(s.screenBuffer));
				break;
			case 0x00EE: 
				s.pc = s.stack[--s.sp];
				break;
			}
			break;
		}
		case 0x1000: 
			s.pc = nnn;
			break;  
		case 0x2000:
			s.stack[s.sp++] = s.pc;
			s.pc = nnn;
			break;
		case 0x3000:
			if (regX == nn) skipInstr();
			break;
		case 0x4000:
			if (regX != nn) skipInstr();
			break;
		case 0x5000:
			switch (n)
			{
			case 0x0000:
				if (regX == regY) skipInstr();
				break;
			}
			break;
		case 0x6000:
			regX = nn;
			break;
		case 0x7000: 
			regX += nn;
			break;
		case 0x8000:
			switch (n)
			{
			case 0x0000:
				regX = regY;
				break;
			case 0x0001:
				regX |= regY;
				if (Quirks::VFReset) s.V[0xF] = 0;
				break;
			case 0x0002:
				regX &= regY;
				if (Quirks::VFReset) s.V[0xF] = 0;
				break;
			case 0x0003:
				regX ^= regY;
				if (Quirks::VFReset) s.V[0xF] = 0;
				break;
			case 0x0004:
			{
				regX += regY;
				s.V[0xF] = regX < regY;
				break;
			}
			case 0x0005:
			{
				const uint8_t flag = regX >= regY;
				regX -= regY;
				s.V[0xF] = flag;
				break;
			}
			case 0x0006: 
			{
			    if (!Quirks::Shifting) regX = regY;
				const uint8_t lsb = regX & 0x1;
				regX >>= 1;
				s.V[0xF] = lsb;
				break;
			}
			case 0x0007: 
				regX = regY - regX;
				s.V[0xF] = regY >= regX;
				break;
			case 0x000E: 
			{
				if (!Quirks::Shifting) regX = regY;
				const uint8_t msb = regX >> 7;
				regX <<= 1;
				s.V[0xF] = msb;
				break;
			}
			}
			break;
		case 0x9000:
			switch (n)
			{
			case 0x0000:
				if (regX != regY) skipInstr();
				break;
			}
			break;
		case 0xA000:
			s.I = nnn;
			break;
		case 0xB000:
			if (Quirks::Jumping) s.pc = regX + nnn;
			else s.pc = s.V[0] + nnn;
			break;
		case 0xC000:
			regX = rngDistr(rngEng) & nn;
			break;
		case 0xD000: 
			drawSprite(regX & (ChipState::SCR_WIDTH - 1), regY & (ChipState::SCR_HEIGHT - 1), n);
			break;
		case 0xE000:
			switch (nn)
			{
			case 0x009E:
				if (s.keys[regX & 0xF]) skipInstr();
				break;
			case 0x00A1:
				if (!s.keys[regX & 0xF]) skipInstr();
				break;
			}
			break;
		case 0xF000:
			switch (nn)
			{
			case 0x0007:
				regX = s.delayTimer;
				break;
			case 0x000A: 
				if (s.firstFX0ACall)
				{
					s.inputReg = &regX;
					s.firstFX0ACall = false;
				}
				else if (s.inputReg == nullptr)
				{
					s.firstFX0ACall = true;
					break;
				}

				s.pc -= 2;
				break;
			case 0x001E:
				s.I += regX;
				break;
			case 0x0015:
				s.delayTimer = regX;
				break;
			case 0x0018:
				s.soundTimer = regX;
				break;
			case 0x0029:
				s.I = (regX & 0xF) * 0x5;
				break;
			case 0x0033:
				s.RAM[s.I & 0xFFF] = regX / 100;
				s.RAM[(s.I + 1) & 0xFFF] = (regX / 10) % 10;
				s.RAM[(s.I + 2) & 0xFFF] = regX % 10;
				break;
			case 0x0055:
				for (int i = 0; i <= x; i++)
					s.RAM[(s.I + i) & 0xFFF] = s.V[i];

				if (Quirks::MemoryIncrement) s.I += x + 1;
				break;
			case 0x0065:
				for (int i = 0; i <= x; i++)
					s.V[i] = s.RAM[(s.I + i) & 0xFFF];

				if (Quirks::MemoryIncrement) s.I += x + 1;
				break;
			}
			break;
		}

		#undef n
		#undef nn
		#undef nnn
		#undef x
		#undef regX
		#undef regY
		#undef skipInstr
	}

private:
	std::default_random_engine rngEng { std::random_device{}() };
	std::uniform_int_distribution<> rngDistr { 0, 255 };

	void initialize() override
	{
		s.reset();
	}

	void drawSprite(uint8_t Xpos, uint8_t Ypos, uint8_t height)
	{
		s.V[0xF] = 0;
		const bool partialDraw { Xpos > 56 };

		for (int i = 0; i < height; i++)
		{
			const uint8_t spriteRow { s.RAM[(s.I + i) & 0xFFF] };

			if (Quirks::Clipping)
			{
				if (Ypos >= ChipState::SCR_HEIGHT)
					break;
			}
			else
				Ypos &= (ChipState::SCR_HEIGHT - 1);

			uint64_t spriteMask;

			if (partialDraw)
			{
				const uint64_t leftPart { static_cast<uint64_t>(spriteRow) >> (Xpos - 56) };

				if (Quirks::Clipping)
					spriteMask = leftPart;
				else
				{
					const uint64_t rightPart { static_cast<uint64_t>(spriteRow) << (64 - (Xpos - 56)) };
					spriteMask = leftPart | rightPart;
				}
			}
			else
				spriteMask = static_cast<uint64_t>(spriteRow) << (63 - Xpos - 7);

			uint64_t& screenRow { s.screenBuffer[Ypos] };
			s.V[0xF] |= ((screenRow & spriteMask) != 0);

			screenRow ^= spriteMask;
			Ypos++;
		}
	}
};