# Lifter and decoder defects

Every one of these was silent: the build stayed clean, the engine kept running,
and something far away came out wrong. They are kept here because the shape of
each mistake is more useful than the fix -- most were found by measuring
something that should have been invariant, not by reading code.

## Return addresses, idiv, relocated immediates, resource tags

Four defects, found by measurement rather than reading, in the order they
blocked progress:

1. **Return addresses were pushed as a literal `0`.** Free until guest code reads
   one back -- Borland's `_vprinter` does, via `call $+3` then
   `pop cx; add cx,3; jmp cx`. The computed jump went to `seg1:0003`,
   `dispatch_near` missed, and its miss path popped a frame the guest still
   owned: **44 bytes of guest stack leaked per formatted string** until the 64 KB
   stack wrapped past 0 and saved-`DS` slots read back as 0. Fixed by pushing the
   real return offset, by treating `call $+3` as push-only (a C call there ran
   the rest of the function twice), and by seeding a basic-block start after
   every unconditional transfer so the computed target had a label at all.
2. **`idiv r/m32` was lifted as the 16-bit form** -- dividend from `DX:AX`,
   divisor truncated to `int16`. `cdq; idiv dword` is the engine's standard
   fixed-point scale-then-divide, so its geometry was wrong everywhere: **172
   sites**, concentrated in seg036 (51), seg044 (25), seg039 (22), seg047 (21).
3. **Relocated immediates were resolved for `mov` but not `push`**, so every far
   pointer built as `push <selector>; push <offset>` carried the raw 0xFFFF
   placeholder and aliased into the guard region. **139 sites.** This is what
   made the WinG BITMAPINFO read back as ASCII.
4. Petz resource tags needed an exact `(tag,id)` record to resolve, but those
   records only cover the ids the data segment mentions literally -- `STBL/1000`
   exists as type 0x7F04 and had none, so `FindResource` returned 0 and the
   engine threw.

## Segment overrides, indirect jumps, stack leaks

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

## x87 defects (found together, fixed together)

Five, each masking the next, all in the FPU path:

- FWAIT-prefixed instructions were lifted twice (~3,800 sites), because the
  decoder emitted both the `FWAIT` and the bare ESC behind it.
- `fild`/`fistp qword` were lifted as the 16-bit form, so `ftol` never produced
  a correct `DX`.
- `fld`/`fstp tword` read 80-bit values as doubles: pi/4 came back as -8.9e43.
- `fprem` and `fxam` were unimplemented.
- `fpu_compare` clobbered `AH`.

With those fixed the trig tables came out exactly right and the ball rasteriser
went from 49 circles a run to 102,750.

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
