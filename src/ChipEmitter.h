#pragma once

#include <vector>
#include <cstring>
#include <algorithm>
#include <numeric>

#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>

#include "ChipState.h"
#include "macros.h"

class ChipJITCore;

class ChipEmitter : Xbyak::CodeGenerator
{
private:
	ChipJITCore& core;

	Xbyak::util::Cpu cpuCaps;
	bool AVX, BMI2, AMD;

	inline void checkCPUSupport()
	{
		cpuCaps = Xbyak::util::Cpu();
		AVX = cpuCaps.has(Xbyak::util::Cpu::tAVX);
		BMI2 = cpuCaps.has(Xbyak::util::Cpu::tBMI2);
		AMD = cpuCaps.has(Xbyak::util::Cpu::tAMD);
	}
	
	uint8_t* uncompiledBlockHandlerPtr;
	uint8_t* dispatcherPtr;
	uint64_t codeStartIndex;

	int32_t executeFlagBaseOffset;
	int32_t JITMapBaseOffset;
	Xbyak::Label dispatcher, dispatcherEnd;

	std::array<uint8_t, 16> allocatedVRegs{};
	std::array<bool, 16> modifiedVRegs{};

	static constexpr int MAX_ALLOC_REGS { 6  };
	static constexpr uint8_t NOT_ALLOCATED { static_cast<uint8_t>(-1) };

	std::array<Xbyak::Reg8, MAX_ALLOC_REGS> V_REGS_8 { sil, dil, r9b, r10b, r11b, r15b };
	std::array<Xbyak::Reg32, MAX_ALLOC_REGS> V_REGS_32 { esi, edi, r9d, r10d, r11d, r15d };
	std::array<Xbyak::Reg64, MAX_ALLOC_REGS> V_REGS_64 { rsi, rdi, r9, r10, r11, r15 };

#ifdef _WIN32
	static constexpr std::array CALLER_SAVED_V_REGS { false, false, true, true, true, false };
#else
	static constexpr std::array CALLER_SAVED_V_REGS { true, true, true, true, true, false };
#endif

	void MOV_VREG_TO_32(Xbyak::Reg32 dst, uint8_t reg);

	template <typename Op>
	inline void PerformOp(const Xbyak::Operand& op1, const Xbyak::Operand& op2, Op op)
	{
		if (!op1.isMEM() || !op2.isMEM())
			op(op1, op2);
		else
		{
			movzx(ecx, op2);
			op(op1, cl);
		}
	}

	inline void MOV(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			mov(dst, src);
		});
	}
	inline void CMP(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			cmp(dst, src);
		});
	}
	inline void AND(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			and_(dst, src);
		});
	}
	inline void XOR(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			xor_(dst, src);
		});
	}
	inline void OR(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			or_(dst, src);
		});
	}
	inline void SUB(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			sub(dst, src);
		});
	}
	inline void ADD(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			add(dst, src);
		});
	}

	bool emitSaveVregs();
	void emitLoadVregs();
	void emitCallFunc(uint64_t func, bool stackAligned);

	void emitBlockInvalidation(int count, uint16_t pc);
	void emitUncompiledBlockHandler();
	void emitDispatcher();

public:
	static constexpr size_t MAX_CACHE_SIZE { 524288 };

	std::array<int, 16> VRegUsage{};

	uint16_t instructions { 0 };
	uint16_t branchedInstrs { 0 };
	bool blockHasInstrSkips { false };

	ChipEmitter(ChipJITCore& c, std::atomic<bool>& executeFlag);

	inline uint8_t* getCodePtr() const { return const_cast<uint8_t*>(getCode()); }
	inline uint8_t* getCodeEndPtr() const { return getCodePtr() + getSize(); }
	inline size_t getCodeSize() const { return getSize(); }

	uint8_t* getUncompiledPtr() const { return uncompiledBlockHandlerPtr; }

	inline void clearCache()
	{
		setSize(codeStartIndex);
	}

	FORCE_INLINE uint64_t execute() const
	{
		return reinterpret_cast<uint64_t(*)()>(dispatcherPtr)();
	}
	
	void reset();
	void allocateRegs();
	void emitPrologue();
	void emitEpilogue(uint16_t pc = -1);

	void emitIllegalOPHandler();
	void emitBreakpoint();

	uint8_t* emitJumpPlaceholder();
	void emitInstrCountAddPlaceholder();
	void emitInstrCountAdd(int32_t instrs);

	void patchBranchInstr(uint8_t* branchCodeEndPtr, bool incBeforeBranch);
	void patchAddImm32(uint8_t* addCodeEndPtr, int32_t imm);

	void emit00E0();
	void emit00EE();
	void emit1NNN(uint16_t addr);
	void emit2NNN(uint16_t addr, uint16_t pc);
	void emit5XY0(uint8_t x, uint8_t y, bool incBranches);
	void emit9XY0(uint8_t x, uint8_t y, bool incBranches);
	void emit3XNN(uint8_t x, uint8_t val, bool incBranches);
	void emit4XNN(uint8_t x, uint8_t val, bool incBranches);
	void emitEX9E(uint8_t x, bool incBranches);
	void emitEXA1(uint8_t x, bool incBranches);
	void emit6XNN(uint8_t x, uint8_t val);
	void emit7XNN(uint8_t x, uint8_t val);
	void emit8XY0(uint8_t x, uint8_t y);
	void emit8XY1(uint8_t x, uint8_t y, bool calcFlag);
	void emit8XY2(uint8_t x, uint8_t y, bool calcFlag);
	void emit8XY3(uint8_t x, uint8_t y, bool calcFlag);
	void emit8XY4(uint8_t x, uint8_t y, bool calcFlag);
	void emit8XY5(uint8_t x, uint8_t y, bool calcFlag);
	void emit8XY6(uint8_t x, uint8_t y, bool calcFlag);
	void emit8XY7(uint8_t x, uint8_t y, bool calcFlag);
	void emit8XYE(uint8_t x, uint8_t y, bool calcFlag);
	void emitANNN(uint16_t val);
	void emitBNNN(uint16_t addr, uint8_t x);
	void emitCXNN(uint8_t x, uint8_t val);
	void emitDXYN(uint8_t x, uint8_t y, uint8_t n, bool calcFlag);
	void emitFX07(uint8_t x);
	void emitFX15(uint8_t x);
	void emitFX18(uint8_t x);
	void emitFX1E(uint8_t x);
	void emitFX29(uint8_t x);
	void emitFX33(uint8_t x, uint16_t pc);
	void emitFX55(uint8_t x, uint16_t pc);
	void emitFX65(uint8_t x);
	void emitFX0A(uint8_t x, uint16_t pc);
};