"""
win16.py - Win16 import resolver for the CATZ recomp.

Unlike El-Fish (a single TSXLIB DOS extender), CATZDLL.DLL is a real Windows
3.1 application that imports from EIGHT modules: KERNEL, USER, GDI, SOSLIB03,
WING, MMSYSTEM, TOOLHELP, WIN87EM. NE import relocations carry a `module_idx`
(1-based into ne.module_names) plus an ordinal, so resolution is module-aware.

Strategy: authoritative names come from IDA (which ships Win16 ordinal->API
maps) and are written to analysis/win16_imports.json by tools/ida_export.py.
This module loads that map when present, falls back to a small built-in seed of
high-confidence ordinals, and finally to a `MODULE_OrdN` stub name so the
recomp always links. Each resolved import becomes a C shim `void NAME(CPU*)`.
"""

import os
import json
from dataclasses import dataclass

_ANALYSIS = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'analysis'))
_JSON = os.path.join(_ANALYSIS, 'win16_imports.json')


@dataclass
class Win16Import:
    module: str         # KERNEL, USER, GDI, SOSLIB03, WING, MMSYSTEM, TOOLHELP, WIN87EM
    ordinal: int        # export ordinal in that module
    name: str           # C shim name, e.g. 'KERNEL_GlobalAlloc'
    api: str            # bare API name, e.g. 'GlobalAlloc' (or 'Ord15' if unknown)
    category: str       # 'mem','file','gdi','user','sound','blit','fpu','sys','unknown'
    known: bool = False # True if resolved from IDA map or built-in seed


# Modules whose imports are floating-point emulation trampolines. CATZ uses real
# x87 instructions in the code stream (only ~6 WIN87EM refs), so unlike El-Fish
# there is no inline FPU-trampoline skipping to do; WIN87EM calls are real calls.
_FPU_MODULES = {'WIN87EM'}

_CATEGORY_BY_MODULE = {
    'KERNEL': 'sys', 'USER': 'user', 'GDI': 'gdi', 'SOSLIB03': 'sound',
    'WING': 'blit', 'MMSYSTEM': 'sound', 'TOOLHELP': 'sys', 'WIN87EM': 'fpu',
}

# High-confidence built-in seed of Win16 system ordinals (Windows 3.1).
# Used only as a fallback before the IDA-derived map is available. Names that
# matter most for bring-up (memory, module, file, basic GDI/USER) are covered;
# everything else resolves to MODULE_OrdN until IDA fills it in.
_SEED = {
    'KERNEL': {
        5: 'LocalAlloc', 6: 'LocalLock', 7: 'LocalFree', 8: 'LocalUnlock',
        15: 'GlobalAlloc', 16: 'GlobalFree', 17: 'GlobalReAlloc', 18: 'GlobalLock',
        19: 'GlobalUnlock', 20: 'GlobalSize', 23: 'LockSegment', 24: 'UnlockSegment',
        30: 'WaitEvent', 47: 'GetModuleHandle', 49: 'GetModuleFileName',
        50: 'GetProcAddress', 51: 'MakeProcInstance', 52: 'FreeProcInstance',
        74: 'OpenFile', 81: '_lclose', 82: '_lread', 83: '_lcreat', 84: '_llseek',
        85: '_lopen', 86: '_lwrite', 88: 'lstrcpy', 89: 'lstrcat', 90: 'lstrlen',
        91: 'InitTask', 95: 'LoadLibrary', 96: 'FreeLibrary', 97: 'GetTempDrive',
        127: 'GetPrivateProfileInt', 128: 'GetPrivateProfileString',
        129: 'WritePrivateProfileString', 130: 'GetProfileInt', 131: 'GetProfileString',
    },
    'GDI': {
        27: 'CreateCompatibleDC', 31: 'SetPixel', 34: 'BitBlt', 35: 'StretchBlt',
        45: 'SelectObject', 53: 'CreateBitmap', 54: 'CreateBitmapIndirect',
        62: 'CreateCompatibleBitmap', 68: 'DeleteDC', 69: 'DeleteObject',
        80: 'GetDeviceCaps', 65: 'RealizePalette', 66: 'GetPaletteEntries',
        360: 'CreatePalette', 361: 'GetPaletteEntries', 363: 'SetPaletteEntries',
        370: 'CreateDIBitmap', 371: 'SetDIBits', 372: 'GetDIBits',
        439: 'SetDIBitsToDevice', 489: 'StretchDIBits',
    },
    'USER': {
        1: 'MessageBox', 10: 'SetTimer', 12: 'KillTimer', 39: 'BeginPaint',
        40: 'EndPaint', 41: 'CreateWindow', 42: 'ShowWindow', 57: 'RegisterClass',
        66: 'GetDC', 68: 'ReleaseDC', 107: 'DefWindowProc', 108: 'GetMessage',
        110: 'PostMessage', 111: 'SendMessage', 113: 'TranslateMessage',
        114: 'DispatchMessage', 124: 'UpdateWindow', 125: 'InvalidateRect',
        173: 'LoadCursor', 174: 'LoadIcon', 420: 'wsprintf',
    },
}

_loaded_map = None


def _load_map():
    """Load the IDA-derived import map (module -> {ordinal: api_name}), cached."""
    global _loaded_map
    if _loaded_map is not None:
        return _loaded_map
    _loaded_map = {}
    if os.path.exists(_JSON):
        try:
            with open(_JSON, encoding='utf-8') as f:
                raw = json.load(f)
            for mod, ords in raw.items():
                _loaded_map[mod.upper()] = {int(k): v for k, v in ords.items()}
        except Exception:
            _loaded_map = {}
    return _loaded_map


def _sanitize(api: str) -> str:
    out = []
    for ch in api:
        out.append(ch if (ch.isalnum() or ch == '_') else '_')
    return ''.join(out)


def get_import(module: str, ordinal: int) -> Win16Import:
    """Resolve a (module, ordinal) import to a Win16Import shim descriptor."""
    mod = (module or 'UNK').upper()
    cat = _CATEGORY_BY_MODULE.get(mod, 'unknown')

    api = None
    known = False
    idamap = _load_map().get(mod)
    if idamap and ordinal in idamap:
        api = idamap[ordinal]
        known = True
    elif mod in _SEED and ordinal in _SEED[mod]:
        api = _SEED[mod][ordinal]
        known = True

    if api:
        name = f'{mod}_{_sanitize(api)}'
    else:
        api = f'Ord{ordinal}'
        name = f'{mod}_Ord{ordinal}'

    return Win16Import(module=mod, ordinal=ordinal, name=name, api=api,
                       category=cat, known=known)


def is_fpu_module(module: str) -> bool:
    return (module or '').upper() in _FPU_MODULES


def module_name(ne, module_idx: int) -> str:
    """Map a 1-based NE module_idx to its imported module name."""
    if 1 <= module_idx <= len(ne.module_names):
        return ne.module_names[module_idx - 1]
    return f'MOD{module_idx}'
