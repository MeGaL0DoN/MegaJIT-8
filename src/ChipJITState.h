#pragma once

#include <array>
#include <vector>
#include "ChipState.h"

struct JITBlock
{
	uint16_t pc{};
	uint32_t cacheSize{};
	std::array<uint64_t, ChipState::RAM_SIZE / 64> compiledRam{};

	explicit JITBlock(uint16_t pc) : pc(pc)
	{}
};

struct ChipJITState
{
	std::array<uint8_t*, ChipState::RAM_SIZE> blockMap{};
	std::array<uint64_t, ChipState::RAM_SIZE / 64> compiledRam{};
	std::vector<JITBlock> blocks{};
};