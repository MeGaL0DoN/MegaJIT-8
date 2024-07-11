#include "ChipJITState.h"
#include "ChipEmitter.h"

void ChipJITState::invalidateBlocks(uint16_t startAddr, uint16_t endAddr)
{
    for (auto& block : blocks)
    {
        if (block.startPC <= endAddr && block.endPC >= startAddr)
            JIT.blockMap[block.startPC].isValid = false;
    }
}

ChipJITState JIT{};