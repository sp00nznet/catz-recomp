# Investigations

Traces kept for the method rather than the conclusion. The first one below was
superseded -- the rects were being corrupted upstream by a DGROUP sizing bug and
a decoder fault, not by the ball geometry -- but the elimination work is worth
keeping.

## The pet's missing rect (resolved; trail preserved)

The pet is rendered every frame and clipped away. Chain, each step verified by
watchpoint or printed value, innermost first:

1. `XBallz::DisplayBallzFrame` gets a clip rect of `(-9429,0)-(10050,-9193)`.
   Bottom above top, so it is empty and no ball is plotted. Measured: 82,944
   writes of the clear colour into the pet's 288x288 surface, ZERO of any other
   value.
2. The rect lives at the draw-port object `+0x96`; `seg007_20F8` memcpy's it
   from the same object's `+0x124`.
3. `+0x124` is written by `XApt::MoveDrawRect` (`seg002:216A`), which stores what
   it is handed; it is already wrong on arrival.
4. Its caller is `ScriptSprite::PopScript` (`seg041`), passing a local rect at
   `ss:[bp-0x26]`.
5. That local is produced by a virtual call on the object at ScriptSprite
   `+0x242` -- vtable slot `+0x30`, which resolves to
   **`XBallz::MoveFrameRectBall`** (`seg47:212D`). Printed across that call:

       in  (2107,9765,-1468,-9425)  ->  out (-28871,21018,-3516,23089)

   Its input rect is ALREADY inverted, so this is propagation, not the origin.
6. `XBallz::MoveFrameRect` (`seg47:1E64`) is innocent -- printed immediately
   before and after, the rect is byte-identical.

The most useful clue is that the FIRST rect in a run is sane -- `(306,156,334,184)`,
a plausible 28x28 -- and later ones are not. MoveFrameRectBall moves a rect by the
delta between two BallStates, so the rect looks like it diverges frame over frame
as those deltas go wrong, rather than being born wrong.

Corrections to an earlier version of this section, so the wrong trail is not
followed: ScriptSprite `+0x242` is a POINTER TO AN OBJECT (its first words are a
vtable pointer), not a stored rect. Reading it as a rect gave `(5714,45,0,42)`
and led to blaming `seg047_1702`, which is actually an allocation path. That was
wrong.

Ruled out, so it is not re-checked:

- The rect plumbing is sound. Sane rects travel the identical path in the same
  run: `(306,156,334,184)`, `(0,0,167,333)`, `(4,39,163,109)`.
- `XApt::ESpace` is not the discriminator; sane and garbage rects both arrive
  with space 0.
- No systemic lifter gap remains to blame: no dropped instructions, no `esc_N`
  unlifted, no 32-bit addressing mishandled, and the 11 remaining `???` x87
  decodes are data-in-code misreads (`00 00 FF FF 00 00 DA FF`).

Next step, and the reason this stopped here: the question is now what the
BallState deltas are SUPPOSED to be, which needs the ball-frame data model rather
than another pointer chase. The sibling `catzng` project already parses the same
LNZ/BDT files and knows the expected ball geometry -- checking the engine's ball
positions against it would give ground truth instead of inference. Also worth
trying first: these measurements are all from the Adoption Kit
(`CATZ_DLG_RESULT=2001`); the real playpen may not be broken the same way.

`CATZ_DUMP_BLIT=snapN` (with `CATZ_DUMP_DIR`) dumps every live WinG surface at
one instant with a distinct-value count. The count is the useful part: a uniform
fill and real artwork both report "all bytes nonzero".
