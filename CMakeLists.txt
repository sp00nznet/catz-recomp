cmake_minimum_required(VERSION 3.16)
project(catz_recomp C)

set(CMAKE_C_STANDARD 11)

# pcrecomp supplies the Win16 host runtime (runtime/win16) and the tools that
# generate work/. Default: the checkout next to this one.
set(PCRECOMP_DIR "${CMAKE_SOURCE_DIR}/../tools" CACHE PATH "pcrecomp checkout")
set(WIN16_RT "${PCRECOMP_DIR}/runtime/win16")
# Your own install of the game: the tree with PTZFILES, PLAYPENZ and SOUNDS.
set(CATZ_DATA_DIR "${CMAKE_SOURCE_DIR}/../catzng/catz" CACHE PATH "Catz install")

# NOT an optimization preference — a correctness requirement. The lifter turns
# every intra-segment jump into `target(cpu); return;`, so a guest loop spanning
# lifted-function boundaries is host recursion. Only sibling-call optimization
# (-O2) turns it back into a loop; at -O0 the palette inverse-table build
# (4096*256 iterations) blows any host stack.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_C_FLAGS MATCHES "-O")
    # -g1: function names + line tables only, so addr2line can symbolize the
    # host backtrace that stands in for the guest call stack.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -g1")
endif()

# Lifted sources, generated into work/ by tools/relift.py. Never committed.
set(WORK_DIR "${CMAKE_SOURCE_DIR}/work" CACHE PATH "generated tree")
file(GLOB SEGMENT_SOURCES CONFIGURE_DEPENDS "${WORK_DIR}/src/*.c")
if(NOT SEGMENT_SOURCES)
    message(FATAL_ERROR "No lifted sources in ${WORK_DIR}/src. Run: python tools/relift.py --ida")
endif()

include_directories("${WORK_DIR}/runtime" "${WIN16_RT}" "${WIN16_RT}/host")

add_library(catz_segments STATIC ${SEGMENT_SOURCES})
target_compile_options(catz_segments PRIVATE
    -Wall
    -Wno-unused-variable
    -Wno-unused-label
    -Wno-unused-but-set-variable
    -Wno-unused-function
)

# Win16 API shims (KERNEL/USER/GDI/SOSLIB03/WING -> Win32), from pcrecomp,
# plus the handful of facts about Catz they need (runtime/game.c).
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS "${WIN16_RT}/host/*.c")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/main\\.c$")
add_library(catz_runtime STATIC ${RUNTIME_SOURCES} runtime/game.c)
target_compile_definitions(catz_runtime PRIVATE
    RECOMP_GAME_DIR="${CMAKE_SOURCE_DIR}/game"
    RECOMP_DATA_DIR="${CATZ_DATA_DIR}"
    RECOMP_WIN_INI="${CMAKE_SOURCE_DIR}/runtime/win_ini.ini")

add_executable(catz "${WIN16_RT}/host/main.c")
target_compile_definitions(catz PRIVATE
    RECOMP_IMAGE_PATH="${WORK_DIR}/mem_image.bin")
target_link_libraries(catz catz_segments catz_runtime catz_segments m user32 gdi32 winmm)
# Lifted frames are far fatter than the 16-bit originals; the engine's C++
# construction recurses deep enough to blow the 1 MB default host stack.
# No ASLR: runtime addresses == file VAs, so backtrace output can be pasted
# straight into addr2line.
target_link_options(catz PRIVATE -Wl,--stack,268435456
                                 -Wl,--disable-dynamicbase
                                 -Wl,--disable-high-entropy-va)
