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

#ifndef CATZ_IMAGE_PATH
#define CATZ_IMAGE_PATH "build_data/mem_image.bin"
#endif

/* Function-entry ring buffer (declared in cpu.h, filled by TRACE_FN). */
const char *g_fn_ring[CATZ_FN_RING_SIZE];
unsigned g_fn_ring_pos = 0;

/* Global CPU pointer so the Win32 backend / WndProc bridge can invoke guest
 * code (the registered window procedure) on delivered messages. */
CPU *g_cpu = NULL;

void dump_fn_ring(int n)
{
    if (n <= 0 || n > (int)CATZ_FN_RING_SIZE) n = (int)CATZ_FN_RING_SIZE;
    fprintf(stderr, "--- last %d functions entered (oldest first) ---\n", n);
    for (int i = n; i > 0; i--) {
        unsigned idx = (g_fn_ring_pos - (unsigned)i) & (CATZ_FN_RING_SIZE - 1);
        const char *name = g_fn_ring[idx];
        if (name) fprintf(stderr, "  %s\n", name);
    }
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

int main(int argc, char *argv[])
{
    const char *img = (argc > 1) ? argv[1] : CATZ_IMAGE_PATH;

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
    seg001_0000(&cpu);                      /* CATZDLL LibMain */
    printf("CATZDLL init returned (ax=%04X)\n", cpu.ax);

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
