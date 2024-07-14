# MegaJIT-8

MegaJIT-8 is a Chip 8 emulator with x86-64 JIT-compiler, made in C++ using GLFW, ImGUI and Xbyak.

## Build

Currently only windows build using visual studio is supported, however all libraries used in the emulator are cross-platform, so it in the future it will be possible to build on Linux and Intel MacOS.

### Usage:

Use File->load to load game ROM. File->Reload or ESC to restart current ROM. Press TAB to pause the emulator.
Default keyboard layout is: 
| 1 | 2 | 3 | 4 |
| --- | --- | --- | --- |
| Q | W | E | R |
| A | S | D | F |
| Z | X | C | V |

Layout can be changed by editing data/keyConfig.ini - you should enter the key scancodes on the right side. 

Under CPU menu tab, it is possible to switch between interpreter and JIT while running the ROM. In JIT mode, disassembly containing the compiled code can be exported. Unlimited mode checkbox is used for benchmarking. When it is enabled, Emulator works on maximum speed, and number of millions of instructions per second is displayed.

### Demo - 700 MIPS in 1dcell.benchmark (Ryzen 5 7530u Laptop)
https://github.com/MeGaL0DoN/MegaJIT-8/assets/62940883/805ccc04-22db-4010-aa72-58fde9492498