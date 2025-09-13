cmake_minimum_required(VERSION 3.16)
project(MegaJIT_8)

set(CMAKE_CXX_STANDARD 20)
include(FetchContent)

if(MSVC)
    add_compile_options(
            $<$<CONFIG:>:/MT>
            $<$<CONFIG:Release>:/MT>
    )
elseif(MINGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")
endif()

add_executable(MegaJIT_8
        Main.cpp
        resources.h
        utils.h
        Shader.cpp
        Shader.h
        ChipCore.h
        ChipCore.cpp
        ChipEmitter.h
        ChipEmitter.cpp
        ChipJITCore.h
        ChipCachedCore.h
        ChipInterpretCore.h
        ChipAsmInterpretCore.h
        ChipAsmInterpretCore.cpp
        ChipState.h
        ChipState.cpp
        ChipJITState.h
        ChipCachedState.h )

include(CheckIPOSupported)
check_ipo_supported(RESULT supported OUTPUT error)

if (supported)
    message(STATUS "IPO / LTO enabled")
    set_property(TARGET MegaJIT_8 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "IPO / LTO not supported: <${error}>")
endif()

if (MSVC)
    set_target_properties(
    MegaJIT_8 PROPERTIES
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
elseif(MINGW)
    set_target_properties(
            MegaJIT_8 PROPERTIES
            LINK_FLAGS_RELEASE "-Wl,-subsystem,windows -s"
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_target_properties(
            MegaJIT_8 PROPERTIES
            LINK_FLAGS_RELEASE "-s"
    )
endif()

set(GLFW_BUILD_TESTS OFF CACHE BOOL "Disable GLFW tests")
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Disable GLFW examples")
set(GLFW_BUILD_DOCS OFF CACHE BOOL "Disable GLFW docs")
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)

set(NFD_BUILD_TESTS OFF CACHE BOOL "Disable NFD tests")
set(NFD_BUILD_EXAMPLES OFF CACHE BOOL "Disable NFD examples")
FetchContent_Declare(
    nativefiledialog-extended
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        v1.2.1
)

set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "Disable Zydis examples")
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "Disable Zydis tools")
FetchContent_Declare(
    zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_SUBMODULES ""
    GIT_TAG        v4.0.0
)

FetchContent_MakeAvailable(zydis glfw nativefiledialog-extended)

add_subdirectory("Libs/glad")
add_subdirectory("Libs/ImGUI")

target_link_libraries(MegaJIT_8 glfw glad nfd imgui Zydis)

if(WIN32)
    target_link_libraries(MegaJIT_8 winmm) # for timeBeginPeriod(1)
endif()