#pragma once

#include <cstring>
#include <fstream>
#include "ChipState.h"

class ChipCore
{
public:
	static inline bool EnableAudio { false };

	static void initAudio();
	static void setVolume(double val);

	explicit ChipCore(ChipState& s) : s(s)
	{}

	virtual ~ChipCore() = default;

	virtual uint64_t execute() = 0;

	bool loadROM(std::istream& is)
	{
		if (!is) 
			return false;

		is.seekg(0, std::ios::end);
		const std::ifstream::pos_type size { is.tellg() };

		if (size <= (ChipState::RAM_SIZE - 0x200))
		{
			initialize();
			is.seekg(0, std::ios::beg);
			is.read(reinterpret_cast<char*>(&s.RAM[0x200]), size);

			return true;
		}

		return false;
	}

	const std::array<uint64_t, ChipState::SCR_HEIGHT>& getScreenBuffer() const { return s.screenBuffer; }
	bool awaitingKeyPress() const { return s.inputReg != -1; }

	inline void setKey(uint8_t key, bool isPressed) const
	{
		s.keys[key & 0xF] = isPressed;
		if (awaitingKeyPress() && !isPressed)
		{
			s.V[s.inputReg] = key;
			s.inputReg = -1;
		}
	}

	inline void resetKeys() const { std::memset(s.keys.data(), 0, sizeof(s.keys)); }

	inline void updateTimers() const
	{
		if (s.delayTimer > 0) s.delayTimer--;
		if (s.soundTimer > 0) s.soundTimer--;
	}

protected:
	ChipState& s;

	virtual void initialize()
	{
		s.reset();
	}
};