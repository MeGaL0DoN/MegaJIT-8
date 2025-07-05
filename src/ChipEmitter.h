#pragma once

#include <vector>
#include <cstring>
#include <algorithm>
#include <numeric>

#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>

#include "ChipState.h"
#include "ChipJITState.h"
#include "Quirks.h"
#include "macros.h"

extern ChipState s;
extern ChipJITState JIT;

class ChipEmitter : Xbyak::CodeGenerator
{
private:

#ifdef _WIN32
#define ARG1 rcx
#define ARG2 rdx
#define ARG3 r8
#define ARG4 r9
#else
#define ARG1 rdi
#define ARG2 rsi
#define ARG3 rdx
#define ARG4 rcx
#endif

#define BASE r11
#define BRANCH_SKIP_REG r10

#define SP byte[BASE + offsetof(ChipState, sp)]
#define PC word[BASE + offsetof(ChipState, pc)]
#define STACK_PTR(offset) word[BASE + offsetof(ChipState, stack) + (offset * sizeof(uint16_t))]
#define KEY(offset) byte[BASE + offsetof(ChipState, keys) + offset]
#define REG_PTR(num) byte[BASE + offsetof(ChipState, V) + num]
#define I_REG_PTR word[BASE + offsetof(ChipState, I)]
#define RAM_PTR(offset) byte[BASE + offsetof(ChipState, RAM) + offset]
#define SCREEN_PTR(offset) qword[BASE + offsetof(ChipState, screenBuffer) + (offset * sizeof(uint64_t))]

#ifdef _WIN32
	static constexpr size_t MAX_ALLOC_REGS { 7 };
#else
	// sil and dil are callee saved only in Windows calling convention.
	static constexpr size_t MAX_ALLOC_REGS { 5 };
#endif

	std::vector<uint8_t> allocatedRegs{};
	bool IregAllocated { false };

#define I_FULL_REG r15

	const Xbyak::Operand& V_FULL_REG(uint8_t num)
	{
		switch (num)
		{
			case 0: return rbx;
			case 1: return rbp;
			case 2: return r12;
			case 3: return r13;
			case 4: return r14;
			case 5: return rsi;
			case 6: return rdi;
			default: UNREACHABLE()
		}
	}

	const Xbyak::Reg8* Vreg { nullptr };

	inline bool GET_VREG(uint8_t num)
	{
		const auto pos { std::find(allocatedRegs.begin(), allocatedRegs.end(), num) };

		if (pos != allocatedRegs.end())
		{
			switch (std::distance(allocatedRegs.begin(), pos))
			{
				case 0: Vreg = &bl; break;
				case 1: Vreg = &bpl; break;
				case 2: Vreg = &r12b; break;
				case 3: Vreg = &r13b; break;
				case 4: Vreg = &r14b; break;
				case 5: Vreg = &sil; break;
				case 6: Vreg = &dil; break;
			}

			return true;
		}

		return false;
	}

#define V_REG(num) (GET_VREG(num) ? (const Xbyak::Operand&)*Vreg : (const Xbyak::Operand&)REG_PTR(num))
#define I_REG (IregAllocated ? (const Xbyak::Operand&)r15w : (const Xbyak::Operand&)I_REG_PTR)
#define FLAG_REG V_REG(0xF)

	template <typename Op>
	inline void PerformOp(const Xbyak::Operand& op1, const Xbyak::Operand& op2, Op op)
	{
		if (op1.isREG() || op2.isREG())
			op(op1, op2);
		else
		{
			mov(cl, op2);
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

	inline void callFunc(uint64_t func)
	{
		push(BASE);
		if (blockHasInstrSkips) push(BRANCH_SKIP_REG);

		mov(rax, func);
#ifdef _WIN32
		sub(rsp, 32);
#endif
		call(rax);
#ifdef _WIN32
		add(rsp, 32);
#endif
		if (blockHasInstrSkips) pop(BRANCH_SKIP_REG);
		pop(BASE);
	}

	template <bool toMem>
	inline void store(int count, const Xbyak::Label& end)
	{
		movzx(eax, I_REG);
		if constexpr (toMem)
		{
			cmp(ax, 0xFFF - count);
			ja(end, T_NEAR);
		}

		for (int i = 0; i <= count; i++)
		{
			if constexpr (toMem)
				MOV(RAM_PTR(i + rax), V_REG(i));
			else
				MOV(V_REG(i), RAM_PTR(i + rax));
		}
	}

	static bool invalidateBlocks(uint16_t startAddr, uint16_t endAddr)
	{
		bool invalidated { false };

		for (const auto& block : JIT.blocks)
		{
			for (const auto& range : block.pcRanges)
			{
            	if (range.first <= endAddr && range.second >= startAddr) 
				{
					JIT.blockMap[block.pc] &= (~0x80);
					invalidated = true;
                	break;
				}
            }
        }

		return invalidated;
	}

	inline void emitBlockInvalidation(int count, uint16_t pc)
	{
		movzx(ARG1, I_REG);
		lea(ARG2, ptr[ARG1 + count]);
		callFunc((size_t)invalidateBlocks);

		Xbyak::Label noSelfModifyingCode;
		test(al, al);
		jz(noSelfModifyingCode);
		// early block end if self-modifying code has occurred.
		emitEpilogue(pc);
		L(noSelfModifyingCode);
	}

	Xbyak::util::Cpu cpuCaps;
	bool avxSupport { false };

	inline void checkCPUSupport()
	{
		cpuCaps = Xbyak::util::Cpu();
		avxSupport = cpuCaps.has(Xbyak::util::Cpu::tAVX);
	}

public:
	static constexpr size_t MAX_CACHE_SIZE { 262144 };
	static constexpr size_t ALLOC_THRESHOLD { 3 };

	std::array<uint32_t, 16> VRegUsage{};
	uint8_t IRegUsage { 0 };

	uint16_t instructions { 0 };
	uint16_t branchedInstrs { 0 };
	bool blockHasInstrSkips { false };

	ChipEmitter() : Xbyak::CodeGenerator(MAX_CACHE_SIZE)
	{
		checkCPUSupport();
	}
	
	void resetState()
	{
		allocatedRegs.clear();
		std::memset(VRegUsage.data(), 0, sizeof(VRegUsage));
		IRegUsage = 0;
		IregAllocated = false;
		instructions = 0;
		branchedInstrs = 0;
		blockHasInstrSkips = false;
	}

	void allocateRegs()
	{
		std::vector<std::pair<uint8_t, uint32_t>> usageList{};
		usageList.reserve(16);

		for (int i = 0; i < 16; i++)
		{
			if (VRegUsage[i] >= ALLOC_THRESHOLD)
				usageList.push_back({ i, VRegUsage[i] });
		}

		std::sort(usageList.begin(), usageList.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

		for (const auto [reg, _] : usageList)
		{
			allocatedRegs.push_back(reg);

			if (allocatedRegs.size() == MAX_ALLOC_REGS)
				break;
		}

		if (IRegUsage >= ALLOC_THRESHOLD)
			IregAllocated = true;
	}

	void emitPrologue()
	{
		mov(BASE, (size_t)&s);

		if (blockHasInstrSkips)
			xor_(BRANCH_SKIP_REG, BRANCH_SKIP_REG);

		if (IregAllocated)
		{
			push(I_FULL_REG);
			movzx(I_FULL_REG, I_REG_PTR);
		}

		for (int i = 0; i < allocatedRegs.size(); i++)
		{
			push(V_FULL_REG(i));
			mov(V_REG(allocatedRegs[i]), REG_PTR(allocatedRegs[i]));
		}
	}

	void emitEpilogue(uint16_t pc = -1)
	{
		for (int i = allocatedRegs.size() - 1; i >= 0; i--)
		{
			mov(REG_PTR(allocatedRegs[i]), V_REG(allocatedRegs[i]));
			pop(V_FULL_REG(i));
		}

		if (blockHasInstrSkips)
			lea(eax, ptr[BRANCH_SKIP_REG + instructions - branchedInstrs]);
		else
			mov(eax, instructions - branchedInstrs);

		if (IregAllocated)
		{
			mov(I_REG_PTR, I_REG);
			pop(I_FULL_REG);
		}

		if (pc != static_cast<uint16_t>(-1))
			mov(PC, pc);

		ret();
	}

	void emitIllegalOPHandler()
	{

	}

	void emitBreakpoint() { int3(); }

	FORCE_INLINE uint64_t execute(uint32_t offset) const
	{
		return reinterpret_cast<uint32_t(*)()>(const_cast<uint8_t*>(getCode()) + offset)();
	}

	inline void emit00E0()
	{
		lea(rcx, ptr[BASE + offsetof(ChipState, screenBuffer)]);

		if (avxSupport)
		{
			vxorpd(ymm0, ymm0, ymm0);

			for (int i = 0; i < 32; i += 4)
				vmovdqu(ptr[rcx + i * 8], ymm0);

			vzeroupper();
		}
		else
		{
			pxor(xmm0, xmm0);

			for (int i = 0; i < 32; i += 2)
				movdqu(ptr[rcx + i * 8], xmm0);
		}
	}

	inline uint8_t* getCodePtr() const { return const_cast<uint8_t*>(getCode()); }
	inline uint8_t* getCodeEndPtr() const { return getCodePtr() + getSize(); }
	inline size_t getCodeSize() const { return getSize(); }
 
	inline void clearCache() { resetSize(); }

	inline void emit00EE()
	{
		dec(SP);
		movzx(ecx, SP);
		mov(cx, STACK_PTR(rcx));
		mov(PC, cx);
	}

	inline void emit1NNN(uint16_t addr)
	{
		mov(PC, addr);
	}

	inline void emit2NNN(uint16_t addr, uint16_t pc)
	{
		movzx(ecx, SP);
		mov(STACK_PTR(rcx), pc);
		mov(PC, addr);
		inc(SP);
	}

	inline void emitUncondJumpPlaceholder()
	{
		db(0xE9);
		dd(0);
	}

	inline void patchBranchInstr(uint8_t* branchCodeEndPtr, bool incBeforeBranch)
	{
	 	constexpr auto INC_R64_SIZE { 3 };
		const auto offset { incBeforeBranch ? INC_R64_SIZE : 0 };

	 	int32_t* jumpOffsetPtr { reinterpret_cast<int32_t*>(branchCodeEndPtr - offset - sizeof(int32_t)) };
	 	const int32_t jumpOffset { static_cast<int32_t>(getCodeEndPtr() - branchCodeEndPtr + offset) };
	 	*jumpOffsetPtr = jumpOffset;
	}

	inline void emit5XY0(uint8_t x, uint8_t y, bool incBranches)
	{
		CMP(V_REG(x), V_REG(y));
		// jz
		db(0x0F);
		db(0x84);
		dd(0); // reserve 4 bytes for offset

		if (incBranches)
			inc(BRANCH_SKIP_REG);
	}
	inline void emit9XY0(uint8_t x, uint8_t y, bool incBranches)
	{
		CMP(V_REG(x), V_REG(y));
		// jnz
		db(0x0F);
		db(0x85);
		dd(0);
	
		if (incBranches)
			inc(BRANCH_SKIP_REG);
	}
	inline void emit3XNN(uint8_t x, uint8_t val, bool incBranches)
	{
		cmp(V_REG(x), val);
		// jz
		db(0x0F);
		db(0x84);
		dd(0);
		
		if (incBranches)
			inc(BRANCH_SKIP_REG);
	}
	inline void emit4XNN(uint8_t x, uint8_t val, bool incBranches)
	{
		cmp(V_REG(x), val);
		// jnz
		db(0x0F);
		db(0x85);
		dd(0);
		
		if (incBranches)
			inc(BRANCH_SKIP_REG);
	}

	inline void emitEX9E(uint8_t x, bool incBranches)
	{
		mov(cl, V_REG(x));
		and_(ecx, 0xF);
		mov(cl, KEY(rcx));
		test(cl, cl);
		// jnz
		db(0x0F);
		db(0x85);
		dd(0);
		
		if (incBranches)
			inc(BRANCH_SKIP_REG);
	}
	inline void emitEXA1(uint8_t x, bool incBranches)
	{
		mov(cl, V_REG(x));
		and_(ecx, 0xF);
		mov(cl, KEY(rcx));
		test(cl, cl);
		// jz
		db(0x0F);
		db(0x84);
		dd(0);
		
		if (incBranches)
			inc(BRANCH_SKIP_REG);
	}

	inline void emit6XNN(uint8_t x, uint8_t val)
	{
		mov(V_REG(x), val);
	}

	inline void emit7XNN(uint8_t x, uint8_t val)
	{
		add(V_REG(x), val);
	}

	inline void emit8XY0(uint8_t x, uint8_t y)
	{
		MOV(V_REG(x), V_REG(y));
	}
	inline void emit8XY1(uint8_t x, uint8_t y, bool calcFlag)
	{
		OR(V_REG(x), V_REG(y));

		if (Quirks::VFReset && calcFlag)
			mov(FLAG_REG, 0);
	}
	inline void emit8XY2(uint8_t x, uint8_t y, bool calcFlag)
	{
		AND(V_REG(x), V_REG(y));

		if (Quirks::VFReset && calcFlag) 
			mov(FLAG_REG, 0);
	}
	inline void emit8XY3(uint8_t x, uint8_t y, bool calcFlag)
	{
		XOR(V_REG(x), V_REG(y));

		if (Quirks::VFReset && calcFlag)
			mov(FLAG_REG, 0);
	}
	inline void emit8XY4(uint8_t x, uint8_t y, bool calcFlag)
	{
		ADD(V_REG(x), V_REG(y));

		if (calcFlag)
			setc(FLAG_REG);
	}
	inline void emit8XY5(uint8_t x, uint8_t y, bool calcFlag)
	{
		SUB(V_REG(x), V_REG(y));

		if (calcFlag)
			setnc(FLAG_REG);
	}
	inline void emit8XY6(uint8_t x, uint8_t y, bool calcFlag)
	{
		if (x == 0xF)
		{
			if (calcFlag)
			{
				if (!Quirks::Shifting && x != y)
					MOV(V_REG(x), V_REG(y));

				and_(FLAG_REG, 0x1);
			}
		}
		else
		{
			if (!Quirks::Shifting && x != y)
				MOV(V_REG(x), V_REG(y));

			shr(V_REG(x), 1);

			if (calcFlag)
				setc(FLAG_REG);
		}
	}
	inline void emit8XY7(uint8_t x, uint8_t y, bool calcFlag)
	{
		mov(al, V_REG(y));
		sub(al, V_REG(x));

		if (x != 0xF)
			mov(V_REG(x), al);

		if (calcFlag)
			setnc(FLAG_REG);
	}

	inline void emit8XYE(uint8_t x, uint8_t y, bool calcFlag)
	{
		if (x == 0xF)
		{
			if (calcFlag)
			{
				if (!Quirks::Shifting && x != y)
					MOV(V_REG(x), V_REG(y));

				shr(FLAG_REG, 7);
			}
		}
		else
		{
			if (!Quirks::Shifting && x != y)
				MOV(V_REG(x), V_REG(y));

			shl(V_REG(x), 1);

			if (calcFlag)
				setc(FLAG_REG);
		}
	}

	inline void emitANNN(uint16_t val)
	{
		mov(I_REG, val);
	}

	inline void emitBNNN(uint16_t addr, uint8_t x)
	{
		movzx(eax, Quirks::Jumping ? V_REG(x) : V_REG(0));
		add(ax, addr);
		and_(ax, 0xFFF);
		mov(PC, ax);
	}

	inline void emitCXNN(uint8_t x, uint8_t val)
	{
		rdtsc(); // using cpu timestamp as a random number
		and_(eax, val);
		mov(V_REG(x), al);
	}

	inline void emitDXYN(uint8_t x, uint8_t y, uint8_t n, bool calcFlag)
	{
		if (n == 0)
		{
			if (calcFlag)
				mov(FLAG_REG, 0);

			return;
		}

		Xbyak::Label loopEnd;

		movzx(ecx, V_REG(x));
		and_(ecx, (ChipState::SCR_WIDTH - 1));

		movzx(eax, V_REG(y));
		and_(eax, (ChipState::SCR_HEIGHT - 1));

		movzx(r8d, I_REG);

		if (calcFlag)
			mov(FLAG_REG, 0);

		for (int i = 0; i < n; i++)
		{
			movzx(edx, RAM_PTR(i + r8));

			if (calcFlag)
				mov(r9, SCREEN_PTR(rax));

			shl(rdx, ChipState::SCR_WIDTH - 8);

			if (Quirks::Clipping)
				shr(rdx, cl);
			else
				ror(rdx, cl);

			if (calcFlag)
			{
				test(r9, rdx);
				Xbyak::Label skip;
				jz(skip);
				mov(FLAG_REG, 1);
				L(skip);
				xor_(r9, rdx);
				mov(SCREEN_PTR(rax), r9);
			}
			else
				xor_(SCREEN_PTR(rax), rdx);

			if (i != (n - 1))
			{
				if (Quirks::Clipping)
				{
					cmp(eax, (ChipState::SCR_HEIGHT - 1));
					je(loopEnd, T_NEAR);
					inc(eax);
				}
				else
				{
					inc(eax);
					and_(eax, (ChipState::SCR_HEIGHT - 1));
				}
			}
		}

		L(loopEnd);
	}

	inline void emitFX07(uint8_t x)
	{
		MOV(V_REG(x), byte[BASE + offsetof(ChipState, delayTimer)]);
	}

	inline void emitFX15(uint8_t x)
	{
		MOV(byte[BASE + offsetof(ChipState, delayTimer)], V_REG(x));
	}
	inline void emitFX18(uint8_t x)
	{
		MOV(byte[BASE + offsetof(ChipState, soundTimer)], V_REG(x));
	}

	inline void emitFX1E(uint8_t x)
	{
		movzx(eax, V_REG(x));
		add(I_REG, ax);
		and_(I_REG, 0xFFF);
	}

	inline void emitFX29(uint8_t x)
	{
		movzx(ecx, V_REG(x));
		and_(ecx, 0xF);
		lea(ecx, ptr[rcx + (rcx * 4)]);
		mov(I_REG, cx);
	}

	inline void emitFX33(uint8_t x, uint16_t pc)
	{
		Xbyak::Label end;
		movzx(r8d, I_REG);
		movzx(eax, V_REG(x));

		lea(edx, ptr[rax + 4 * rax]);
		lea(ecx, ptr[rax + 8 * rdx]);
		shr(ecx, 12);
		mov(RAM_PTR(r8), cl);
		cmp(r8w, 0xFFE);
		ja(end, T_NEAR);
		imul(ecx, eax, 205);
		shr(ecx, 11);
		lea(edx, ptr[rcx + 4 * rcx]);
		lea(edx, ptr[rdx + 4 * rdx]);
		add(edx, ecx);
		shr(edx, 7);
		and_(edx, 6);
		lea(edx, ptr[rdx + 4 * rdx]);
		mov(r9d, ecx);
		sub(r9b, dl);
		mov(RAM_PTR(1 + r8), r9b);
		cmp(r8w, 0xFFD);
		ja(end, T_NEAR);
		add(ecx, ecx);
		lea(ecx, ptr[rcx + 4 * rcx]);
		sub(al, cl);
		mov(RAM_PTR(2 + r8), al);

		emitBlockInvalidation(2, pc);
		L(end);
	}

	inline void emitFX55(uint8_t x, uint16_t pc)
	{
		Xbyak::Label end;
		store<true>(x, end);
		emitBlockInvalidation(x, pc);

		if (Quirks::MemoryIncrement)
			add(I_REG, x + 1);

		L(end);
	}

	inline void emitFX65(uint8_t x)
	{
		Xbyak::Label end;
		store<false>(x, end);

		if (Quirks::MemoryIncrement)
			add(I_REG, x + 1);

		L(end);
	}

	inline void emitFX0A(uint8_t x, uint16_t pc)
	{
		Xbyak::Label firstCall, end;

		mov(PC, pc - 2);
		cmp(byte[BASE + offsetof(ChipState, firstFX0ACall)], 1);
		jz(firstCall);

		cmp(qword[BASE + offsetof(ChipState, inputReg)], 0);
		jnz(end);
		mov(byte[BASE + offsetof(ChipState, firstFX0ACall)], 1);
		mov(PC, pc);
		jmp(end);

		L(firstCall);
		lea(rax, REG_PTR(x));
		mov(qword[BASE + offsetof(ChipState, inputReg)], rax);
		mov(byte[BASE + offsetof(ChipState, firstFX0ACall)], 0);

		L(end);
	}
};