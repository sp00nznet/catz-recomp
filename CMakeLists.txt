cmake_minimum_required(VERSION 3.16)
project(catz_recomp C)

set(CMAKE_C_STANDARD 11)

# NOT an optimization preference — a correctness requirement. The lifter turns
# every intra-segment jump into `target(cpu); return;`, so a guest loop spanning
# lifted-function boundaries is host recursion. Only sibling-call optimization
# (-O2) turns it back into a loop; at -O0 the palette inverse-table build
# (seg044 12AD/1356/1357, 4096*256 iterations) blows any host stack.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_C_FLAGS MATCHES "-O")
    # -g1: function names + line tables only, so addr2line can symbolize the
    # host backtrace that stands in for the guest call stack.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -g1")
endif()

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
    target_compile_definitions(catz_runtime PRIVATE
        CATZ_GAME_DIR="${CMAKE_SOURCE_DIR}/game"
        CATZ_DATA_DIR="${CMAKE_SOURCE_DIR}/../catzng/catz")
endif()

# Host: CATZ.EXE-equivalent window shell that loads the engine and drives it.
if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/main.c" AND SEGMENT_SOURCES)
    add_executable(catz runtime/main.c)
    target_compile_definitions(catz PRIVATE
        CATZ_IMAGE_PATH="${CMAKE_SOURCE_DIR}/build_data/mem_image.bin")
    target_link_libraries(catz catz_segments catz_runtime m user32 gdi32)
    # Lifted frames are far fatter than the 16-bit originals; the engine's C++
    # construction recurses deep enough to blow the 1 MB default host stack.
    # No ASLR: runtime addresses == file VAs, so backtrace output can be pasted
    # straight into addr2line.
    target_link_options(catz PRIVATE -Wl,--stack,268435456
                                     -Wl,--disable-dynamicbase
                                     -Wl,--disable-high-entropy-va)
endif()
