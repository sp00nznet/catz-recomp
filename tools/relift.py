"""
relift.py - Regenerate Catz's lifted tree from your own copy of the game.

Everything below work/ is derived from CATZDLL.DLL and CATZ.WAD and is never
committed. One command, so a re-lift cannot silently skip a step:

    python tools/relift.py            # IDA maps must already be in work/ida/
    python tools/relift.py --ida      # export them first (needs IDA 9 idalib)

What it runs, all from pcrecomp (PCRECOMP_DIR, default ../tools):
  ida/ida_export.py   per-module code maps  -> work/ida/<MODULE>.json
  ne/ne_link.py       lift both modules into one numbering, build the image,
                      and regenerate the glue (segments.h, dispatch, stubs)
  ne/patch_borland_eh.py  the Borland EH cleanup-chain guard
"""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
PC = os.path.abspath(os.environ.get('PCRECOMP_DIR', os.path.join(ROOT, '..', 'tools')))
MODULES = ['game/CATZDLL.DLL', 'game/CATZ.WAD']    # DLLs first, the host last


def run(*cmd):
    print('>', ' '.join(cmd), flush=True)
    subprocess.run(cmd, cwd=ROOT, check=True)


def main():
    os.makedirs(os.path.join(ROOT, 'work', 'ida'), exist_ok=True)
    if '--ida' in sys.argv:
        for m in MODULES:
            stem = os.path.splitext(os.path.basename(m))[0]
            run('py', '-3.11', os.path.join(PC, 'tools', 'ida', 'ida_export.py'),
                m, f'work/ida/{stem}.json')
    run(sys.executable, os.path.join(PC, 'tools', 'ne', 'ne_link.py'),
        '--work', 'work', '--ida-dir', 'work/ida', '--seeds', 'tools/entry_seeds.json',
        '--prefix', 'recomp',
        '--shims', os.path.join(PC, 'runtime', 'win16', 'host'), '--shims', 'runtime',
        *MODULES)
    run(sys.executable, os.path.join(PC, 'tools', 'ne', 'patch_borland_eh.py'),
        '--work', 'work', *MODULES)


if __name__ == '__main__':
    main()
