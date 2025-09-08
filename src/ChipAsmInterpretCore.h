#pragma once

#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>

#include "ChipState.h"
#include "ChipCore.h"

class ChipAsmInterpretCore : public ChipCore, private Xbyak::CodeGenerator
{
public:
	ChipAsmInterpretCore(ChipState& s, std::atomic<bool>& executeFlag);

	uint64_t execute() override { return codePtr(); }

private:
	uint64_t(*codePtr)();
};