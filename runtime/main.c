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

    /* Initial segment/register state from the NE header. */
    cpu.ss = CATZ_STACK_SEG ? CATZ_STACK_SEG : CATZ_AUTO_DATA_SEG;
    cpu.sp = CATZ_STACK_SP ? CATZ_STACK_SP : 0xFFFE;
    cpu.ds = CATZ_AUTO_DATA_SEG;
    cpu.es = CATZ_AUTO_DATA_SEG;
    cpu.cs = CATZ_ENTRY_SEG;

    printf("CATZ Recomp - starting\n");
    printf("  image: %s (%.2f MB)\n", img, CATZ_IMAGE_SIZE / 1048576.0);
    printf("  entry: seg%u:%04X  auto-data: seg%u\n",
           CATZ_ENTRY_SEG, CATZ_ENTRY_IP, CATZ_AUTO_DATA_SEG);
    fflush(stdout);

    seg001_0000(&cpu);  /* NE entry point (DLL init) */
    printf("entry returned (ax=%04X)\n", cpu.ax);

    /* --- Export-call bridge ---
     * Drive the engine through its exported XApt/CatSprite methods. A far export
     * is reached by pushing its PASCAL args (left-to-right) then a far return
     * frame; its RETF pops the frame (+args for `retf N`). Returns AX. */
    call_export(&cpu, seg045_0000, NULL, 0);   /* XApt IsWindowsNT (ord 1124) */
    printf("IsWindowsNT() -> %u\n", cpu.ax);

    cpu_free(&cpu);
    return 0;
}
