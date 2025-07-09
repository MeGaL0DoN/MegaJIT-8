#pragma once

#include <vector>
#include "ChipState.h"

struct JITBlock
{
	uint16_t pc{};
	std::vector<std::pair<uint16_t, uint16_t>> pcRanges{};
	uint32_t cacheSize{};

	JITBlock(uint16_t pc) : pc(pc) 
	{}
};

struct ChipJITState
{
	std::array<uint32_t, ChipState::RAM_SIZE> blockMap{};
	std::vector<JITBlock> blocks{};

	inline void reset()
	{
		blocks.clear();
		std::memset(blockMap.data(), 0, sizeof(blockMap));
	}
};