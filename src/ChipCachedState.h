#pragma once

#include <vector>
#include "ChipState.h"

struct CacheBlock
{
	uint16_t pc{};
	uint16_t endPC{};
	uint16_t cacheOffset{};
	uint8_t instrCount{};

	CacheBlock(uint16_t pc) : pc(pc)
	{}
};

struct ChipCachedState
{
	std::array<uint8_t, ChipState::RAM_SIZE> blockMap{};
	std::vector<CacheBlock> blocks{};

	inline void reset()
	{
		blocks.clear();
		std::memset(blockMap.data(), 0, sizeof(blockMap));
	}
};