#include "ChipEmitter.h"
#include "ChipJITCore.h"

#ifdef _WIN32
#define ARG1 rcx
#define ARG2 rdx
#define ARG3 r8
#define ARG4 r9

constexpr int MAX_ALLOC_REGS { 7 };
#else

#define ARG1 rdi
#define ARG2 rsi
#define ARG3 rdx
#define ARG4 rcx

// sil and dil are callee saved only in Windows calling convention.
constexpr int MAX_ALLOC_REGS { 5 };
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

#define V_REG(num) (GET_VREG(num) ? (const Xbyak::Operand&)*vreg : (const Xbyak::Operand&)REG_PTR(num))
#define I_REG (IregAllocated ? (const Xbyak::Operand&)r15w : (const Xbyak::Operand&)I_REG_PTR)
#define I_REG64 r15
#define FLAG_REG V_REG(0xF)

constexpr int WIN_SHADOW_SPACE{ 32 };

void ChipEmitter::reset()
{
	allocatedRegs.clear();
	std::memset(VRegUsage.data(), 0, sizeof(VRegUsage));
	IRegUsage = 0;
	IregAllocated = false;
	instructions = 0;
	branchedInstrs = 0;
	blockHasInstrSkips = false;
}

void ChipEmitter::emitUncompiledBlockHandler()
{
	uint64_t addr;
	const auto ptr { &ChipJITCore::compileBlock };
	std::memcpy(&addr, &ptr, sizeof(addr));

	sub(rsp, WIN_SHADOW_SPACE + 8);
	mov(ARG1, reinterpret_cast<uint64_t>(&core));
	mov(rax, addr);
	call(rax);
	add(rsp, WIN_SHADOW_SPACE + 8);
	ret();

	codeStartOffset = getSize();
}

void ChipEmitter::callFunc(uint64_t func)
{
	push(BASE);

	if (blockHasInstrSkips)
		push(BRANCH_SKIP_REG);
	else
		stackAligned = !stackAligned;

	if (stackAligned)
	{
#ifdef _WIN32
		sub(rsp, WIN_SHADOW_SPACE);
#endif
	}
	else
		sub(rsp, WIN_SHADOW_SPACE + 8);

	mov(rax, func);
	call(rax);

	if (stackAligned)
	{
#ifdef _WIN32
		add(rsp, WIN_SHADOW_SPACE);
#endif
	}
	else
		add(rsp, WIN_SHADOW_SPACE + 8);

	if (blockHasInstrSkips)
		pop(BRANCH_SKIP_REG);
	else
		stackAligned = !stackAligned;

	pop(BASE);
}

void ChipEmitter::emitBlockInvalidation(int count, uint16_t pc)
{
	mov(ARG1, reinterpret_cast<uint64_t>(&core));
	movzx(ARG2, I_REG);
	lea(ARG3, ptr[ARG2 + count]);

	uint64_t addr;
	const auto ptr { &ChipJITCore::invalidateBlocks };
	std::memcpy(&addr, &ptr, sizeof(addr));
	callFunc(addr);

	Xbyak::Label noSelfModifyingCode;
	test(al, al);
	jz(noSelfModifyingCode);
	// early block end if self-modifying code has occurred.
	emitEpilogue(pc);
	L(noSelfModifyingCode);
}

void ChipEmitter::emitIllegalOPHandler()
{

}

void ChipEmitter::emitBreakpoint() 
{
	int3(); 
}

const Xbyak::Reg64& ChipEmitter::V_REG64(uint8_t num)
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

bool ChipEmitter::GET_VREG(uint8_t num)
{
	const auto pos { std::find(allocatedRegs.begin(), allocatedRegs.end(), num) };

	if (pos != allocatedRegs.end())
	{
		switch (std::distance(allocatedRegs.begin(), pos))
		{
			case 0: vreg = &bl; break;
			case 1: vreg = &bpl; break;
			case 2: vreg = &r12b; break;
			case 3: vreg = &r13b; break;
			case 4: vreg = &r14b; break;
			case 5: vreg = &sil; break;
			case 6: vreg = &dil; break;
		}

		return true;
	}

	return false;
}

void ChipEmitter::allocateRegs()
{
	constexpr int ALLOC_THRESHOLD { 3 };

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

void ChipEmitter::emitPrologue()
{
	mov(BASE, (size_t)&core.s);

	if (blockHasInstrSkips)
		xor_(BRANCH_SKIP_REG, BRANCH_SKIP_REG);

	if (IregAllocated)
	{
		push(I_REG64);
		movzx(I_REG64, I_REG_PTR);
		stackAligned = !stackAligned;
	}

	for (int i = 0; i < allocatedRegs.size(); i++)
	{
		push(V_REG64(i));
		mov(V_REG(allocatedRegs[i]), REG_PTR(allocatedRegs[i]));
		stackAligned = !stackAligned;
	}
}

void ChipEmitter::emitEpilogue(uint16_t pc)
{
	for (int i = allocatedRegs.size() - 1; i >= 0; i--)
	{
		mov(REG_PTR(allocatedRegs[i]), V_REG(allocatedRegs[i]));
		pop(V_REG64(i));
	}

	if (blockHasInstrSkips)
		lea(eax, ptr[BRANCH_SKIP_REG + instructions - branchedInstrs]);
	else
		mov(eax, instructions - branchedInstrs);

	if (IregAllocated)
	{
		mov(I_REG_PTR, I_REG);
		pop(I_REG64);
	}

	if (pc != static_cast<uint16_t>(-1))
		mov(PC, pc);

	ret();
}

void ChipEmitter::emitJumpPlaceholder()
{
	db(0xE9);
	dd(0);
}

void ChipEmitter::patchBranchInstr(uint8_t* branchCodeEndPtr, bool incBeforeBranch)
{
	constexpr auto INC_R64_SIZE{ 3 };
	const auto offset { incBeforeBranch ? INC_R64_SIZE : 0 };

	int32_t* jumpOffsetPtr { reinterpret_cast<int32_t*>(branchCodeEndPtr - offset - sizeof(int32_t)) };
	const int32_t jumpOffset { static_cast<int32_t>(getCodeEndPtr() - branchCodeEndPtr + offset) };
	*jumpOffsetPtr = jumpOffset;
}

#define quirks core.s.quirks

void ChipEmitter::emit00E0()
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

void ChipEmitter::emit00EE()
{
	dec(SP);
	movzx(ecx, SP);
	mov(cx, STACK_PTR(rcx));
	mov(PC, cx);
}

void ChipEmitter::emit1NNN(uint16_t addr)
{
	mov(PC, addr);
}

void ChipEmitter::emit2NNN(uint16_t addr, uint16_t pc)
{
	movzx(ecx, SP);
	mov(STACK_PTR(rcx), pc);
	mov(PC, addr);
	inc(SP);
}

void ChipEmitter::emit5XY0(uint8_t x, uint8_t y, bool incBranches)
{
	CMP(V_REG(x), V_REG(y));
	// jz
	db(0x0F);
	db(0x84);
	dd(0); // reserve 4 bytes for offset

	if (incBranches)
		inc(BRANCH_SKIP_REG);
}
void ChipEmitter::emit9XY0(uint8_t x, uint8_t y, bool incBranches)
{
	CMP(V_REG(x), V_REG(y));
	// jnz
	db(0x0F);
	db(0x85);
	dd(0);

	if (incBranches)
		inc(BRANCH_SKIP_REG);
}
void ChipEmitter::emit3XNN(uint8_t x, uint8_t val, bool incBranches)
{
	cmp(V_REG(x), val);
	// jz
	db(0x0F);
	db(0x84);
	dd(0);

	if (incBranches)
		inc(BRANCH_SKIP_REG);
}
void ChipEmitter::emit4XNN(uint8_t x, uint8_t val, bool incBranches)
{
	cmp(V_REG(x), val);
	// jnz
	db(0x0F);
	db(0x85);
	dd(0);

	if (incBranches)
		inc(BRANCH_SKIP_REG);
}

void ChipEmitter::emitEX9E(uint8_t x, bool incBranches)
{
	movzx(ecx, V_REG(x));
	and_(ecx, 0xF);
	movzx(ecx, KEY(rcx));
	test(ecx, ecx);
	// jnz
	db(0x0F);
	db(0x85);
	dd(0);

	if (incBranches)
		inc(BRANCH_SKIP_REG);
}
void ChipEmitter::emitEXA1(uint8_t x, bool incBranches)
{
	movzx(ecx, V_REG(x));
	and_(ecx, 0xF);
	movzx(ecx, KEY(rcx));
	test(ecx, ecx);
	// jz
	db(0x0F);
	db(0x84);
	dd(0);

	if (incBranches)
		inc(BRANCH_SKIP_REG);
}

void ChipEmitter::emit6XNN(uint8_t x, uint8_t val)
{
	mov(V_REG(x), val);
}

void ChipEmitter::emit7XNN(uint8_t x, uint8_t val)
{
	add(V_REG(x), val);
}

void ChipEmitter::emit8XY0(uint8_t x, uint8_t y)
{
	MOV(V_REG(x), V_REG(y));
}
void ChipEmitter::emit8XY1(uint8_t x, uint8_t y, bool calcFlag)
{
	OR(V_REG(x), V_REG(y));

	if (quirks.vfReset && calcFlag)
		mov(FLAG_REG, 0);
}
void ChipEmitter::emit8XY2(uint8_t x, uint8_t y, bool calcFlag)
{
	AND(V_REG(x), V_REG(y));

	if (quirks.vfReset && calcFlag)
		mov(FLAG_REG, 0);
}
void ChipEmitter::emit8XY3(uint8_t x, uint8_t y, bool calcFlag)
{
	XOR(V_REG(x), V_REG(y));

	if (quirks.vfReset && calcFlag)
		mov(FLAG_REG, 0);
}
void ChipEmitter::emit8XY4(uint8_t x, uint8_t y, bool calcFlag)
{
	if (x == 0xF && !calcFlag)
		return;

	ADD(V_REG(x), V_REG(y));

	if (calcFlag)
		setc(FLAG_REG);
}
void ChipEmitter::emit8XY5(uint8_t x, uint8_t y, bool calcFlag)
{
	if (x == 0xF && !calcFlag)
		return;

	SUB(V_REG(x), V_REG(y));

	if (calcFlag)
		setnc(FLAG_REG);
}
void ChipEmitter::emit8XY6(uint8_t x, uint8_t y, bool calcFlag)
{
	if (x == 0xF)
	{
		if (calcFlag)
		{
			if (!quirks.shifting && x != y)
				MOV(V_REG(x), V_REG(y));

			and_(FLAG_REG, 0x1);
		}
	}
	else
	{
		if (!quirks.shifting && x != y)
			MOV(V_REG(x), V_REG(y));

		shr(V_REG(x), 1);

		if (calcFlag)
			setc(FLAG_REG);
	}
}
void ChipEmitter::emit8XY7(uint8_t x, uint8_t y, bool calcFlag)
{
	if (x == 0xF && !calcFlag)
		return;

	movzx(eax, V_REG(y));
	sub(al, V_REG(x));

	if (x != 0xF)
		mov(V_REG(x), al);

	if (calcFlag)
		setnc(FLAG_REG);
}

void ChipEmitter::emit8XYE(uint8_t x, uint8_t y, bool calcFlag)
{
	if (x == 0xF)
	{
		if (calcFlag)
		{
			if (!quirks.shifting && x != y)
				MOV(V_REG(x), V_REG(y));

			shr(FLAG_REG, 7);
		}
	}
	else
	{
		if (!quirks.shifting && x != y)
			MOV(V_REG(x), V_REG(y));

		shl(V_REG(x), 1);

		if (calcFlag)
			setc(FLAG_REG);
	}
}

void ChipEmitter::emitANNN(uint16_t val)
{
	mov(I_REG, val);
}

void ChipEmitter::emitBNNN(uint16_t addr, uint8_t x)
{
	movzx(eax, quirks.jumping ? V_REG(x) : V_REG(0));
	add(ax, addr);
	and_(ax, 0xFFF);
	mov(PC, ax);
}

void ChipEmitter::emitCXNN(uint8_t x, uint8_t val)
{
	rdtsc(); // using cpu timestamp as a random number
	and_(eax, val);
	mov(V_REG(x), al);
}

void ChipEmitter::emitDXYN(uint8_t x, uint8_t y, uint8_t n, bool calcFlag)
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

		if (quirks.clipping)
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
			if (quirks.clipping)
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

void ChipEmitter::emitFX07(uint8_t x)
{
	MOV(V_REG(x), byte[BASE + offsetof(ChipState, delayTimer)]);
}
void ChipEmitter::emitFX15(uint8_t x)
{
	MOV(byte[BASE + offsetof(ChipState, delayTimer)], V_REG(x));
}
void ChipEmitter::emitFX18(uint8_t x)
{
	MOV(byte[BASE + offsetof(ChipState, soundTimer)], V_REG(x));
}

void ChipEmitter:: emitFX1E(uint8_t x)
{
	movzx(eax, V_REG(x));
	add(I_REG, ax);
	and_(I_REG, 0xFFF);
}

void ChipEmitter::emitFX29(uint8_t x)
{
	movzx(ecx, V_REG(x));
	and_(ecx, 0xF);
	lea(ecx, ptr[rcx + (rcx * 4)]);
	mov(I_REG, cx);
}

void ChipEmitter::emitFX33(uint8_t x, uint16_t pc)
{
	Xbyak::Label end;
	movzx(r8d, I_REG);
	movzx(eax, V_REG(x));

	lea(edx, ptr[rax + 4 * rax]);
	lea(ecx, ptr[rax + 8 * rdx]);
	shr(ecx, 12);
	mov(RAM_PTR(r8), cl);
	cmp(r8w, 0xFFF);
	je(end, T_NEAR);
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
	cmp(r8w, 0xFFE);
	je(end, T_NEAR);
	add(ecx, ecx);
	lea(ecx, ptr[rcx + 4 * rcx]);
	sub(al, cl);
	mov(RAM_PTR(2 + r8), al);

	L(end);
	emitBlockInvalidation(2, pc);
}

void ChipEmitter::emitFX55(uint8_t x, uint16_t pc)
{
	Xbyak::Label end;

	if (IregAllocated)
		cmp(I_REG, 0xFFF - x);
	else
	{
		movzx(eax, I_REG);
		cmp(ax, 0xFFF - x);
	}

	ja(end, T_NEAR);

	for (int i = 0; i <= x; i++)
		MOV(RAM_PTR(i + (IregAllocated ? I_REG64 : rax)), V_REG(i));

	emitBlockInvalidation(x, pc);

	if (quirks.memoryIncrement)
	{
		add(I_REG, x + 1);
		and_(I_REG, 0xFFF);
	}

	L(end);
}
void ChipEmitter::emitFX65(uint8_t x)
{
	Xbyak::Label end;

	if (!IregAllocated)
		movzx(eax, I_REG);

	for (int i = 0; i <= x; i++)
		MOV(V_REG(i), RAM_PTR(i + (IregAllocated ? I_REG64 : rax)));

	if (quirks.memoryIncrement)
	{
		add(I_REG, x + 1);
		and_(I_REG, 0xFFF);
	}

	L(end);
}

void ChipEmitter::emitFX0A(uint8_t x, uint16_t pc)
{
	Xbyak::Label firstCall, end;

	mov(PC, pc - 2);
	cmp(byte[BASE + offsetof(ChipState, firstFX0ACall)], 1);
	jz(firstCall);

	cmp(byte[BASE + offsetof(ChipState, inputReg)], -1);
	jnz(end);
	mov(byte[BASE + offsetof(ChipState, firstFX0ACall)], 1);
	mov(PC, pc);
	jmp(end);

	L(firstCall);
	mov(byte[BASE + offsetof(ChipState, inputReg)], x);
	mov(byte[BASE + offsetof(ChipState, firstFX0ACall)], 0);

	L(end);
}