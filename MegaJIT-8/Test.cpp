//#include <iostream>
//
//#include "ChipJITCore.h"
//#include "ChipInterpretCore.h"
//
//extern ChipState s;
//
//#define BENCH_JIT 1
//
//constexpr uint64_t instructionCount = 1000000000;
//
//int main() 
//{
//    s.enableDrawLocking = false;
//
//#if BENCH_JIT == 1
//    ChipJITCore ch;
//    ch.setSlowMode(false);
//#else
//    ChipInterpretCore ch;
//#endif
//
//    ch.loadROM("ROMs/1dcell.benchmark");
//
//    auto start = std::chrono::high_resolution_clock::now();
//    uint64_t totalInstructions = 0;
//
//    while (totalInstructions < instructionCount) 
//        totalInstructions += ch.execute();
//
//    auto end = std::chrono::high_resolution_clock::now();
//
//    std::chrono::duration<double> elapsed = end - start;
//    double elapsedSeconds = elapsed.count();
//
//    uint64_t instructionsPerSecond = totalInstructions / elapsedSeconds;
//
//    // Output the results
//    std::cout << "Executed " << totalInstructions << " instructions in " << elapsedSeconds << " seconds." << std::endl;
//    std::cout << "Instructions per second: " << instructionsPerSecond << std::endl;
//
//    getchar();
//    return 0;
//}