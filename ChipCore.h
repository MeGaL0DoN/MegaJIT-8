#pragma once
#include <filesystem>
#include <cstring>
#include <fstream>

#include "ChipState.h"

extern ChipState s;

class ChipCore
{
public:
	static inline bool enableAudio;
	static void initAudio();
	static void setVolume(double val);

	bool loadROM(const std::filesystem::path& path)
	{
		std::ifstream ifs(path, std::ios::binary | std::ios::ate);
		if (!ifs) return false;

		std::ifstream::pos_type size = ifs.tellg();

		if (size <= sizeof(s.RAM) - 0x200)
		{
			initialize();
			romLoaded = true;

			ifs.seekg(0, std::ios::beg);
			ifs.read(reinterpret_cast<char*>(&s.RAM[0x200]), size);

			return true;
		}

		return false;
	}

	virtual uint64_t execute() = 0;

	const std::array<uint64_t, ChipState::SCRHeight>& getScreenBuffer() { return s.screenBuffer; }
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
		if (s.delay_timer > 0) s.delay_timer--;
		if (s.sound_timer > 0) s.sound_timer--;
	}

protected:
	static inline bool romLoaded { false };
	virtual void initialize() = 0;
};