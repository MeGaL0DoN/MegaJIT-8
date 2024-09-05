cmake_minimum_required(VERSION 3.28)
project(udis86)

set(CMAKE_CXX_STANDARD 20)

add_library(udis86 decode.c decode.h extern.h syn.c syn.h syn-att.c syn-intel.c types.h udint.h udis86.c itab.c itab.h)

target_include_directories(udis86 PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_include_directories(udis86 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
