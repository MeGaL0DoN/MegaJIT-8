#pragma once
#include <filesystem>
#include <cstring>
#include <fstream>

#include "ChipState.h"

extern ChipState s;

class ChipCore
{
public:
	static inline bool enableAudio { false };
	static void initAudio();
	static void setVolume(double val);

	bool loadROM(std::istream& is)
	{
		if (!is) return false;

		is.seekg(0, std::ios::end);
		const std::ifstream::pos_type size { is.tellg() };

		if (size <= (ChipState::RAM_SIZE - 0x200))
		{
			initialize();
			romLoaded = true;

			is.seekg(0, std::ios::beg);
			is.read(reinterpret_cast<char*>(&s.RAM[0x200]), size);

			return true;
		}

		return false;
	}

	bool isRomLoaded() { return romLoaded; }

	const std::array<uint64_t, ChipState::SCR_HEIGHT>& getScreenBuffer() { return s.screenBuffer; }
	inline bool awaitingKeyPress() { return s.inputReg != nullptr; }

	inline void setKey(uint8_t key, bool isPressed)
	{
		s.keys[key & 0xF] = isPressed;
		if (awaitingKeyPress() && !isPressed)
		{
			*s.inputReg = key;
			s.inputReg = nullptr;
		}
	}

	inline void resetKeys() { std::memset(s.keys.data(), 0, sizeof(s.keys)); }

	inline void updateTimers()
	{
		if (s.delayTimer > 0) s.delayTimer--;
		if (s.soundTimer > 0) s.soundTimer--;
	}

protected:
	static inline bool romLoaded { false };
	virtual void initialize() = 0;
};