#include "ChipAsmInterpretCore.h"
#include "utils.h"

#define BASE rbp
#define TABLE_BASE rbx
#define INSTR_COUNT r13

#define PC_64 rdi
#define PC_32 edi
#define PC_16 di
#define I_REG_64 rsi
#define I_REG_32 esi
#define I_REG_16 si
#define SP_64 r12
#define SP_32 r12d
#define SP_8 r12b

#define V_REG(num) byte[BASE + offsetof(ChipState, V) + num]
#define FLAG_REG V_REG(0xF)
#define RAM_PTR(offset) byte[BASE + I_REG_64 + offsetof(ChipState, RAM) + offset]
#define STACK_PTR(offset) word[BASE + offsetof(ChipState, stack) + (offset * sizeof(uint16_t))]
#define SCREEN_PTR(offset) ptr[BASE + offsetof(ChipState, screenBuffer) + (offset * sizeof(uint64_t))]

#define DELAY_PTR byte[BASE + offsetof(ChipState, delayTimer)]
#define SOUND_PTR byte[BASE + offsetof(ChipState, soundTimer)]
#define KEY_PTR(offset) byte[BASE + offsetof(ChipState, keys) + offset]
#define FX0A_FLAG_PTR byte[BASE + offsetof(ChipState, firstFX0ACall)]
#define FX0A_REG_PTR byte[BASE + offsetof(ChipState, inputReg)]

ChipAsmInterpretCore::ChipAsmInterpretCore(ChipState& s, std::atomic<bool>& executeFlag) : ChipCore(s, executeFlag), Xbyak::CodeGenerator(786432)
{
	Xbyak::Label opInvalid, op00E0, op00EE, op1NNN, op2NNN, op3XNN, op4XNN, op5XY0, op6XNN, op7XNN,
				 op8XY0, op8XY1, op8XY2, op8XY3, op8XY4, op8XY5, op8XY6, op8XY7, op8XYE, op9XY0,
				 opANNN, opBNNN, opCXNN, opDXY0, opDXY1, opDXYN, opEX9E, opEXA1, opFX07, opFX0A,
				 opFX15, opFX18, opFX1E, opFX29, opFX33, opFX55, opFX65;

	Xbyak::Label dispatchTable;
	L(dispatchTable);

	for (int i = 0; i <= 0xFFFF; i++)
	{
		const int msn { (i & 0x00F0) >> 4 }, lsn { (i & 0x0F00) >> 8 }, lsb { i >> 8 };

		switch (msn)
		{
		case 0x0:
			putL(i == 0xE000 ? op00E0 : i == 0xEE00 ? op00EE : opInvalid);
			break;
		case 0x1:
			putL(op1NNN);
			break;
		case 0x2:
			putL(op2NNN);
			break;
		case 0x3:
			putL(op3XNN);
			break;
		case 0x4:
			putL(op4XNN);
			break;
		case 0x5:
			putL(lsn == 0 ? op5XY0 : opInvalid);
			break;
		case 0x6:
			putL(op6XNN);
			break;
		case 0x7:
			putL(op7XNN);
			break;
		case 0x8:
		{
			const std::array labels { &op8XY0, &op8XY1, &op8XY2, &op8XY3, &op8XY4, &op8XY5, &op8XY6, &op8XY7, &opInvalid,
									  &opInvalid, &opInvalid, &opInvalid, &opInvalid, &opInvalid, &op8XYE, &opInvalid };

			putL(*labels[lsn]);
			break;
		}
		case 0x9:
			putL(lsn == 0 ? op9XY0 : opInvalid);
			break;
		case 0xA:
			putL(opANNN);
			break;
		case 0xB:
			putL(opBNNN);
			break;
		case 0xC:
			putL(opCXNN);
			break;
		case 0xD:
			putL(lsn == 0 ? opDXY0 : lsn == 1 ? opDXY1 : opDXYN);
			break;
		case 0xE:
			putL(lsb == 0x9E ? opEX9E : lsb == 0xA1 ? opEXA1 : opInvalid);
			break;
		case 0xF:
		{
			switch (lsb)
			{
			case 0x07:
				putL(opFX07);
				break;
			case 0x0A:
				putL(opFX0A);
				break;
			case 0x15:
				putL(opFX15);
				break;
			case 0x18:
				putL(opFX18);
				break;
			case 0x1E:
				putL(opFX1E);
				break;
			case 0x29:
				putL(opFX29);
				break;
			case 0x33:
				putL(opFX33);
				break;
			case 0x55:
				putL(opFX55);
				break;
			case 0x65:
				putL(opFX65);
				break;
			default:
				putL(opInvalid);
				break;
			}
			break;
		}
		}
	}

	codePtr = getCurr<uint64_t(*)()>();

	push(rbp);
	push(rbx);
	push(r12);
	push(r13);

#ifdef _WIN32
	push(rsi);
	push(rdi);
#endif

	lea(TABLE_BASE, ptr[rip + dispatchTable]);
	mov(BASE, addr(&s));

	xor_(INSTR_COUNT, INSTR_COUNT);
	movzx(PC_32, word[BASE + offsetof(ChipState, pc)]);
	movzx(I_REG_32, word[BASE + offsetof(ChipState, I)]);
	movzx(SP_32, byte[BASE + offsetof(ChipState, sp)]);

	Xbyak::Label dispatcher, end;
	L(dispatcher);

	auto DISPATCH = [&]()
	{
		inc(INSTR_COUNT);
		cmp(byte[BASE + offset(&s, &executeFlag)], 0);
		jz(end, T_NEAR);

		movzx(eax, word[BASE + offsetof(ChipState, RAM) + PC_64]);
		add(PC_32, 2);
		jmp(qword[TABLE_BASE + rax * 8]);
	};

	movzx(eax, word[BASE + offsetof(ChipState, RAM) + PC_64]);
	add(PC_32, 2);
	jmp(qword[TABLE_BASE + rax * 8]);

	auto loadNNN = [&]()
	{
		rol(ax, 8);
		and_(ax, 0xFFF);
	};
	auto loadVx = [&]()
	{
		and_(eax, 0xF);
		movzx(eax, V_REG(rax));
	};
	auto loadVxPtr = [&]()
	{
		mov(ecx, eax);
		and_(ecx, 0xF);
	};
	auto loadVyAndVxPtr = [&]()
	{
		mov(ecx, eax);
		shr(eax, 12);
		movzx(eax, V_REG(rax));
		and_(ecx, 0xF);
	};

	L(op00E0);
	if (Xbyak::util::Cpu().has(Xbyak::util::Cpu::tAVX))
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
	DISPATCH();

	L(op00EE);
	dec(SP_32);
	movzx(ecx, SP_8);
	movzx(PC_32, STACK_PTR(rcx));
	DISPATCH();

	L(op1NNN);
	loadNNN();
	movzx(PC_32, ax);
	DISPATCH();

	L(op2NNN);
	movzx(ecx, SP_8);
	loadNNN();
	mov(STACK_PTR(rcx), PC_16);
	movzx(PC_32, ax);
	inc(SP_32);
	DISPATCH();

	L(op3XNN);
	loadVxPtr();
	cmp(V_REG(rcx), ah);
	Xbyak::Label skipNotEq;
	jne(skipNotEq);
	add(PC_32, 2);
	L(skipNotEq);
	DISPATCH();

	L(op4XNN);
	loadVxPtr();
	cmp(V_REG(rcx), ah);
	Xbyak::Label skipEq;
	je(skipEq);
	add(PC_32, 2);
	L(skipEq);
	DISPATCH();

	L(op5XY0);
	loadVyAndVxPtr();
	cmp(V_REG(rcx), al);
	Xbyak::Label skipRegNotEq;
	jne(skipRegNotEq);
	add(PC_32, 2);
	L(skipRegNotEq);
	DISPATCH();

	L(op6XNN);
	loadVxPtr();
	mov(V_REG(rcx), ah);
	DISPATCH();

	L(op7XNN);
	loadVxPtr();
	add(V_REG(rcx), ah);
	DISPATCH();

	L(op8XY0);
	loadVyAndVxPtr();
	mov(V_REG(rcx), al);
	DISPATCH();

	L(op8XY1);
	loadVyAndVxPtr();
	or_(V_REG(rcx), al);
	mov(FLAG_REG, 0); // VF reset quirk
	DISPATCH();

	L(op8XY2);
	loadVyAndVxPtr();
	and_(V_REG(rcx), al);
	mov(FLAG_REG, 0); // VF reset quirk
	DISPATCH();

	L(op8XY3);
	loadVyAndVxPtr();
	xor_(V_REG(rcx), al);
	mov(FLAG_REG, 0); // VF reset quirk
	DISPATCH();

	L(op8XY4);
	loadVyAndVxPtr();
	add(V_REG(rcx), al);
	setc(FLAG_REG);
	DISPATCH();

	L(op8XY5);
	loadVyAndVxPtr();
	sub(V_REG(rcx), al);
	setnc(FLAG_REG);
	DISPATCH();

	L(op8XY6); // shifting quirk
	loadVyAndVxPtr();
	shr(eax, 1);
	mov(V_REG(rcx), al);
	setc(FLAG_REG);
	DISPATCH();

	L(op8XY7);
	loadVyAndVxPtr();
	sub(al, V_REG(rcx));
	mov(V_REG(rcx), al);
	setnc(FLAG_REG);
	DISPATCH();

	L(op8XYE); // shifting quirk
	loadVyAndVxPtr();
	add(al, al);
	mov(V_REG(rcx), al);
	setc(FLAG_REG);
	DISPATCH();

	L(op9XY0);
	loadVyAndVxPtr();
	cmp(V_REG(rcx), al);
	Xbyak::Label skipRegEq;
	je(skipRegEq);
	add(PC_32, 2);
	L(skipRegEq);
	DISPATCH();

	L(opANNN);
	loadNNN();
	movzx(I_REG_32, ax);
	DISPATCH();

	L(opBNNN); // jumping quirk off
	movzx(ecx, V_REG(0));
	loadNNN();
	movzx(PC_32, ax);
	add(PC_32, ecx);
	and_(PC_32, 0xFFF);
	DISPATCH();

	L(opCXNN);
	mov(ecx, eax);
	rdtsc(); // using cpu timestamp as a random number
	and_(eax, ch);
	and_(ecx, 0xF);
	mov(V_REG(rcx), al);
	DISPATCH();

	L(opDXY0);
	mov(FLAG_REG, 0);
	DISPATCH();

	L(opDXY1);
	loadVyAndVxPtr();
	movzx(ecx, V_REG(rcx));
	and_(eax, ChipState::SCR_HEIGHT - 1);
	movzx(r8d, RAM_PTR(0));
	mov(r9, SCREEN_PTR(rax));
	shl(r8, 56);
	shrx(r8, r8, rcx);
	test(r8, r9);
	setnz(FLAG_REG);
	xor_(r8, r9);
	mov(SCREEN_PTR(rax), r8);
	DISPATCH();

	L(opDXYN);
	Xbyak::Label dxynEnd;
	movzx(edx, ah);
	and_(edx, 0xF);
	loadVyAndVxPtr();
	movzx(ecx, V_REG(rcx));
	mov(FLAG_REG, 0);
	and_(eax, ChipState::SCR_HEIGHT - 1);

	for (int i = 0; i < 0xF; i++)
	{
		movzx(r8d, RAM_PTR(i));
		mov(r9, SCREEN_PTR(rax));
		shl(r8, 56);
		shrx(r8, r8, rcx);
		test(r8, r9);
		Xbyak::Label noCollision;
		jz(noCollision);
		mov(FLAG_REG, 1);
		L(noCollision);
		xor_(r8, r9);
		mov(SCREEN_PTR(rax), r8);
		dec(edx);
		jz(dxynEnd, T_NEAR);
		cmp(eax, ChipState::SCR_HEIGHT - 1);
		jz(dxynEnd, T_NEAR);
		inc(eax);
	}

	L(dxynEnd);
	DISPATCH();

	L(opEX9E);
	loadVx();
	movzx(eax, KEY_PTR(rax));
	test(al, al);
	Xbyak::Label skipNotPressed;
	je(skipNotPressed);
	add(PC_32, 2);
	L(skipNotPressed);
	DISPATCH();

	L(opEXA1);
	loadVx();
	movzx(eax, KEY_PTR(rax));
	test(al, al);
	Xbyak::Label skipPressed;
	jne(skipPressed);
	add(PC_32, 2);
	L(skipPressed);
	DISPATCH();

	L(opFX07);
	movzx(ecx, DELAY_PTR);
	and_(eax, 0xF);
	mov(V_REG(rax), cl);
	DISPATCH();

	L(opFX0A);
	// TODO
	DISPATCH();

	L(opFX15);
	loadVx();
	mov(DELAY_PTR, al);
	DISPATCH();

	L(opFX18);
	loadVx();
	mov(SOUND_PTR, al);
	DISPATCH();

	L(opFX1E);
	loadVx();
	add(I_REG_32, eax);
	and_(I_REG_32, 0xFFF);
	DISPATCH();

	L(opFX29);
	loadVx();
	and_(eax, 0xF);
	lea(I_REG_32, ptr[rax + rax * 4]);
	DISPATCH();

	L(opFX33);
	Xbyak::Label fx33oob;
	loadVx();
	lea(ecx, ptr[rax + 4 * rax]);
	lea(ecx, ptr[rax + 8 * rcx]);
	shr(ecx, 12);
	mov(RAM_PTR(0), cl);
	cmp(I_REG_16, 0xFFF);
	je(fx33oob);
	imul(ecx, ecx, 100);
	sub(eax, ecx);
	imul(ecx, eax, 205);
	shr(ecx, 11);
	mov(RAM_PTR(1), cl);
	cmp(I_REG_16, 0xFFE);
	je(fx33oob);
	add(ecx, ecx);
	lea(ecx, ptr[rcx + 4 * rcx]);
	sub(al, cl);
	mov(RAM_PTR(2), al);
	L(fx33oob);
	DISPATCH();

	L(opFX55);
	Xbyak::Label fx55end, fx55oob;
	and_(eax, 0xF);
	lea(ecx, ptr[I_REG_64 + rax]);
	cmp(ecx, 0xFFF);
	ja(fx55oob, T_NEAR);
	inc(eax);
	mov(edx, eax);

	for (int i = 0; i <= 0xF; i++)
	{
		movzx(ecx, V_REG(i));
		mov(RAM_PTR(i), cl);
		dec(eax);
		jz(fx55end, T_NEAR);
	}

	L(fx55end);
	add(I_REG_32, edx); // memory increment quirk
	and_(I_REG_32, 0xFFF);
	L(fx55oob);
	DISPATCH();

	L(opFX65);
	Xbyak::Label fx65end;
	and_(eax, 0xF);
	inc(eax);
	mov(edx, eax);

	for (int i = 0; i <= 0xF; i++)
	{
		movzx(ecx, RAM_PTR(i));
		mov(V_REG(i), cl);
		dec(eax);
		jz(fx55end, T_NEAR);
	}

	L(fx65end);
	add(I_REG_32, edx); // memory increment quirk
	and_(I_REG_32, 0xFFF);
	DISPATCH();

	L(opInvalid);
	jmp(opInvalid); // TODO

	L(end);
	mov(rax, INSTR_COUNT);
	mov(word[BASE + offsetof(ChipState, pc)], PC_16);
	mov(word[BASE + offsetof(ChipState, I)], I_REG_16);
	mov(byte[BASE + offsetof(ChipState, sp)], SP_8);

#ifdef _WIN32
	pop(rdi);
	pop(rsi);
#endif

	pop(r13);
	pop(r12);
	pop(rbx);
	pop(rbp);
	ret();
}