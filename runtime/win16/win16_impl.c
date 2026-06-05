/*
 * win16_impl.c - Real implementations of the init-path Win16 shims.
 *
 * These override the generated stubs in win16_stubs.c (gen_win16_stubs.py skips
 * any name defined here). Calling convention: the lifter reaches each shim via
 *   push cs; push 0; NAME(cpu);
 * so on entry the stack is [sp]=retIP(0), [sp+2]=retCS, [sp+4]=last PASCAL arg
 * (lowest address). Each shim reads args with a16/a32 and cleans up like the
 * real __far __pascal routine's RETF would: ret(cpu, purge_bytes).
 *
 * Memory model: a Win16 HANDLE == our flat selector. GlobalAlloc backs at least
 * a full 64K segment per selector, so GlobalLock is just sel:0000 and
 * GlobalReAlloc never has to move a block (a selector addresses <=64K anyway).
 */
#include "runtime_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CATZ_TRACE_WIN16
#define IMPL_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define IMPL_LOG(...) ((void)0)
#endif

/* arg word `off` bytes above the first (lowest) arg slot at sp+4 */
static inline uint16_t a16(CPU *cpu, int off) {
    return mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4 + off));
}
static inline uint32_t a32(CPU *cpu, int off) {
    return (uint32_t)a16(cpu, off) | ((uint32_t)a16(cpu, off + 2) << 16);
}
/* far retaddr (4) + PASCAL arg bytes */
static inline void ret(CPU *cpu, int purge) { cpu->sp += 4 + purge; }

static void read_asciiz(CPU *cpu, uint16_t seg, uint16_t off, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        uint8_t c = mem_read8(cpu, seg, (uint16_t)(off + i));
        if (!c) break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

/* ===== KERNEL: memory (handle == selector) =====
 * A real free-list allocator over the flat heap, NOT a pure bump allocator: the
 * Borland RTL startup probes free memory by GlobalAlloc'ing 4K blocks until
 * failure, then GlobalFree's them all and allocates its real heap. Without
 * reclaiming freed blocks the probe permanently drains the heap and the next
 * allocation (in _setargv) fails. Per-selector size lets GlobalSize be exact
 * and GlobalReAlloc copy. Freed blocks are reused first-fit (uniform probe
 * blocks reuse perfectly); no splitting (fine for bring-up). */
static uint32_t g_sel_size[0x10000];   /* requested size per live selector */
static uint32_t g_sel_base[0x10000];   /* flat base per selector (for free)   */
#define MAX_FREE 16384
static uint32_t fl_base[MAX_FREE], fl_size[MAX_FREE];
static int      fl_n;

static uint16_t galloc(CPU *cpu, uint32_t bytes) {
    if (bytes == 0) bytes = 1;
    uint32_t need = (bytes + 0xF) & ~0xFu;
    /* first-fit over freed blocks */
    for (int i = 0; i < fl_n; i++) {
        if (fl_size[i] >= need) {
            uint32_t base = fl_base[i], bsz = fl_size[i];
            fl_base[i] = fl_base[--fl_n]; fl_size[i] = fl_size[fl_n];
            if (cpu->next_sel == 0) return 0;
            uint16_t s = cpu->next_sel++;
            cpu->sel_base[s] = base; g_sel_base[s] = base; g_sel_size[s] = bsz;
            return s;
        }
    }
    uint16_t s = cpu_alloc_selector(cpu, need);   /* bump fresh */
    if (s) { g_sel_base[s] = cpu->sel_base[s]; g_sel_size[s] = need; }
    return s;
}

static void gfree(CPU *cpu, uint16_t sel) {
    if (!sel || !g_sel_size[sel]) return;
    if (fl_n < MAX_FREE) { fl_base[fl_n] = g_sel_base[sel]; fl_size[fl_n] = g_sel_size[sel]; fl_n++; }
    IMPL_LOG("[win16] GlobalFree(%04X) freelist=%d\n", sel, fl_n);
    g_sel_size[sel] = 0;
}

void KERNEL_GLOBALALLOC(CPU *cpu) {
    uint16_t wFlags = a16(cpu, 4);
    uint32_t bytes  = a32(cpu, 0);
    uint16_t sel    = galloc(cpu, bytes);
    IMPL_LOG("[win16] GlobalAlloc(flags=%04X, %u) -> %04X\n", wFlags, bytes, sel);
    cpu->ax = sel;                          /* handle */
    if (sel) cpu->flags &= ~FLAG_CF; else cpu->flags |= FLAG_CF;
    ret(cpu, 6);
}

void KERNEL_GLOBALREALLOC(CPU *cpu) {
    uint16_t hMem  = a16(cpu, 6);
    uint32_t bytes = a32(cpu, 2);
    if (bytes == 0) bytes = 1;
    uint32_t old = (hMem ? g_sel_size[hMem] : 0);
    if (bytes <= old) {                     /* fits: keep handle */
        cpu->ax = hMem;
    } else {                                /* grow: new block, copy, free old */
        uint16_t nsel = galloc(cpu, bytes);
        if (nsel && hMem) {
            for (uint32_t i = 0; i < old; i++)
                mem_write8(cpu, nsel, (uint16_t)i, mem_read8(cpu, hMem, (uint16_t)i));
            gfree(cpu, hMem);
        }
        cpu->ax = nsel;
    }
    if (cpu->ax) cpu->flags &= ~FLAG_CF; else cpu->flags |= FLAG_CF;
    ret(cpu, 8);
}

void KERNEL_GLOBALFREE(CPU *cpu) {
    uint16_t hMem = a16(cpu, 0);
    gfree(cpu, hMem);
    cpu->ax = 0;                            /* NULL == success */
    ret(cpu, 2);
}

void KERNEL_GLOBALLOCK(CPU *cpu) {          /* -> far ptr DX:AX = handle:0000 */
    uint16_t hMem = a16(cpu, 0);
    cpu->dx = hMem; cpu->ax = 0;
    ret(cpu, 2);
}

void KERNEL_GLOBALUNLOCK(CPU *cpu) { cpu->ax = 0; ret(cpu, 2); }

void KERNEL_GLOBALSIZE(CPU *cpu) {          /* -> DWORD size in DX:AX */
    uint16_t hMem = a16(cpu, 0);
    uint32_t sz = (hMem ? g_sel_size[hMem] : 0);
    cpu->ax = (uint16_t)sz; cpu->dx = (uint16_t)(sz >> 16);
    ret(cpu, 2);
}

void KERNEL_GLOBALHANDLE(CPU *cpu) {        /* -> DX:AX = selector:handle (same) */
    uint16_t sel = a16(cpu, 0);
    cpu->dx = sel; cpu->ax = sel;
    ret(cpu, 2);
}

/* ===== KERNEL: local heap =====
 * LocalInit(uSegment, uStart, uEnd) registers a segment's local heap with the
 * kernel and returns nonzero on success. Borland's RTL near-malloc (used by
 * _setargv etc.) keys off this succeeding; a stub returning 0 made startup
 * report "Out of memory in _setargv". */
void KERNEL_LOCALINIT(CPU *cpu) {
    uint16_t uEnd = a16(cpu, 0), uStart = a16(cpu, 2), uSeg = a16(cpu, 4);
    IMPL_LOG("[win16] LocalInit(seg=%04X, start=%04X, end=%04X) ds=%04X\n",
             uSeg, uStart, uEnd, cpu->ds);
    cpu->ax = 1;                            /* TRUE - heap initialized */
    ret(cpu, 6);
}

/* ===== KERNEL: module / version / task ===== */

void KERNEL_GETWINFLAGS(CPU *cpu) {         /* DX:AX: WF_PMODE|WF_CPU386|WF_ENHANCED */
    cpu->dx = 0; cpu->ax = 0x0029;
    ret(cpu, 0);
}

void KERNEL_GETVERSION(CPU *cpu) {          /* Windows 3.10 (AX), DOS 5.00 (DX) */
    cpu->ax = 0x0A03; cpu->dx = 0x0005;
    ret(cpu, 0);
}

void KERNEL_GETCURRENTTASK(CPU *cpu) {      /* nonzero fake HTASK */
    cpu->ax = 0x00FF;
    ret(cpu, 0);
}

void KERNEL_GETMODULEUSAGE(CPU *cpu) { cpu->ax = 1; ret(cpu, 2); }

void KERNEL_GETMODULEFILENAME(CPU *cpu) {
    uint16_t nSize = a16(cpu, 0), off = a16(cpu, 2), seg = a16(cpu, 4);
    const char *path = "C:\\CATZ\\CATZDLL.DLL";
    uint16_t i = 0;
    for (; path[i] && i + 1 < nSize; i++)
        mem_write8(cpu, seg, (uint16_t)(off + i), (uint8_t)path[i]);
    mem_write8(cpu, seg, (uint16_t)(off + i), 0);
    cpu->ax = i;
    ret(cpu, 8);
}

/* ===== USER: init-path window/message ===== */

void USER_MESSAGEBOX(CPU *cpu) {
    /* args (sp+4 up): uType@0, lpCaption off@2/seg@4, lpText off@6/seg@8, hWnd@10 */
    uint16_t toff = a16(cpu, 6),  tseg = a16(cpu, 8);    /* lpText */
    uint16_t coff = a16(cpu, 2),  cseg = a16(cpu, 4);    /* lpCaption */
    char text[256] = "", cap[128] = "";
    read_asciiz(cpu, tseg, toff, text, sizeof(text));
    read_asciiz(cpu, cseg, coff, cap, sizeof(cap));
    fprintf(stderr, "[MessageBox] \"%s\" | \"%s\"\n", cap, text);
    cpu->ax = 1;                            /* IDOK */
    ret(cpu, 12);
}

void USER_ENUMTASKWINDOWS(CPU *cpu) {
    /* Return TRUE without invoking the callback: the engine is enumerating to
     * detect an existing instance window; finding none keeps it on the normal
     * first-run path instead of the "already running" branch. */
    cpu->ax = 1;
    ret(cpu, 10);
}

void USER_GETTICKCOUNT(CPU *cpu) {          /* DX:AX ms, monotonic so waits end */
    static uint32_t t = 0;
    t += 16;
    cpu->ax = (uint16_t)t; cpu->dx = (uint16_t)(t >> 16);
    ret(cpu, 0);
}

void USER_MESSAGEBEEP(CPU *cpu) { cpu->ax = 1; ret(cpu, 2); }

/* ===== INT 21h (DOS services occasionally used by the Borland startup) ===== */

void dos_int21(CPU *cpu) {
    switch (cpu->ah) {
    case 0x4C:                              /* terminate process */
        printf("[INT21/4C] program exit, code=%u\n", cpu->al);
        fflush(stdout);
        exit(cpu->al);
    case 0x30:                              /* DOS version -> 5.00 */
        cpu->al = 5; cpu->ah = 0;
        break;
    default:
        IMPL_LOG("[INT21] ah=%02X (ignored)\n", cpu->ah);
        break;
    }
    /* `int` is not a far call - no stack frame to clean. */
}
