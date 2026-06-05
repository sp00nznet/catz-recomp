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

## Status: Multi-Module Host+Engine Runs Through Window Creation ✅

**Both modules are recompiled and cross-linked.** `CATZ.WAD` (the host EXE) and
`CATZDLL.DLL` (the engine) build into one program that runs the **full Win16
application startup**: CATZDLL `LibMain` init, then the CATZ.WAD host's WinMain —
`InitTask → InitApp → SetMessageQueue → CTL3D → RegisterClass → CreateWindow →
GetSystemMenu → AppendMenu → GetWindowRect` — i.e. it **creates its window** and
proceeds into engine init. It currently stops at an "Abnormal Program
Termination" (Borland abort) once it reads config/resources the stub shims
return empty for (`LoadString`, `GetPrivateProfileString`) — the threshold where
real resource/config shims + a WndProc bridge + render path are needed.

How the modules are combined:
- CATZ.WAD segments are renumbered 60–66 (CATZDLL keeps 1–59); both share one
  flat image (`gen_image_combined.py`).
- CATZ.WAD's CATZDLL imports are resolved by ordinal → CATZDLL export entry →
  lifted function (direct C call), so cross-module calls need no thunks.
- KERNEL/USER/GDI/CTL3DV2/COMMDLG imports → the shared Win16 shims (IDA-named).
- `main.c` runs CATZDLL `LibMain` (DLL DGROUP) then the CATZ.WAD entry
  (host DGROUP/stack).

Correctness fixes that got the engine this far:

1. **386 instruction support** — `0x66` operand-size + `0x0F` two-byte opcodes
   in the shared decoder/lifter (10,210 + 3,429 sites, **0 residual `db 0x66`**).
   The engine is pervasively 386-compiled (eax–edi, movsx/movzx, setcc, etc.).
2. **`les`/`lds` far-pointer lifting bug** — when the destination register was
   also part of the address (`les di,[di]`, the linked-list-walk idiom), the
   lifter clobbered the register before reading the segment word, silently
   breaking every far-pointer traversal. This caused the heap arena's free-list
   to always read empty → infinite 4 KB arena allocation → `_setargv` OOM. Fixed
   by reading both words into temps before writing. (The "huge-pointer model"
   was a misdiagnosis.)

### Module architecture (mapped)

```
CATZ.EXE  (CATZLOAD, 30 KB)  dependency-check launcher → WinExec("CATZ.WAD …")
CATZ.WAD  (CATZ, NE EXE, 71 KB)  the real host — WinMain, window, message loop;
                                 imports CATZDLL (670 refs/94 ord), USER (362/81),
                                 GDI, COMMDLG, CTL3DV2
CATZDLL.DLL  (615 KB)  the engine — sprites, rendering, 1,263 XApt/CatSprite exports
```

Next: implement real resource/config shims (`LoadString`/`LoadResource` from the
WAD resource table, profile strings), back USER/GDI with real Win32/GDI32 + a
WndProc callback bridge (runtime invokes lifted window procs), and drive the
message loop → engine render (WinG/DIB) → first frame.

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

1. **Drive the engine via its exports** — a CATZ.EXE-equivalent host that calls
   the exported XApt entry points (1,263 exports) to create the playscene/pet,
   plus a USER message loop + window-proc dispatch.
2. **GDI/WinG render → first frame**: GetDC/BeginPaint, CreateDIBitmap/BitBlt/
   StretchDIBits, palette, WinG fast blit → SDL2/GDI32.
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
