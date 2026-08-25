/*
 * main.c - CATZ recomp runtime entry (bring-up host).
 *
 * Loads the flat memory image (segments + applied internal relocations), sets
 * up the selector->base table and initial CPU/segment state from the NE
 * header, and calls the lifted DLL entry (seg001_0000 == LibMain/init). This is
 * the first execution attempt; expect it to stop on an unimplemented Win16 shim
 * - that is the signal for which API to implement next (hottest first; see
 * analysis/xref_imports.txt).
 *
 * The real game host is CATZ.EXE (a thin window shell that LoadLibrary's the
 * engine and pumps messages); that lives in the Win16 USER shim layer and is
 * built out once init runs clean.
 */
#include "cpu.h"
#include "segments.h"
#include "mem_layout.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

/* Watchdog: if the engine hangs in lifted code, dump the recent call ring so we
 * can see the spinning loop, then exit. Enable with -DCATZ_WATCHDOG. */
static DWORD WINAPI watchdog_thread(LPVOID p) {
    (void)p;
    Sleep(5000);
    fprintf(stderr, "\n[watchdog] 5s elapsed - recent call ring (hang loop?):\n");
    dump_fn_ring(60);
    fflush(stderr);
    _exit(99);
    return 0;
}

#ifndef CATZ_IMAGE_PATH
#define CATZ_IMAGE_PATH "build_data/mem_image.bin"
#endif

/* Function-entry ring buffer (declared in cpu.h, filled by TRACE_FN). */
const char *g_fn_ring[CATZ_FN_RING_SIZE];
unsigned g_fn_ring_pos = 0;

/* Global CPU pointer so the Win32 backend / WndProc bridge can invoke guest
 * code (the registered window procedure) on delivered messages. */
CPU *g_cpu = NULL;

/* Where the current run of ds==0 began — for the null-selector hunt. */
const char *g_ds_zero_from = "?";
unsigned g_ds_run = 0;

#ifdef CATZ_WATCH_SP
/* See cpu.h: the guest stack is one 64 KB segment and it drains. Report the
 * call chain each time sp crosses a new low-water mark — if the chain repeats
 * at every threshold the stack is leaking (a callee not balancing its frame);
 * if it keeps growing it is honest recursion. */
void catz_sp_check(const char *nm)
{
    static const uint16_t floors[] = { 0xC000, 0x8000, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400 };
    static unsigned next_floor = 0;
    uint16_t sp = g_cpu ? g_cpu->sp : 0xFFFE;

    /* A null DS is legitimate for a moment after `lds` loads a NULL far pointer
     * (seg001_5A45 is that branch and reloads DS immediately), so entry-time
     * ds==0 on its own means nothing. What is NOT legitimate is DS *staying*
     * null across many calls — that is a lost DGROUP, and any far pointer built
     * as `push ds` from then on carries a null selector. Discriminate on run
     * length and report where the run began. */
    if (g_cpu) {
        if (g_cpu->ds == 0) {
            if (g_ds_run++ == 0) g_ds_zero_from = nm;   /* first call of this run */
        } else {
            g_ds_run = 0;
        }
    }
    /* A single-step plunge is a stack switch or a bogus `sub sp`, not ordinary
       frame growth; report the pair of calls it happened between. */
    {   static uint16_t prev_sp = 0xFFFE; static const char *prev_nm = "?";
        static int shown = 0;
        if (shown < 8 && prev_sp > sp && (uint16_t)(prev_sp - sp) > 0x400) {
            shown++;
            fprintf(stderr, "[SP-DROP] %04X -> %04X (-%u) between %s and %s\n",
                    prev_sp, sp, (unsigned)(prev_sp - sp), prev_nm, nm);
            fprintf(stderr, "  last 40 labels:");
            for (int i = 40; i > 0; i--) {
                const char *r = g_fn_ring[(g_fn_ring_pos - (unsigned)i) & (CATZ_FN_RING_SIZE - 1)];
                if (r) fprintf(stderr, " %s", r);
            }
            fprintf(stderr, "\n");
        }
        prev_sp = sp; prev_nm = nm;
    }
    if (next_floor >= sizeof floors / sizeof floors[0]) return;
    if (sp > floors[next_floor]) return;
    fprintf(stderr, "\n[SP-LOW] guest sp=%04X (below %04X) at %s\n", sp, floors[next_floor], nm);
    next_floor++;
    dump_guest_stack(g_cpu, 48);
}
#endif

/*
 * Guest call stack. Win16 far functions open with `inc bp; push bp; mov bp,sp`
 * and close with `pop bp; dec bp; retf`, so the saved bp at [bp] carries a
 * mark bit: odd means THIS frame belongs to a far function and its return
 * address at [bp+2] is 4 bytes (offset then segment); even means near, 2 bytes,
 * returning into the same segment as the frame we came from.
 *
 * Far more useful than the flat call ring, which only shows what ran recently,
 * not who is waiting on what.
 */
void dump_guest_stack(CPU *cpu, int max)
{
    /* The guest stack can't be walked: the lifter pushes a literal 0 as the
     * far-return offset (control actually returns through the host C stack), so
     * every return address on it is fake. The HOST stack is the guest call
     * stack — one C frame per lifted function still executing. Print raw
     * addresses as file VAs; `addr2line -f -e build/catz.exe` names them.
     * ponytail: no dbghelp/PDB plumbing to symbolize in-process, the two-step
     * is fine for a debug dump. Note -O2 tail-merges the innermost chain, so
     * the deepest few lifted labels appear as their sibling-call parent. */
    (void)cpu;
    void *frames[128];
    USHORT n = CaptureStackBackTrace(0, (ULONG)(max > 128 ? 128 : max), frames, NULL);
    fprintf(stderr, "--- guest call stack (%u host frames, innermost first) ---\n"
                    "    symbolize: addr2line -f -e build/catz.exe \\\n     ", n);
    for (USHORT i = 0; i < n; i++)
        fprintf(stderr, " %p", frames[i]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Runaway recursion in lifted code kills the host with STATUS_STACK_OVERFLOW and
 * no clue where. Dump the tail of the call ring in order — a recursion shows up
 * as a repeating cycle. SetThreadStackGuarantee (in main) leaves us enough stack
 * to actually run this handler. */
static LONG CALLBACK stack_overflow_veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_STACK_OVERFLOW) {
        /* Any other fatal exception kills the host with no output at all --
           the process just returns a shell-level status and the run looks like
           it "stopped" wherever stderr happened to end. Name it, and print the
           same guest evidence, before letting the OS take it. Non-fatal codes
           (breakpoints, C++ EH, debugger notifications) pass straight through. */
        if (code != EXCEPTION_ACCESS_VIOLATION &&
            code != EXCEPTION_ILLEGAL_INSTRUCTION &&
            code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
            code != EXCEPTION_PRIV_INSTRUCTION &&
            code != EXCEPTION_IN_PAGE_ERROR &&
            code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
            return EXCEPTION_CONTINUE_SEARCH;
        fprintf(stderr, "\n[CRASH] exception %08lX at %p; last 40 guest calls:\n",
                (unsigned long)code, ep->ExceptionRecord->ExceptionAddress);
        for (int i = 40; i > 0; i--) {
            const char *nm = g_fn_ring[(g_fn_ring_pos - (unsigned)i) & (CATZ_FN_RING_SIZE - 1)];
            if (nm) fprintf(stderr, " %s", nm);
        }
        fprintf(stderr, "\n");
        if (g_cpu) dump_guest_stack(g_cpu, 40);
        fflush(stderr);
        _exit(96);
    }
    fprintf(stderr, "\n[STACK-OVERFLOW] host stack exhausted; last 64 calls "
                    "(oldest first), total=%u:\n", g_fn_ring_pos);
    for (int i = 64; i > 0; i--) {
        const char *nm = g_fn_ring[(g_fn_ring_pos - (unsigned)i) & (CATZ_FN_RING_SIZE - 1)];
        if (nm) fprintf(stderr, "  %s\n", nm);
    }
    if (g_cpu) dump_guest_stack(g_cpu, 40);
    fflush(stderr);
    _exit(98);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* sp on return from a call must be the pre-call sp plus the callee's purge.
   Below it means leaked guest stack; far above it means the callee unwound
   a frame it did not own. Either way the 64 KB guest stack is being eaten. */
void catz_div0(const char *kind)
{
    static int fired = 0;
    if (fired++ >= 6) return;
    fprintf(stderr, "[DIV0] guest %s by zero; registers left unchanged\n", kind);
    if (fired == 1) { dump_fn_ring(24); if (g_cpu) dump_guest_stack(g_cpu, 24); }
    fflush(stderr);
}

void catz_sp_broke(const char *callee, uint16_t sp0, uint16_t sp1)
{
    static int fired = 0;
    if (fired++ >= 8) return;
    fprintf(stderr, "[SP-BROKE] %s returned sp %04X->%04X\n", callee, sp0, sp1);
    fprintf(stderr, "  last 24 labels:");
    for (int i = 24; i > 0; i--) {
        const char *r = g_fn_ring[(g_fn_ring_pos - (unsigned)i) & (CATZ_FN_RING_SIZE - 1)];
        if (r) fprintf(stderr, " %s", r);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

void catz_bp_broke(const char *callee, uint16_t bp0, uint16_t bp1, uint16_t sp0, uint16_t sp1)
{
    static int fired = 0;
    if (fired++ >= 8) return;
    fprintf(stderr, "[BP-BROKE] %s returned bp %04X->%04X  sp %04X->%04X\n",
            callee, bp0, bp1, sp0, sp1);
    /* The ring shows which labels actually ran inside the callee, i.e. which
     * edge it left through — for an early return that is far more informative
     * than the call stack, which only says who was waiting. */
    fprintf(stderr, "  last 24 labels:");
    for (int i = 24; i > 0; i--) {
        const char *r = g_fn_ring[(g_fn_ring_pos - (unsigned)i) & (CATZ_FN_RING_SIZE - 1)];
        if (r) fprintf(stderr, " %s", r + 3);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

void dump_fn_ring(int n)
{
    if (n <= 0 || n > (int)CATZ_FN_RING_SIZE) n = (int)CATZ_FN_RING_SIZE;
    /* Histogram: tally occurrences of each distinct name pointer across the full
     * ring (same lifted fn => same literal pointer). The most frequent names are
     * the spin loop. */
    const char *names[256]; int counts[256]; int nd = 0;
    for (unsigned i = 0; i < CATZ_FN_RING_SIZE; i++) {
        const char *nm = g_fn_ring[i];
        if (!nm) continue;
        int j = 0; for (; j < nd; j++) if (names[j] == nm) break;
        if (j == nd && nd < 256) { names[nd] = nm; counts[nd] = 0; nd++; }
        if (j < 256) counts[j]++;
    }
    fprintf(stderr, "--- ring histogram (top spin functions) ---\n");
    for (int top = 0; top < 15; top++) {
        int best = -1;
        for (int j = 0; j < nd; j++) if (counts[j] >= 0 && (best < 0 || counts[j] > counts[best])) best = j;
        if (best < 0 || counts[best] <= 0) break;
        fprintf(stderr, "  %5d  %s\n", counts[best], names[best]);
        counts[best] = -1;
    }
    /* The histogram names the spin, but not the order. The tail in call
       order is what shows which branch a per-frame handler actually takes. */
    fprintf(stderr, "--- last %d calls, oldest first ---\n", n);
    for (int i = n; i > 0; i--) {
        const char *nm = g_fn_ring[(g_fn_ring_pos - (unsigned)i) & (CATZ_FN_RING_SIZE - 1)];
        if (nm) fprintf(stderr, " %s", nm);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "--- end (total calls=%u) ---\n", g_fn_ring_pos);
}

/* Call a far-exported lifted function. argw holds arg words in PASCAL push
 * order (left-to-right); a far return frame is pushed so the callee's RETF
 * balances. Returns AX (DX:AX for 32-bit/far results — read cpu->dx for hi). */
static uint16_t call_export(CPU *cpu, void (*fn)(CPU *), const uint16_t *argw, int nwords)
{
    for (int i = 0; i < nwords; i++) push16(cpu, argw[i]);
    push16(cpu, cpu->cs);
    push16(cpu, 0);
    fn(cpu);
    return cpu->ax;
}

static int load_image(CPU *cpu, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open image: %s\n", path); return 0; }
    size_t n = fread(cpu->mem, 1, CATZ_IMAGE_SIZE, f);
    fclose(f);
    if (n != CATZ_IMAGE_SIZE) {
        fprintf(stderr, "short read: %zu of %u\n", n, (unsigned)CATZ_IMAGE_SIZE);
        return 0;
    }
    return 1;
}

#ifdef CATZ_WATCH_MEM
static void catz_watch_init(void);
#endif

int g_fncount;
static struct { const char *n; unsigned long c; } g_fnc[8192];
int g_watch_ds;
/* CATZ_WATCH_DS: the WAD is an EXE, so its own code runs with DS pinned to
   DGROUP for the whole program. Anything else means a callee clobbered it --
   which is how a Win16 stub that forgets to pop its arguments shows up, since
   the caller then restores DS from the wrong stack slot. Also tracks the low
   water mark of the guest stack. */
void catz_fn_hit(const char *n) {
    if (g_watch_ds && g_cpu) {
        static unsigned long ncall; static uint16_t lo = 0xFFFF; static int shown;
        if (g_cpu->sp < lo) lo = g_cpu->sp;
        if (++ncall % 200000 == 0)
            fprintf(stderr, "[sp] after %lu calls: sp=%04X ss=%04X min=%04X\n",
                    ncall, g_cpu->sp, g_cpu->ss, lo);
        if (n && n[3] == '0' && n[4] == '6' && n[5] == '3'
            && g_cpu->ds != CATZ_AUTO_DATA_SEG && shown++ < 12)
            fprintf(stderr, "[dsbad] %s ds=%04X\n", n, g_cpu->ds);
        if (!g_fncount) return;
    }
    uintptr_t h = ((uintptr_t)n >> 3) * 2654435761u;
    for (unsigned k = 0; k < 8192; k++) {
        unsigned x = (unsigned)((h + k) & 8191u);
        if (!g_fnc[x].n) { g_fnc[x].n = n; g_fnc[x].c = 1; return; }
        if (g_fnc[x].n == n) { g_fnc[x].c++; return; }
    }
}
static void fnc_report(void) {
    for (int i = 0; i < 8192; i++)
        if (g_fnc[i].n) fprintf(stderr, "[fnc] %s %lu\n", g_fnc[i].n, g_fnc[i].c);
}
uint16_t g_wsel;
static struct { const char *n; uint16_t s; unsigned long c; } g_selw[4096];
static int g_nselw;
void catz_sel_write(uint16_t seg, uint16_t off, uint16_t val) {
    {   static int inited; static long lo = -1, hi = -1;
        if (!inited) { inited = 1;
            const char *a = getenv("CATZ_WATCH_LO"), *b = getenv("CATZ_WATCH_HI");
            if (a) lo = strtol(a, NULL, 16);
            if (b) hi = strtol(b, NULL, 16); }
        if (lo >= 0 && off >= lo && off <= hi) {
            static int nn;
            const char *fn = g_fn_ring[(g_fn_ring_pos - 1) & (CATZ_FN_RING_SIZE - 1)];
            if (nn++ < 4000)
                fprintf(stderr, "[w] %04X:%04X = %04X by %s\n", seg, off, val, fn ? fn : "?");
            /* first hit only: shows who is writing here */
            if (nn == 8)
                for (int q = 1; q <= 44; q++) {
                    const char *r = g_fn_ring[(g_fn_ring_pos - (unsigned)q) & (CATZ_FN_RING_SIZE - 1)];
                    fprintf(stderr, "[wring] %2d %s\n", q, r ? r : "?");
                }
        } }
    if (g_wsel != 0xFFFFu) {
        static int shown;
        if (!shown) { shown = 1;
            fprintf(stderr, "[selw] first write to %04X, ring tail (newest first):\n", seg);
            for (int k = 1; k <= 60; k++) {
                const char *r = g_fn_ring[(g_fn_ring_pos - (unsigned)k) & (CATZ_FN_RING_SIZE - 1)];
                fprintf(stderr, "   %2d %s\n", k, r ? r : "?");
            }
        }
    }
    const char *n = g_fn_ring[(g_fn_ring_pos - 1) & (CATZ_FN_RING_SIZE - 1)];
    if (!n) n = "?";
    if (g_wsel != 0xFFFFu) seg = 0;
    uintptr_t h = ((uintptr_t)n >> 3) * 31u + seg * 2654435761u;
    for (unsigned k = 0; k < 8192; k++) {
        unsigned x = (unsigned)((h + k) & 4095u);
        if (!g_selw[x].n) { g_selw[x].n = n; g_selw[x].s = seg; g_selw[x].c = 1; return; }
        if (g_selw[x].n == n && g_selw[x].s == seg) { g_selw[x].c++; return; }
    }
}
static void selw_report(void) {
    for (int r = 0; r < 300; r++) {
        int b = -1;
        for (int i = 0; i < 4096; i++)
            if (g_selw[i].n && g_selw[i].c && (b < 0 || g_selw[i].c > g_selw[b].c)) b = i;
        if (b < 0) break;
        fprintf(stderr, "[selw] %04X %-20s %lu\n", g_selw[b].s, g_selw[b].n, g_selw[b].c);
        g_selw[b].c = 0;
    }
}

int main(int argc, char *argv[])
{
    { const char *ws = getenv("CATZ_WATCH_SEL");
      if (ws) { g_wsel = (uint16_t)strtoul(ws, NULL, 16); atexit(selw_report); } }
    if (getenv("CATZ_FN_COUNT")) { g_fncount = 1; atexit(fnc_report); }
    if (getenv("CATZ_WATCH_DS")) g_watch_ds = 1;
    const char *img = (argc > 1) ? argv[1] : CATZ_IMAGE_PATH;

    /* Unbuffered: the engine dies by crash/abort often enough that a lost
     * stdout buffer hides exactly the lines that say where it got to. */
    setvbuf(stdout, NULL, _IONBF, 0);

    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    AddVectoredExceptionHandler(1, stack_overflow_veh);
#ifdef CATZ_WATCH_MEM
    catz_watch_init();
#endif

    CPU cpu;
    cpu_init(&cpu);
    g_cpu = &cpu;

    /* Flat image + a heap for dynamically allocated selectors (past the image).
     * 24 MB is period-appropriate (Catz targeted ~8 MB machines): it bounds the
     * engine's free-memory probe (GlobalAlloc-until-fail) to a realistic count
     * instead of the tens of thousands a huge heap would report. */
    uint32_t total = CATZ_IMAGE_SIZE + (24u << 20);  /* +24 MB heap */
    if (!cpu_alloc_mem(&cpu, total)) {
        fprintf(stderr, "Failed to allocate %u bytes\n", total);
        return 1;
    }
    if (!load_image(&cpu, img)) { cpu_free(&cpu); return 1; }

    /* selector -> flat base: NE segment index n maps to SEG_SEGMENT_BASE[n];
     * any other selector falls back to the guard region. */
    for (uint32_t s = 0; s < 0x10000; ++s)
        cpu.sel_base[s] = CATZ_GUARD_BASE;
    for (uint32_t n = 0; n <= CATZ_NUM_SEG; ++n)
        cpu.sel_base[n] = SEG_SEGMENT_BASE[n];

    printf("CATZ Recomp - starting\n");
    printf("  image: %s (%.2f MB)\n", img, CATZ_IMAGE_SIZE / 1048576.0);
    printf("  host entry: seg%u:%04X (CATZ.WAD)  dll-data seg%u  wad-data seg%u\n",
           CATZ_ENTRY_SEG, CATZ_ENTRY_IP, CATZ_DLL_AUTO_DATA_SEG, CATZ_AUTO_DATA_SEG);
    fflush(stdout);

    /* 1) Initialize the CATZDLL engine (LibMain) with the DLL's DGROUP in DS. */
    cpu.ds = cpu.es = CATZ_DLL_AUTO_DATA_SEG;
    cpu.ss = CATZ_STACK_SEG;
    cpu.sp = CATZ_STACK_SP ? CATZ_STACK_SP : 0xFFFE;
    cpu.cs = CATZ_DLL_ENTRY_SEG;
    /* Win16 DLL entry contract: DI = hInstance, DS = DGROUP, CX = the heap
       size from the NE header, ES:SI = command line. CX left at 0 made the
       DLL take its "jcxz" path and skip LocalInit, so it ran with no local
       heap at all. */
    cpu.di = CATZ_DLL_AUTO_DATA_SEG;        /* hInstance */
    cpu.cx = CATZ_DLL_HEAP_SIZE;
    cpu.si = 0;
    seg001_0000(&cpu);                      /* CATZDLL LibMain */
    printf("CATZDLL init returned (ax=%04X)\n", cpu.ax);

#ifdef CATZ_WATCHDOG
    CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
#else
    (void)watchdog_thread;
#endif

    /* 2) Run the CATZ.WAD host startup (Borland C0 -> WinMain): window +
     *    message loop, driving CATZDLL via the cross-module calls already
     *    resolved in the lifted code. WAD has its own DGROUP + stack. */
    cpu.ds = cpu.es = CATZ_AUTO_DATA_SEG;
    cpu.ss = CATZ_STACK_SEG;
    cpu.sp = CATZ_STACK_SP ? CATZ_STACK_SP : 0xFFFE;
    cpu.cs = CATZ_ENTRY_SEG;
    seg060_0000(&cpu);                      /* CATZ.WAD entry (host WinMain) */
    printf("CATZ.WAD host returned (ax=%04X)\n", cpu.ax);

    cpu_free(&cpu);
    return 0;
}

/* A branch whose target the decoder never resolved (see ne_lift). Reaching one
 * means we are executing a path the static recompile does not cover — say so
 * with a stack rather than wandering off. */
void catz_unreachable(const char *seg, unsigned off)
{
    fprintf(stderr, "\n[UNLIFTED] branch into seg%s:%04X has no lifted code\n", seg, off);
    dump_guest_stack(g_cpu, 40);
    _exit(97);
}

#ifdef CATZ_WATCH_MEM
uint16_t g_watch_seg = 0xFFFF, g_watch_off = 0xFFFF;

int g_watch_armed = 0;               /* gate: only report while a frame is live */

void catz_watch_hit(uint16_t seg, uint16_t off, uint16_t val)
{
    static int n = 0;
    if (!g_watch_armed) return;
    if (n++ >= 12) return;
    fprintf(stderr, "\n[WATCH] write %04X:%04X = %04X\n", seg, off, val);
    dump_guest_stack(g_cpu, 24);
}

void catz_watch_frame(uint16_t seg, uint16_t off, int on)
{
    if (on) { g_watch_seg = seg; g_watch_off = off; }
    g_watch_armed = on;
}

static void catz_watch_init(void)
{
    const char *w = getenv("CATZ_WATCH");
    unsigned s, o;
    if (w && sscanf(w, "%x:%x", &s, &o) == 2) {
        g_watch_seg = (uint16_t)s; g_watch_off = (uint16_t)o;
        /* Arming from the env var has to actually arm: the frame gate
           (catz_watch_frame) is for narrowing a stack slot to one frame, not a
           precondition for watching a heap address. */
        g_watch_armed = 1;
        fprintf(stderr, "[WATCH] armed on %04X:%04X\n", g_watch_seg, g_watch_off);
    }
}
#endif

/* DS is callee-saved in Win16 (a function that reloads DGROUP restores the
 * caller's DS with `pop ds`). A callee that returns with DS changed leaves the
 * caller building far pointers from a wrong — often null — selector. */
void catz_ds_broke(const char *callee, uint16_t ds0, uint16_t ds1)
{
    static int fired = 0;
    if (fired++ >= 6) return;
    fprintf(stderr, "[DS-BROKE] %s returned ds %04X->%04X\n", callee, ds0, ds1);
    if (fired == 1) dump_guest_stack(g_cpu, 30);
    fflush(stderr);
}
