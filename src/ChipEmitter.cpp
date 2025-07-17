#include "ChipEmitter.h"
#include "ChipJITCore.h"

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

#define BASE rbp
#define INSTR_COUNT r12
#define I_REG_64 rbx
#define I_REG_32 ebx
#define I_REG_16 bx
#define PC_64 r13
#define PC_32 r13d
#define PC_16 r13w
#define SP_64 r14
#define SP_32 r14d
#define SP_8 r14b

#define V_REG(num) (allocatedVRegs[num] != NOT_ALLOCATED ? V_REGS_8[allocatedVRegs[num]] : (const Xbyak::Operand&)REG_PTR(num))
#define FLAG_REG r15b 
#define FLAG_REG_32 r15d

#define I_REG_PTR word[BASE + offsetof(ChipState, I)]
#define PC_PTR word[BASE + offsetof(ChipState, pc)]
#define SP_PTR byte[BASE + offsetof(ChipState, sp)]

#define STACK_PTR word[BASE + offsetof(ChipState, stack) + (SP_64 * sizeof(uint16_t))]
#define KEY_PTR(offset) byte[BASE + offsetof(ChipState, keys) + offset]
#define REG_PTR(num) byte[BASE + offsetof(ChipState, V) + num]
#define RAM_PTR(offset) byte[BASE + I_REG_64 + offsetof(ChipState, RAM) + offset]
#define SCREEN_PTR(offset) ptr[BASE + offsetof(ChipState, screenBuffer) + (offset * sizeof(uint64_t))]

#define BLOCK_PTR qword[BASE + JITMapBaseOffset + (PC_64 * 8)]
#define EXECUTE_FLAG_PTR byte[BASE + executeFlagBaseOffset]

constexpr int WIN_SHADOW_SPACE { 32 };

template <typename F>
uint64_t addr(F func)
{
	uint64_t addr;
	std::memcpy(&addr, &func, sizeof(addr));
	return addr;
}

ChipEmitter::ChipEmitter(ChipJITCore& c, std::atomic<bool>& executeFlag) : Xbyak::CodeGenerator(MAX_CACHE_SIZE), core(c)
{
	checkCPUSupport();

	uncompiledBlockHandlerPtr = getCodeEndPtr();
	emitUncompiledBlockHandler();

	dispatcherPtr = getCodeEndPtr();
	emitDispatcher();

	codeStartIndex = getSize();
	executeFlagBaseOffset = reinterpret_cast<uint64_t>(&executeFlag) - reinterpret_cast<uint64_t>(&core.s);
}

void ChipEmitter::reset()
{
	std::memset(allocatedVRegs.data(), NOT_ALLOCATED, sizeof(allocatedVRegs));
	allocatedVRegs[0xF] = MAX_ALLOC_REGS - 1; // V[0xF] is always allocated in r15.

	std::memset(VRegWeight.data(), 0, sizeof(VRegWeight));
	std::memset(modifiedVRegs.data(), false, sizeof(modifiedVRegs)); 
	std::memset(initialValUseVRegs.data(), false, sizeof(initialValUseVRegs));

	instructions = 0;
	branchedInstrs = 0;
}

void ChipEmitter::emitUncompiledBlockHandler()
{
	mov(ARG1, reinterpret_cast<uint64_t>(&core));
	mov(ARG2, PC_64);
	emitCallFunc(addr(&ChipJITCore::compileBlock), true);
	jmp(rax);
}

void ChipEmitter::emitDispatcher()
{
	push(rbx);
	push(rbp);
	push(r12);
	push(r13);
	push(r14);
	push(r15);

#ifdef _WIN32
	push(rsi);
	push(rdi);
#endif
	sub(rsp, 8); // 16-byte alignment

	mov(BASE, reinterpret_cast<uint64_t>(&core.s));
	JITMapBaseOffset = reinterpret_cast<uint64_t>(&core.JIT.blockMap) - reinterpret_cast<uint64_t>(&core.s);

	movzx(I_REG_32, I_REG_PTR);
	movzx(PC_32, PC_PTR);
	movzx(SP_32, SP_PTR);
	movzx(FLAG_REG_32, REG_PTR(0xF));
	xor_(INSTR_COUNT, INSTR_COUNT);

	L(dispatcher);
	jmp(BLOCK_PTR);
	L(dispatcherEnd);

	mov(I_REG_PTR, I_REG_16);
	mov(PC_PTR, PC_16);
	mov(SP_PTR, SP_8);
	mov(REG_PTR(0xF), FLAG_REG);

	mov(rax, INSTR_COUNT);

	add(rsp, 8); 
#ifdef _WIN32
	pop(rdi);
	pop(rsi);
#endif

	pop(r15);
	pop(r14);
	pop(r13);
	pop(r12);
	pop(rbp);
	pop(rbx);
	ret();
}

// 32 -> 32 reg movs can take 0 cycle due to register renaming, so doing them whenever possible.
void ChipEmitter::MOV_VREG_TO_32(Xbyak::Reg32 dst, uint8_t reg)
{
	const auto val { allocatedVRegs[reg] };

	if (val != NOT_ALLOCATED)
		mov(dst, V_REGS_32[val]);
	else
		movzx(dst, V_REG(reg));
}

bool ChipEmitter::emitSaveVregs()
{
	bool stackAligned { true };

	for (int i = 0; i < 16; i++)
	{
		const auto val { allocatedVRegs[i] };

		if (val == NOT_ALLOCATED || !CALLER_SAVED_V_REGS[val])
			continue;

		push(V_REGS_64[val]);
		stackAligned = !stackAligned;
	}

	return stackAligned;
}
void ChipEmitter::emitLoadVregs()
{
	for (int i = 15; i >= 0; i--)
	{
		const auto val { allocatedVRegs[i] };

		if (val == NOT_ALLOCATED || !CALLER_SAVED_V_REGS[val])
			continue;

		pop(V_REGS_64[val]);
	}
}
void ChipEmitter::emitCallFunc(uint64_t func, bool stackAligned) 
{
#ifdef _WIN32
	sub(rsp, WIN_SHADOW_SPACE + (stackAligned ? 0 : 8));
#else
	if (!stackAligned)
		sub(rsp, 8);
#endif
	mov(rax, func);
 	call(rax);
#ifdef _WIN32
	add(rsp, WIN_SHADOW_SPACE + (stackAligned ? 0 : 8));
#else
	if (!stackAligned)
		add(rsp, 8);
#endif
}

void ChipEmitter::emitBlockInvalidation(int count, uint16_t pc)
{
	const bool stackAligned { emitSaveVregs() };
	mov(ARG1, reinterpret_cast<uint64_t>(&core));
	mov(ARG2, I_REG_64);
	lea(ARG3, ptr[ARG2 + count]);
	emitCallFunc(addr(&ChipJITCore::invalidateBlocks), stackAligned);
	emitLoadVregs();

	Xbyak::Label noSelfModifyingCode;
	test(al, al);
	jz(noSelfModifyingCode);
	// early block end if self-modifying code has occurred.
	emitEpilogue(pc);
	L(noSelfModifyingCode);
}

void ChipEmitter::emitIllegalOPHandler()
{
	const bool stackAligned { emitSaveVregs() };
	mov(ARG1, reinterpret_cast<uint64_t>(&core));
	emitCallFunc(addr(&ChipJITCore::illegalOpcodeHandler), stackAligned);
	emitLoadVregs();
}

void ChipEmitter::emitBreakpoint() 
{
	int3(); 
}

void ChipEmitter::allocateRegs()
{
	constexpr int ALLOC_THRESHOLD { 2 };

	std::vector<std::pair<uint8_t, uint16_t>> usageList{};
	usageList.reserve(16);

	for (int i = 0; i < 15; i++)
	{
		if (VRegWeight[i] >= ALLOC_THRESHOLD)
			usageList.push_back({ i, VRegWeight[i] });
	}

	std::sort(usageList.begin(), usageList.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

	for (int i = 0; i < usageList.size() && i < (MAX_ALLOC_REGS - 1); i++) // -1 because V[0xF] is always allocated.
		allocatedVRegs[usageList[i].first] = i;
}

void ChipEmitter::emitPrologue()
{
	for (int i = 0; i < 15; i++)
	{
		const auto val { allocatedVRegs[i] };

		if (val != NOT_ALLOCATED && initialValUseVRegs[i])
			movzx(V_REGS_32[val], REG_PTR(i));
	}
}

void ChipEmitter::emitEpilogue(uint16_t pc)
{
	for (int i = 0; i < 15; i++)
	{
		const auto val { allocatedVRegs[i] };

		if (val != NOT_ALLOCATED && modifiedVRegs[i])
			mov(REG_PTR(i), V_REGS_8[val]);
	}

	add(INSTR_COUNT, instructions - branchedInstrs);

	if (pc != static_cast<uint16_t>(-1))
		mov(PC_32, pc);

	cmp(EXECUTE_FLAG_PTR, 0);

	if (AMD)
	{
		jnz(dispatcher);
		jmp(dispatcherEnd);
	}
	else
	{
		jz(dispatcherEnd);
		jmp(BLOCK_PTR);
	}
}

uint8_t* ChipEmitter::emitJumpPlaceholder()
{
	db(0xE9);
	dd(0);
	return getCodeEndPtr();
}
void ChipEmitter::patchBranchInstr(uint8_t* branchCodeEndPtr, bool incBeforeBranch)
{
	constexpr auto INC_R64_SIZE { 3 };
	const auto offset { incBeforeBranch ? INC_R64_SIZE : 0 };

	int32_t* jumpOffsetPtr { reinterpret_cast<int32_t*>(branchCodeEndPtr - offset - sizeof(int32_t)) };
	const int32_t jumpOffset { static_cast<int32_t>(getCodeEndPtr() - branchCodeEndPtr + offset) };
	*jumpOffsetPtr = jumpOffset;
}

void ChipEmitter::emitInstrCountAdd(int32_t instrs)
{
	add(INSTR_COUNT, instrs);
}
void ChipEmitter::emitInstrCountAddPlaceholder()
{
	add(INSTR_COUNT, INT32_MAX);
}
void ChipEmitter::patchImm32(uint8_t* codeEndPtr, int32_t imm)
{
	*reinterpret_cast<int32_t*>(codeEndPtr - sizeof(int32_t)) = imm;
}

#define quirks core.s.quirks

void ChipEmitter::emit00E0()
{
	if (AVX)
	{
		vxorpd(ymm0, ymm0, ymm0);

		for (int i = 0; i < 32; i += 4)
			vmovdqu(SCREEN_PTR(i), ymm0);

		vzeroupper();
	}
	else
	{
		pxor(xmm0, xmm0);

		for (int i = 0; i < 32; i += 2)
			movdqu(SCREEN_PTR(i), xmm0);
	}
}

void ChipEmitter::emit00EE()
{
	dec(SP_8);
	movzx(PC_32, STACK_PTR);
}

void ChipEmitter::emit1NNN(uint16_t addr)
{
	mov(PC_32, addr);
}

void ChipEmitter::emit2NNN(uint16_t addr, uint16_t pc)
{
	mov(STACK_PTR, pc);
	mov(PC_32, addr);
	inc(SP_8);
}

void ChipEmitter::emit5XY0(uint8_t x, uint8_t y, bool incBranches)
{
	CMP(V_REG(x), V_REG(y));
	// jz
	db(0x0F);
	db(0x84);
	dd(0); // reserve 4 bytes for offset

	if (incBranches)
		inc(INSTR_COUNT);
}
void ChipEmitter::emit9XY0(uint8_t x, uint8_t y, bool incBranches)
{
	CMP(V_REG(x), V_REG(y));
	// jnz
	db(0x0F);
	db(0x85);
	dd(0);

	if (incBranches)
		inc(INSTR_COUNT);
}
void ChipEmitter::emit3XNN(uint8_t x, uint8_t val, bool incBranches)
{
	cmp(V_REG(x), val);
	// jz
	db(0x0F);
	db(0x84);
	dd(0);

	if (incBranches)
		inc(INSTR_COUNT);
}
void ChipEmitter::emit4XNN(uint8_t x, uint8_t val, bool incBranches)
{
	cmp(V_REG(x), val);
	// jnz
	db(0x0F);
	db(0x85);
	dd(0);

	if (incBranches)
		inc(INSTR_COUNT);
}

void ChipEmitter::emitEX9E(uint8_t x, bool incBranches)
{
	MOV_VREG_TO_32(ecx, x);
	and_(ecx, 0xF);
	movzx(ecx, KEY_PTR(rcx));
	test(ecx, ecx);
	// jnz
	db(0x0F);
	db(0x85);
	dd(0);

	if (incBranches)
		inc(INSTR_COUNT);
}
void ChipEmitter::emitEXA1(uint8_t x, bool incBranches)
{
	MOV_VREG_TO_32(ecx, x);
	and_(ecx, 0xF);
	movzx(ecx, KEY_PTR(rcx));
	test(ecx, ecx);
	// jz
	db(0x0F);
	db(0x84);
	dd(0);

	if (incBranches)
		inc(INSTR_COUNT);
}

void ChipEmitter::emit6XNN(uint8_t x, uint8_t val)
{
	const auto i { allocatedVRegs[x] };

	if (i != NOT_ALLOCATED)
	{
		if (val == 0)
			xor_(V_REGS_32[i], V_REGS_32[i]);
		else
			mov(V_REGS_32[i], val);
	}
	else
		mov(REG_PTR(x), val);
}

void ChipEmitter::emit7XNN(uint8_t x, uint8_t val)
{
	if (val == 0)
		return;

	add(V_REG(x), val);
}

void ChipEmitter::emit8XY0(uint8_t x, uint8_t y)
{
	if (x == y)
		return;

	const auto xVal { allocatedVRegs[x] }, yVal { allocatedVRegs[y] };

	if (xVal != NOT_ALLOCATED)
	{
		if (yVal != NOT_ALLOCATED)
			mov(V_REGS_32[xVal], V_REGS_32[yVal]);
		else
			movzx(V_REGS_32[xVal], REG_PTR(y));
	}
	else
		MOV(V_REG(x), V_REG(y));
}
void ChipEmitter::emit8XY1(uint8_t x, uint8_t y, bool calcFlag)
{
	if (quirks.vfReset)
	{
		if (x != 0xF)
			OR(V_REG(x), V_REG(y));

		if (calcFlag)
			xor_(FLAG_REG_32, FLAG_REG_32);
	}
	else
		OR(V_REG(x), V_REG(y));
}
void ChipEmitter::emit8XY2(uint8_t x, uint8_t y, bool calcFlag)
{
	if (quirks.vfReset)
	{
		if (x != 0xF)
			AND(V_REG(x), V_REG(y));

		if (calcFlag)
			xor_(FLAG_REG_32, FLAG_REG_32);
	}
	else
		AND(V_REG(x), V_REG(y));
}
void ChipEmitter::emit8XY3(uint8_t x, uint8_t y, bool calcFlag)
{
	if (quirks.vfReset)
	{
		if (x != 0xF)
			XOR(V_REG(x), V_REG(y));

		if (calcFlag)
			xor_(FLAG_REG_32, FLAG_REG_32);
	}
	else
		XOR(V_REG(x), V_REG(y));
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
			if (!quirks.shifting && y != 0xF)
				emit8XY0(x, y);

			and_(FLAG_REG, 0x1);
		}
	}
	else
	{
		if (!quirks.shifting && x != y)
			emit8XY0(x, y);

		shr(V_REG(x), 1);

		if (calcFlag)
			setc(FLAG_REG);
	}
}
void ChipEmitter::emit8XY7(uint8_t x, uint8_t y, bool calcFlag)
{
	if (x == 0xF && !calcFlag)
		return;

	MOV_VREG_TO_32(eax, y);
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
			if (!quirks.shifting && y != 0xF)
				emit8XY0(x, y);

			shr(FLAG_REG, 7);
		}
	}
	else
	{
		if (!quirks.shifting && x != y)
			emit8XY0(x, y);

		shl(V_REG(x), 1);

		if (calcFlag)
			setc(FLAG_REG);
	}
}

void ChipEmitter::emitANNN(uint16_t val)
{
	mov(I_REG_32, val);
}

void ChipEmitter::emitBNNN(uint16_t addr, uint8_t x)
{
	const auto reg { quirks.jumping ? x : 0 };
	movzx(PC_32, V_REG(reg));
	add(PC_32, addr);
	and_(PC_32, 0xFFF);
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
			xor_(FLAG_REG_32, FLAG_REG_32);

		return;
	}

	Xbyak::Label loopEnd;

	MOV_VREG_TO_32(ecx, x);
	and_(ecx, (ChipState::SCR_WIDTH - 1));

	MOV_VREG_TO_32(eax, y);
	and_(eax, (ChipState::SCR_HEIGHT - 1));

	if (calcFlag)
		xor_(FLAG_REG_32, FLAG_REG_32);

	for (int i = 0; i < n; i++)
	{
		movzx(edx, RAM_PTR(i));

		if (calcFlag)
			mov(r8, SCREEN_PTR(rax));

		shl(rdx, ChipState::SCR_WIDTH - 8);

		if (quirks.clipping)
		{
			if (BMI2)
				shrx(rdx, rdx, rcx); // it's slightly faster
			else
				shr(rdx, cl);
		}
		else
			ror(rdx, cl);

		if (calcFlag)
		{
			test(r8, rdx);
			Xbyak::Label skip;
			// surprisingly flag calculation using branch is faster than branchless (setnz + or)
			jz(skip);
			mov(FLAG_REG_32, 1);
			L(skip);
			xor_(r8, rdx);
			mov(SCREEN_PTR(rax), r8);
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

void ChipEmitter::emitFX1E(uint8_t x)
{
	movzx(eax, V_REG(x));
	add(I_REG_32, eax);
	and_(I_REG_32, 0xFFF);
}

void ChipEmitter::emitFX29(uint8_t x)
{
	MOV_VREG_TO_32(eax, x);
	and_(eax, 0xF);
 	lea(I_REG_32, ptr[rax + rax * 4]);
}

void ChipEmitter::emitFX33(uint8_t x, uint16_t pc)
{
	Xbyak::Label end;
	movzx(eax, V_REG(x));

	lea(edx, ptr[rax + 4 * rax]);
	lea(ecx, ptr[rax + 8 * rdx]);
	shr(ecx, 12);
	mov(RAM_PTR(0), cl);
	cmp(I_REG_32, 0xFFF);
	je(end, T_NEAR);
	imul(ecx, eax, 205);
	shr(ecx, 11);
	lea(edx, ptr[rcx + 4 * rcx]);
	lea(edx, ptr[rdx + 4 * rdx]);
	add(edx, ecx);
	shr(edx, 7);
	and_(edx, 6);
	lea(edx, ptr[rdx + 4 * rdx]);
	mov(r8d, ecx);
	sub(r8b, dl);
	mov(RAM_PTR(1), r8b);
	cmp(I_REG_32, 0xFFE);
	je(end, T_NEAR);
	add(ecx, ecx);
	lea(ecx, ptr[rcx + 4 * rcx]);
	sub(al, cl);
	mov(RAM_PTR(2), al);

	L(end);
	emitBlockInvalidation(2, pc);
}

void ChipEmitter::emitFX55(uint8_t x, uint16_t pc)
{
	Xbyak::Label end;

	if (x != 0)
	{
		cmp(I_REG_32, 0xFFF - x);
		ja(end, T_NEAR);
	}

	for (int i = 0; i <= x; i++)
		MOV(RAM_PTR(i), V_REG(i));

	emitBlockInvalidation(x, pc);

	if (quirks.memoryIncrement)
	{
		add(I_REG_32, x + 1);
		and_(I_REG_32, 0xFFF);
	}

	L(end);
}
void ChipEmitter::emitFX65(uint8_t x)
{
	Xbyak::Label end;

	for (int i = 0; i <= x; i++)
		MOV(V_REG(i), RAM_PTR(i));

	if (quirks.memoryIncrement)
	{
		add(I_REG_32, x + 1);
		and_(I_REG_32, 0xFFF);
	}

	L(end);
}

void ChipEmitter::emitFX0A(uint8_t x, uint16_t pc)
{
	Xbyak::Label firstCall, end;
	const auto val { allocatedVRegs[x] };

	mov(PC_32, pc - 2);
	cmp(byte[BASE + offsetof(ChipState, firstFX0ACall)], 1);
	jz(firstCall);

	cmp(byte[BASE + offsetof(ChipState, inputReg)], -1);
	jnz(end);

	if (val != NOT_ALLOCATED)
		movzx(V_REGS_32[val], REG_PTR(x));

	mov(byte[BASE + offsetof(ChipState, firstFX0ACall)], 1);
	mov(PC_32, pc);
	jmp(end);

	L(firstCall);
	mov(byte[BASE + offsetof(ChipState, inputReg)], x);
	mov(byte[BASE + offsetof(ChipState, firstFX0ACall)], 0);

	L(end);
}