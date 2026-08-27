# Diagnostics

All in-tree, all environment-gated, all no-ops in a normal run. These exist
because almost every defect in this project was found by measuring something
that should have been invariant.

## Finding out where the engine got to

| Variable | What it answers |
|----------|-----------------|
| `CATZ_TRAP=<substring>` | When the engine's own log prints a matching line, dump the guest call stack. Its log names a source file and line but not the path that reached it. |
| `CATZ_STUB_HITS=1` | List every unimplemented Win16 API the session reached, once each, at exit. |
| `CATZ_FN_COUNT=1` | Per-function hit counts, reported at exit. Finds a spin. |
| `CATZ_WATCH_DS` | Flag a null `DS` the moment it appears rather than where it hurts. |
| `dump_guest_stack()` | The guest stack cannot be walked -- the lifter pushes only the return offset -- so this captures the **host** stack, one C frame per lifted function, and prints file VAs. ASLR is off, so `addr2line -f -e build/catz.exe <addrs>` names them directly. |

## Seeing what was drawn, without screen capture

| Variable | What it answers |
|----------|-----------------|
| `CATZ_DUMP_WIN=<n>` | Replay every blit into a private canvas and write it out after n blits (and again at 2n) -- exactly what reached the window. |
| `CATZ_DUMP_WIN_FROM=<n>` | Ignore blits before n, so a trail is distinguishable from art drawn once at startup. |
| `CATZ_DUMP_BLIT=snap<n>` | Dump every live WinG surface at one instant, with size and distinct-value counts. |
| `CATZ_LOG_BLT=<n>` | Log the first n distinct blit source/destination rectangles. |
| `CATZ_LOG_S2S=<n>` | The same for surface-to-surface copies. |
| `CATZ_DIFF=<n>` | Dump what changed on a surface between consecutive blits, with a bounding box -- i.e. exactly what one frame drew. |
| `CATZ_STALE=<n>` | An age map: which pixels have not been repainted. |
| `CATZ_LOG_ROP` / `CATZ_LOG_TEXT` | Raster ops requested; strings drawn and measured. |

A caution learned the hard way: the canvas replays the *blit stream*. Anything
clean there but wrong on screen is happening host-side -- that is how the
background erase was found.

## Driving the game without touching the mouse

| Variable | What it answers |
|----------|-----------------|
| `CATZ_DLG_RESULT=a,b,c` | Answer each dialog in order with one code; the last repeats. `-1` leaves that dialog live for a real click. |
| `CATZ_CURSOR=x,y[,b]` | Pin where the engine thinks the pointer is, and optionally whether the button is down -- the engine polls `GetCursorPos` and `GetAsyncKeyState` rather than reading message coordinates. `@<file>` re-reads the file each call, so a drag can move. |

Together these drive the whole UI headlessly: open the Options menu, pick a
breed, name a cat, drag a toy off the shelf.

## A caveat about the function ring

`CATZ_TRAP_RING` prints the recently executed functions, and it is tempting to
diff two runs and read the difference as "this path was taken and that one was
not". It is not reliable for that. The lifter turns intra-segment jumps into
tail calls and `-O2` merges them, so a label that really did execute can be
absent from the ring, having been folded into its sibling. A chain of ten
functions appeared reproducibly in two runs of a working menu item and never in
a broken one, which looked like the answer and was not.

Use it to find candidates. Confirm them with a counter or a printed value at the
branch itself, never with absence from the ring alone.

## Historical

`tools/bpguard.py` instruments all ~10k call sites and reports any callee that
fails to preserve **BP** or **DS**, or returns with **SP** below the pre-call
value. It found every systemic lifter bug in the early going. Revert with
`git checkout src/`.
