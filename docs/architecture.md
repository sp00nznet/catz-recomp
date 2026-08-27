# Architecture

How CATZ is put together, and how the recompilation mirrors it.

## Module architecture

```
CATZ.EXE  (CATZLOAD, 30 KB)  dependency-check launcher → WinExec("CATZ.WAD …")
CATZ.WAD  (CATZ, NE EXE, 71 KB)  the real host — WinMain, window, message loop;
                                 imports CATZDLL (670 refs/94 ord), USER (362/81),
                                 GDI, COMMDLG, CTL3DV2
CATZDLL.DLL  (615 KB)  the engine — sprites, rendering, 1,263 XApt/CatSprite exports
```

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

### Imported Win16 modules

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

## Pipeline results

| Stage | Result |
|-------|--------|
| Recon | 50 code segments, 233,762 instructions, 17,423 relocs, 1 cluster |
| IDA code map | 1,497 real functions, 191,187 verified code heads |
| Imports | 162 ordinals across 8 modules, all named by IDA |
| Lift | all 50 segments → 285K lines C; **0 unresolved far calls** |
| Glue | flat image (705 KB, 14,495 relocs applied); **0 unresolved stubs** |
| Build | `catz.exe` (12.6 MB), 0 errors / 0 warnings, link-clean |

## 386 instruction support (shared toolbox)

`0x66` operand-size and `0x0F` two-byte opcodes are implemented in
`pcrecomp/tools/disasm/decode16.py` + `lift/lift16.py` (REG32/IMM32 operand
types, 32-bit `_read`/`_write`, `flags_*32`/`push32`/`pop32` in `cpu.h`, plus
movsx/movzx/setcc/2-op-imul/bt/string-`d`). Backward-compatible: without a
`0x66` prefix decode is byte-identical, so elfish is unaffected (re-lift +
glue-regen needed only if elfish is rebuilt; its `cpu.h` already updated).

## Where the render lives

Tracing the per-frame path (the call-ring tail under `-DCATZ_WATCHDOG`) settled
this: it is **not** in the host shell's WNDPROC. CATZ.WAD's WNDPROC only drains a
command queue (`ds:[0x404]`, always 0) and re-posts `WM_CATZ_WINTERFACE`; its
WM_PAINT is `BeginPaint`/`EndPaint` with nothing between. The playpen is a
*dialog*: `DialogBoxParam(..., dlgproc=seg62:0A11)` creates the real game window,
and the engine's own `SetTimer`, WM_TIMER tick and WM_PAINT -- the one that blits
the WinG surface -- live in that DLGPROC.

## Build notes that are easy to lose

- **`-O2` is required for correctness.** The lifter turns intra-segment jumps
  into tail calls; at `-O0` guest loops become host recursion and blow the
  stack. Pinned in `CMakeLists.txt`.
- `CATZ_DATA_DIR` must point at a real game install (`../catzng/catz`, the tree
  containing `PTZFILES`).
- Compiler is MSYS2 mingw64 gcc — `PATH` must include `C:/msys64/mingw64/bin`.
- After any re-lift: `lift_dll.py`, `lift_wad.py`, **`patch_lifted.py`**, then
  `gen_segments_h.py`, `gen_dispatch.py`, `gen_stubs.py`. Skipping
  `patch_lifted.py` silently drops the EH cleanup-chain guards.
