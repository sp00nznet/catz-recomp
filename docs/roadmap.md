# Roadmap

Ordered by what a player would notice, not by what is interesting to write.
Each item says how it was found, because the evidence is usually the useful part.

Status of the whole: the playpen is playable — the pet renders and behaves, toys
work, adoption works end to end, the shelf panel and its menus are real, and the
game saves. What follows is what is left.

## Next

**1. Sound.** The engine already asks for every effect and falls back to
`sndPlaySound` — its own log says *"Playing sound via boring windows sounds"* —
and `MMSYSTEM.SNDPLAYSOUND` is still a stub returning 0. The 236 files in
`SOUNDS/` are RIFF/WAV despite the `.WVV` extension on most of them, so
`PlaySound` can take them as they are once the path goes through the same
resolver the file I/O uses. This is the cheapest large win left.

**2. The four Options entries that open dialogs.** General Options, CatNapz
Options, Choose Another Kitten and Create Adoption Kit highlight, log their
selection, and then unwind. Instrumenting every entry into `DialogBoxParam`,
`CreateDialog` and `CreateDialogParam` shows the click reaches **none** of them,
so the engine is deciding not to open the dialog somewhere upstream of any Win16
call. Everything else on the menu works, including Clean Up Toys, which was
verified by dragging a toy out and watching it return to the shelf.

**3. `CheckMenuItem`.** A stub, so the Options toggles never show their state —
you cannot see which way a setting is set.

**4. `SetWindowText`.** A stub, so the pet's name never reaches the window
caption.

## Features the engine has and we do not expose

**5. Desktop mode.** Catz can run loose on the desktop rather than in a window;
the runtime has a `CATZ_DESKTOP` path already. The stubs behind it are
`GetDesktopWindow`, `SetWindowPos` and `EnumWindows`.

**6. The camera.** The shelf panel has a camera button and the save file has an
`autoSavePhotos` key, so the photo feature is present and unexercised.

**7. CatNapz.** The screensaver mode, with its own Options entry and an
"Activate CatNapz Now" command.

**8. Real SOS audio.** 37 `SOSLIB03` ordinals are imported; two are reached.
Only worth doing if the `sndPlaySound` path in (1) proves too thin — the SOS
driver layer buys stereo and mixing that the simple path does not.

## Correctness debt

**9. `sqrt: DOMAIN error`.** The engine's own RTL reports it, a few hundred
times in some sessions and not at all in others, so something is handing `sqrt`
a negative. Harmless so far, but it is a real numerical fault and the kind that
usually turns out to matter.

**10. Z-order.** The pet is composited over the shelf panel's menus — visible in
any capture with a menu open. The panel should be above the playpen.

**11. Remaining lifted instructions.** Decoded but still emitted as comments:
`shrd` (144 sites), `rcl` (111), `sahf` (68), `shld` (66), `rcr` (9). None have
produced an observed fault yet, which only means nothing has taken those paths.

**12. `catz_unreachable` stubs.** 316 branch targets landed inside another
instruction and were given abort stubs rather than bodies. Each one is a decode
boundary that may or may not be reachable.

**13. `BitBlt`.** Reached and still a stub. Every raster op the engine issues is
`SRCCOPY`, so this is likely mechanical.

## Structural

**14. `CATZ.EXE` is not used.** The real launcher does a dependency check and
then `WinExec`s `CATZ.WAD`; the recomp starts at the WAD directly. Wiring it up
would exercise the launcher's own paths.

**15. More than one pet.** The save format has `Cat1`/`Age1` numbering, so the
engine expects a family. Only one is ever loaded.

**16. Frame pacing.** `SetTimer` now rounds to the Win16 18.2 Hz tick, which
matches the ~18 fps the engine was written for. Worth measuring against real
hardware behaviour rather than trusting the arithmetic.
