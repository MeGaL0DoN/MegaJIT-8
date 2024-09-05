#pragma once

#include <vector>
#include "ChipState.h"

struct JITBlock
{
	uint16_t startPC{};
	uint16_t endPC{};

	uint32_t cacheSize{};
	uint32_t cacheOffset{};

	JITBlock(uint16_t startPC) : startPC(startPC) 
	{
	}
};

struct JITMapEntry
{
	bool isValid { false };
	int16_t block{ -1 };
};

struct ChipJITState
{
	std::array<JITMapEntry, ChipState::RAM_SIZE> blockMap{};
	std::vector<JITBlock> blocks{};

	inline void reset()
	{
		blocks.clear();
		std::fill(blockMap.begin(), blockMap.end(), JITMapEntry{});
	}
};