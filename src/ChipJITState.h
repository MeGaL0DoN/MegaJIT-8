#pragma once

#include <vector>
#include "ChipState.h"

struct JITBlock
{
	uint16_t pc{};
	std::vector<std::pair<uint16_t, uint16_t>> pcRanges{};

	uint32_t cacheSize{};
	uint32_t cacheOffset{};

	JITBlock(uint16_t pc) : pc(pc) 
	{}
};

struct ChipJITState
{
	std::array<uint8_t, ChipState::RAM_SIZE> blockMap{};
	std::vector<JITBlock> blocks{};

	inline void reset()
	{
		blocks.clear();
		std::memset(blockMap.data(), 0x7F, sizeof(blockMap));
	}
};