#pragma once

#include <vector>
#include "ChipState.h"

struct JITBlock
{
	uint16_t pc{};
	uint32_t cacheSize{};
	std::vector<std::pair<uint16_t, uint16_t>> pcRanges{};

	JITBlock(uint16_t pc) : pc(pc) 
	{}
};

struct ChipJITState
{
	std::array<uint8_t*, ChipState::RAM_SIZE> blockMap{};
	std::vector<JITBlock> blocks{};
};