#pragma once

#include <random>
#include <vector>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <xbyak/xbyak.h>

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

#define SP word[BASE + offsetof(ChipState, sp)]
#define PC word[BASE + offsetof(ChipState, pc)]
#define STACK_PTR word[BASE + offsetof(ChipState, stack) + (rcx * sizeof(uint16_t))]
#define KEY(offset) byte[BASE + offsetof(ChipState, keys) + offset]
#define REG_PTR(num) byte[BASE + offsetof(ChipState, V) + num]
#define I_REG_PTR word[BASE + offsetof(ChipState, I)]
#define RAM_PTR(offset) byte[BASE + offsetof(ChipState, RAM) + offset]
#define BRANCH_CNT_PTR qword[BASE + offsetof(ChipState, BLOCK_NOT_TAKEN_BRANCHES)]

#ifdef _WIN32
	static constexpr uint8_t MAX_ALLOC_REGS = 6;
#else
	static constexpr uint8_t MAX_ALLOC_REGS = 5;
#endif

	std::vector<uint8_t> allocatedRegs{};
	bool IregAllocated{ false };
	bool flagRegAllocated{ false };
	uint64_t blockBranches { 0 };

#define I_FULL_REG r15

	const Xbyak::Operand& V_FULL_REG(uint8_t num)
	{
		switch (num)
		{
			case 0: return bx;
			case 1: return bp;
			case 2: return r12w;
			case 3: return r13w;
			case 4: return r14w;
			case 5: return di;
		}
	}

	const Xbyak::Reg8* Vreg{ nullptr };

	inline bool GET_VREG(uint8_t num) 
	{
		auto pos = std::find(allocatedRegs.begin(), allocatedRegs.end(), num);

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
			}

			return true;
		}

		return false;
	}

#define V_REG(num) (GET_VREG(num) ? (const Xbyak::Operand&)*Vreg : (const Xbyak::Operand&)REG_PTR(num))
#define I_REG (IregAllocated ? (const Xbyak::Operand&)r15w : (const Xbyak::Operand&)I_REG_PTR)
#define FLAG_REG (flagRegAllocated ? (const Xbyak::Operand&)bl : (const Xbyak::Operand&)REG_PTR(0xF))

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

	inline void callFunc(size_t func)
	{
		mov(rax, func);
#ifdef _WIN32
		sub(rsp, 32);
#endif
		call(rax);
#ifdef _WIN32
		add(rsp, 32);
#endif
	}

	template<bool toMem>
	inline void store(uint8_t count)
	{
		movzx(rdx, I_REG);

		for (int i = 0; i <= count; i++)
		{
			lea(rax, ptr[rdx + i]);
			and_(rax, 0xFFF);

			if constexpr (toMem)
				MOV(RAM_PTR(rax), V_REG(i));
			else
				MOV(V_REG(i), RAM_PTR(rax));
		}
	}

	// to remove
	template <bool clipping>
	static void drawSprite(uint8_t regX, uint8_t regY, uint8_t height)
	{
		uint8_t Ypos = regY % ChipState::SCRHeight;
		const uint8_t Xpos = regX % ChipState::SCRWidth;

		s.V[0xF] = 0;
		const bool partialDraw = Xpos > 56;

		for (int i = 0; i < height; i++)
		{
			uint8_t spriteRow = s.RAM[(s.I + i) & 0xFFF];

			if constexpr (clipping)
			{
				if (Ypos >= ChipState::SCRHeight)
					break;
			}
			else
				Ypos %= ChipState::SCRHeight;

			uint64_t spriteMask;

			if (partialDraw)
			{
				uint64_t leftPart = static_cast<uint64_t>(spriteRow) >> (Xpos - 56);

				if constexpr (clipping)
					spriteMask = leftPart;
				else
				{
					uint64_t rightPart = static_cast<uint64_t>(spriteRow) << (64 - (Xpos - 56));
					spriteMask = leftPart | rightPart;
				}
			}
			else
				spriteMask = static_cast<uint64_t>(spriteRow) << (63 - Xpos - 7);

			uint64_t& screenRow = s.screenBuffer[Ypos];
			s.V[0xF] |= ((screenRow & spriteMask) != 0);

			screenRow ^= spriteMask;
			Ypos++;
		}

	}

	static void invalidateBlocks(uint16_t startAddr, uint16_t endAddr)
	{
		for (auto& block : JIT.blocks)
		{
			if (block.startPC <= endAddr && block.endPC >= startAddr)
				JIT.blockMap[block.startPC].isValid = false;
		}
	}

	inline void resetState()
	{
		allocatedRegs.clear();
		std::memset(VRegUsage.data(), 0, sizeof(VRegUsage));
		IRegUsage = 0;
		IregAllocated = false;
		flagRegAllocated = false;
		instructions = 0;
		blockBranches = 0;
	}

	bool SSE2Support { false };
	bool AVXSupport { false };

	inline void checkCPUSupport()
	{
		push(rbx);
		mov(r8, (size_t)&SSE2Support);
		mov(r9, (size_t)&AVXSupport);

		mov(eax, 1);
		cpuid();

		test(edx, 1 << 26);
		jz("end");
		mov(byte[r8], 1);

		test(ecx, 1 << 28);
		jz("end");

		mov(ecx, 0);
		xgetbv();
		test(eax, 6);
		jz("end");

		mov(byte[r9], 1);

		L("end");
		pop(rbx);
		ret();

		execute(0);
	}

public:
	static constexpr uint32_t MAX_CACHE_SIZE = 131072;

	std::array<uint8_t, 16> VRegUsage{};
	uint8_t IRegUsage{ 0 };

	uint64_t instructions { 0 };

	void allocateRegs()
	{
		if (VRegUsage[0xF] >= 3)
		{
			allocatedRegs.push_back(0xF);
			flagRegAllocated = true;
		}

		for (int i = 0; i < 15; i++)
		{
			if (VRegUsage[i] >= 3)
			{
				allocatedRegs.push_back(i);
				if (allocatedRegs.size() == MAX_ALLOC_REGS) break;
			}
		}

		if (IRegUsage >= 3)
			IregAllocated = true;
	}

	ChipEmitter() : Xbyak::CodeGenerator(MAX_CACHE_SIZE)
	{
		checkCPUSupport();
	}

	void emitPrologue()
	{
		mov(BASE, (size_t)&s);

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

	void emitEpilogue()
	{
		for (int i = allocatedRegs.size() - 1; i >= 0; i--)
		{
			mov(REG_PTR(allocatedRegs[i]), V_REG(allocatedRegs[i]));
			pop(V_FULL_REG(i));
		}

		mov(rax, instructions - blockBranches);

		if (blockBranches > 0)
		{
			add(rax, BRANCH_CNT_PTR);
			mov(BRANCH_CNT_PTR, 0);
		}

		if (IregAllocated)
		{
			mov(I_REG_PTR, I_REG);
			pop(I_FULL_REG);
		}

		ret();

		resetState();
	}

	FORCE_INLINE uint64_t execute(uint32_t offset) const
	{
		return reinterpret_cast<uint16_t(*)()>(const_cast<uint8_t*>(getCode()) + offset)();
	}

	inline void emit00E0()
	{
		lea(rcx, ptr[BASE + offsetof(ChipState, screenBuffer)]);

		if (AVXSupport)
		{
			vxorpd(ymm0, ymm0, ymm0);

			for (int i = 0; i < 32; i += 4)
				vmovdqu(ptr[rcx + i * 8], ymm0);
		}
		else if (SSE2Support)
		{
			pxor(xmm0, xmm0);

			for (int i = 0; i < 32; i += 2)
				movdqu(ptr[rcx + i * 8], xmm0);
		}
		else
		{
			for (int i = 0; i < 32; i++)
				mov(qword[rcx + i * 8], 0);
		}
	}

	inline const uint8_t* getCodePtr() const { return getCode(); }
	inline size_t getCodeSize() const { return getSize(); }

	inline void clearCache() { resetSize(); }

	void emitJumpLabel()
	{		
		L("@@");
		blockBranches++;
	}

	inline void emit00EE()
	{
		dec(SP);
		mov(cx, SP);
		and_(rcx, 0xF);
		mov(cx, STACK_PTR);
		mov(PC, cx);
	}

	inline void emit1NNN(uint16_t addr)
	{
		mov(PC, addr);
	}

	inline void emit2NNN(uint16_t addr)
	{
		mov(cx, SP);
		and_(rcx, 0xF);
		mov(ax, PC);
		mov(STACK_PTR, ax);
		mov(PC, addr);
		inc(SP);
	}

	template<bool jumpLabel>
	inline void emit5XY0(uint8_t regX, uint8_t regY)
	{
		if constexpr (!jumpLabel)
			xor_(cx, cx);

		CMP(V_REG(regX), V_REG(regY));

		if constexpr (jumpLabel)
		{
			jz("@f", T_NEAR);
			inc(BRANCH_CNT_PTR);
		}
		else
		{
			setz(cl);
			shl(cl, 1);
			add(PC, cx);
		}
	}
	template<bool jumpLabel>
	inline void emit9XY0(uint8_t regX, uint8_t regY)
	{
		if constexpr (!jumpLabel)
			xor_(cx, cx);

		CMP(V_REG(regX), V_REG(regY));

		if constexpr (jumpLabel)
		{
			jnz("@f", T_NEAR);
			inc(BRANCH_CNT_PTR);
		}
		else
		{
			setnz(cl);
			shl(cl, 1);
			add(PC, cx);
		}
	}
	template<bool jumpLabel>
	inline void emit3XNN(uint8_t regX, uint8_t val)
	{
		if constexpr (!jumpLabel)
			xor_(cx, cx);

		cmp(V_REG(regX), val);

		if constexpr (jumpLabel)
		{
			jz("@f", T_NEAR);
			inc(BRANCH_CNT_PTR);
		}
		else
		{
			setz(cl);
			shl(cl, 1);
			add(PC, cx);
		}
	}
	template<bool jumpLabel>
	inline void emit4XNN(uint8_t regX, uint8_t val)
	{
		if constexpr (!jumpLabel)
			xor_(cx, cx);

		cmp(V_REG(regX), val);

		if constexpr (jumpLabel)
		{
			jnz("@f", T_NEAR);
			inc(BRANCH_CNT_PTR);
		}
		else
		{
			setnz(cl);
			shl(cl, 1);
			add(PC, cx);
		}
	}

	template<bool jumpLabel>
	inline void emitEX9E(uint8_t regX)
	{
		movzx(rcx, V_REG(regX));
		and_(rcx, 0xF);
		mov(cl, KEY(rcx));

		if constexpr (jumpLabel)
		{
			test(cl, cl);
			jnz("@f", T_NEAR);
			inc(BRANCH_CNT_PTR);
		}
		else
		{
			shl(cl, 1);
			add(PC, cx);
		}
	}
	template<bool jumpLabel>
	inline void emitEXA1(uint8_t regX)
	{
		movzx(rcx, V_REG(regX));
		and_(rcx, 0xF);
		mov(cl, KEY(rcx));

		if constexpr (jumpLabel)
		{
			test(cl, cl);
			jz("@f", T_NEAR);
			inc(BRANCH_CNT_PTR);
		}
		else
		{
			xor_(cl, 1);
			shl(cl, 1);
			add(PC, cx);
		}
	}

	inline void emit6XNN(uint8_t reg, uint8_t val)
	{
		mov(V_REG(reg), val);
	}

	inline void emit7XNN(uint8_t reg, uint8_t val)
	{
		add(V_REG(reg), val);
	}

	inline void emit8XY0(uint8_t regX, uint8_t regY)
	{
		MOV(V_REG(regX), V_REG(regY));
	}
	inline void emit8XY1(uint8_t regX, uint8_t regY)
	{
		OR(V_REG(regX), V_REG(regY));
		if (Quirks::VFReset) mov(FLAG_REG, 0);
	}
	inline void emit8XY2(uint8_t regX, uint8_t regY)
	{
		AND(V_REG(regX), V_REG(regY));
		if (Quirks::VFReset) mov(FLAG_REG, 0);
	}
	inline void emit8XY3(uint8_t regX, uint8_t regY)
	{
		XOR(V_REG(regX), V_REG(regY));
		if (Quirks::VFReset) mov(FLAG_REG, 0);
	}
	inline void emit8XY4(uint8_t regX, uint8_t regY)
	{
		ADD(V_REG(regX), V_REG(regY));
		setc(FLAG_REG);
	}
	inline void emit8XY5(uint8_t regX, uint8_t regY)
	{
		SUB(V_REG(regX), V_REG(regY));
		setnc(FLAG_REG);
	}
	inline void emit8XY6(uint8_t regX, uint8_t regY)
	{
		if (!Quirks::Shifting) emit8XY0(regX, regY);

		if (regX == 0xF) // small optimization if operand is flag reg.
			and_(FLAG_REG, 0x1);
		else
		{
			MOV(FLAG_REG, V_REG(regX));
			and_(FLAG_REG, 0x1);
			shr(V_REG(regX), 1);
		}
	}
	inline void emit8XY7(uint8_t regX, uint8_t regY)
	{
		mov(cl, V_REG(regY));
		sub(cl, V_REG(regX));

		if (regX != 0xF) // small optimization if operand is flag reg.
			mov(V_REG(regX), cl);

		setnc(FLAG_REG);
	}

	inline void emit8XYE(uint8_t regX, uint8_t regY)
	{
		if (!Quirks::Shifting) emit8XY0(regX, regY);

		if (regX == 0xF) // small optimization if operand is flag reg.
			shr(FLAG_REG, 7);
		else
		{
			MOV(FLAG_REG, V_REG(regX));
			shr(FLAG_REG, 7);
			shl(V_REG(regX), 1);
		}
	}

	inline void emitANNN(uint16_t val)
	{
		mov(I_REG, val);
	}

	inline void emitBNNN(uint16_t val, uint8_t regX)
	{
		mov(PC, val);
		movzx(cx, Quirks::Jumping ? V_REG(regX) : V_REG(0));
		add(PC, cx);
	}

	inline void emitCXNN(uint8_t regX, uint8_t val)
	{
		rdtsc(); // using cpu timestamp as a random number
		and_(eax, val);
		mov(V_REG(regX), al);
	}

	inline void emitDXYN(uint8_t regX, uint8_t regY, uint8_t height)
	{
		mov(FLAG_REG, 0);
		if (height == 0) return;

		Xbyak::Label loopEnd;

		mov(r8b, V_REG(regY));
		and_(r8, (ChipState::SCRHeight - 1));

		mov(r9b, V_REG(regX));
		and_(r9, (ChipState::SCRWidth - 1));

		movzx((height == 1 ? rax : rdx), I_REG);

		for (int i = 0; i < height; i++)
		{
			Xbyak::Label drawXoring, fullDraw;

			if (height > 1)
				lea(rax, ptr[rdx + i]);

			and_(rax, 0xFFF);
			movzx(rax, RAM_PTR(rax));

			if (i > 0)
			{
				if (Quirks::Clipping)
				{
					cmp(r8b, ChipState::SCRHeight);
					jae(loopEnd, T_NEAR);
				}
				else
					and_(r8b, (ChipState::SCRHeight - 1));
			}

			mov(r10, rax);
			cmp(r9b, 56);
			jbe(fullDraw); 

			lea(rcx, ptr[r9 - 56]);
			shr(r10, cl);

			if (!Quirks::Clipping)
			{
				mov(cl, 120);
				sub(cl, r9b); 
				shl(rax, cl);
				or_(r10, rax);
			}

			jmp(drawXoring);
			L(fullDraw);

			mov(cl, 56);
			sub(cl, r9b);
			shl(r10, cl);

			L(drawXoring);

			lea(rcx, ptr[BASE + offsetof(ChipState, screenBuffer) + (r8 * sizeof(uint64_t))]);

			test(qword[rcx], r10);
			setnz(al);
			or_(FLAG_REG, al);
			xor_(qword[rcx], r10);

			inc(r8b);
		}

		L(loopEnd);

		//push(BASE);
		//movzx(ARG1, V_REG(regX));
		//movzx(ARG2, V_REG(regY));
		//mov(ARG3, height);

		//if (Quirks::Clipping)
		//{
		//	callFunc((size_t)drawSprite<true>);
		//}
		//else
		//{

		//	callFunc((size_t)drawSprite<false>);
		//}

		//if (flagRegAllocated) mov(FLAG_REG, REG_PTR(0xF));
		//pop(BASE);
	}

	inline void emitFX07(uint8_t regX)
	{
		MOV(V_REG(regX), byte[BASE + offsetof(ChipState, delay_timer)]);
	}

	inline void emitFX15(uint8_t regX)
	{
		MOV(byte[BASE + offsetof(ChipState, delay_timer)], V_REG(regX));
	}
	inline void emitFX18(uint8_t regX)
	{
		MOV(byte[BASE + offsetof(ChipState, sound_timer)], V_REG(regX));
	}

	inline void emitFX1E(uint8_t regX)
	{
		movzx(cx, V_REG(regX));
		add(I_REG, cx);
	}

	inline void emitFX29(uint8_t regX)
	{
		mov(al, V_REG(regX));
		and_(al, 0xF);
		imul(al, al, 0x5);

		if (IregAllocated) movzx(I_FULL_REG, al);
		else
		{
			movzx(ax, al);
			mov(I_REG_PTR, ax);
		}
	}

	inline void emitFX33(uint8_t regX)
	{
		movzx(rcx, I_REG);
		movzx(r8, V_REG(regX));

		mov(edx, r8d);
		imul(edx, edx, 205);
		shr(edx, 11);

		mov(r9d, edx);
		imul(r9d, r9d, 10);
		sub(r9d, r8d);
		neg(r9d);

		lea(r10, ptr[rcx + 2]);
		and_(r10, 0xFFF);
		mov(RAM_PTR(r10), r9b);

		mov(r9d, edx);
		imul(edx, edx, 205);
		shr(edx, 11);
		imul(edx, edx, 10);
		sub(r9d, edx);

		lea(r10, ptr[rcx + 1]);
		and_(r10, 0xFFF);
		mov(RAM_PTR(r10), r9b);

		imul(r8d, r8d, 41);
		shr(r8d, 12);

		and_(rcx, 0xFFF);
		mov(RAM_PTR(rcx), r8b);
	}

	inline void emitFX55(uint8_t regX)
	{
		store<true>(regX);

		movzx(ARG1, I_REG);
		lea(ARG2, ptr[ARG1 + regX]);

		if (Quirks::MemoryIncrement && !IregAllocated)
			push(BASE);

		callFunc((size_t)invalidateBlocks);

		if (Quirks::MemoryIncrement)
		{
			if (!IregAllocated) pop(BASE);
			add(I_REG, regX + 1);
		}
	}

	inline void emitFX65(uint8_t regX)
	{
		store<false>(regX);
		if (Quirks::MemoryIncrement) add(I_REG, regX + 1);
	}

	inline void emitFX0A(uint8_t regX)
	{
		mov(rax, (size_t)(&s.V[regX]));
		mov(qword[BASE + offsetof(ChipState, inputReg)], rax);
	}
};