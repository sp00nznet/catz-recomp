# The Win16 layer

Everything the engine does to the outside world goes through
`runtime/win16/`. `gen_win16_stubs.py` generates a stub for every imported
ordinal with the correct PASCAL stack purge; anything hand-written in
`win16_impl.c` or `win32_backend.c` replaces its stub automatically.

Run with `CATZ_STUB_HITS=1` to list the unimplemented APIs a session actually
reached. That list is the work queue, and it is short now:

```
__FPMATH  ENUMWINDOWS  SETWINDOWTEXT  CHECKMENUITEM  SENDMESSAGE
GETDESKTOPWINDOW  SETWINDOWPOS  BITBLT  SNDPLAYSOUND  TIMERCOUNT
SOSDIGIDETECTINIT  SOSDIGIUSERSERVICE
```

## Recurring shapes of bug

The same few mistakes accounted for most of the rendering and input defects,
and each looked like a different symptom:

**A stub that returns 0 is not neutral.** The engine checks return values and
carries on with a null object, so the failure surfaces layers away from the
call. `CreateSolidBrush` returning 0 killed the first real frame.
`GetTextExtent` returning 0 gave the panel's menu bar zero-width items, so
DrawText clipped "&Options" away to the underline under its accelerator -- and
sized the system menu to nothing, so a second click landed on *Close* and shut
the program down. `WritePrivateProfileString` returning 0 meant the engine could
not persist anything at all: not settings, not the date, not a newly adopted
cat.

**The PASCAL purge has to be exact.** The callee pops its own arguments, so a
stub with the wrong byte count shifts the caller's frame. `GetCursorPos` popped
only its return address, which left `DS` reading 0, which made `PetParams` a
null segment, which divided by zero, which painted every ball colour 0 -- a
black cat, four layers from the cause.

**A WinG DC is not in the host handle table.** WinG DC handles start at
`0x0DC0`, well past the host table, so `get_hdc` returned NULL for them and
whole categories of call were dropped in silence. That is why surface-to-surface
blits never happened (nothing erased the previous frame, so the pet smeared
across the playpen) and why `Ellipse`/`Polygon` never drew (the milk bottle came
off the shelf and simply was not there). A WinG surface is raw bytes in guest
memory and GDI cannot draw on it, so each WinG DC now keeps a host memory DC
over a matching DIB section, with only the affected rectangle copied in and out.

**Win16 semantics differ from Win32 in ways that matter.** `PeekMessage` and
`GetMessage` were ignoring the window and message-range filters; CATZDLL pumps
messages inside its own processing with narrow filters, so handing it the
shell's own posted message made the pump never return, and `WM_TIMER` -- which
Windows only synthesises when no posted message is waiting -- stopped arriving.
Win16 timers also run off the 18.2 Hz tick, rounding up to a multiple of
54.925 ms; honouring the engine's requested 10 ms literally ran the pet at
~100 Hz.

**Some calls must not be passed through.** `SwapMouseButton` swaps the buttons
system-wide and the engine passes 0, which would quietly un-swap the mouse of
anyone running left-handed. It is answered from `GetSystemMetrics` instead.

## Palette

The engine leaves the colour table in its `BITMAPINFO` blank and supplies its
256 colours through `CreatePalette` after asking the system for the current
hardware palette. Both of those were stubs, so every surface blitted against an
all-zero table. `GetSystemPaletteEntries` now synthesises the classic 256-colour
layout (10 system colours, a 6x6x6 cube, 10 more), and the realized palette is
used as the DIB colour table.
