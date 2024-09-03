cmake_minimum_required(VERSION 3.28)
project(MegaJIT_8)

set(CMAKE_CXX_STANDARD 20)

if(MSVC)
    add_compile_options(
            $<$<CONFIG:>:/MT> #---------|
            $<$<CONFIG:Release>:/MT> #--|
    )
elseif(MINGW)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")
endif()

add_executable(MegaJIT_8 main.cpp
        main.cpp
        Quirks.h
        resources.h
        macros.h
        Shader.cpp
        Shader.h
        Test.cpp
        ChipCore.cpp
        ChipCore.h
        ChipEmitter.h
        ChipJITState.cpp
        ChipJITState.h
        ChipState.cpp
        ChipState.h
        ChipInterpretCore.h
        ChipJITCore.h)

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

file(COPY ROMs DESTINATION ${CMAKE_BINARY_DIR})

add_subdirectory("Libs/glad")
target_link_libraries(MegaJIT_8 glad)

add_subdirectory("Libs/GLFW")
target_link_libraries(MegaJIT_8 glfw)

add_subdirectory("Libs/nativefiledialog-extended")
target_link_libraries(MegaJIT_8 nfd)

add_subdirectory("Libs/udis86")
target_link_libraries(MegaJIT_8 udis86)

add_subdirectory("Libs/ImGUI")
target_link_libraries(MegaJIT_8 imgui)