#pragma once

#include <random>
#include <vector>
#include <algorithm>
#include <numeric>
#include <xbyak/xbyak.h>

#include "ChipState.h"
#include "ChipJITState.h"
#include "Quirks.h"

extern ChipState s;
extern ChipJITState JIT;

static std::default_random_engine rngEng{ std::random_device{}() };
static std::uniform_int_distribution<> rngDistr{ 0, 255 };

class ChipEmitter : Xbyak::CodeGenerator
{
private:

#define SP word[rsi + offsetof(ChipState, sp)] 
#define PC word[rsi + offsetof(ChipState, pc)] 
#define STACK_PTR word[rsi + offsetof(ChipState, stack) + (rcx * sizeof(uint16_t))]
#define KEY(offset) byte[rsi + offsetof(ChipState, keys) + offset]
#define REG_PTR(num) byte[rsi + offsetof(ChipState, V) + num]
#define I_REG_PTR word[rsi + offsetof(ChipState, I)]

	static constexpr uint8_t MAX_ALLOC_REGS = 5; 

	std::vector<uint8_t> allocatedRegs{};
	bool IregAllocated{ false };
	bool flagRegAllocated{ false };

#define I_FULL_REG r15

	const Xbyak::Operand& V_FULL_REG(uint8_t num)
	{
		switch (num)
		{
		case 0: return bx;
		case 1: return di;
		case 2: return r12w;
		case 3: return r13w;
		case 4: return r14w;
		}
	}

#define FLAG_REG bl

	const Xbyak::Reg8* Vreg{ nullptr };

	inline bool GET_VREG(uint8_t num)
	{
		auto pos = std::find(allocatedRegs.begin(), allocatedRegs.end(), num);

		if (pos != allocatedRegs.end())
		{
			switch (std::distance(allocatedRegs.begin(), pos))
			{
			case 0: Vreg = &bl; break;
			case 1: Vreg = &dil; break;
			case 2: Vreg = &r12b; break;
			case 3: Vreg = &r13b; break;
			case 4: Vreg = &r14b; break;
			}

			return true;
		}

		return false;
	}

#define V_REG(num) (GET_VREG(num) ? (const Xbyak::Operand&)*Vreg : (const Xbyak::Operand&)REG_PTR(num))
#define I_REG (IregAllocated ? (const Xbyak::Operand&)r15w : (const Xbyak::Operand&)I_REG_PTR)

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

	inline void LOAD_RAM_PTR(const Xbyak::Reg64& dest)
	{
		if (IregAllocated) lea(dest, ptr[rsi + offsetof(ChipState, RAM) + I_FULL_REG]);
		else
		{
			movzx(dest, I_REG_PTR);
			lea(dest, ptr[rsi + offsetof(ChipState, RAM) + dest]);
		}
	}

	inline void callFunc(size_t func)
	{
		mov(rax, func);
		sub(rsp, 32);
		call(rax);
		add(rsp, 32);
	}

	template<bool toMem>
	inline void store(uint8_t count)
	{
		LOAD_RAM_PTR(rax);

		for (int i = 0; i <= count; i++)
		{
			if constexpr (toMem)
				MOV(byte[rax + i], V_REG(i));
			else
				MOV(V_REG(i), byte[rax + i]);
		}

		if (Quirks::MemoryIncrement) add(I_REG, count + 1);
	}

	//// Static functions for JIT to call

	static uint8_t genRandomNum()
	{
		return rngDistr(rngEng);
	}

	static void invalidateBlocks(uint16_t startAddr, uint16_t endAddr)
	{
		JIT.invalidateBlocks(startAddr, endAddr);
	}

	template <bool clipping, bool drawLocking>
	static void drawSprite(uint8_t regX, uint8_t regY, uint8_t height, uint8_t* RAMptr) 
	{
		uint8_t Ypos = regY % ChipState::SCRHeight;
		const uint8_t Xpos = regX % ChipState::SCRWidth;

		s.V[0xF] = 0;
		const bool partialDraw = Xpos > 56;

		if constexpr (drawLocking)
		{
			while (s.drawLock.exchange(true)) {}
		}

		for (int i = 0; i < height; i++)
		{
			uint8_t spriteRow = *(RAMptr + i);

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

		if constexpr (drawLocking)
		{
			s.drawLock.store(false);
		}
	}

	/////////////////////////

	inline void reset()
	{
		allocatedRegs.clear();
		std::memset(VRegUsage.data(), 0, VRegUsage.size());
		IRegUsage = 0;
		IregAllocated = false;
		flagRegAllocated = false;
		instructions = 0;
	}

public:
	static constexpr uint32_t MAX_CACHE_SIZE = 131072; 

	std::array<uint8_t, 16> VRegUsage{};
	uint8_t IRegUsage{ 0 };

	uint16_t instructions { 0 };

	void allocateRegs()
	{
		if (VRegUsage[0xF] > 0)
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
	}

	void emitPrologue()
	{
		push(rsi);
		mov(rsi, (size_t)&s);

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

		if (IregAllocated)
		{
			mov(I_REG_PTR, I_REG);
			pop(I_FULL_REG);
		}

		pop(rsi);
		mov(eax, instructions);
		ret();

		reset();
	}

	inline uint16_t execute(uint32_t offset) const
	{
		return reinterpret_cast<uint16_t(*)()>(const_cast<uint8_t*>(getCode()) + offset)();
	}

	inline void emit00E0()
	{
		mov(rcx, (size_t)s.screenBuffer.data());
		xor_(edx, edx);
		mov(r8, sizeof(s.screenBuffer));
		callFunc((size_t)std::memset);
	}

	inline const uint8_t* getCodePtr() const { return getCode(); }
	inline size_t getCodeSize() const { return getSize(); }

	inline void clearCache() { resetSize(); }

	void emitLabel()
	{
		L("@@");
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
			jz("@f");
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
			jnz("@f");
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
			jz("@f");
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
			jnz("@f");
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
			jz("@f");
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
		if (Quirks::VFReset) xor_(FLAG_REG, FLAG_REG);
	}
	inline void emit8XY2(uint8_t regX, uint8_t regY)
	{
		AND(V_REG(regX), V_REG(regY));
		if (Quirks::VFReset) xor_(FLAG_REG, FLAG_REG);
	}
	inline void emit8XY3(uint8_t regX, uint8_t regY)
	{
		XOR(V_REG(regX), V_REG(regY));
		if (Quirks::VFReset) xor_(FLAG_REG, FLAG_REG);
	}
	inline void emit8XY4(uint8_t regX, uint8_t regY)
	{
		ADD(V_REG(regX), V_REG(regY));
		setc(FLAG_REG);
	}
	inline void emit8XY5(uint8_t regX, uint8_t regY)
	{
		SUB(V_REG(regX), V_REG(regY));
		setc(FLAG_REG);
		xor_(FLAG_REG, 0x1); // for subtraction the flag is opposite, 0 if underflow and 1 if not.
	}
	inline void emit8XY6(uint8_t regX, uint8_t regY)
	{
		if (!Quirks::Shifting) emit8XY0(regX, regY);

		if (regX == 0xF) // small optimization if operand is flag reg.
			and_(FLAG_REG, 0x1);
		else
		{
			mov(FLAG_REG, V_REG(regX));
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

		setc(FLAG_REG);
		xor_(FLAG_REG, 0x1);
	}

	inline void emit8XYE(uint8_t regX, uint8_t regY)
	{
		if (!Quirks::Shifting) emit8XY0(regX, regY);

		if (regX == 0xF) // small optimization if operand is flag reg.
			shr(FLAG_REG, 7);
		else
		{
			mov(FLAG_REG, V_REG(regX));
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
		callFunc((size_t)genRandomNum);
		and_(al, val);
		mov(V_REG(regX), al);
	}

	inline void emitDXYN(uint8_t regX, uint8_t regY, uint8_t height)
	{
		mov(cl, V_REG(regX));
		mov(dl, V_REG(regY));
		mov(r8b, height);
		LOAD_RAM_PTR(r9);

		if (Quirks::Clipping)
		{
			if (s.enableDrawLocking)
				callFunc((size_t)drawSprite<true, true>);
			else
				callFunc((size_t)drawSprite<true, false>);
		}
		else
		{
			if (s.enableDrawLocking)
				callFunc((size_t)drawSprite<false, true>);
			else
				callFunc((size_t)drawSprite<false, false>);
		}

		if (flagRegAllocated) mov(FLAG_REG, REG_PTR(0xF));
	}

	inline void emitFX07(uint8_t regX)
	{
		MOV(V_REG(regX), byte[rsi + offsetof(ChipState, delay_timer)]);
	}

	inline void emitFX15(uint8_t regX)
	{
		MOV(byte[rsi + offsetof(ChipState, delay_timer)], V_REG(regX));
	}
	inline void emitFX18(uint8_t regX)
	{
		MOV(byte[rsi + offsetof(ChipState, sound_timer)], V_REG(regX));
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
		LOAD_RAM_PTR(rcx);

		mov(r8b, V_REG(regX));

		movzx(ax, r8b);
		mov(r9b, 100);
		div(r9b);
		mov(dl, ah);
		mov(byte[rcx], al);

		movzx(ax, r8b);
		mov(r9b, 10);
		div(r9b);
		xor_(ah, ah);
		div(r9b);
		mov(byte[rcx + 1], ah);

		movzx(ax, dl);
		div(r9b);
		mov(byte[rcx + 2], ah);
	}

	inline void emitFX55(uint8_t regX)
	{
		store<true>(regX);

		movzx(rcx, I_REG);
		lea(rdx, ptr[rcx + regX]);
		callFunc((size_t)invalidateBlocks);
	}

	inline void emitFX65(uint8_t regX)
	{
		store<false>(regX);
	}

	inline void emitFX0A(uint8_t regX)
	{
		mov(rax, (size_t)(&s.V[regX]));
		mov(qword[rsi + offsetof(ChipState, inputReg)], rax);
	}
};