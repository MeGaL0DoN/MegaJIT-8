#pragma once

#include <cstring>
#include <optional>

#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>

#include "ChipState.h"
#include "utils.h"

//struct RegAllocation
//{
//	uint8_t ind{};
//	bool needsLoad{};
//	bool needsStore{};
//};

class ChipJITCore;

class ChipEmitter : Xbyak::CodeGenerator
{
public:
	static constexpr size_t MAX_CACHE_SIZE{ 1048576 };
	static constexpr int MAX_ALLOC_REGS{ 7 };
	static constexpr uint8_t NOT_ALLOCATED{ static_cast<uint8_t>(-1) };

private:
	ChipJITCore& core;

	Xbyak::util::Cpu cpuCaps;
	bool SSE41, AVX, AVX2, BMI2, AMD_CPU;

	void checkCPUSupport()
	{
		cpuCaps = Xbyak::util::Cpu();
		SSE41 = cpuCaps.has(Xbyak::util::Cpu::tSSE41);
		AVX = cpuCaps.has(Xbyak::util::Cpu::tAVX);
		AVX2 = cpuCaps.has(Xbyak::util::Cpu::tAVX2);
		BMI2 = cpuCaps.has(Xbyak::util::Cpu::tBMI2);
		AMD_CPU = cpuCaps.has(Xbyak::util::Cpu::tAMD);
	}

	uint8_t* uncompiledBlockHandlerPtr;
	uint64_t(*dispatcherPtr)();
	uint64_t codeStartIndex;

	int32_t executeFlagBaseOffset;
	Xbyak::Label dispatcherEnd, rspBackup;

	std::array<uint8_t, 16> allocatedVRegs{};

	static constexpr int RSP_ALLOC_IND { 5 };
	std::optional<uint8_t> rspAllocReg;

	std::array<Xbyak::Reg8, MAX_ALLOC_REGS> V_REGS_8 { sil, dil, r9b, r10b, r11b, spl, r15b };
	std::array<Xbyak::Reg32, MAX_ALLOC_REGS> V_REGS_32 { esi, edi, r9d, r10d, r11d, esp, r15d };
	std::array<Xbyak::Reg64, MAX_ALLOC_REGS> V_REGS_64 { rsi, rdi, r9, r10, r11, rsp, r15 };

#ifdef _WIN32
	static constexpr std::array CALLER_SAVED_V_REGS{ false, false, true, true, true, false, false };
#else
	static constexpr std::array CALLER_SAVED_V_REGS{ true, true, true, true, true, false, false };
#endif

	void MOV_TO_REG(uint8_t r, const Xbyak::Operand& op);

	template <typename Op>
	void PerformOp(const Xbyak::Operand& op1, const Xbyak::Operand& op2, Op op)
	{
		if (!op1.isMEM() || !op2.isMEM())
			op(op1, op2);
		else
		{
			movzx(eax, op2);
			op(op1, al);
		}
	}

	void MOV(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			mov(dst, src);
			});
	}
	void CMP(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			cmp(dst, src);
			});
	}
	void AND(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			and_(dst, src);
			});
	}
	void XOR(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			xor_(dst, src);
			});
	}
	void OR(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			or_(dst, src);
			});
	}
	void SUB(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			sub(dst, src);
			});
	}
	void ADD(const Xbyak::Operand& op1, const Xbyak::Operand& op2) {
		PerformOp(op1, op2, [this](const Xbyak::Operand& dst, const Xbyak::Operand& src) {
			add(dst, src);
			});
	}

	template <bool toMem>
	void emitRegCopy(int cnt);
	void emitSelfModifyingCodeCheck(int cnt, uint16_t pc);

	bool emitPushAllocRegs();
	void emitPopAllocRegs();

	template<typename T>
	void emitCallFunc(T func, bool stackAligned);

	void emitUncompiledBlockHandler();
	void emitDispatcher();

public:
	std::array<int, 16> VRegWeight{};
	std::array<bool, 16> modifiedVRegs{};
	std::array<bool, 16> initialValUseVRegs{};

	//std::array<RegAllocation, 16> allocatedRegs;
	uint16_t instructions{ 0 };
	uint16_t branchedInstrs{ 0 };

	ChipEmitter(ChipJITCore& c, std::atomic<bool>& executeFlag);

	uint8_t* getCodePtr() const { return getCode<uint8_t*>(); }
	uint8_t* getCodeEndPtr() const { return getCurr<uint8_t*>(); }
	size_t getCodeSize() const { return getSize(); }

	uint8_t* getUncompiledPtr() const { return uncompiledBlockHandlerPtr; }

	void clearCache() { setSize(codeStartIndex); }

	FORCE_INLINE uint64_t execute() const { return dispatcherPtr(); }

	uint8_t* newBlock();
	void allocateRegs();
	void emitPrologue();
	void emitEpilogue(uint16_t pc = -1);

	//void emitLoadAllocRegs();
	//void emitStoreAllocRegs();

	void emitIllegalOPHandler();
	void emitBreakpoint();

	uint8_t* emitJumpPlaceholder();
	void emitInstrCountAddPlaceholder();
	void emitInstrCountAdd(int32_t instrs);

	void patchBranchInstr(uint8_t* branchCodeEndPtr, bool incBeforeBranch) const;
	void patchImm32(uint8_t* codeEndPtr, int32_t imm) const;

	void emit00E0();
	void emit00EE(bool restorePC);
	void emit1NNN(uint16_t addr);
	void emit2NNN(uint16_t addr, uint16_t pc, bool setPC);
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