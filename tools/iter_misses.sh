#!/usr/bin/env bash
# Iterate the indirect-call (vtable) miss fix loop:
#   seed missed targets -> re-lift affected segments -> regen glue -> rebuild
#   with dispatch tracing -> run -> report remaining misses + outcome.
set -e
export PATH="/c/msys64/mingw64/bin:$PATH"
cd "$(dirname "$0")/.."

LOG=build/run.log
# Which CATZDLL (seg<=59) segments had misses this round?
DLL_SEGS=$(grep -oE 'MISS seg=([0-9]+)' "$LOG" 2>/dev/null | grep -oE '[0-9]+' \
           | awk '$1<=59' | sort -un)
WAD_MISS=$(grep -oE 'MISS seg=(6[0-6])' "$LOG" 2>/dev/null | head -1)

echo "== seeding missed targets =="
py -3 tools/fix_misses.py "$LOG"

echo "== re-lifting affected modules =="
for s in $DLL_SEGS; do
  printf '  CATZDLL seg%d\n' "$s"
  py -3 tools/ne_lift.py game/CATZDLL.DLL --seg "$s" > "$(printf 'src/seg%03d.c' "$s")"
done
if [ -n "$WAD_MISS" ]; then echo "  CATZ.WAD"; py -3 tools/lift_wad.py >/dev/null 2>&1; fi

echo "== regen glue =="
py -3 tools/gen_stubs.py | tail -1
py -3 tools/gen_segments_h.py >/dev/null
py -3 tools/gen_dispatch.py >/dev/null

echo "== build (dispatch trace) + run =="
sed -i '1i #define ELFISH_TRACE_RUNTIME 1\n#include <stdio.h>' src/_dispatch.c
cmake --build build >/tmp/b.log 2>&1 && echo "build ok" || { echo "BUILD FAIL"; grep error: /tmp/b.log | head; }
timeout 15 ./build/catz.exe > "$LOG" 2>&1 || true
sed -i '1d;1d' src/_dispatch.c

echo "== result =="
echo "dispatch misses: $(grep -cE 'MISS seg=' "$LOG")"
grep -oE 'dispatch_(far|near) MISS seg=[0-9]+ off=[0-9A-F]+' "$LOG" | sort | uniq -c | sort -rn | head
echo "--- tail (non-GlobalAlloc) ---"
grep -vE 'GlobalAlloc\(flags' "$LOG" | tail -6
