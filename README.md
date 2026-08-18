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

## Status (2026-08): constructs the pet from its real assets; dies in sqrt

Current run: image load → CATZDLL `LibMain` → CATZ.WAD `WinMain` → real Win32
window + WndProc bridge → CATZREZX (327 resources) → reads its whole
configuration (`LASTCAT.INI → catz0.cat`) → loads the playpen bitmap → **loads
the pet**: `Apersian.lnz`, `kpersian.lnz` (breed files), `cat.bhd` (animation
index), `cat.scp` (behaviour script), `cat0.bdt` (animation frames) → then
`MessageBox "sqrt: DOMAIN error"`. Reproduce with `build/catz.exe`.

**Not yet rendering and not yet pumping messages.** `WinGStretchBlt` is now a
real `StretchDIBits` from the WinG surface, but it has never executed — init
still dies first, just much later.

### Next: the `sqrt` domain error (measured, not yet fixed)

`seg036_3C86` computes a vector length as `sqrt(x*x + y*y + z*z)`: three
`movsx`/`imul` pairs into `eax`, stored to `[bp-0x60]`, then `fild dword` →
`fstp qword` → the RTL `sqrt` (`seg001_134A`). Reached via
`seg063_1439 → seg034_1327 → seg016_1291 → seg047_2CFC → seg047_26ED →
seg047_44C9 → seg047_3C8E → seg036_01BB → seg036_3C86`.

Instrumenting the operand gives:

```
[SUMSQ] x=-18385 y=-17153 z=-7773  sum=692653163
[SUMSQ] x=-17754 y=-21399 z=30464  sum=1701177013
...
[SUMSQ] x=32361  y=-23773 z=-32333 sum=-1637154557   <- int32 overflow
```

The overflow is the symptom: 32361² + 23773² + 32333² = 2,657,812,739, which
does not fit in `int32`, so `fild` reads it back negative. **The bug is the
inputs.** These are ball coordinates for a cat skeleton — they should be tens to
hundreds, not the full ±32767 range. Every sample is garbage-scaled, including
the ones that happen not to overflow, so this is not an edge case reached after
good frames.

`x`/`y`/`z` (`[bp-0x36]`, `[bp-0x34]`, `[bp-0x32]`) are built in the same
function by 16-bit `imul`s over `[bp-0x30]`, `[bp-0x2C]`, `[bp-0x2E]` and
`[bp-0x26]`, `[bp-0x28]`, `[bp-0x2A]` — a fixed-point rotation applied to a
position. So chase the source of those two operand groups: the run has just
opened `cat0.bdt`, so mis-read BDT frame data is the leading candidate, ahead of
a mis-lifted x87/386 op in the transform (`shld`/`shrd`/`rcl`/`rcr`/`sahf` are
decoded but still TODO).

<details><summary>Resolved: the "Abnormal program termination" abort (2026-08)</summary>

The abort had one root cause, three lifter defects deep. The chain, from the
bottom up:

1. **The lifter pushed a literal `0` as every call's return address.** Free
   until guest code reads it back.
2. **Borland's `_vprinter` reads it back.** `seg001_291B` is `call $+3` — the
   "push IP" idiom — and `seg001_2AFC` later does `pop cx; add cx,3; jmp cx` to
   resume past it. With `0` pushed, that computed `jmp` targeted `seg1:0003`.
3. **`dispatch_near` missed**, and its miss path pops a frame the guest still
   owns, so `_vprinter` (`seg001_269C`) returned mid-body with its `0x2A`-byte
   frame and saved `si/di/es` still on the stack: **44 bytes of guest stack
   leaked per formatted string**.
4. The 64 KB guest stack drained, SP wrapped past `0`, and saved-`DS` slots read
   back as `0`.
5. A null `DS` makes `push ds; push 0x13F0` produce `0000:13F0`, so
   `GetPrivateProfileString` got a null `lpAppName`/`lpFileName` — which in
   Win16 means "return the section list", i.e. the literal string `"Catz"`.
6. The engine opened a file named `Catz`, it did not exist, it threw, and no
   handler was found.

The earlier "Negative result: the null-DS config reads are NOT the abort's
cause" commit was **wrong**, and instructively so: that experiment substituted
the section pointer only, leaving the *filename* pointer null, so the read still
returned garbage and the abort still fired. Half a fix measured as no fix.

Fixes, all in the shared toolbox:

- `ne_lift.py` pushes the **real return offset** (`local_off + inst.length`) for
  near, far, and indirect calls instead of `0`.
- `ne_lift.py` treats `call $+3` as **push-only** — emitting a C call there ran
  the rest of the function twice (once nested, once via the fall-through) and
  popped a frame that no longer existed.
- `ne_decode.py` seeds a basic-block start at **the instruction after any
  unconditional transfer** (`jmp`/`ret`/`retf`/`iret`). Nothing falls into such
  an address, so it is either dead or a computed-jump target — `0x2921` here had
  no label at all, which is why the dispatcher could not have resolved it even
  with the right address.

Measurement that found it: `tools/bpguard.py` (now also checking SP balance)
plus the `[SP-DROP]` detector in `catz_sp_check`. `seg001_269C returned sp
FA8E->FA62` was the first violation in the run; every later one was downstream
damage.

</details>

### Build notes that are easy to lose

- **`-O2` is required for correctness.** The lifter turns intra-segment jumps
  into tail calls; at `-O0` guest loops become host recursion and blow the
  stack. Pinned in `CMakeLists.txt`.
- `CATZ_DATA_DIR` must point at a real game install (`../catzng/catz`, the tree
  containing `PTZFILES`).
- Compiler is MSYS2 mingw64 gcc — `PATH` must include `C:/msys64/mingw64/bin`.
- After any re-lift: `lift_dll.py`, `lift_wad.py`, **`patch_lifted.py`**, then
  `gen_segments_h.py`, `gen_dispatch.py`, `gen_stubs.py`. Skipping
  `patch_lifted.py` silently drops the EH cleanup-chain guards.

### Diagnostics (all in-tree)

| Tool | What it answers |
|------|-----------------|
| `dump_guest_stack()` | The guest call stack — the guest stack can't be walked (the lifter pushes only the return *offset*, not a walkable frame), so this captures the **host** stack and prints file VAs for `addr2line -f -e build/catz.exe`. ASLR is off so addresses paste straight in. |
| `tools/bpguard.py` | Instruments all ~10k call sites and reports any callee that fails to preserve **BP or DS**, or that returns with **SP** below the pre-call value. Found every systemic lifter bug so far. Revert with `git checkout src/`. |
| `[SP-DROP]` / `[SP-LOW]` (in `-DCATZ_WATCH_SP`) | Single-step guest-SP plunges and low-water floors, each with the last 40 guest labels. A leak shows up here long before it corrupts anything visible. |
| `[NULL-SEL]` (always on) | A profile call whose app/file pointer arrives as `0000:<offset>` — a far pointer built from a null DS — with the caller's stack. |
| `CATZ_WATCH=seg:off` + `-DCATZ_WATCH_MEM` | Guest write watchpoint with host backtrace; can be gated to one frame's lifetime (stack slots alias across frames, which produces false leads otherwise). |
| `-DCATZ_WATCH_SP` | Guest stack low-water marks; also tracks where each run of `ds==0` begins. |
| `-DCATZ_TRACE_WIN16` | Every shim call, including all file I/O and the engine's own `OutputDebugString` log. |
| `-DELFISH_TRACE_RUNTIME` | Dispatcher misses — an unlifted indirect target shows up here. |

### Lifter bugs fixed (these were silent)

The two that mattered most were invisible until measured, and each corrupted
the engine globally:

- **Segment overrides dropped on `lods`/`movs`/`cmps`.** The decoder recorded
  the prefix; the lifter hardcoded `cpu->ds`. Borland's `_vprinter` scans its
  format string with `es: lodsb`, so *every formatted string in the engine* was
  read from the wrong segment.
- **Register-indirect `jmp`/`call` dropped entirely.** `jmp cx` became a bare
  comment, so the C function fell off its end and returned. In `_vprinter` that
  is the `pop cx; add cx,3; jmp cx` idiom — the function returned straight
  after its prologue with its frame still allocated, corrupting BP, then DS,
  then every far pointer built as `push ds`.
- **Literal-`0` return addresses, `call $+3` lifted as a real call, and no label
  after an unconditional transfer.** Together these leaked 44 bytes of guest
  stack per formatted string until the 64 KB stack wrapped. See the resolved
  abort above — this was the one that had to be fixed to load a pet.

Also: jump-table targets and `OFFSET16` catch-handler relocations are now
lifted (a missing catch handler meant *no* exception could ever be caught), the
BCD adjust instructions (`aam`/`aad`/`daa`/`das`) are implemented rather than
dropped, and `TOOLHELP.GlobalEntryHandle` is real — as a stub returning FALSE
it made the engine report 59 phantom "you've stomped on memory" errors about a
heap that was fine.

<details><summary>Earlier milestone: multi-module host+engine through window creation</summary>

### Multi-Module Host+Engine Runs Through Window Creation ✅

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

</details>

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
