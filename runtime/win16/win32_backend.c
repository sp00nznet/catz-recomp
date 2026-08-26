/*
 * win32_backend.c - Back the Win16 USER/GDI window shims with real Win32.
 *
 * We run on Windows, so the recompiled 16-bit engine can drive a real window:
 *   - RegisterClass captures the guest's WNDPROC (a lifted seg:off) and
 *     registers a real Win32 class whose C WndProc bridges back into guest code.
 *   - CreateWindow makes a real top-level window; a small table maps the 16-bit
 *     guest HWND/HDC handles to the real ones.
 *   - GetMessage/DispatchMessage drive the real Win32 message pump; on each
 *     dispatched message Win32 calls host_wndproc, which calls the guest WNDPROC
 *     via the runtime far-call dispatcher (PASCAL args + far frame).
 *
 * Drawing (GDI/WinG) is layered on top as the engine calls it.
 */
#include "runtime_api.h"
#include "ne_resources.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

extern CPU *g_cpu;

/* arg helpers (shim entry stack: [sp]=retIP, [sp+2]=retCS, [sp+4]=last arg) */
static inline uint16_t b_a16(CPU *cpu, int off) {
    return mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4 + off));
}
static inline uint32_t b_a32(CPU *cpu, int off) {
    return (uint32_t)b_a16(cpu, off) | ((uint32_t)b_a16(cpu, off + 2) << 16);
}
static inline void b_ret(CPU *cpu, int purge) { cpu->sp += 4 + purge; }
static void b_asciiz(CPU *cpu, uint16_t seg, uint16_t off, char *out, int max);

/* ---- guest HWND/HDC handle tables (16-bit guest handle <-> real) ---- */
#define MAXH 256
static HWND g_hwnd[MAXH];
static HDC  g_hdc[MAXH];
static int  g_nhwnd = 1, g_nhdc = 1;   /* 0 reserved as NULL */

static void handle_table_full(const char *what) {
    static const char *told[4]; static int n;
    for (int i = 0; i < n; i++) if (told[i] == what) return;
    if (n < 4) told[n++] = what;
    fprintf(stderr, "[win32] %s handle table exhausted; the engine will see a creation failure\n", what);
}

static uint16_t put_hwnd(HWND h) {
    for (int i = 1; i < g_nhwnd; i++) if (g_hwnd[i] == h) return (uint16_t)i;
    if (g_nhwnd < MAXH) { g_hwnd[g_nhwnd] = h; return (uint16_t)g_nhwnd++; }
    handle_table_full("window");
    return 0;
}
static HWND get_hwnd(uint16_t g) { return (g && g < MAXH) ? g_hwnd[g] : NULL; }
/* Slots must be recycled: the engine takes a DC every frame, and a table that
 * only ever grows ran out within seconds -- put_hdc then returned 0 and the
 * engine reported "Closing already closed screen DC" for the rest of the run. */
static uint16_t put_hdc(HDC h) {
    if (!h) return 0;
    for (int i = 1; i < g_nhdc; i++) if (!g_hdc[i]) { g_hdc[i] = h; return (uint16_t)i; }
    if (g_nhdc < MAXH) { g_hdc[g_nhdc] = h; return (uint16_t)g_nhdc++; }
    handle_table_full("DC");
    return 0;
}
static void free_hdc(uint16_t g) { if (g && g < MAXH) g_hdc[g] = NULL; }
static HDC get_hdc(uint16_t g) { return (g && g < MAXH) ? g_hdc[g] : NULL; }

/* GDI objects (bitmaps so far) get their own guest handle space. */
#define MAXGDI 256
static HGDIOBJ g_gdi[MAXGDI];
static int g_ngdi = 1;
static uint16_t put_hgdi(HGDIOBJ h) {
    if (!h) return 0;
    for (int i = 1; i < g_ngdi; i++) if (!g_gdi[i]) { g_gdi[i] = h; return (uint16_t)i; }
    if (g_ngdi >= MAXGDI) { handle_table_full("GDI object"); return 0; }
    g_gdi[g_ngdi] = h; return (uint16_t)g_ngdi++;
}

static HGDIOBJ get_hgdi(uint16_t g) { return (g && g < MAXGDI) ? g_gdi[g] : NULL; }

/* ---- guest window classes ----
 * One global WNDPROC was wrong: CATZ.WAD and CATZDLL each register their own
 * class, and every window was being routed to whichever registered last. The
 * engine's own window therefore ran the host shell's WNDPROC, so its WM_PAINT --
 * the one that blits the WinG surface -- never ran. Key the proc by class name,
 * and remember which proc each HWND was created with. */
#define MAXCLS 32
typedef struct { char name[64]; uint16_t seg, off; } GuestClass;
static GuestClass g_cls[MAXCLS];
static int g_ncls;

/* Parallel to g_hwnd: the guest WNDPROC this window was created with. */
static uint16_t g_hwnd_proc_seg[MAXH], g_hwnd_proc_off[MAXH];

/* Last class registered; the fallback for a window whose class we never saw. */
static uint16_t g_wndproc_seg = 0, g_wndproc_off = 0;

/* The first window created is the application frame; only it ends the app. */
static HWND g_main_hwnd;
/* The engine makes two windows: a framed playpen and a 640x480 WS_POPUP that
   is its pet overlay -- the thing it would normally paint straight onto the
   desktop. GetDC(NULL) asks for that overlay, so it has to go to the popup;
   sending it to the framed playpen painted the pet layer's clear colour over
   the whole pen. */
#define CATZ_SCREEN_W 640
#define CATZ_SCREEN_H 480
static HWND g_screen_hwnd;

static GuestClass *cls_find(const char *name) {
    for (int i = 0; i < g_ncls; i++)
        if (!strcmp(g_cls[i].name, name)) return &g_cls[i];
    return NULL;
}

static void hwnd_proc(HWND h, uint16_t *seg, uint16_t *off) {
    for (int i = 1; i < g_nhwnd; i++)
        if (g_hwnd[i] == h) { *seg = g_hwnd_proc_seg[i]; *off = g_hwnd_proc_off[i]; return; }
    *seg = g_wndproc_seg; *off = g_wndproc_off;
}

/* Call the guest WNDPROC(hWnd, uMsg, wParam, lParam) via the far dispatcher.
 * Win16 PASCAL: push args left-to-right; lParam is a DWORD (hi word first). */
static uint16_t call_guest_wndproc(uint16_t pseg, uint16_t poff,
                                   uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam) {
    CPU *cpu = g_cpu;
    if (!pseg) return 0;
    /* The WNDPROC runs re-entrantly (CreateWindow delivers WM_NCCREATE/CREATE
     * synchronously while the guest is mid-call), so snapshot the FULL guest
     * register state and restore it afterwards. */
    CPU save = *cpu;
    /* The Borland C0 exception/cleanup machinery keeps THREE chain heads in the
     * task's stack segment: ss:[0x10] (CATZDLL cleanup chain), ss:[0x14] (WAD
     * cleanup chain) and ss:[0x16] (the C++ catch/setjmp chain). The WNDPROC runs
     * re-entrantly on the SAME stack and pushes its own frames onto these chains;
     * once we restore the interrupted guest's SP those frames no longer exist, so
     * every head must be put back -- otherwise a head points at a discarded frame
     * (or a foreign module's setjmp buffer) and the interrupted guest's later
     * chain walk derails into a runaway loop. Snapshot all three alongside the
     * CPU. (Restoring only ss:[0x14] left 0x10/0x16 dangling, which corrupted the
     * cross-module EH chain non-deterministically.) */
    uint16_t saved_exc_head_10 = mem_read16(cpu, cpu->ss, 0x10);
    uint16_t saved_exc_head    = mem_read16(cpu, cpu->ss, 0x14);
    uint16_t saved_exc_head_16 = mem_read16(cpu, cpu->ss, 0x16);
    cpu->ds = cpu->es = (pseg <= 59) ? CATZ_DLL_AUTO_DATA_SEG : CATZ_AUTO_DATA_SEG;
    push16(cpu, hwnd);
    push16(cpu, msg);
    push16(cpu, wparam);
    push16(cpu, (uint16_t)(lparam >> 16));
    push16(cpu, (uint16_t)(lparam & 0xFFFF));
    push16(cpu, cpu->cs); push16(cpu, 0);          /* far return frame */
    dispatch_far(cpu, pseg, poff);
    uint16_t ret = cpu->ax;
    /* restore everything except the heap/memory bookkeeping (which the WNDPROC
     * may have legitimately advanced via GlobalAlloc). */
    uint8_t *mem = cpu->mem; uint32_t *sel = cpu->sel_base;
    uint32_t hn = cpu->heap_next; uint16_t ns = cpu->next_sel;
    *cpu = save;
    cpu->mem = mem; cpu->sel_base = sel; cpu->heap_next = hn; cpu->next_sel = ns;
    /* Put the cleanup/catch chain heads back: the WNDPROC's frames no longer exist. */
    mem_write16(cpu, cpu->ss, 0x10, saved_exc_head_10);
    mem_write16(cpu, cpu->ss, 0x14, saved_exc_head);
    mem_write16(cpu, cpu->ss, 0x16, saved_exc_head_16);
    return ret;
}

/* Structural/creation messages carry host-only data (CREATESTRUCT/MINMAXINFO/
 * NCCALCSIZE far pointers the guest can't read) and must not be allowed to
 * fail creation, so we DefWindowProc them. Runtime messages (paint, timers,
 * input, command) are forwarded to the guest WNDPROC. */
static int forward_to_guest(UINT msg) {
    if (getenv("CATZ_NO_WNDPROC")) return 0;   /* diagnostic: never call guest */
    switch (msg) {
        case WM_NCCREATE: case WM_CREATE: case WM_NCCALCSIZE:
        case WM_GETMINMAXINFO: case WM_NCDESTROY: case WM_DESTROY:
        case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED:
        case WM_GETICON: case WM_NCPAINT: case WM_NCACTIVATE:
            return 0;
        default:
            return 1;
    }
}

static LRESULT CALLBACK host_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    /* Only the application's own frame ends the app. Quitting on any window's
       WM_DESTROY meant closing a modal dialog tore down the whole program --
       which is exactly what happened the first time a valid serial let the
       wizard actually finish a dialog. */
    if (msg == WM_DESTROY) {
        if (hWnd == g_main_hwnd) { PostQuitMessage(0); return 0; }
        return 0;
    }
    uint16_t pseg, poff;
    hwnd_proc(hWnd, &pseg, &poff);
    if (pseg && forward_to_guest(msg)) {
        uint16_t gh = put_hwnd(hWnd);
        LRESULT gr = call_guest_wndproc(pseg, poff, gh, (uint16_t)msg,
                                        (uint16_t)wParam, (uint32_t)lParam);
        /* The engine runs a shutdown chain on WM_CLOSE but never calls
           DestroyWindow or PostQuitMessage, so the window stayed up and the
           title-bar X did nothing. Let the guest run (it saves the pet), then
           apply the default: destroy the window. */
        if (msg == WM_CLOSE) return DefWindowProc(hWnd, msg, wParam, lParam);
        return gr;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

/* ===== USER: window class + creation ===== */

void USER_REGISTERCLASS(CPU *cpu) {
    /* Win16 WNDCLASS: style@0(2) lpfnWndProc@2(4 far) cbClsExtra@6 cbWndExtra@8
     * hInstance@10 hIcon@12 hCursor@14 hbrBackground@16 lpszMenuName@18(4)
     * lpszClassName@22(4). */
    uint16_t off = b_a16(cpu, 0), seg = b_a16(cpu, 2);
    uint16_t poff = mem_read16(cpu, seg, (uint16_t)(off + 2));
    uint16_t pseg = mem_read16(cpu, seg, (uint16_t)(off + 4));
    uint16_t noff = mem_read16(cpu, seg, (uint16_t)(off + 22));
    uint16_t nseg = mem_read16(cpu, seg, (uint16_t)(off + 24));

    char name[64] = "";
    if (nseg) b_asciiz(cpu, nseg, noff, name, sizeof name);
    if (!name[0]) snprintf(name, sizeof name, "CatzGuestCls%d", g_ncls);

    g_wndproc_seg = pseg; g_wndproc_off = poff;

    GuestClass *c = cls_find(name);
    if (!c && g_ncls < MAXCLS) {
        c = &g_cls[g_ncls++];
        snprintf(c->name, sizeof c->name, "%s", name);
        WNDCLASSA wc; memset(&wc, 0, sizeof wc);
        wc.style         = CS_OWNDC;
        wc.lpfnWndProc   = host_wndproc;
        wc.hInstance     = GetModuleHandle(NULL);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = c->name;
        RegisterClassA(&wc);
    }
    if (c) { c->seg = pseg; c->off = poff; }

    fprintf(stderr, "[win32] RegisterClass(\"%s\") -> guest WNDPROC seg%u:%04X\n",
            name, pseg, poff);
    cpu->ax = 0xC001;
    b_ret(cpu, 4);
}

void USER_CREATEWINDOW(CPU *cpu) {
    /* CreateWindow(lpClassName@26, lpWindowName@22, dwStyle@18, x@16, y@14,
     * nWidth@12, nHeight@10, hWndParent@8, hMenu@6, hInstance@4, lpParam@0).
     * Every argument used to be ignored, so the engine's window was created as
     * another top-level 640x480 frame under the shell's class -- wrong size,
     * wrong parent, and wrong WNDPROC. */
    if (getenv("CATZ_FAKE_WIN")) { cpu->ax = 0x0CA7; b_ret(cpu, 30); return; }

    uint16_t coff = b_a16(cpu, 26), cseg = b_a16(cpu, 28);
    uint16_t toff = b_a16(cpu, 22), tseg = b_a16(cpu, 24);
    uint32_t style = b_a32(cpu, 18);
    int x = (int16_t)b_a16(cpu, 16), y = (int16_t)b_a16(cpu, 14);
    int w = (int16_t)b_a16(cpu, 12), h = (int16_t)b_a16(cpu, 10);
    HWND parent = get_hwnd(b_a16(cpu, 8));

    char cname[64] = "", title[128] = "Catz (recomp)";
    if (cseg) b_asciiz(cpu, cseg, coff, cname, sizeof cname);
    if (tseg) b_asciiz(cpu, tseg, toff, title, sizeof title);

    GuestClass *c = cname[0] ? cls_find(cname) : NULL;
    const char *hostcls = c ? c->name : (g_ncls ? g_cls[0].name : "CatzGuestCls0");

    /* CW_USEDEFAULT is 0x8000 in Win16 and a different value in Win32. */
    if ((uint16_t)x == 0x8000) x = CW_USEDEFAULT;
    if ((uint16_t)y == 0x8000) y = CW_USEDEFAULT;
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    /* A child needs a parent; without WS_CHILD force a top-level frame. */
    if (!(style & WS_CHILD)) parent = NULL;
    else if (!parent) style &= ~(uint32_t)WS_CHILD;

    /* The engine makes two top-level windows: the framed playpen, and a 640x480
       WS_POPUP it immediately hides because it means to paint the bare desktop.
       Showing both left two overlapping surfaces with different content, which
       looks exactly like the pet smearing. Keep the pet indoors instead: give
       the frame a client area of the 640x480 we already report through
       GetClientRect, and make the overlay a child filling it. One window, one
       drawing surface. CATZ_DESKTOP=1 keeps the original two-window layout. */
    int as_child = 0;
    if (!getenv("CATZ_DESKTOP") && g_main_hwnd && !g_screen_hwnd
        && (style & WS_POPUP) && !(style & WS_CHILD)) {
        as_child = 1;
        style  = WS_CHILD | WS_VISIBLE;
        parent = g_main_hwnd;
        x = y = 0;
        w = CATZ_SCREEN_W;
        h = CATZ_SCREEN_H;
    }

    HWND hw = CreateWindowExA(0, hostcls, title, (DWORD)style, x, y, w, h,
                              parent, NULL, GetModuleHandle(NULL), NULL);
    uint16_t gh = hw ? put_hwnd(hw) : 0;
    if (hw && !g_main_hwnd) {
        g_main_hwnd = hw;
        if (!getenv("CATZ_DESKTOP")) {      /* client area == the engine's screen */
            RECT rc = { 0, 0, CATZ_SCREEN_W, CATZ_SCREEN_H };
            AdjustWindowRect(&rc, (DWORD)style, FALSE);
            SetWindowPos(hw, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (hw && !g_screen_hwnd
        && (as_child || ((style & WS_POPUP) && !(style & WS_CHILD))))
        g_screen_hwnd = hw;
    if (gh) {
        g_hwnd_proc_seg[gh] = c ? c->seg : g_wndproc_seg;
        g_hwnd_proc_off[gh] = c ? c->off : g_wndproc_off;
    }
    fprintf(stderr, "[win32] CreateWindow(\"%s\" title=\"%s\" style=%08lX %dx%d parent=%p) "
                    "-> real=%p guest=%u proc=seg%u:%04X err=%lu\n",
            cname, title, (unsigned long)style, w, h, (void *)parent, (void *)hw, gh,
            gh ? g_hwnd_proc_seg[gh] : 0, gh ? g_hwnd_proc_off[gh] : 0,
            hw ? 0UL : (unsigned long)GetLastError());
    cpu->ax = gh;
    b_ret(cpu, 30);
}

/* The guest WNDPROC passes everything it does not handle to DefWindowProc,
 * which was a stub returning 0. WM_CLOSE is one of those: nothing called
 * DestroyWindow, so clicking the playpen's X did nothing at all. Hand the
 * message to the real DefWindowProc. Win16 window procs return a LONG in DX:AX.
 * Args are PASCAL: hwnd, msg, wParam, lParam. */
void USER_DEFWINDOWPROC(CPU *cpu) {
    uint16_t gh   = b_a16(cpu, 8);
    UINT     msg  = b_a16(cpu, 6);
    WPARAM   wp   = b_a16(cpu, 4);
    LPARAM   lp   = (LPARAM)(int32_t)b_a32(cpu, 0);
    HWND     h    = get_hwnd(gh);
    LRESULT  r    = h ? DefWindowProc(h, msg, wp, lp) : 0;
    cpu->ax = (uint16_t)(r & 0xFFFF);
    cpu->dx = (uint16_t)((uint32_t)r >> 16);
    b_ret(cpu, 10);
}

/* Both of these were stubs, so nothing could ever close: the engine's WM_CLOSE
 * handler called DestroyWindow, it did nothing, and the window stayed up. */
void USER_DESTROYWINDOW(CPU *cpu) {
    uint16_t g = b_a16(cpu, 0);
    HWND h = get_hwnd(g);
    if (h) { DestroyWindow(h); if (g < MAXH) g_hwnd[g] = NULL; }
    cpu->ax = 1;
    b_ret(cpu, 2);
}

void USER_POSTQUITMESSAGE(CPU *cpu) {
    PostQuitMessage((int)(int16_t)b_a16(cpu, 0));
    cpu->ax = 0;
    b_ret(cpu, 2);
}

/* nCmdShow was ignored and every call forced SW_SHOW, so a window the engine
 * asked to HIDE stayed up. It hides the 640x480 pet overlay right after
 * creating it -- leaving that visible put a second window full of older frames
 * on top of the playpen, which is indistinguishable from the pet smearing.
 * Win16 SW_* values match Win32, so pass it straight through. */
void USER_SHOWWINDOW(CPU *cpu) {          /* (hwnd@2, nCmdShow@0) */
    HWND h = get_hwnd(b_a16(cpu, 2));
    int cmd = (int)(int16_t)b_a16(cpu, 0);
    /* One deliberate exception: the engine hides the 640x480 popup because it
       wants to paint the bare desktop instead. We have repurposed exactly that
       window as the pet's screen (GetDC(NULL) returns its DC), so keeping the
       pet indoors means keeping it shown. Everything else is honoured. */
    if (h && h == g_screen_hwnd && cmd == SW_HIDE && !getenv("CATZ_DESKTOP"))
        cmd = SW_SHOWNA;                  /* show it, but do not steal focus */
    cpu->ax = (uint16_t)(h ? ShowWindow(h, cmd) : 0);
    b_ret(cpu, 4);
}

void USER_UPDATEWINDOW(CPU *cpu) {
    HWND h = get_hwnd(b_a16(cpu, 0));
    if (h) UpdateWindow(h);
    cpu->ax = 0; b_ret(cpu, 2);
}

/* ---- GDI objects ----
 * These were stubs returning 0. The engine treats a null GDI handle as fatal:
 * XDrawPort's brush creation throws "exception 2" at WDRAW.CPP:1935 the moment
 * CreateSolidBrush comes back 0, which is where the first real frame died. */
void GDI_CREATESOLIDBRUSH(CPU *cpu) {          /* (COLORREF@0) */
    COLORREF c = (COLORREF)b_a32(cpu, 0);
    cpu->ax = put_hgdi(CreateSolidBrush(c));
    b_ret(cpu, 4);
}

void GDI_CREATEPEN(CPU *cpu) {                 /* (style@6, width@4, colour@0) */
    int style = (int16_t)b_a16(cpu, 6);
    int width = (int16_t)b_a16(cpu, 4);
    COLORREF c = (COLORREF)b_a32(cpu, 0);
    cpu->ax = put_hgdi(CreatePen(style, width, c));
    b_ret(cpu, 8);
}

void GDI_GETSTOCKOBJECT(CPU *cpu) {            /* (index@0) */
    cpu->ax = put_hgdi(GetStockObject((int)(int16_t)b_a16(cpu, 0)));
    b_ret(cpu, 2);
}

void GDI_SELECTOBJECT(CPU *cpu) {              /* (hdc@2, hgdiobj@0) -> previous */
    uint16_t gdc = b_a16(cpu, 2), gob = b_a16(cpu, 0);
    /* A WinG surface is not a real GDI object here; selecting one just records
       which surface that WinG DC draws to and blits from. */
    extern int wing_is_dc(uint16_t);
    extern int wing_is_bmp(uint16_t);
    extern uint16_t wing_select(uint16_t, uint16_t);
    if (wing_is_dc(gdc) && wing_is_bmp(gob)) {
        cpu->ax = wing_select(gdc, gob);
        b_ret(cpu, 4);
        return;
    }
    HDC dc = get_hdc(gdc);
    HGDIOBJ o = get_hgdi(gob);
    HGDIOBJ prev = (dc && o) ? SelectObject(dc, o) : NULL;
    cpu->ax = prev ? put_hgdi(prev) : 0;
    b_ret(cpu, 4);
}

void GDI_DELETEOBJECT(CPU *cpu) {              /* (hgdiobj@0) */
    uint16_t g = b_a16(cpu, 0);
    HGDIOBJ o = get_hgdi(g);
    if (o) { DeleteObject(o); g_gdi[g] = NULL; }
    cpu->ax = 1;
    b_ret(cpu, 2);
}

void GDI_CREATECOMPATIBLEDC(CPU *cpu) {        /* (hdc@0) */
    HDC dc = get_hdc(b_a16(cpu, 0));
    HDC mem = CreateCompatibleDC(dc);
    cpu->ax = mem ? put_hdc(mem) : 0;
    b_ret(cpu, 2);
}

void GDI_DELETEDC(CPU *cpu) {                  /* (hdc@0) */
    uint16_t g = b_a16(cpu, 0);
    HDC dc = get_hdc(g);
    if (dc) DeleteDC(dc);
    free_hdc(g);
    cpu->ax = 1;
    b_ret(cpu, 2);
}

void GDI_CREATECOMPATIBLEBITMAP(CPU *cpu) {    /* (hdc@4, w@2, h@0) */
    HDC dc = get_hdc(b_a16(cpu, 4));
    int w = (int16_t)b_a16(cpu, 2), h = (int16_t)b_a16(cpu, 0);
    HBITMAP bm = dc ? CreateCompatibleBitmap(dc, w, h) : NULL;
    cpu->ax = bm ? put_hgdi(bm) : 0;
    b_ret(cpu, 6);
}

void GDI_PATBLT(CPU *cpu) {                    /* (hdc@12,x@10,y@8,w@6,h@4,rop@0) */
    HDC dc = get_hdc(b_a16(cpu, 12));
    int x = (int16_t)b_a16(cpu, 10), y = (int16_t)b_a16(cpu, 8);
    int w = (int16_t)b_a16(cpu, 6), h = (int16_t)b_a16(cpu, 4);
    DWORD rop = b_a32(cpu, 0);
    cpu->ax = (uint16_t)(dc ? PatBlt(dc, x, y, w, h, rop) : 0);
    b_ret(cpu, 14);
}

void USER_FILLRECT(CPU *cpu) {                 /* (hdc@6, lpRect@2/4, hbr@0) */
    HDC dc = get_hdc(b_a16(cpu, 6));
    uint16_t roff = b_a16(cpu, 2), rseg = b_a16(cpu, 4);
    HBRUSH br = (HBRUSH)get_hgdi(b_a16(cpu, 0));
    if (dc && rseg && br) {
        RECT r;
        r.left   = (int16_t)mem_read16(cpu, rseg, roff);
        r.top    = (int16_t)mem_read16(cpu, rseg, (uint16_t)(roff + 2));
        r.right  = (int16_t)mem_read16(cpu, rseg, (uint16_t)(roff + 4));
        r.bottom = (int16_t)mem_read16(cpu, rseg, (uint16_t)(roff + 6));
        FillRect(dc, &r, br);
    }
    cpu->ax = 1;
    b_ret(cpu, 8);
}

/* SetViewportOrgEx(hdc, X, Y, lpPoint) sets the DC's viewport origin and hands
 * the PREVIOUS one back through lpPoint. XDrawPort::XCopyBits uses it exactly
 * that way: it zeroes the origin, then offsets its destination rect by the old
 * value so the rect is in device coordinates for the WinG blit that follows.
 *
 * As a stub this popped its arguments and wrote nothing, so lpPoint kept
 * whatever was on the stack -- and the engine offset the pet's destination rect
 * by that junk. It sat next to the rect locals, so the garbage was usually the
 * previous rect's bottom: the pet was stamped `bottom` pixels too low, by a
 * different amount every frame as the frame height changed, which is what
 * smeared cat faces across the screen. */
/* The remaining out-parameter APIs the engine reaches. A stub that pops its
 * arguments but never writes through its pointer leaves the caller reading its
 * own stack -- that is how SetViewportOrgEx smeared the pet across the screen,
 * so fill these in rather than leave the same trap set. */
void GDI_GETOBJECT(CPU *cpu) {             /* (hgdiobj@6, cb@4, lpv@0/2) */
    HGDIOBJ o = get_hgdi(b_a16(cpu, 6));
    int cb = (int16_t)b_a16(cpu, 4);
    uint16_t off = b_a16(cpu, 0), seg = b_a16(cpu, 2);
    BITMAP bm;
    if (o && seg && cb >= 14 && GetObject(o, sizeof bm, &bm)) {
        /* Win16 BITMAP: bmType, bmWidth, bmHeight, bmWidthBytes (int16 each),
           bmPlanes, bmBitsPixel (bytes), bmBits (far pointer). */
        mem_write16(cpu, seg, off,                    (uint16_t)bm.bmType);
        mem_write16(cpu, seg, (uint16_t)(off + 2),    (uint16_t)bm.bmWidth);
        mem_write16(cpu, seg, (uint16_t)(off + 4),    (uint16_t)bm.bmHeight);
        mem_write16(cpu, seg, (uint16_t)(off + 6),    (uint16_t)bm.bmWidthBytes);
        mem_write8 (cpu, seg, (uint16_t)(off + 8),    (uint8_t)bm.bmPlanes);
        mem_write8 (cpu, seg, (uint16_t)(off + 9),    (uint8_t)bm.bmBitsPixel);
        mem_write16(cpu, seg, (uint16_t)(off + 10),   0);
        mem_write16(cpu, seg, (uint16_t)(off + 12),   0);
        cpu->ax = 14;
    } else {
        cpu->ax = 0;
    }
    b_ret(cpu, 8);
}

void MMSYSTEM_TIMEGETDEVCAPS(CPU *cpu) {   /* (lpTimeCaps@2/4, cb@0) */
    uint16_t off = b_a16(cpu, 2), seg = b_a16(cpu, 4);
    if (seg) {                              /* TIMECAPS { wPeriodMin, wPeriodMax } */
        mem_write16(cpu, seg, off, 1);
        mem_write16(cpu, seg, (uint16_t)(off + 2), 0xFFFF);
    }
    cpu->ax = 0;                            /* TIMERR_NOERROR */
    b_ret(cpu, 6);
}

void USER_GETDLGITEMINT(CPU *cpu) {        /* (hwnd@8, id@6, lpXlated@2/4, sgn@0) */
    uint16_t off = b_a16(cpu, 2), seg = b_a16(cpu, 4);
    if (seg) mem_write16(cpu, seg, off, 0); /* not translated */
    cpu->ax = 0;
    b_ret(cpu, 10);
}

/* GetCursorPos(LPPOINT) is another out-parameter API the engine calls every
 * frame -- it is how the pet knows where the mouse is. As a stub it wrote
 * nothing, so the engine read whatever was on the stack as the cursor position.
 * Report it in the same space as the DC we hand out for GetDC(NULL): the pet
 * overlay window's client area. */
void USER_GETCURSORPOS(CPU *cpu) {         /* (lpPoint@0/2) */
    uint16_t off = b_a16(cpu, 0), seg = b_a16(cpu, 2);
    POINT p = { 0, 0 };
    /* CATZ_CURSOR=x,y pins the pointer where the engine thinks it is, so a
       click can be aimed at a widget without commandeering the real mouse. */
    const char *fc = getenv("CATZ_CURSOR");
    if (fc) {
        p.x = atoi(fc);
        const char *c = strchr(fc, ',');
        p.y = c ? atoi(c + 1) : 0;
    } else {
        GetCursorPos(&p);
        HWND h = g_screen_hwnd ? g_screen_hwnd : g_main_hwnd;
        if (h) ScreenToClient(h, &p);
    }
    if (seg) {
        mem_write16(cpu, seg, off, (uint16_t)(int16_t)p.x);
        mem_write16(cpu, seg, (uint16_t)(off + 2), (uint16_t)(int16_t)p.y);
    }
    cpu->ax = 0;
    b_ret(cpu, 4);
}

void GDI_SETVIEWPORTORGEX(CPU *cpu) {      /* (hdc@8, X@6, Y@4, lpPoint@0/2) */
    HDC dc = get_hdc(b_a16(cpu, 8));
    int x = (int16_t)b_a16(cpu, 6), y = (int16_t)b_a16(cpu, 4);
    uint16_t off = b_a16(cpu, 0), seg = b_a16(cpu, 2);
    POINT old = { 0, 0 };
    if (dc) SetViewportOrgEx(dc, x, y, &old);
    if (seg) {
        mem_write16(cpu, seg, off, (uint16_t)(int16_t)old.x);
        mem_write16(cpu, seg, (uint16_t)(off + 2), (uint16_t)(int16_t)old.y);
    }
    cpu->ax = 1;
    b_ret(cpu, 10);
}

void USER_GETDC(CPU *cpu) {
    uint16_t gw = b_a16(cpu, 0);
    HWND h = get_hwnd(gw);
    /* GetDC(NULL) is the SCREEN dc, not an error -- and it is what the engine
     * asks for: Catz walk on the desktop, so OpenScreenDrawPort wants the real
     * screen. Rejecting it left that port with no DC and every frame reported
     * "Closing already closed screen DC" instead of blitting.
     *
     * Painting the desktop directly is genuine Catz behaviour but it scribbles
     * over whatever else is on screen, so keep the pet inside the playpen window
     * by default and hand back the frame window's DC. Set CATZ_DESKTOP=1 for the
     * original loose-on-the-desktop behaviour. */
    if (!gw && !getenv("CATZ_DESKTOP")) {
        HWND alt = g_screen_hwnd ? g_screen_hwnd : g_main_hwnd;
        if (alt) h = alt;
    }
    HDC dc = GetDC(h);
    cpu->ax = dc ? put_hdc(dc) : 0;
    b_ret(cpu, 2);
}

/* BeginPaint/EndPaint. As stubs these returned a null HDC, so the engine's
 * WM_PAINT handler bailed AND the update region was never validated -- Windows
 * re-posted WM_PAINT forever and the engine did nothing else. Win16 PAINTSTRUCT
 * is 16-bit throughout: hdc@0, fErase@2, rcPaint@4 (4 x int16), fRestore@12,
 * fIncUpdate@14, rgbReserved@16..31. */
/* InvalidateRect is how the engine asks for its next frame. As a stub it did
 * nothing, so after the initial paints the window was never dirtied again and
 * the engine sat in GetMessage forever. NULL lpRect means the whole client. */
/* The engine's frame clock. As a stub returning 0 it created no timer, so after
 * init the engine sat in GetMessage with nothing ever waking it -- everything
 * ran once and then stopped. A NULL lpTimerFunc means "post WM_TIMER to the
 * window", which the WndProc bridge already delivers; a non-NULL one would need
 * a callback thunk, so say so rather than silently dropping it. */
/* The engine drives its own frame pipeline by posting WM_CATZ_WINTERFACE to its
 * parent window on every timer tick, so a stubbed PostMessage means the pipeline
 * never advances past the tick that requests it. */
/* DialogBoxParam. The Catz playpen IS a dialog: the engine's whole frame
 * pipeline -- its own SetTimer, its WM_TIMER tick and its WM_PAINT (the one that
 * blits the WinG surface) -- lives in the DLGPROC passed here, not in the host
 * shell's WNDPROC. The old stub returned 0 immediately, so WM_INITDIALOG never
 * fired, the engine's timer was never created, and nothing ever rendered; the
 * caller (seg062_07B6) just re-created the dialog forever.
 *
 * ponytail: the Win16 DLGTEMPLATE is not converted to a Win32 one, so the
 * dialog is created as a plain window running the guest DLGPROC and no child
 * controls exist. That is enough for the playpen, which draws itself; add real
 * template conversion when a dialog's controls have to be interacted with.
 * Modal, per Win16: it pumps the whole queue until EndDialog. */
static int      g_dlg_depth;
static int      g_dlg_ended[8];
static uint16_t g_dlg_result[8];
static HWND     g_dlg_hwnd[8];

/* Dialog item text. No child controls exist (no template conversion), so the
 * engine's SetDlgItemText went nowhere and the matching GetDlgItemText came back
 * empty -- which is why the startup wizard's name screen never validated and
 * re-showed itself forever. Keep the text in a small store so it round-trips
 * exactly as a real edit control would. */
#define MAXDLGTXT 64
typedef struct { uint16_t hdlg, id; char text[128]; } DlgText;
static DlgText g_dlgtxt[MAXDLGTXT];
static int g_ndlgtxt;

static DlgText *dlgtxt(uint16_t hdlg, uint16_t id, int create) {
    for (int i = 0; i < g_ndlgtxt; i++)
        if (g_dlgtxt[i].hdlg == hdlg && g_dlgtxt[i].id == id) return &g_dlgtxt[i];
    if (!create || g_ndlgtxt >= MAXDLGTXT) return NULL;
    DlgText *t = &g_dlgtxt[g_ndlgtxt++];
    t->hdlg = hdlg; t->id = id; t->text[0] = 0;
    return t;
}

void USER_SETDLGITEMTEXT(CPU *cpu) {         /* (hDlg@6, nIDDlgItem@4, lpString@0/2) */
    uint16_t hdlg = b_a16(cpu, 6), id = b_a16(cpu, 4);
    uint16_t soff = b_a16(cpu, 0), sseg = b_a16(cpu, 2);
    char buf[128] = "";
    if (sseg) b_asciiz(cpu, sseg, soff, buf, sizeof buf);
    HWND c = GetDlgItem(get_hwnd(hdlg), id);
    if (c) SetWindowTextA(c, buf);            /* a real control now exists */
    DlgText *t = dlgtxt(hdlg, id, 1);         /* mirror it for items that do not */
    if (t) snprintf(t->text, sizeof t->text, "%s", buf);
    cpu->ax = 1;
    b_ret(cpu, 8);
}

void USER_GETDLGITEMTEXT(CPU *cpu) {         /* (hDlg@8, nID@6, lpString@2/4, nMax@0) */
    uint16_t hdlg = b_a16(cpu, 8), id = b_a16(cpu, 6);
    uint16_t soff = b_a16(cpu, 2), sseg = b_a16(cpu, 4);
    uint16_t nmax = b_a16(cpu, 0);
    char live[128] = "";
    HWND c = GetDlgItem(get_hwnd(hdlg), id);
    if (c) GetWindowTextA(c, live, sizeof live);
    DlgText *t = dlgtxt(hdlg, id, 0);
    const char *src = live[0] ? live : (t ? t->text : "");
    uint16_t n = 0;
    if (sseg && nmax) {
        for (; src[n] && n + 1 < nmax; n++)
            mem_write8(cpu, sseg, (uint16_t)(soff + n), (uint8_t)src[n]);
        mem_write8(cpu, sseg, (uint16_t)(soff + n), 0);
    }
    cpu->ax = n;
    b_ret(cpu, 10);
}

void USER_GETDLGITEM(CPU *cpu) {             /* (hDlg@2, nIDDlgItem@0) */
    /* No real control window, but returning 0 makes callers treat the item as
     * missing and bail. Hand back a stable non-zero pseudo-handle per id. */
    uint16_t hdlg = b_a16(cpu, 2), id = b_a16(cpu, 0);
    HWND c = GetDlgItem(get_hwnd(hdlg), id);
    cpu->ax = c ? put_hwnd(c) : (uint16_t)(0xD000 | (id & 0x0FFF));
    b_ret(cpu, 4);
}

void USER_ENDDIALOG(CPU *cpu) {
    HWND h        = get_hwnd(b_a16(cpu, 2));   /* hDlg@2 */
    uint16_t code = b_a16(cpu, 0);             /* nResult@0 */
    for (int i = g_dlg_depth - 1; i >= 0; i--) {
        if (h && g_dlg_hwnd[i] != h) continue;
        g_dlg_ended[i] = 1; g_dlg_result[i] = code;
        break;
    }
    fprintf(stderr, "[win32] EndDialog(result=%u)\n", code);
    cpu->ax = 1;
    b_ret(cpu, 4);
}

/* ---- Win16 DLGTEMPLATE -> real child controls ----
 * Rather than repack the template into a Win32 DLGTEMPLATE (aligned, Unicode,
 * a different layout end to end), parse the Win16 one and create each item with
 * CreateWindowEx against the matching predefined class. The dialog window itself
 * is ours already, so this is all that was missing: with no controls the engine
 * could set no text, read none back, and no button could ever be pressed.
 *
 *   DLGTEMPLATE: DWORD style; BYTE nItems; WORD x,y,cx,cy;
 *                sz menu; sz class; sz caption;
 *                [DS_SETFONT: WORD pointSize; sz typeface]
 *   DLGITEMTEMPLATE: WORD x,y,cx,cy; WORD id; DWORD style;
 *                    class (sz, or one byte 0x80..0x85); sz text; BYTE cbExtra
 */
#define DS_SETFONT_W16 0x40

static const uint8_t *w16_sz(const uint8_t *p, const uint8_t *end, char *out, int max) {
    int i = 0;
    while (p < end && *p) { if (i < max - 1) out[i++] = (char)*p; p++; }
    out[i] = 0;
    return (p < end) ? p + 1 : end;
}

static const char *w16_ctl_class(uint8_t ord) {
    switch (ord) {
        case 0x80: return "Button";
        case 0x81: return "Edit";
        case 0x82: return "Static";
        case 0x83: return "ListBox";
        case 0x84: return "ScrollBar";
        case 0x85: return "ComboBox";
        default:   return NULL;
    }
}

/* Returns the dialog's own size in dialog units converted to pixels, or 0. */
static int dlg_build(CPU *cpu, uint16_t hinst, uint16_t tmpl, HWND dlg,
                     int *out_w, int *out_h, char *cap, int capsz) {
    uint16_t hr = ne_find_resource(hinst, 5 /* RT_DIALOG */, NULL, (int)tmpl, NULL);
    uint32_t len = 0;
    const uint8_t *p = hr ? ne_resource_bytes(hr, &len) : NULL;
    if (!p || len < 14) return 0;
    const uint8_t *end = p + len;

    uint32_t style = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                   | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    int nitems = p[4];
    int cx = p[9] | (p[10] << 8), cy = p[11] | (p[12] << 8);
    p += 13;

    char buf[128];
    p = w16_sz(p, end, buf, sizeof buf);          /* menu    */
    p = w16_sz(p, end, buf, sizeof buf);          /* class   */
    p = w16_sz(p, end, cap, capsz);               /* caption */
    if (style & DS_SETFONT_W16) {
        if (p + 2 <= end) p += 2;                 /* point size */
        p = w16_sz(p, end, buf, sizeof buf);      /* typeface   */
    }

    /* Dialog units -> pixels using the system dialog base units. */
    LONG bu = GetDialogBaseUnits();
    int bux = LOWORD(bu), buy = HIWORD(bu);
    *out_w = cx * bux / 4;
    *out_h = cy * buy / 8;

    int made = 0;
    for (int i = 0; i < nitems && p + 14 <= end; i++) {
        int ix = p[0] | (p[1] << 8), iy = p[2] | (p[3] << 8);
        int iw = p[4] | (p[5] << 8), ih = p[6] | (p[7] << 8);
        uint16_t id = (uint16_t)(p[8] | (p[9] << 8));
        uint32_t istyle = (uint32_t)p[10] | ((uint32_t)p[11] << 8)
                        | ((uint32_t)p[12] << 16) | ((uint32_t)p[13] << 24);
        p += 14;

        char cls[64] = "", text[128] = "";
        if (p < end && *p >= 0x80 && *p <= 0x85) {
            const char *k = w16_ctl_class(*p);
            snprintf(cls, sizeof cls, "%s", k ? k : "Static");
            p++;
        } else {
            p = w16_sz(p, end, cls, sizeof cls);
        }
        p = w16_sz(p, end, text, sizeof text);
        if (p < end) p += 1 + *p;                 /* cbExtra + creation data */

        HWND c = CreateWindowExA(0, cls, text,
                                 (DWORD)(istyle | WS_CHILD) & ~(DWORD)WS_POPUP,
                                 ix * bux / 4, iy * buy / 8,
                                 iw * bux / 4, ih * buy / 8,
                                 dlg, (HMENU)(UINT_PTR)id,
                                 GetModuleHandle(NULL), NULL);
        if (c) made++;
    }
    fprintf(stderr, "[win32] dialog %u template: %d/%d controls, %dx%d dlgunits "
                    "-> %dx%d px\n", tmpl, made, nitems, cx, cy, *out_w, *out_h);
    return 1;
}

void USER_DIALOGBOXPARAM(CPU *cpu) {
    uint16_t tmpl  = b_a16(cpu, 10);           /* lpTemplateName@10/12 (an id) */
    HWND parent    = get_hwnd(b_a16(cpu, 8));  /* hWndParent@8 */
    uint16_t poff  = b_a16(cpu, 4);            /* lpDialogFunc@4/6 */
    uint16_t pseg  = b_a16(cpu, 6);
    uint32_t param = b_a32(cpu, 0);            /* dwInitParam@0 */

    if (g_dlg_depth >= (int)(sizeof g_dlg_ended / sizeof g_dlg_ended[0]) || !pseg) {
        cpu->ax = 0; b_ret(cpu, 16); return;
    }
    int d = g_dlg_depth++;
    g_dlg_ended[d] = 0; g_dlg_result[d] = 0;

    RECT rc = { 0, 0, 470, 370 };
    if (parent) GetClientRect(parent, &rc);
    if (rc.right - rc.left < 16 || rc.bottom - rc.top < 16) { rc.right = 470; rc.bottom = 370; }

    const char *hostcls = g_ncls ? g_cls[0].name : "CatzGuestCls0";
    HWND h = CreateWindowExA(0, hostcls, "Catz", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, GetModuleHandle(NULL), NULL);
    if (h) {
        int tw = 0, th = 0;
        char cap[128] = "";
        if (dlg_build(cpu, b_a16(cpu, 14), tmpl, h, &tw, &th, cap, sizeof cap)
                && tw > 16 && th > 16) {
            if (cap[0]) SetWindowTextA(h, cap);
            RECT want = { 0, 0, tw, th };
            AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(h, NULL, 0, 0, want.right - want.left, want.bottom - want.top,
                         SWP_NOMOVE | SWP_NOZORDER);
        }
    }
    uint16_t gh = h ? put_hwnd(h) : 0;
    if (gh) { g_hwnd_proc_seg[gh] = pseg; g_hwnd_proc_off[gh] = poff; }
    g_dlg_hwnd[d] = h;
    fprintf(stderr, "[win32] DialogBoxParam(template=%u dlgproc=seg%u:%04X) "
                    "-> hwnd=%p guest=%u %dx%d\n",
            tmpl, pseg, poff, (void *)h, gh,
            (int)(rc.right - rc.left), (int)(rc.bottom - rc.top));

    if (h) {
        ShowWindow(h, SW_SHOW);
        /* WM_INITDIALOG is what the engine hangs its whole setup off. */
        call_guest_wndproc(pseg, poff, gh, 0x0110, gh, param);
        UpdateWindow(h);
        /* Bring-up knob: with no dialog controls nothing can press a button, so
           CATZ_DLG_RESULT lets a run answer every dialog with one code and walk
           the startup wizard forward (0x7D4 is its "Next"). Off by default. */
        const char *forced = getenv("CATZ_DLG_RESULT");
        if (forced) {
            /* Comma-separated: one answer per dialog shown, in order; the last
               value repeats. Enough to script a walk through the wizard. */
            static int seen = 0;
            const char *q = forced;
            for (int k = 0; k < seen; k++) {
                const char *c = strchr(q, (int)44);
                if (!c) break;
                q = c + 1;
            }
            seen++;
            g_dlg_ended[d] = 1;
            g_dlg_result[d] = (uint16_t)strtoul(q, NULL, 0);
            fprintf(stderr, "[win32] dialog %u auto-answered %u\n",
                    tmpl, g_dlg_result[d]);
        }
        MSG m;
        while (!g_dlg_ended[d] && GetMessageA(&m, NULL, 0, 0)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        fprintf(stderr, "[win32] dialog loop exit: ended=%d (0 means WM_QUIT)\n",
                g_dlg_ended[d]);
        DestroyWindow(h);
    }
    g_dlg_depth--;
    cpu->ax = h ? g_dlg_result[d] : 0;
    b_ret(cpu, 16);
}

void USER_POSTMESSAGE(CPU *cpu) {
    HWND h        = get_hwnd(b_a16(cpu, 8));   /* hWnd@8 */
    uint16_t msg  = b_a16(cpu, 6);             /* uMsg@6 */
    uint16_t wp   = b_a16(cpu, 4);             /* wParam@4 */
    uint32_t lp   = ((uint32_t)b_a16(cpu, 2) << 16) | b_a16(cpu, 0);  /* lParam@0/2 */
    BOOL ok = h ? PostMessageA(h, msg, wp, (LPARAM)lp) : FALSE;
    cpu->ax = (uint16_t)(ok ? 1 : 0);
    b_ret(cpu, 10);
}

void USER_SETTIMER(CPU *cpu) {
    HWND h        = get_hwnd(b_a16(cpu, 8));   /* hWnd@8 */
    uint16_t id   = b_a16(cpu, 6);             /* nIDEvent@6 */
    uint16_t ms   = b_a16(cpu, 4);             /* uElapse@4 */
    uint16_t pseg = b_a16(cpu, 2);             /* lpTimerFunc@0/2 */
    if (pseg)
        fprintf(stderr, "[win16] *** SetTimer with a TIMERPROC (%04X) - "
                        "callback thunk not implemented, using WM_TIMER\n", pseg);
    UINT_PTR t = h ? SetTimer(h, id, ms ? ms : 1, NULL) : 0;
    fprintf(stderr, "[win32] SetTimer(id=%u, %ums) -> %u\n", id, ms, (unsigned)t);
    cpu->ax = (uint16_t)t;
    b_ret(cpu, 10);
}

void USER_KILLTIMER(CPU *cpu) {
    HWND h      = get_hwnd(b_a16(cpu, 2));     /* hWnd@2 */
    uint16_t id = b_a16(cpu, 0);               /* nIDEvent@0 */
    if (h) KillTimer(h, id);
    cpu->ax = 1;
    b_ret(cpu, 4);
}

void USER_INVALIDATERECT(CPU *cpu) {
    HWND h = get_hwnd(b_a16(cpu, 6));                    /* hWnd@6 */
    uint16_t roff = b_a16(cpu, 2), rseg = b_a16(cpu, 4); /* lpRect@2/4 */
    BOOL erase = (BOOL)(int16_t)b_a16(cpu, 0);           /* bErase@0 */
    RECT r;
    if (rseg) {
        r.left   = (int16_t)mem_read16(cpu, rseg, roff);
        r.top    = (int16_t)mem_read16(cpu, rseg, (uint16_t)(roff + 2));
        r.right  = (int16_t)mem_read16(cpu, rseg, (uint16_t)(roff + 4));
        r.bottom = (int16_t)mem_read16(cpu, rseg, (uint16_t)(roff + 6));
    }
    if (h) InvalidateRect(h, rseg ? &r : NULL, erase);
    cpu->ax = 1;
    b_ret(cpu, 8);
}

/* timeGetTime -> DX:AX milliseconds. Returning 0 made every elapsed-time test
 * see no time passing, so nothing animated. Shares USER_GETTICKCOUNT's clock
 * so the engine's two time sources cannot disagree. */
void MMSYSTEM_TIMEGETTIME(CPU *cpu) {
    extern uint32_t catz_tick_ms(void);
    uint32_t t = catz_tick_ms();
    cpu->ax = (uint16_t)t; cpu->dx = (uint16_t)(t >> 16);
    b_ret(cpu, 0);
}

void USER_BEGINPAINT(CPU *cpu) {
    HWND h = get_hwnd(b_a16(cpu, 4));                    /* hWnd@4 */
    uint16_t poff = b_a16(cpu, 0), pseg = b_a16(cpu, 2); /* lpPaint@0/2 */
    PAINTSTRUCT ps;
    HDC dc = h ? BeginPaint(h, &ps) : NULL;
    uint16_t gdc = dc ? put_hdc(dc) : 0;
    if (pseg) {
        const int16_t f[8] = {
            (int16_t)gdc, (int16_t)(dc ? ps.fErase : 0),
            (int16_t)ps.rcPaint.left,  (int16_t)ps.rcPaint.top,
            (int16_t)ps.rcPaint.right, (int16_t)ps.rcPaint.bottom,
            0, 0,
        };
        for (int i = 0; i < 8; i++)
            mem_write16(cpu, pseg, (uint16_t)(poff + 2 * i), (uint16_t)f[i]);
        for (int i = 16; i < 32; i++)
            mem_write8(cpu, pseg, (uint16_t)(poff + i), 0);
    }
    cpu->ax = gdc;
    b_ret(cpu, 6);
}

void USER_ENDPAINT(CPU *cpu) {
    HWND h = get_hwnd(b_a16(cpu, 4));
    uint16_t poff = b_a16(cpu, 0), pseg = b_a16(cpu, 2);
    PAINTSTRUCT ps;
    memset(&ps, 0, sizeof ps);
    if (pseg) {
        ps.hdc          = get_hdc(mem_read16(cpu, pseg, poff));
        ps.fErase       = (BOOL)(int16_t)mem_read16(cpu, pseg, (uint16_t)(poff + 2));
        ps.rcPaint.left = (int16_t)mem_read16(cpu, pseg, (uint16_t)(poff + 4));
        ps.rcPaint.top  = (int16_t)mem_read16(cpu, pseg, (uint16_t)(poff + 6));
        ps.rcPaint.right  = (int16_t)mem_read16(cpu, pseg, (uint16_t)(poff + 8));
        ps.rcPaint.bottom = (int16_t)mem_read16(cpu, pseg, (uint16_t)(poff + 10));
    }
    if (h) EndPaint(h, &ps);
    if (pseg) free_hdc(mem_read16(cpu, pseg, poff));
    cpu->ax = 1;
    b_ret(cpu, 6);
}

void USER_RELEASEDC(CPU *cpu) {
    uint16_t gdc = b_a16(cpu, 0);   /* HWND@2, HDC@0 */
    HWND h = get_hwnd(b_a16(cpu, 2));
    HDC dc = get_hdc(gdc);
    if (h && dc) ReleaseDC(h, dc);
    free_hdc(gdc);
    cpu->ax = 1; b_ret(cpu, 4);
}

/* ===== USER: message loop ===== */

void USER_GETMESSAGE(CPU *cpu) {
    /* GetMessage(LPMSG@2/4, HWND@.., UINT, UINT) -> BOOL. Pump real messages. */
    uint16_t moff = b_a16(cpu, 6), mseg = b_a16(cpu, 8);
    MSG m;
    BOOL r = GetMessageA(&m, NULL, 0, 0);
    if (r) {
        /* Win16 MSG: hwnd@0(2) message@2(2) wParam@4(2) lParam@6(4) time@10(4) pt@14(4) */
        mem_write16(cpu, mseg, (uint16_t)(moff + 0), put_hwnd(m.hwnd));
        mem_write16(cpu, mseg, (uint16_t)(moff + 2), (uint16_t)m.message);
        mem_write16(cpu, mseg, (uint16_t)(moff + 4), (uint16_t)m.wParam);
        mem_write16(cpu, mseg, (uint16_t)(moff + 6), (uint16_t)(m.lParam & 0xFFFF));
        mem_write16(cpu, mseg, (uint16_t)(moff + 8), (uint16_t)(m.lParam >> 16));
    }
    cpu->ax = (uint16_t)(r ? 1 : 0);
    b_ret(cpu, 10);   /* lpMsg(4)+hWnd(2)+min(2)+max(2) */
}

void USER_PEEKMESSAGE(CPU *cpu) {
    uint16_t moff = b_a16(cpu, 8), mseg = b_a16(cpu, 10);
    uint16_t wRemove = b_a16(cpu, 0);
    MSG m;
    BOOL r = PeekMessageA(&m, NULL, 0, 0, wRemove ? PM_REMOVE : PM_NOREMOVE);
    if (r) {
        mem_write16(cpu, mseg, (uint16_t)(moff + 0), put_hwnd(m.hwnd));
        mem_write16(cpu, mseg, (uint16_t)(moff + 2), (uint16_t)m.message);
        mem_write16(cpu, mseg, (uint16_t)(moff + 4), (uint16_t)m.wParam);
        mem_write16(cpu, mseg, (uint16_t)(moff + 6), (uint16_t)(m.lParam & 0xFFFF));
        mem_write16(cpu, mseg, (uint16_t)(moff + 8), (uint16_t)(m.lParam >> 16));
    }
    cpu->ax = (uint16_t)(r ? 1 : 0);
    b_ret(cpu, 12);
}

void USER_TRANSLATEMESSAGE(CPU *cpu) { cpu->ax = 0; b_ret(cpu, 4); }

void USER_DISPATCHMESSAGE(CPU *cpu) {
    /* lpMsg far ptr @0/2: re-read and dispatch via real Win32 -> host_wndproc. */
    uint16_t moff = b_a16(cpu, 0), mseg = b_a16(cpu, 2);
    MSG m; memset(&m, 0, sizeof m);
    m.hwnd    = get_hwnd(mem_read16(cpu, mseg, (uint16_t)(moff + 0)));
    m.message = mem_read16(cpu, mseg, (uint16_t)(moff + 2));
    m.wParam  = mem_read16(cpu, mseg, (uint16_t)(moff + 4));
    m.lParam  = (uint32_t)mem_read16(cpu, mseg, (uint16_t)(moff + 6))
              | ((uint32_t)mem_read16(cpu, mseg, (uint16_t)(moff + 8)) << 16);
    DispatchMessageA(&m);
    cpu->ax = 0; b_ret(cpu, 4);
}

/* ---- resources: LoadBitmap ---- */

static void b_asciiz(CPU *cpu, uint16_t seg, uint16_t off, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        char c = (char)mem_read8(cpu, seg, (uint16_t)(off + i));
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

/* LoadBitmap(HINSTANCE, LPCSTR) — a Win16 RT_BITMAP resource is a packed DIB:
 * BITMAPINFOHEADER, color table, then bits (no BITMAPFILEHEADER). Hand it to
 * GDI32 as a real HBITMAP so the engine's SelectObject/BitBlt path works. */
void USER_LOADBITMAP(CPU *cpu) {
    uint16_t noff = b_a16(cpu, 0), nseg = b_a16(cpu, 2), hinst = b_a16(cpu, 4);

    /* MAKEINTRESOURCE: a far pointer with a zero segment is an integer id. */
    char name[128] = "";
    int id = -1;
    if (nseg == 0) id = noff;
    else b_asciiz(cpu, nseg, noff, name, sizeof name);

    uint16_t hrsrc = ne_find_resource(hinst, 2 /* RT_BITMAP */, NULL,
                                      id, id < 0 ? name : NULL);
    uint32_t len = 0;
    const uint8_t *p = hrsrc ? ne_resource_bytes(hrsrc, &len) : NULL;
    if (!p || len < sizeof(BITMAPINFOHEADER)) {
        fprintf(stderr, "[win32] LoadBitmap(%04X, %s) NOT FOUND\n",
                hinst, id >= 0 ? "#id" : name);
        cpu->ax = 0; b_ret(cpu, 6); return;
    }

    const BITMAPINFOHEADER *bih = (const BITMAPINFOHEADER *)p;
    /* Colour table size: explicit biClrUsed, else 2^bitcount for <=8bpp. */
    unsigned ncolors = bih->biClrUsed;
    if (!ncolors && bih->biBitCount <= 8) ncolors = 1u << bih->biBitCount;
    const uint8_t *bits = p + bih->biSize + ncolors * sizeof(RGBQUAD);

    HDC dc = GetDC(NULL);
    HBITMAP hbm = CreateDIBitmap(dc, bih, CBM_INIT, bits,
                                 (const BITMAPINFO *)bih, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);

    cpu->ax = put_hgdi(hbm);
    fprintf(stderr, "[win32] LoadBitmap(%04X, %s) %ldx%ld %ubpp -> guest=%u\n",
            hinst, id >= 0 ? "#id" : name, (long)bih->biWidth, (long)bih->biHeight,
            bih->biBitCount, cpu->ax);
    b_ret(cpu, 6);
}

/* Expose the guest->real HDC mapping to the WinG blit in win16_impl.c. */
HDC catz_real_hdc(uint16_t guest_hdc) { return get_hdc(guest_hdc); }
HWND catz_real_hwnd(uint16_t guest_hwnd) { return get_hwnd(guest_hwnd); }
