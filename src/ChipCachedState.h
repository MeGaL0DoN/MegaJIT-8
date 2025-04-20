#pragma once

#include <vector>
#include "ChipState.h"

struct CacheBlock
{
	uint16_t cacheOffset{};
	uint16_t startPC{};
	uint16_t endPC{};
	uint8_t instrCount{};

	CacheBlock(uint16_t startPC) : startPC(startPC)
	{}
};

struct CacheMapEntry
{
	bool isValid { false };
	int16_t block { -1 };
};

struct ChipCachedState
{
	std::array<CacheMapEntry, ChipState::RAM_SIZE> blockMap{};
	std::vector<CacheBlock> blocks{};

	inline void reset()
	{
		blocks.clear();
		std::fill(blockMap.begin(), blockMap.end(), CacheMapEntry{});
	}
};