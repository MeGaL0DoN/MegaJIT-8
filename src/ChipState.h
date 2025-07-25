#pragma once

#include <array>
#include <cstdint>
#include <atomic>

struct quirkConfig
{
	bool vfReset { true };
	bool memoryIncrement { true };
	bool clipping { true };
	bool shifting { false };
	bool jumping { false };
};

struct ChipState
{
	static constexpr int RAM_SIZE { 4096 };
	static constexpr int RAM_DEADBUF_SIZE { 32 };

	static constexpr int SCR_WIDTH { 64 };
	static constexpr int SCR_HEIGHT { 32 };

	std::array<uint8_t, RAM_SIZE + RAM_DEADBUF_SIZE> RAM{};
	std::array<uint8_t, 16> V{};

	std::array<uint16_t, 256> stack{};
	uint8_t sp{};

	uint16_t I{};
	uint16_t pc{};

	uint8_t delayTimer{};
	uint8_t soundTimer{};

	alignas(32) std::array<uint64_t, SCR_HEIGHT> screenBuffer{};

	std::array<uint8_t, 16> keys{};
	int8_t inputReg { -1 };
	bool firstFX0ACall { true };

	quirkConfig quirks{};

	void reset();
};