#include "ChipEmitter.h"
#include "ChipJITCore.h"
#include "utils.h"

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
#define CORE_PTR ptr[BASE + offset(&core.s, &core)]
#define INSTR_COUNT r12
#define PC_64 r13
#define PC_32 r13d
#define PC_16 r13w
#define I_REG_64 rbx
#define I_REG_32 ebx
#define I_REG_16 bx
#define SP_64 r14
#define SP_32 r14d
#define SP_8 r14b

#define V_REG(num) (allocatedVRegs[num] != NOT_ALLOCATED ? V_REGS_8[allocatedVRegs[num]] : (const Xbyak::Operand&)byte[REG_PTR(num)])
#define FLAG_REG r15b 
#define FLAG_REG_32 r15d

#define I_REG_PTR word[BASE + offsetof(ChipState, I)]
#define PC_PTR word[BASE + offsetof(ChipState, pc)]
#define SP_PTR byte[BASE + offsetof(ChipState, sp)]

#define REG_PTR(num) BASE + offsetof(ChipState, V) + num
#define RAM_PTR(offset) BASE + I_REG_64 + offsetof(ChipState, RAM) + offset
#define SCREEN_PTR(offset) ptr[BASE + offsetof(ChipState, screenBuffer) + (offset * sizeof(uint64_t))]
#define STACK_PTR(offset) word[BASE + offsetof(ChipState, stack) + (offset * sizeof(uint16_t))]

#define DELAY_PTR byte[BASE + offsetof(ChipState, delayTimer)]
#define SOUND_PTR byte[BASE + offsetof(ChipState, soundTimer)]
#define KEY_PTR(offset) byte[BASE + offsetof(ChipState, keys) + offset]
#define FX0A_FLAG_PTR byte[BASE + offsetof(ChipState, firstFX0ACall)]
#define FX0A_REG_PTR byte[BASE + offsetof(ChipState, inputReg)]

#define EXECUTE_FLAG_PTR byte[BASE + executeFlagBaseOffset]
#define DISPATCH() jmp(ptr[BASE + offset(&core.s, &core.JIT.blockMap) + (PC_64 * 8)])

ChipEmitter::ChipEmitter(ChipJITCore& c, std::atomic<bool>& executeFlag) : Xbyak::CodeGenerator(MAX_CACHE_SIZE), core(c)
{
	checkCPUSupport();

	uncompiledBlockHandlerPtr = getCurr<uint8_t*>();
	emitUncompiledBlockHandler();

	L(rspBackup);
	dq(0);

	dispatcherPtr = getCurr<uint64_t(*)()>();
	emitDispatcher();

	codeStartIndex = getSize();
	executeFlagBaseOffset = offset(&core.s, &executeFlag);
}

uint8_t* ChipEmitter::newBlock()
{
	//std::ranges::fill(allocatedRegs, RegAllocation { NOT_ALLOCATED });
	//allocatedRegs[0xF] = RegAllocation { MAX_ALLOC_REGS - 1 };

	std::memset(allocatedVRegs.data(), NOT_ALLOCATED, sizeof(allocatedVRegs));
	allocatedVRegs[0xF] = MAX_ALLOC_REGS - 1; // V[0xF] is always allocated in r15.
	rspAllocReg = std::nullopt;

	std::memset(VRegWeight.data(), 0, sizeof(VRegWeight));
	std::memset(modifiedVRegs.data(), false, sizeof(modifiedVRegs)); 
	std::memset(initialValUseVRegs.data(), false, sizeof(initialValUseVRegs));

	instructions = 0;
	branchedInstrs = 0;

	return getCurr<uint8_t*>();
}

void ChipEmitter::emitUncompiledBlockHandler()
{
	mov(rsp, qword[rip + rspBackup]);
	lea(ARG1, CORE_PTR);
	mov(ARG2, PC_64);
	emitCallFunc(&ChipJITCore::compileBlock, true);
	jmp(rax);
}

void ChipEmitter::emitDispatcher()
{
	constexpr int WIN_SHADOW_SPACE { 32 };

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
	sub(rsp, 8 + WIN_SHADOW_SPACE); // for 16-byte alignment + shadow space on windows
	mov(qword[rip + rspBackup], rsp);

	mov(BASE, addr(&core.s));

	xor_(INSTR_COUNT, INSTR_COUNT);
	movzx(I_REG_32, I_REG_PTR);
	movzx(PC_32, PC_PTR);
	movzx(SP_32, SP_PTR);
	movzx(FLAG_REG_32, byte[REG_PTR(0xF)]);

	DISPATCH();
	L(dispatcherEnd);

	mov(I_REG_PTR, I_REG_16);
	mov(PC_PTR, PC_16);
	mov(SP_PTR, SP_8);
	mov(byte[REG_PTR(0xF)], FLAG_REG);

	mov(rax, INSTR_COUNT);

	mov(rsp, qword[rip + rspBackup]);
	add(rsp, 8 + WIN_SHADOW_SPACE);
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

void ChipEmitter::MOV_TO_REG(uint8_t r, const Xbyak::Operand& op)
{
	const auto i { allocatedVRegs[r] };

	if (i != NOT_ALLOCATED)
		movzx(V_REGS_32[i], op); // breaking dependency
	else
		MOV(byte[REG_PTR(r)], op);
}

//void ChipEmitter::emitLoadAllocRegs()
//{
//	for (int i = 0; i < 16; i++)
//	{
//		if (allocatedRegs[i].ind != NOT_ALLOCATED && allocatedRegs[i].needsLoad)
//			movzx(V_REGS_32[allocatedRegs[i].ind], REG_PTR(i));
//	}
//}
//void ChipEmitter::emitStoreAllocRegs()
//{
//	for (int i = 0; i < 16; i++)
//	{
//		if (allocatedRegs[i].ind != NOT_ALLOCATED && allocatedRegs[i].needsStore)
//			mov(REG_PTR(i), V_REGS_8[allocatedRegs[i].ind]);
//	}
//}

bool ChipEmitter::emitPushAllocRegs()
{
	if (!rspAllocReg.has_value())
	{
		for (int i = 0; i < 16; i++)
		{
			if (allocatedVRegs[i] == RSP_ALLOC_IND)
			{
				rspAllocReg = i;
				break;
			}
		}
	}

	if (rspAllocReg.has_value())
		mov(byte[REG_PTR(*rspAllocReg)], spl);

	mov(rsp, qword[rip + rspBackup]);

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
void ChipEmitter::emitPopAllocRegs()
{
	for (int i = 15; i >= 0; i--)
	{
		const auto val { allocatedVRegs[i] };

		if (val == NOT_ALLOCATED || !CALLER_SAVED_V_REGS[val])
			continue;

		pop(V_REGS_64[val]);
	}

	if (rspAllocReg.has_value())
		movzx(esp, byte[REG_PTR(*rspAllocReg)]);
}

template<typename T>
void ChipEmitter::emitCallFunc(T func, bool stackAligned)
{
	if (!stackAligned)
		sub(rsp, 8);

	mov(rax, addr(func));
 	call(rax);
}

void ChipEmitter::emitSelfModifyingCodeCheck(int cnt, uint16_t pc)
{
	auto checkSmc = [&](bool secondTest, bool endImmediately)
	{
		Xbyak::Label noSmc;
		const auto bitsetOffset { offset(&core.s, &core.JIT.compiledRam) };

		if (!secondTest)
		{
			mov(edx, I_REG_32);
			shr(edx, 6);
			test(qword[BASE + rdx * 8 + bitsetOffset], r8);
		}
		else
			test(qword[BASE + rdx * 8 + bitsetOffset + 8], r8);

		jz(noSmc, T_NEAR);

		bool stackAligned { emitPushAllocRegs() };

		if (!secondTest && !endImmediately)
		{
			push(rcx); // remainder
			stackAligned = !stackAligned;
		}

		lea(ARG1, CORE_PTR);
#ifndef _WIN32 // rdx and r8 are ARG's 2 and 3 on win64 calling convention, so values are already there.
		mov(ARG2, rdx);
		mov(ARG3, r8);
#endif
		emitCallFunc(&ChipJITCore::invalidateBlocks, stackAligned);

		if (!secondTest && !endImmediately)
		{
			pop(rcx);
			// reloading index
			mov(edx, I_REG_32);
			shr(edx, 6);
		}

		emitPopAllocRegs();

		if (endImmediately)
			emitEpilogue(pc); // exiting the current block, because self-modifying code could modify it.
		else
			mov(eax, 1); // smc flag

		L(noSmc);
	};

	mov(ecx, I_REG_32);
	and_(ecx, 64 - 1);
	mov(r8d, (1ULL << cnt) - 1);

	Xbyak::Label noBitsPastBoundary, end;

	cmp(ecx, 64 - cnt); // check if (bit + count) > 64 (next qword needs to be tested too)
	jbe(noBitsPastBoundary, T_NEAR);
	lea(edx, ptr[rcx - (64 - cnt)]);
	xor_(eax, eax); // smc flag
	sub(ecx, edx);
	shl(r8, cl);
	mov(ecx, edx); // saving the remainder
	checkSmc(false, false);

	// setting r8d to a mask with 'ecx' low bits set.
	mov(r8d, -1);
	if (BMI2)
		bzhi(r8d, r8d, ecx);
	else
	{
		shl(r8d, cl);
		not_(r8d);
	}

	checkSmc(true, false);
	test(al, al); // smc check
	jz(end, T_NEAR);
	emitEpilogue(pc);

	L(noBitsPastBoundary);
	shl(r8, cl);
	checkSmc(false, true);
	L(end);
}

void ChipEmitter::emitIllegalOPHandler()
{
	const bool stackAligned { emitPushAllocRegs() };
	mov(ARG1, CORE_PTR);
	emitCallFunc(&ChipJITCore::illegalOpcodeHandler, stackAligned);
	emitPopAllocRegs();
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
			movzx(V_REGS_32[val], byte[REG_PTR(i)]);
	}
}

void ChipEmitter::emitEpilogue(uint16_t pc)
{
	for (int i = 0; i < 15; i++)
	{
		const auto val { allocatedVRegs[i] };

		if (val != NOT_ALLOCATED && modifiedVRegs[i])
			mov(byte[REG_PTR(i)], V_REGS_8[val]);
	}

	movzx(eax, EXECUTE_FLAG_PTR);

	if (pc != static_cast<uint16_t>(-1))
		mov(PC_32, pc);

	add(INSTR_COUNT, instructions - branchedInstrs);

	test(al, al);
	jz(dispatcherEnd);
	DISPATCH();
}

uint8_t* ChipEmitter::emitJumpPlaceholder()
{
	db(0xE9);
	dd(0);
	return getCodeEndPtr();
}
void ChipEmitter::patchBranchInstr(uint8_t* branchCodeEndPtr, bool incBeforeBranch) const
{
	constexpr auto INC_R64_SIZE { 3 };
	const auto offset { incBeforeBranch ? INC_R64_SIZE : 0 };

	const int32_t jumpOffset { static_cast<int32_t>(getCodeEndPtr() - branchCodeEndPtr + offset) };
	uint8_t* offsetPtr { branchCodeEndPtr - offset - sizeof(int32_t) };
	std::memcpy(offsetPtr, &jumpOffset, sizeof(jumpOffset));
}

void ChipEmitter::emitInstrCountAdd(int32_t instrs)
{
	add(INSTR_COUNT, instrs);
}
void ChipEmitter::emitInstrCountAddPlaceholder()
{
	add(INSTR_COUNT, INT32_MAX);
}
void ChipEmitter::patchImm32(uint8_t* codeEndPtr, int32_t imm) const
{
	uint8_t* targetPtr { codeEndPtr - sizeof(int32_t) };
	std::memcpy(targetPtr, &imm, sizeof(imm));
}

#define quirks core.s.quirks

void ChipEmitter::emit00E0()
{
	if (AVX)
	{
		vpxor(xmm0, xmm0, xmm0);

		for (int i = 0; i < 32; i += 4)
			vmovdqa(SCREEN_PTR(i), ymm0);

		vzeroupper();
	}
	else
	{
		pxor(xmm0, xmm0);

		for (int i = 0; i < 32; i += 2)
			movdqa(SCREEN_PTR(i), xmm0);
	}
}

void ChipEmitter::emit00EE(bool restorePC)
{
	dec(SP_32);

	if (restorePC)
	{
		movzx(ecx, SP_8);
		movzx(PC_32, STACK_PTR(rcx));
	}
}

void ChipEmitter::emit1NNN(uint16_t addr)
{
	mov(PC_32, addr);
}

void ChipEmitter::emit2NNN(uint16_t addr, uint16_t pc, bool setPC)
{
	movzx(ecx, SP_8);
	mov(STACK_PTR(rcx), pc);

	if (setPC)
		mov(PC_32, addr);

	inc(SP_32);
}

void ChipEmitter::emit5XY0(uint8_t x, uint8_t y, bool incBranches)
{
	CMP(V_REG(x), V_REG(y));
	// jz
	db(0x0F);
	db(0x84);
	dd(0); // reserve 4 bytes for the offset

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
	const auto i { allocatedVRegs[x] };

	if (i != NOT_ALLOCATED && val == 0)
		test(V_REGS_8[i], V_REGS_8[i]);
	else
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
	const auto i { allocatedVRegs[x] };

	if (i != NOT_ALLOCATED && val == 0)
		test(V_REGS_8[i], V_REGS_8[i]);
	else
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
	movzx(ecx, V_REG(x));
	and_(ecx, 0xF);
	movzx(ecx, KEY_PTR(rcx));
	test(cl, cl);
	// jnz
	db(0x0F);
	db(0x85);
	dd(0);

	if (incBranches)
		inc(INSTR_COUNT);
}
void ChipEmitter::emitEXA1(uint8_t x, bool incBranches)
{
	movzx(ecx, V_REG(x));
	and_(ecx, 0xF);
	movzx(ecx, KEY_PTR(rcx));
	test(cl, cl);
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
		mov(byte[REG_PTR(x)], val);
}

void ChipEmitter::emit7XNN(uint8_t x, uint8_t val)
{
	switch (val)
	{
	case 0x0:
		return;
	case 0x1:
		inc(V_REG(x));
		break;
	case 0xFF:
		dec(V_REG(x));
		break;
	default:
		add(V_REG(x), val);
		break;
	}
}

void ChipEmitter::emit8XY0(uint8_t x, uint8_t y)
{
	if (x == y)
		return;

	MOV_TO_REG(x, V_REG(y));
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
			if (!quirks.shifting)
				emit8XY0(x, y);

			and_(FLAG_REG, 0x1);
		}
	}
	else
	{
		if (!quirks.shifting)
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

	movzx(eax, V_REG(y));
	sub(al, V_REG(x));

	if (x != 0xF)
		MOV_TO_REG(x, al);

	if (calcFlag)
		setnc(FLAG_REG);
}

void ChipEmitter::emit8XYE(uint8_t x, uint8_t y, bool calcFlag)
{
	if (x == 0xF)
	{
		if (calcFlag)
		{
			if (!quirks.shifting)
				emit8XY0(x, y);

			shr(FLAG_REG, 7);
		}
	}
	else
	{
		if (!quirks.shifting)
			emit8XY0(x, y);

		const auto i { allocatedVRegs[x] };

		if (i != NOT_ALLOCATED)
			add(V_REGS_8[i], V_REGS_8[i]); // is slightly faster than shift
		else
			shl(byte[REG_PTR(x)], 1);

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

	Xbyak::Label drawEnd, drawUnknownRem;

	const bool wideDraw { n >= 2 };
	int i { 0 }, cnt { n };
	bool loadedInitialVals { false };

	auto LOAD_INITIAL_DXYN_VALS = [&]()
	{
		if (loadedInitialVals)
			return;

		movzx(eax, V_REG(y));
		movzx(ecx, V_REG(x));

		if (calcFlag)
			xor_(FLAG_REG_32, FLAG_REG_32);

		and_(eax, (ChipState::SCR_HEIGHT - 1));

		if (wideDraw)
		{
			xor_(r8d, r8d); // row counter
			and_(ecx, ChipState::SCR_WIDTH - 1); // SSE/AVX shifts don't mask shift count
		}

		loadedInitialVals = true;
	};

	if (wideDraw)
	{
		bool loadedSimdConsts { false };
		LOAD_INITIAL_DXYN_VALS();

		auto AVX_DXYN = [&](int width)
		{
			// y/xmm0 = 4 bytes from ram zero-extended to each quad lane; y/xmm1 = 4 rows from the screen buffer
			// y/xmm2 = shift amount (x) in low lane; y/xmm3 = 64 - x in low lane (for wrapping); y/xmm4 = temp.

			const auto regs { width == 4 ? std::array<Xbyak::Xmm, 5> {ymm0, ymm1, ymm2, ymm3, ymm4} : std::array {xmm0, xmm1, xmm2, xmm3, xmm4} };

			while (cnt >= width)
			{
				cmp(eax, ChipState::SCR_HEIGHT - width);
				ja(drawUnknownRem, T_NEAR);

				vpmovzxbq(regs[0], ptr[RAM_PTR(i)]);

				if (calcFlag)
					vmovdqu(regs[1], SCREEN_PTR(rax));

				if (!loadedSimdConsts)
				{
					if (!quirks.clipping)
					{
						mov(edx, 64);
						sub(edx, ecx);
						vmovq(xmm3, rdx);
					}

					vmovq(xmm2, rcx);
					loadedSimdConsts = true;
				}

				vpsllq(regs[0], regs[0], 56);

				if (quirks.clipping)
					vpsrlq(regs[0], regs[0], regs[2]);
				else
				{
					vpsllq(regs[4], regs[0], regs[3]); // y/xmm4 = data << (64 - x)
					vpsrlq(regs[0], regs[0], regs[2]);
					vpor(regs[0], regs[0], regs[4]);
				}

				if (calcFlag)
				{
					vptest(regs[0], regs[1]);
					Xbyak::Label skip;
					jz(skip);
					mov(FLAG_REG_32, 1);
					L(skip);

					vpxor(regs[0], regs[0], regs[1]);
				}
				else
					vpxor(regs[0], regs[0], SCREEN_PTR(rax));

				vmovdqu(SCREEN_PTR(rax), regs[0]);

				i += width;
				cnt -= width;

				if (cnt < 4 && width == 4) // if it's the last avx2 iteration
					vzeroupper();

				add(eax, width);

				// update row counter only if it's not the last SIMD iteration, because if it is then the remainder is guaranteed to be 1 or 0 so it won't be checked.
				if (cnt >= 2)
					add(r8d, width);
			}
		};
		auto SSE_DXYN = [&]()
		{
			// xmm0 = 4 bytes from ram zero-extended to each quad lane; xmm1 = 4 rows from the screen buffer
			// xmm2 = shift amount (x) in low lane; xmm3 = 64 - x in low lane (for wrapping); xmm4 = temp; xmm5 = zero.

			bool zeroedXmm { false };

			while (cnt >= 2)
			{
				cmp(eax, ChipState::SCR_HEIGHT - 2);
				ja(drawUnknownRem, T_NEAR);

				auto loadConsts = [&]
				{
					if (loadedSimdConsts)
						return;

					if (!quirks.clipping)
					{
						mov(edx, 64);
						sub(edx, ecx);
						movq(xmm3, rdx);
					}

					movq(xmm2, rcx);
					loadedSimdConsts = true;
				};

				if (SSE41)
				{
					pmovzxbq(xmm0, ptr[RAM_PTR(i)]);
					loadConsts();
				}
				else
				{
					movq(xmm0, qword[RAM_PTR(i)]);

					if (!zeroedXmm)
					{
						pxor(xmm5, xmm5);
						zeroedXmm = true;
					}

					loadConsts();

					punpcklbw(xmm0, xmm5);
					punpcklwd(xmm0, xmm5);
					punpckldq(xmm0, xmm5);
				}

				movdqu(xmm1, SCREEN_PTR(rax));
				psllq(xmm0, 56);

				if (quirks.clipping)
					psrlq(xmm0, xmm2);
				else
				{
					movdqa(xmm4, xmm0);
					psllq(xmm4, xmm3);
					psrlq(xmm0, xmm2);
					por(xmm0, xmm4);
				}

				if (calcFlag)
				{
					if (SSE41)
						ptest(xmm0, xmm1);
					else
					{
						movdqa(xmm4, xmm0);
						pand(xmm4, xmm1);
						pcmpeqb(xmm4, xmm5); // compare against zero
						pmovmskb(edx, xmm4);
						cmp(edx, 0xFFFF);
					}

					Xbyak::Label skip;
					jz(skip);
					mov(FLAG_REG_32, 1);
					L(skip);
				}

				pxor(xmm0, xmm1);
				movdqu(SCREEN_PTR(rax), xmm0);

				i += 2;
				cnt -= 2;

				add(eax, 2);

				if (cnt >= 2)
					add(r8d, 2);
			}
		};

		if (AVX)
		{
			if (AVX2)
				AVX_DXYN(4);

			AVX_DXYN(2);
		}
		else
			SSE_DXYN();
	}

	auto REGULAR_DXYN = [&](bool unknownRemainder)
	{
		Xbyak::Label drawLoop;

		if (wideDraw)
		{
			L(drawLoop);

			if (quirks.clipping)
			{
				cmp(eax, ChipState::SCR_HEIGHT);
				je(drawEnd);
			}
			else
				and_(eax, ChipState::SCR_HEIGHT - 1);
		}

		if (unknownRemainder)
		{
			lea(rdx, ptr[RAM_PTR(0)]);
			movzx(edx, byte[rdx + r8]);
		}
		else
		{
			movzx(edx, byte[RAM_PTR(i)]);

			if (calcFlag)
			{
				LOAD_INITIAL_DXYN_VALS();
				mov(r8, SCREEN_PTR(rax));
			}
		}

		LOAD_INITIAL_DXYN_VALS(); // doing it before accessing the sprite row to lower the impact of latency.
		shl(rdx, 56);

		if (quirks.clipping)
		{
			if (BMI2 && AMD_CPU) // shrx has 3 cycle latency on modern intel cores, but on amd it's faster than shr.
				shrx(rdx, rdx, rcx);
			else
				shr(rdx, cl);
		}
		else
			ror(rdx, cl);

		if (calcFlag)
		{
			if (unknownRemainder)
				test(SCREEN_PTR(rax), rdx);
			else
				test(r8, rdx);

			Xbyak::Label skip;
			// flag calculation using branch is faster than branchless (setnz + or)
			jz(skip);
			mov(FLAG_REG_32, 1);
			L(skip);

			if (unknownRemainder)
				xor_(SCREEN_PTR(rax), rdx);
			else
			{
				xor_(r8, rdx);
				mov(SCREEN_PTR(rax), r8);
			}
		}
		else
			xor_(SCREEN_PTR(rax), rdx);

		if (unknownRemainder)
		{
			inc(r8d);
			inc(eax);
			cmp(r8d, n);
			jne(drawLoop);
		}
	};

	if (n % 2 != 0) // if n is multiple of 2 then all iterations could already be completed by SIMD loop.
		REGULAR_DXYN(false);

	if (wideDraw)
	{
		jmp(drawEnd);
		L(drawUnknownRem);
		REGULAR_DXYN(true);
	}

	L(drawEnd);
}

void ChipEmitter::emitFX07(uint8_t x)
{
	MOV_TO_REG(x, DELAY_PTR);
}
void ChipEmitter::emitFX15(uint8_t x)
{
	MOV(DELAY_PTR, V_REG(x));
}
void ChipEmitter::emitFX18(uint8_t x)
{
	MOV(SOUND_PTR, V_REG(x));
}

void ChipEmitter::emitFX1E(uint8_t x)
{
	movzx(eax, V_REG(x));
	add(I_REG_32, eax);
	and_(I_REG_32, 0xFFF);
}

void ChipEmitter::emitFX29(uint8_t x)
{
	movzx(eax, V_REG(x));
	and_(eax, 0xF);
 	lea(I_REG_32, ptr[rax + rax * 4]);
}

void ChipEmitter::emitFX33(uint8_t x, uint16_t pc)
{
	Xbyak::Label oob;
	movzx(eax, V_REG(x));

	lea(ecx, ptr[rax + 4 * rax]);
	lea(ecx, ptr[rax + 8 * rcx]);
	shr(ecx, 12);
	mov(byte[RAM_PTR(0)], cl);
	cmp(I_REG_16, 0xFFF);
	je(oob, T_NEAR);
	imul(ecx, ecx, 100);
	sub(eax, ecx);
	imul(ecx, eax, 205);
	shr(ecx, 11);
	mov(byte[RAM_PTR(1)], cl);
	cmp(I_REG_16, 0xFFE);
	je(oob, T_NEAR);
	add(ecx, ecx);
	lea(ecx, ptr[rcx + 4 * rcx]);
	sub(al, cl);
	mov(byte[RAM_PTR(2)], al);

	emitSelfModifyingCodeCheck(3, pc);
	L(oob);
}

template <bool toMem>
void ChipEmitter::emitRegCopy(int cnt)
{
	auto regsNotAllocated = [&](int startInd, int endInd) -> bool
	{
		for (int i = startInd; i < endInd; i++)
		{
			if (allocatedVRegs[i] != NOT_ALLOCATED)
				return false;
		}

		return true;
	};

	if (cnt == 16 && regsNotAllocated(0, 16))
	{
		if (AVX)
		{
			if constexpr (toMem)
			{
				vmovdqa(xmm0, xword[REG_PTR(0)]);
				vmovdqu(xword[RAM_PTR(0)], xmm0);
			}
			else
			{
				vmovdqu(xmm0, xword[RAM_PTR(0)]);
				vmovdqa(xword[REG_PTR(0)], xmm0);
			}
		}
		else
		{
			if constexpr (toMem)
			{
				movdqa(xmm0, xword[REG_PTR(0)]);
				movdqu(xword[RAM_PTR(0)], xmm0);
			}
			else
			{
				movdqu(xmm0, xword[RAM_PTR(0)]);
				movdqa(xword[REG_PTR(0)], xmm0);
			}
		}

		return;
	}

	for (int i = 0; cnt > 0; )
	{
		if (cnt >= 8 && regsNotAllocated(i, i + 8))
		{
			if constexpr (toMem)
			{
				mov(rax, qword[REG_PTR(i)]);
				mov(qword[RAM_PTR(i)], rax);
			}
			else
			{
				mov(rax, qword[RAM_PTR(i)]);
				mov(qword[REG_PTR(i)], rax);
			}

			i += 8;
			cnt -= 8;
			continue;
		}
		if (cnt >= 4 && regsNotAllocated(i, i + 4))
		{
			if constexpr (toMem)
			{
				mov(eax, dword[REG_PTR(i)]);
				mov(dword[RAM_PTR(i)], eax);
			}
			else
			{
				mov(eax, dword[RAM_PTR(i)]);
				mov(dword[REG_PTR(i)], eax);

			}

			i += 4;
			cnt -= 4;
			continue;
		}
		if (cnt >= 2 && regsNotAllocated(i, i + 2))
		{
			if constexpr (toMem)
			{
				movzx(eax, word[REG_PTR(i)]);
				mov(word[RAM_PTR(i)], ax);
			}
			else
			{
				movzx(eax, word[RAM_PTR(i)]);
				mov(word[REG_PTR(i)], ax);
			}

			i += 2;
			cnt -= 2;
			continue;
		}

		if constexpr (toMem)
			MOV(byte[RAM_PTR(i)], V_REG(i));
		else
			MOV_TO_REG(i, byte[RAM_PTR(i)]);

		i++;
		cnt--;
	}
}

void ChipEmitter::emitFX55(uint8_t x, uint16_t pc)
{
	Xbyak::Label oob;

	if (x != 0)
	{
		cmp(I_REG_16, 0xFFF - x);
		ja(oob, T_NEAR);
	}

	const int cnt { x + 1 };
	emitRegCopy<true>(cnt);
	emitSelfModifyingCodeCheck(cnt, pc);

	if (quirks.memoryIncrement)
	{
		add(I_REG_32, cnt);
		and_(I_REG_32, 0xFFF);
	}

	L(oob);
}
void ChipEmitter::emitFX65(uint8_t x)
{
	const int cnt { x + 1 };
	emitRegCopy<false>(cnt);

	if (quirks.memoryIncrement)
	{
		add(I_REG_32, cnt);
		and_(I_REG_32, 0xFFF);
	}
}

void ChipEmitter::emitFX0A(uint8_t x, uint16_t pc)
{
	Xbyak::Label waitKey, waitDone, end;

	mov(PC_32, pc - 2);
	cmp(FX0A_FLAG_PTR, 0);
	jz(waitKey);

	mov(FX0A_REG_PTR, x);
	mov(FX0A_FLAG_PTR, 0);

	L(waitKey);
	cmp(FX0A_REG_PTR, -1);
	jz(waitDone);
	wait();
	cmp(EXECUTE_FLAG_PTR, 0);
	jnz(waitKey);
	jmp(end);

	L(waitDone);
	mov(FX0A_FLAG_PTR, 1);
	mov(PC_32, pc);

	L(end);
}