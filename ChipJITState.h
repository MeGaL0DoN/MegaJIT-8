#pragma once

#include <vector>
#include "ChipState.h"

class ChipEmitter;

struct JITBlock
{
	uint16_t startPC;
	uint16_t endPC {};

	uint16_t cacheOffset{};
	uint16_t cacheSize{};

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
	static constexpr uint16_t BLOCK_SIZE = 4096;

	std::array<JITMapEntry, ChipState::RAM_SIZE> blockMap{};
	std::vector<JITBlock> blocks{};

	inline void reset()
	{
		blocks.clear();
		std::fill(blockMap.begin(), blockMap.end(), JITMapEntry{});
	}
};