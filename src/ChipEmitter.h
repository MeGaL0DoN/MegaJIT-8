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

	std::vector<uint8_t> allocatedRegs{};
	bool IregAllocated { false };
	bool stackAligned { false };
	size_t codeStartOffset{};

	Xbyak::util::Cpu cpuCaps;
	bool avxSupport{ false };

	const Xbyak::Reg8* vreg { nullptr };
	const Xbyak::Reg64& V_REG64(uint8_t num);
	bool GET_VREG(uint8_t num);

	template <typename Op>
	inline void PerformOp(const Xbyak::Operand& op1, const Xbyak::Operand& op2, Op op)
	{
		if (op1.isREG() || op2.isREG())
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

	void callFunc(uint64_t func);
	void emitBlockInvalidation(int count, uint16_t pc);
	void emitUncompiledBlockHandler();

	inline void checkCPUSupport()
	{
		cpuCaps = Xbyak::util::Cpu();
		avxSupport = cpuCaps.has(Xbyak::util::Cpu::tAVX);
	}

public:
	static constexpr size_t MAX_CACHE_SIZE { 262144 };

	std::array<uint32_t, 16> VRegUsage{};
	uint8_t IRegUsage { 0 };

	uint16_t instructions { 0 };
	uint16_t branchedInstrs { 0 };
	bool blockHasInstrSkips { false };

	ChipEmitter(ChipJITCore& c) : Xbyak::CodeGenerator(MAX_CACHE_SIZE), core(c)
	{
		checkCPUSupport();
		emitUncompiledBlockHandler();
	}

	inline uint8_t* getCodePtr() const { return const_cast<uint8_t*>(getCode()); }
	inline uint8_t* getCodeEndPtr() const { return getCodePtr() + getSize(); }
	inline size_t getCodeSize() const { return getSize(); }

	inline void clearCache()
	{
		setSize(codeStartOffset);
	}

	FORCE_INLINE uint64_t execute(uint32_t offset) const
	{
		return reinterpret_cast<uint64_t(*)()>(getCodePtr() + offset)();
	}
	
	void reset();
	void allocateRegs();

	void emitPrologue();
	void emitEpilogue(uint16_t pc = -1);

	void emitIllegalOPHandler();
	void emitBreakpoint();

	void emitJumpPlaceholder();
	void patchBranchInstr(uint8_t* branchCodeEndPtr, bool incBeforeBranch);

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