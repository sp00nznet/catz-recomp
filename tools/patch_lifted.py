"""patch_lifted.py - Re-apply the hand-written patches to generated src/.

The lifted sources are regenerated wholesale by lift_dll.py / lift_wad.py, so
any correction that cannot be expressed in the lifter itself has to be replayed
afterwards. Keeping them here (instead of as comments saying "re-apply after a
re-lift") means a re-lift is one command sequence and cannot silently lose them.

Run after lift_dll.py + lift_wad.py, before the gen_* glue scripts.
"""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
SRC = os.path.join(ROOT, 'src')

# Borland's cleanup-chain walker is linked into BOTH modules (CATZDLL seg049,
# CATZ.WAD seg064) at the same offsets. A setjmp/C++ catch buffer is marked by
# the cookie 0xFACE and can show up on the chain as a node address or in a [0]
# next-link slot; following it (or a null link) turns the walk into an infinite
# loop. Stop the walk at the cookie/null.
EH_GUARD = '''    /* Cross-module EH boundary guard (applied by tools/patch_lifted.py). A
     * Borland setjmp/C++ catch buffer is marked by the cookie 0xFACE; it can
     * appear on this cleanup chain as a node address or in a [0] next-link
     * slot. Following it (or a null link) derails the walk into a runaway
     * loop. Stop the walk at the cookie/null. */
    {{ uint16_t _nd = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->bp - 0x2));
      if (_nd == 0x0000 || _nd == 0xFACE ||
          mem_read16(cpu, cpu->ss, _nd) == 0xFACE) {{ seg{seg}_1428(cpu); return; }} }}
'''


def patch_eh_guard(seg):
    path = os.path.join(SRC, f'seg{seg}.c')
    text = open(path, encoding='utf-8', errors='replace').read()
    anchor = f'    TRACE_FN("seg{seg}_141A");\n'
    if anchor not in text:
        print(f'  seg{seg}: anchor missing - SKIPPED (did the lift change?)')
        return False
    if '0xFACE' in text.split(anchor, 1)[1][:600]:
        print(f'  seg{seg}: already patched')
        return True
    text = text.replace(anchor, anchor + EH_GUARD.format(seg=seg), 1)
    open(path, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'  seg{seg}: EH cleanup-chain guard applied')
    return True


def main():
    print('Re-applying hand patches to lifted sources:')
    ok = all([patch_eh_guard('049'), patch_eh_guard('064')])
    if not ok:
        sys.exit(1)


if __name__ == '__main__':
    main()
