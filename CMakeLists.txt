cmake_minimum_required(VERSION 3.5.0)
project(interpreter VERSION 0.1.0 LANGUAGES C)

add_executable(interpreter
    src/main.c
    src/chunk.c
    src/memory.c
    src/debug.c
    src/value.c
    src/object.c
    src/table.c
    src/vm.c
    src/scanner.c
    src/compiler.c
)

# For profiling.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -g")
