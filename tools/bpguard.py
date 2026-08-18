"""Instrument every lifted CALL site (not tail-jump) with a callee-saved-bp check.

The lifter marks real calls with a trailing `/* call 0x... */` comment and tail
jumps with `; return; /* ... */`, so the two are trivially separable. bp is
callee-saved in every Borland-generated Win16 function, so the FIRST callee that
returns with a changed bp is the mis-lifted one.

The same site checks sp: a call site pushes its far/near return frame before the
call and the callee pops it, so sp on return is always the pre-call sp plus a
small purge. A callee that returns with sp BELOW the pre-call value has leaked
guest stack, and a purge over 0x100 means it unwound something it did not own --
either one silently eats the 64 KB guest stack a few bytes at a time.

Usage: python bpguard.py apply | revert   (revert == git checkout src/)
"""
import glob, re, sys, os

ROOT = r'G:\recomp\pc\catz'
CALL = re.compile(r'^(\s+)(seg\d+_[0-9A-Fa-f]+)\(cpu\);(\s+/\* call [^\n]*)$')

def apply():
    n = 0
    for path in glob.glob(os.path.join(ROOT, 'src', '*.c')):
        if os.path.basename(path).startswith('_'):
            continue
        out = []
        for line in open(path, encoding='utf-8', errors='replace').read().split('\n'):
            m = CALL.match(line)
            if m:
                ind, callee, tail = m.groups()
                out.append(f'{ind}{{ uint16_t _gb = cpu->bp, _gs = cpu->sp, _gd = cpu->ds; {callee}(cpu);'
                           f' if (cpu->bp != _gb) catz_bp_broke("{callee}", _gb, cpu->bp, _gs, cpu->sp);'
                           f' if (cpu->ds != _gd) catz_ds_broke("{callee}", _gd, cpu->ds);'
                           f' if (cpu->sp < _gs || (uint16_t)(cpu->sp - _gs) > 0x100)'
                           f' catz_sp_broke("{callee}", _gs, cpu->sp); }}{tail}')
                n += 1
            else:
                out.append(line)
        open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print(f'instrumented {n} call sites')

if __name__ == '__main__':
    apply()
