#include <random>

#include "ChipState.h"
#include "ChipCore.h"
#include "Quirks.h"

extern ChipState s;

class ChipInterpretCore : public ChipCore
{
public:
	uint64_t execute() override
	{
		if (!romLoaded || awaitingKeyPress()) [[unlikely]]
			return 0;

		const uint16_t opcode = (s.RAM[s.pc] << 8) | s.RAM[s.pc + 1];
		s.pc += 2;

		#define memoryAddr (opcode & 0x0FFF)
		#define doubleNibble (opcode & 0x00FF)
		#define xOperand ((opcode & 0x0F00) >> 8)

		#define regY s.V[(opcode & 0x00F0) >> 4]
		#define regX s.V[xOperand]

		#define skipNextInstr() s.pc += 2

		switch (opcode & 0xF000)
		{
		case 0x0000:
		{
			switch (opcode & 0x0FFF)
			{
			case 0x00E0: 
				clearScreen();
				break;
			case 0x00EE: 
				s.pc = s.stack[(--s.sp) & 0xF];
				break;
			}
			break;
		}
		case 0x1000: 
			s.pc = memoryAddr;
			break;  
		case 0x2000:
			s.stack[(s.sp++) & 0xF] = s.pc;
			s.pc = memoryAddr;
			break;
		case 0x3000:
			if (regX == doubleNibble) skipNextInstr();
			break;
		case 0x4000:
			if (regX != doubleNibble) skipNextInstr();
			break;
		case 0x5000:
			switch (opcode & 0x000F)
			{
			case 0:
				if (regX == regY) skipNextInstr();
				break;
			}
			break;
		case 0x6000:
			regX = doubleNibble;
			break;
		case 0x7000: 
			regX += doubleNibble;
			break;
		case 0x8000:
			switch (opcode & 0x000F)
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
				int result = regX + regY;
				regX = result;
				s.V[0xF] = result > 255;
				break;
			}
			case 0x0005:
			{
				int result = regX - regY;
				regX = result;
				s.V[0xF] = result >= 0;
				break;
			}
			case 0x0006: 
			{
			    if (!Quirks::Shifting) regX = regY;
				uint8_t lsb = regX & 1;
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
				uint8_t msb = (regX & 0x80) >> 7;
				regX <<= 1;
				s.V[0xF] = msb;
				break;
			}
			}
			break;
		case 0x9000:
			switch (opcode & 0x000F)
			{
			case 0x0000:
				if (regX != regY) skipNextInstr();
				break;
			}
			break;
		case 0xA000:
			s.I = memoryAddr;
			break;
		case 0xB000:
			if (Quirks::Jumping) s.pc = regX + memoryAddr;
			else s.pc = s.V[0] + memoryAddr;
			break;
		case 0xC000:
			regX = rngDistr(rngEng) & doubleNibble;
			break;
		case 0xD000: 
			drawSprite(regX % ChipState::SCRWidth, regY % ChipState::SCRHeight, opcode & 0x000F);
			break;
		case 0xE000:
			switch (opcode & 0x00FF)
			{
			case 0x009E:
				if (s.keys[regX & 0xF]) skipNextInstr();
				break;
			case 0x00A1:
				if (!s.keys[regX & 0xF]) skipNextInstr();
				break;
			}
			break;
		case 0xF000:
			switch (opcode & 0x00FF)
			{
			case 0x0007:
				regX = s.delay_timer;
				break;
			case 0x000A: 
				s.inputReg = &regX;
				break;
			case 0x001E:
				s.I += regX;
				break;
			case 0x0015:
				s.delay_timer = regX;
				break;
			case 0x0018:
				s.sound_timer = regX;
				break;
			case 0x0029:
				s.I = (regX & 0xF) * 0x5;
				break;
			case 0x0033:
				s.RAM[s.I & 0xFFF] = regX / 100;
				s.RAM[(s.I + 1) & 0xFFF] = (regX / 10) % 10;
				s.RAM[(s.I + 2) & 0xFFF] = (regX % 100) % 10;
				break;
			case 0x0055:
				for (int i = 0; i <= (xOperand & 0xF); i++)
					s.RAM[(s.I + i) & 0xFFF] = s.V[i];

				if (Quirks::MemoryIncrement) s.I += xOperand + 1;
				break;
			case 0x0065:
				for (int i = 0; i <= (xOperand & 0xF); i++) 
					s.V[i] = s.RAM[(s.I + i) & 0xFFF];

				if (Quirks::MemoryIncrement) s.I += xOperand + 1;
				break;
			}
			break;
		}

		return 1;

		#undef memoryAddr
		#undef doubleNibble
		#undef xOperand
		#undef regY
		#undef regX
		#undef skipNextInstr
	}

private:
	std::default_random_engine rngEng { std::random_device{}() };
	std::uniform_int_distribution<> rngDistr { 0, 255 };

	void initialize() override
	{
		s.reset();
	}

	inline void clearScreen()
	{
		std::memset(s.screenBuffer.data(), 0, sizeof(s.screenBuffer));
	}

	inline void drawSprite(uint8_t Xpos, uint8_t Ypos, uint8_t height)
	{
		s.V[0xF] = 0;
		const bool partialDraw = Xpos > 56;

		for (int i = 0; i < height; i++)
		{
			uint8_t spriteRow = s.RAM[(s.I + i) & 0xFFF];

			if (Quirks::Clipping)
			{
				if (Ypos >= ChipState::SCRHeight)
					break;
			}
			else
				Ypos %= ChipState::SCRHeight;

			uint64_t spriteMask;

			if (partialDraw)
			{
				uint64_t leftPart = static_cast<uint64_t>(spriteRow) >> (Xpos - 56);

				if (Quirks::Clipping)
					spriteMask = leftPart;
				else
				{
					uint64_t rightPart = static_cast<uint64_t>(spriteRow) << (64 - (Xpos - 56));
					spriteMask = leftPart | rightPart;
				}
			}
			else
				spriteMask = static_cast<uint64_t>(spriteRow) << (63 - Xpos - 7);

			uint64_t& screenRow = s.screenBuffer[Ypos];
			s.V[0xF] |= ((screenRow & spriteMask) != 0);

			screenRow ^= spriteMask;
			Ypos++;
		}
	}
};