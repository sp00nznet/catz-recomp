# CATZ Static Recompilation

Static recompilation of **Catz** (1996, PF Magic) from its 16-bit NE engine DLL
to native C for Windows 11. The engine — the famous "ball-and-stick" Petz
engine — lives entirely in `CATZDLL.DLL`; `CATZ.EXE` is just a thin window
shell. This project lifts the DLL to C and reimplements its Win16 API
dependencies on top of Win32/SDL2.

Built on the [pcrecomp](https://github.com/sp00nznet/pcrecomp) NE toolchain,
following the [elfish](https://github.com/sp00nznet/elfish) project (the proven
end-to-end NE→C recomp) as the structural template. The key difference: El-Fish
was a self-contained DOS-extender (TSXLIB); CATZ is a real Windows 3.1 app, so
the runtime is a **Win16 API shim layer** rather than DOS interrupt handlers.

## Target: CATZDLL.DLL

| Metric | Value |
|--------|-------|
| Format | NE (16-bit segmented), Win16 DLL, PROTMODE |
| Compiler | Borland C++ (linker 6.1), XApt C++ engine class |
| Code segments | 50 (614,898 bytes) |
| Data segments | 9 |
| Instructions | 233,762 |
| Relocations | 17,423 |
| Exports | 1,263 (XApt class methods, Borland-mangled) |
| Cluster | All 50 segments are one connected component |

### Imported Win16 modules (the shim TODO list)

| Module | Refs | Unique | Recomp strategy |
|--------|------|--------|-----------------|
| KERNEL   | 468 | 32 | GlobalAlloc/Lock/Free → heap; file I/O; thunks |
| GDI      | 235 | 32 | DC/bitmap/palette/BitBlt → GDI32 or SDL2 |
| USER     | 200 | 51 | CreateWindow/messages/input → Win32 / SDL2 |
| SOSLIB03 | 48  | 37 | SOS audio (HMI) → SDL2_mixer |
| WIN87EM  | 6   | 1  | x87 emulation → native doubles |
| TOOLHELP | 5   | 2  | debug helpers → stub |
| WING     | 5   | 4  | WinG fast blit → SDL2 / DIB |
| MMSYSTEM | 4   | 3  | multimedia timer |

The single hottest import is **KERNEL @113 (250×)** — a Borland far-pointer
thunk to be identified via IDA.

## Pipeline

```
ne_parse   →  segments, relocations, imports, entry/export tables
ida_export →  authoritative function bounds + instruction heads + Win16
              ordinal→API names  (IDA Pro 9.1 headless / idalib)
ne_decode  →  IDA-assisted 16-bit disassembly (relocations resolved inline)
ne_lift    →  one C file per code segment; x87 → native doubles;
              far calls/imports resolved via relocations
gen_image      →  flat memory image of data segments + applied relocations
gen_segments_h →  cross-segment prototypes (link-clean)
gen_dispatch   →  indirect call/jmp dispatcher
gen_stubs      →  stubs for unresolved targets
runtime/       →  Win16 API shims (KERNEL/USER/GDI/SOS/WING) + CATZ.EXE host loop
```

## Layout

```
tools/      NE toolchain (adapted from elfish; win16.py replaces tsxlib.py)
runtime/    cpu.h (CPU+FPU model), Win16 shims, main host loop
src/        lifted C, one seg*.c per code segment (generated)
analysis/   recon outputs + IDA exports (win16_imports.json, ida_funcs.json)
build_data/ flat memory image (generated, gitignored)
game/       original binaries (gitignored, not redistributable)
```

## Building

```bash
cmake -B build -A Win32       # 16-bit origin → 32-bit native target
cmake --build build
```

## Status: 386 Decode Complete; Blocked On Borland Far-Heap Model

The full lifted engine **compiles (0 errors, 0 warnings), links with zero
undefined symbols, and executes** the NE entry (`seg001_0000`) through the
init-path Win16 shims into the **Borland C++ RTL far-heap initialization**.

The **`0x66` 32-bit operand-size prefix and `0x0F` two-byte opcodes are now
fully implemented** in the shared decoder + lifter (10,210 `0x66` + 3,429 `0x0F`
sites — **0 residual `db 0x66`**, 0 unhandled). The engine's pervasive 386
codegen (push/pop/mov/add/sub/shift/imul/cdq/cwde on `eax`–`edi`, movsx/movzx,
setcc, jcc-near) lifts to correct 32-bit C.

It now halts at `MessageBox("Out of memory in _setargv")` for a **different,
deeper reason**: `_setargv`'s heap-init loop `GlobalAlloc`s 4 KB moveable blocks
until failure, but the Borland far-heap manager does not recognize the blocks as
usable (even 6145 blocks = 24 MB reads as "not enough"). The arena linkage
relies on **huge-pointer (`__AHINCR`/`__AHSHIFT`) selector arithmetic** — the
next bring-up blocker (a memory-model problem, not a decode gap).

### Pipeline results

| Stage | Result |
|-------|--------|
| Recon | 50 code segments, 233,762 instructions, 17,423 relocs, 1 cluster |
| IDA code map | 1,497 real functions, 191,187 verified code heads |
| Imports | 162 ordinals across 8 modules, all named by IDA |
| Lift | all 50 segments → 285K lines C; **0 unresolved far calls** |
| Glue | flat image (705 KB, 14,495 relocs applied); **0 unresolved stubs** |
| Build | `catz.exe` (12.6 MB), 0 errors / 0 warnings, link-clean |

### Remaining work (prioritized)

1. **Borland far-heap / huge-pointer model** — **THE blocker.** Implement the
   `__AHINCR`/`__AHSHIFT` huge-pointer model so consecutive selectors map to
   consecutive 64 KB flat regions and the Borland far-heap arena links/walks
   correctly; or RE the exact arena format the RTL expects. Unblocks `_setargv`.
2. **USER message loop + GDI/WinG blit** → first rendered frame: GetDC/
   BeginPaint, CreateDIBitmap/BitBlt/StretchDIBits, palette, WinG → SDL2/GDI32.
3. SOS audio + remaining Win16 shims as reached (purge bytes in `tools/win16.py`;
   stubs warn loudly on unknown purge).
4. Minor lifter ops: `shld`/`shrd` (201, decoded but TODO), `rcl`/`rcr`, `sahf`.

### Done: 386 instruction support (shared toolbox)

`0x66` operand-size and `0x0F` two-byte opcodes are implemented in
`pcrecomp/tools/disasm/decode16.py` + `lift/lift16.py` (REG32/IMM32 operand
types, 32-bit `_read`/`_write`, `flags_*32`/`push32`/`pop32` in `cpu.h`, plus
movsx/movzx/setcc/2-op-imul/bt/string-`d`). Backward-compatible: without a
`0x66` prefix decode is byte-identical, so elfish is unaffected (re-lift +
glue-regen needed only if elfish is rebuilt; its `cpu.h` already updated).

### Implemented Win16 shims (`runtime/win16/win16_impl.c`)

`GlobalAlloc/Free/ReAlloc/Lock/Unlock/Size/Handle` (free-list allocator over the
flat heap; handle == selector), `LocalInit`, `GetWinFlags/GetVersion/
GetCurrentTask/GetModuleUsage/GetModuleFileName`, `MessageBox/MessageBeep/
EnumTaskWindows/GetTickCount`, and `INT 21h` (exit, version). All other imports
are generated stubs with correct PASCAL stack purge (`tools/gen_win16_stubs.py`
+ the `PURGE` table in `tools/win16.py`).

See `analysis/` for the import surface, segment clusters, and decode summary.
