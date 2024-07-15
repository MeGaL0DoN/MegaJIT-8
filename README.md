# MegaJIT-8

MegaJIT-8 is a Chip 8 emulator with x86-64 JIT-compiler, made in C++ using GLFW, ImGUI and Xbyak.

## Build

Windows, Linux, and MacOS build using cmake is supported with any C++/20 compiler. All libraries are included as source.

### Usage:

Use File->load to load game ROM. File->Reload or ESC to restart current ROM. Press TAB to pause the emulator.
Keyboard layout is: 
| 1 | 2 | 3 | 4 |
| --- | --- | --- | --- |
| Q | W | E | R |
| A | S | D | F |
| Z | X | C | V |

Under CPU menu tab, it is possible to switch between interpreter and JIT while running the ROM. In JIT mode, disassembly containing the compiled code can be exported. Unlimited mode checkbox is used for benchmarking. When it is enabled, Emulator works on maximum speed, and number of millions of instructions per second is displayed.

### Demo - 820 MIPS in 1dcell.benchmark (Ryzen 5 7530u Laptop)
https://github.com/user-attachments/assets/c9e67ff4-01ac-483e-a059-5966ffb296a0


## License

This project is licensed under the MIT License - see the LICENSE.md file for details
