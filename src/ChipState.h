#pragma once
#include <array>
#include <cstdint>

struct ChipState
{
	static constexpr uint16_t RAM_SIZE { 4096 };
	static constexpr int SCR_WIDTH { 64 };
	static constexpr int SCR_HEIGHT { 32 };

	std::array<uint8_t, RAM_SIZE> RAM{};
	std::array<uint8_t, 32> RAM_DEAD_BUF{};

	std::array<uint8_t, 16> V{};
	uint16_t I{};
	uint16_t pc{};

	uint8_t delayTimer{};
	uint8_t soundTimer{};

	std::array<uint16_t, 256> stack{};
	uint8_t sp{};

	std::array<uint8_t, 16> keys{};
	uint8_t* inputReg{};
	bool firstFX0ACall { true };

	std::array<uint64_t, SCR_HEIGHT> screenBuffer{};

	void reset();
};