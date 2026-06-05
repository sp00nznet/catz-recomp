cmake_minimum_required(VERSION 3.16)
project(catz_recomp C)

set(CMAKE_C_STANDARD 11)

# Lifted segment sources (generated into src/ by ne_lift + gen_* tools).
file(GLOB SEGMENT_SOURCES CONFIGURE_DEPENDS src/*.c)

include_directories(runtime)

# Build the lifted engine as a static library.
if(SEGMENT_SOURCES)
    add_library(catz_segments STATIC ${SEGMENT_SOURCES})
    target_compile_options(catz_segments PRIVATE
        -Wall
        -Wno-unused-variable
        -Wno-unused-label
        -Wno-unused-but-set-variable
        -Wno-unused-function
    )
endif()

# Win16 API shim runtime (KERNEL/USER/GDI/SOSLIB03/WING → Win32/SDL2).
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/win16/*.c)
if(RUNTIME_SOURCES)
    add_library(catz_runtime STATIC ${RUNTIME_SOURCES})
    target_include_directories(catz_runtime PRIVATE runtime)
endif()

# Host: CATZ.EXE-equivalent window shell that loads the engine and drives it.
if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/main.c" AND SEGMENT_SOURCES)
    add_executable(catz runtime/main.c)
    target_compile_definitions(catz PRIVATE
        CATZ_IMAGE_PATH="${CMAKE_SOURCE_DIR}/build_data/mem_image.bin")
    target_link_libraries(catz catz_segments catz_runtime m)
endif()
