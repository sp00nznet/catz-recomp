# Roadmap

Ordered by what a player would notice, not by what is interesting to write.
Each item says how it was found, because the evidence is usually the useful part.

Status of the whole: the playpen is playable — the pet renders and behaves, toys
work, adoption works end to end, the shelf panel and its menus are real, and the
game saves. What follows is what is left.

## Next

**2. The four Options entries that open dialogs.** General Options, CatNapz
Options, Choose Another Kitten and Create Adoption Kit highlight, log their
selection, and then unwind without doing anything. This is the only known
broken feature.

What has been ruled out, so the same ground is not covered again:

- *Not the dialog layer.* Instrumenting every entry into `DialogBoxParam`,
  `CreateDialog` and `CreateDialogParam` shows the click reaches **none** of
  them.
- *Not a missing resource.* `FindResource` reports every miss now, and a session
  that clicks the item reports zero.
- *Not the menu mechanism.* Clean Up Toys was driven the same way and works —
  verified by dragging a toy out and watching it return to the shelf.
- *Not the selection path.* The guest call stack at the moment the item
  unselects is byte-identical between a working item and a broken one, so they
  diverge after that, inside the engine's own command handling.

So the engine decides not to act, upstream of any Win16 call. Next step is the
engine's command dispatch rather than the shim layer.

**3. Audio beyond the simple path.** `sndPlaySound` now plays every effect the
engine asks for, which covers nearly all of them. The 37 `SOSLIB03` ordinals
remain stubbed; they buy stereo and mixing the simple path does not, and are
only worth doing if the difference is audible.

## Features the engine has and we do not expose

**4. Desktop mode.** Catz can run loose on the desktop rather than in a window;
the runtime has a `CATZ_DESKTOP` path already. The stubs behind it are
`GetDesktopWindow`, `SetWindowPos` and `EnumWindows`.

**5. The camera.** The shelf panel has a camera button and the save file has an
`autoSavePhotos` key, so the photo feature is present and unexercised.

**6. CatNapz.** The screensaver mode, with its own Options entry and an
"Activate CatNapz Now" command.

## Correctness debt

**7. Reported black patches that do not reproduce here.** Areas of the playpen
going black, reported around the shelf and the window edges, and reliably from
clicking the cat dancer on the shelf without dragging it off.

One real cause was found and fixed: the frame carried a sizing border while the
engine renders a fixed 640x480, so any resize left client area the engine never
draws (a 900x700 window measured 48.7% black). That is gone.

Whether it was the whole story is unconfirmed. Ruled out by measurement since,
none of which reproduced anything: blit rects outside their surface (none), blits
GDI rejects (none), surfaces with a dead colour table (none), blits painting a
substantial area in the background index (none, down to a third of a large
blit), the cursor coordinate frame (correct), and clicking the cat dancer both
with scripted input and with a real mouse click (no change either way).

`CATZ_WATCH_BLACK=1` arms the detector; if it happens again the log names the
surface and both rectangles of the blit responsible.

**8. `sqrt: DOMAIN error`.** The engine's own RTL reports it, a few hundred
times in some sessions and not at all in others, so something is handing `sqrt`
a negative. Harmless so far, but it is a real numerical fault and the kind that
usually turns out to matter.

**9. Z-order.** The pet is composited over the shelf panel's menus — visible in
any capture with a menu open. The panel should be above the playpen.

**10. Remaining lifted instructions.** Decoded but still emitted as comments:
`shrd` (144 sites), `rcl` (111), `sahf` (68), `shld` (66), `rcr` (9). None have
produced an observed fault yet, which only means nothing has taken those paths.

**11. `catz_unreachable` stubs.** 316 branch targets landed inside another
instruction and were given abort stubs rather than bodies. Each one is a decode
boundary that may or may not be reachable.

## Structural

**12. `CATZ.EXE` is not used.** The real launcher does a dependency check and
then `WinExec`s `CATZ.WAD`; the recomp starts at the WAD directly. Wiring it up
would exercise the launcher's own paths.

**13. More than one pet.** The save format has `Cat1`/`Age1` numbering, so the
engine expects a family. Only one is ever loaded.

**14. Frame pacing.** `SetTimer` now rounds to the Win16 18.2 Hz tick, which
matches the ~18 fps the engine was written for. Worth measuring against real
hardware behaviour rather than trusting the arithmetic.
