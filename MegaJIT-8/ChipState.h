#pragma once
#include <array>
#include <mutex>

struct ChipState
{
	static constexpr uint16_t RAM_SIZE = 4096;
	static constexpr int SCRWidth = 64;
	static constexpr int SCRHeight = 32;

	std::array<uint8_t, RAM_SIZE> RAM{};

	std::array<uint8_t, 16> V{};
	uint16_t I;
	uint16_t pc;

	uint8_t delay_timer;
	uint8_t sound_timer;

	std::array<uint16_t, 16> stack;
	uint16_t sp;

	std::array<uint8_t, 16> keys{};
	uint8_t* inputReg;

	std::array<uint64_t, SCRHeight> screenBuffer{};

	std::atomic<bool> drawLock { false };
	bool enableDrawLocking { true };

	void reset();
};