# CATZ Static Recompilation

Static recompilation of **Catz** (1996, PF Magic) from its 16-bit NE engine DLL
to native C for Windows 11. The engine — the famous "ball-and-stick" Petz
engine — lives entirely in `CATZDLL.DLL`; `CATZ.EXE` is a thin launcher and
`CATZ.WAD` is the host shell. This project lifts both NE modules to C and
reimplements their Win16 API dependencies on top of Win32/GDI32.

![Catz running as recompiled native code](docs/catz-playpen.gif)

*Recompiled Catz: a Persian kitten batting a cat dancer around the playpen, with
the toy shelf, its panel chrome and its menus all drawn by the original engine.*

Built on the [pcrecomp](https://github.com/sp00nznet/pcrecomp) NE toolchain,
following [elfish](https://github.com/sp00nznet/elfish) (the proven end-to-end
NE→C recomp) as the structural template. The difference: El-Fish was a
self-contained DOS-extender; CATZ is a real Windows 3.1 application, so the
runtime is a **Win16 API shim layer** rather than DOS interrupt handlers.

## Status

The game is playable. `build/catz.exe` boots CATZDLL + CATZ.WAD, opens a real
Win32 window, and runs the engine's own frame loop against the original assets.

Working:

- **The playpen renders** — tiled wallpaper, toy shelf, and the pet drawn by the
  engine's ball rasteriser from the real `.lnz`/`.bdt` geometry, at the ~18 fps
  the engine was written for.
- **The pet behaves** — the full state machine: idling, exploring, locomoting,
  stalking, swatting, eating, suckling, sleeping, plus the mouse in its hole.
- **Toys work** — drag any of them off the shelf; the cat plays with them, and
  Clean Up Toys puts them back.
- **Adoption works** end to end: Adopt Me Now, pick a breed from the five
  portraits, name the cat, and it joins the playpen.
- **The shelf panel is real** — caption bar, system menu, and the Options and
  Help menus, all drawn and hit-tested by the engine.
- **Saving works**, so the game keeps its settings and your cat between runs.
- **Sound works** — the engine's own effects, played from the game's `SOUNDS/`
  tree.

Not working yet:

- The four Options entries that open dialogs (General Options, CatNapz Options,
  Choose Another Kitten, Create Adoption Kit) highlight but never act.
  Everything else on the menu works.
- The engine logs `sqrt: DOMAIN error` in some sessions; harmless so far, but it
  means something is handing `sqrt` a negative.

Nine Win16 imports the engine actually reaches are still stubs; run with
the runtime prints each one (`[win16] unimplemented: ...`) the first time it is reached. See the [roadmap](docs/roadmap.md) for
what is planned and what the evidence for each item is.

## Building

The lifted C is **not** in this repository. It is a machine translation of the
retail game's own code, so it is not redistributable — you generate it from your
own copy, the same way you supply the game data.

The toolchain and the Win16 runtime live in
[pcrecomp](https://github.com/sp00nznet/pcrecomp) (`tools/ne/`,
`runtime/win16/`); this repository holds only what is Catz's own. Check pcrecomp
out next to this repository as `tools/`, or point `PCRECOMP_DIR` at it. Then,
with the game binaries in `game/`:

```bash
python tools/relift.py --ida        # IDA code maps, lift, image, glue, EH guard
cmake -B build -G Ninja             # -DCATZ_DATA_DIR=<your install> if not ../catzng/catz
cmake --build build
```

`--ida` exports IDA 9's code maps into `work/ida/` (needs idalib under Python
3.11); once they exist, plain `python tools/relift.py` re-lifts. Everything it
writes goes to `work/`, which is gitignored. `tools/entry_seeds.json` lists the
entry points found by running the game that no code map knows about; add to it
with pcrecomp's `tools/ne/seed_misses.py` from a run log.

Compiler is MSYS2 mingw64 gcc — `PATH` must include `C:/msys64/mingw64/bin`.
**`-O2` is required for correctness**: the lifter turns intra-segment jumps into
tail calls, and at `-O0` guest loops become host recursion and blow the stack.
It is pinned in `CMakeLists.txt`.

## You must supply your own game files

Nothing from the retail game is redistributable, so `game/` and the data tree
are gitignored and you bring your own. Point `CATZ_DATA_DIR` (a CMake
cache variable) at a real install — the directory containing `PTZFILES`,
`PLAYPENZ` and `SOUNDS`.

### …including your own serial number

Catz asks for a serial number on first run, and this project ships neither one
nor any way around one. It is licence data belonging to your copy of the game.
Put it where the game already looks — `catz.ini` in the install root:

```ini
[Catz]
Serial Number=XXXX-XXXX-XXXX
```

Without it the engine stops on its registration screen and the playpen is never
reached. The runtime says so plainly on startup rather than leaving you to work
it out from the wizard.

Note that the game now **writes** to that install: it saves settings, the date
and your adopted cat back to its `.cat` file, exactly as the original did. Keep
a copy of anything you care about.

## Layout

```
tools/      relift.py (the whole lift in one command), entry_seeds.json
runtime/    game.c (the facts about Catz the shared runtime needs), win_ini.ini
analysis/   win16_imports.json (Win16 ordinal names)
docs/       architecture, the Win16 layer, diagnostics, defect writeups
work/       lifted C, IDA maps, memory image, generated headers (gitignored)
game/       original binaries (gitignored, not redistributable)
```

## Licence, and what it does not cover

The code in this repository — its tools, its runtime configuration and the
documentation — is MIT licensed, as is pcrecomp, which it builds on. See
[LICENSE](LICENSE).

That covers this project's own work and nothing else. **Catz is not included and
is not licensed here.** The game binaries, its data files, its artwork and
sounds, and your serial number all belong to your own copy and stay there:
`game/` and the data tree are gitignored, and so is `work/`, which holds the
lifted C -- a machine translation of the game's own code -- and everything else
derived from the binaries. Catz is
© 1996 PF Magic. This is an interoperability and preservation exercise; you need
a copy you already own for any of it to run.

## Documentation

| Page | What is in it |
|------|---------------|
| [Roadmap](docs/roadmap.md) | What is left, ordered by what a player would notice |
| [Architecture](docs/architecture.md) | Module layout, the lift pipeline and its results, where the render actually lives, build notes that are easy to lose |
| [The Win16 layer](docs/win16-layer.md) | How the shims work, and the recurring shapes of bug — a stub returning 0 is not neutral, the PASCAL purge has to be exact, a WinG DC is not a host DC |
| [Diagnostics](docs/diagnostics.md) | Every in-tree instrument, including how to drive the whole UI headlessly without touching the mouse |
| [Lifter defects](docs/lifter-bugs.md) | The silent ones: segment overrides, indirect jumps, stack leaks, five x87 faults |
| [Investigations](docs/investigations.md) | Traces kept for the method rather than the conclusion |
