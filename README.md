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

Under CPU menu tab, you can switch between interpreter, cached interpreter and JIT while running the ROM. In JIT mode, disassembly containing the compiled code can be exported. Unlimited mode checkbox is used for benchmarking. When it is enabled, emulator runs on maximum speed, and the number of instructions per second is displayed.

### Demo - ⚡1000+ MIPS in 1dcell.bnc (Ryzen 5 7530u Laptop)

https://github.com/user-attachments/assets/2e127041-7139-4d9b-9f23-b9de2136564d



## License

This project is licensed under the MIT License - see the LICENSE.md file for details
