/* ne_resources.c - parse the original NE binaries' resource directory + string
 * tables so the Win16 shims can serve real LoadString/FindResource/LoadResource.
 * See ne_resources.h. Pure host-side parsing; no guest CPU/memory access here. */
#include "ne_resources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CATZ_GAME_DIR
#define CATZ_GAME_DIR "game"
#endif

typedef struct {
    uint16_t type_raw;      /* raw NE rtTypeID (0x8000|n int, else name offset)  */
    int      type_int;      /* resolved int type, or -1 if named                 */
    char     type_name[32];
    uint16_t name_raw;      /* raw NE rnID                                        */
    int      name_int;      /* resolved int id, or -1 if named                   */
    char     name_str[64];
    uint32_t off;           /* absolute file offset of the resource bytes        */
    uint32_t len;           /* length in bytes                                    */
} Res;

typedef struct {
    char      name[16];     /* upper, no extension, e.g. "CATZDLL"               */
    uint16_t  hinst;        /* assigned instance handle                          */
    uint8_t  *data;
    long      size;
    uint32_t  ne;           /* offset of NE header                               */
    uint32_t  res_base;     /* absolute offset of the resource table             */
    Res      *res;
    int       nres;
} Mod;

static Mod g_mods[8];
static int g_nmods;

static struct { Mod *m; Res *r; } g_found[512];
static int g_nfound;

static int g_inited;

static int ieq(const char *a, const char *b);   /* case-insensitive equal */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Resolve a raw NE type/name value (rtTypeID or rnID): high bit set => integer,
 * else an offset (from the resource table base) to a length-prefixed name. */
static void resolve_id(Mod *m, uint16_t val, int *out_int, char *out_str, int outsz) {
    out_str[0] = 0;
    if (val & 0x8000) { *out_int = val & 0x7FFF; return; }
    *out_int = -1;
    uint32_t o = m->res_base + val;
    if (o < (uint32_t)m->size) {
        int n = m->data[o];
        int i = 0;
        for (; i < n && i < outsz - 1 && (o + 1 + i) < (uint32_t)m->size; i++)
            out_str[i] = (char)m->data[o + 1 + i];
        out_str[i] = 0;
    }
}

static int parse_ne(Mod *m) {
    if (m->size < 0x40) return 0;
    uint32_t e = rd16(m->data + 0x3C);
    if (e + 64 > (uint32_t)m->size || m->data[e] != 'N' || m->data[e + 1] != 'E') return 0;
    m->ne = e;
    const uint8_t *nh = m->data + e;
    uint32_t res_off = rd16(nh + 36);
    if (res_off == 0) return 1;                 /* no resource table */
    m->res_base = e + res_off;
    if (m->res_base + 2 > (uint32_t)m->size) return 0;
    uint16_t align = rd16(m->data + m->res_base);
    uint32_t p = m->res_base + 2;

    /* first pass: count */
    int total = 0;
    uint32_t q = p;
    while (q + 8 <= (uint32_t)m->size) {
        uint16_t tid = rd16(m->data + q);
        if (tid == 0) break;
        uint16_t cnt = rd16(m->data + q + 2);
        q += 8 + (uint32_t)cnt * 12;
        total += cnt;
    }
    m->res = (Res *)calloc(total ? total : 1, sizeof(Res));
    m->nres = 0;

    while (p + 8 <= (uint32_t)m->size) {
        uint16_t tid = rd16(m->data + p);
        if (tid == 0) break;
        uint16_t cnt = rd16(m->data + p + 2);
        p += 8;
        for (int i = 0; i < cnt && p + 12 <= (uint32_t)m->size; i++, p += 12) {
            Res *r = &m->res[m->nres++];
            r->type_raw = tid;
            resolve_id(m, tid, &r->type_int, r->type_name, sizeof r->type_name);
            r->name_raw = rd16(m->data + p + 6);
            resolve_id(m, r->name_raw, &r->name_int, r->name_str, sizeof r->name_str);
            r->off = (uint32_t)rd16(m->data + p + 0) << align;
            r->len = (uint32_t)rd16(m->data + p + 2) << align;
        }
    }
    return 1;
}

static Mod *register_mod(const char *base, uint16_t hinst) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.DLL", CATZ_GAME_DIR, base);
    FILE *f = fopen(path, "rb");
    if (!f) {   /* WAD has no .DLL extension */
        snprintf(path, sizeof path, "%s/%s.WAD", CATZ_GAME_DIR, base);
        f = fopen(path, "rb");
    }
    if (!f) { fprintf(stderr, "[ne] cannot open %s\n", base); return NULL; }
    Mod *m = &g_mods[g_nmods];
    fseek(f, 0, SEEK_END); m->size = ftell(f); fseek(f, 0, SEEK_SET);
    m->data = (uint8_t *)malloc(m->size);
    if (fread(m->data, 1, m->size, f) != (size_t)m->size) { fclose(f); free(m->data); return NULL; }
    fclose(f);
    snprintf(m->name, sizeof m->name, "%s", base);
    m->hinst = hinst;
    if (!parse_ne(m)) { fprintf(stderr, "[ne] %s: NE parse failed\n", base); free(m->data); return NULL; }
    fprintf(stderr, "[ne] loaded %s (hinst=%04X, %d resources)\n", base, hinst, m->nres);
    g_nmods++;
    return m;
}

void ne_init(void) {
    if (g_inited) return;
    g_inited = 1;
    register_mod("CATZDLL", 59);   /* hinst == CATZDLL DGROUP selector */
    register_mod("CATZ", 66);      /* CATZ.WAD; hinst == WAD DGROUP selector */
}

static Mod *mod_by_hinst(uint16_t hinst) {
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].hinst == hinst) return &g_mods[i];
    return NULL;
}

/* normalize "C:\\PATH\\CATZREZX.DLL" -> "CATZREZX" (upper, no path/ext) */
static void normalize(const char *in, char *out, int outsz) {
    const char *p = in, *slash = in;
    for (; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') slash = p + 1;
    int i = 0;
    for (p = slash; *p && *p != '.' && i < outsz - 1; p++, i++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = c;
    }
    out[i] = 0;
}

uint16_t ne_loadlib(const char *name) {
    ne_init();
    char base[64];
    normalize(name, base, sizeof base);
    if (base[0] == 0) return 0;
    for (int i = 0; i < g_nmods; i++)
        if (strcmp(g_mods[i].name, base) == 0) return g_mods[i].hinst;
    /* Only serve modules that actually ship as game NE files we can parse. */
    if (g_nmods < (int)(sizeof g_mods / sizeof g_mods[0])) {
        uint16_t hinst = (uint16_t)(0xF000 + g_nmods);
        Mod *m = register_mod(base, hinst);
        if (m) return m->hinst;
    }
    return 0;
}

static int load_string_from(Mod *m, uint16_t id, char *out, int max) {
    uint16_t block = (uint16_t)((id >> 4) + 1);   /* RT_STRING block id */
    uint16_t sub   = (uint16_t)(id & 0x0F);
    for (int i = 0; i < m->nres; i++) {
        Res *r = &m->res[i];
        if (r->type_int != 6) continue;           /* RT_STRING */
        if (r->name_int != block) continue;
        uint32_t q = r->off, end = r->off + r->len;
        for (uint16_t s = 0; s <= sub; s++) {
            if (q >= end || q >= (uint32_t)m->size) return 0;
            int n = m->data[q++];
            if (s == sub) {
                int k = 0;
                for (; k < n && k < max - 1 && q + k < (uint32_t)m->size; k++)
                    out[k] = (char)m->data[q + k];
                out[k] = 0;
                return k;
            }
            q += n;
        }
    }
    return 0;
}

int ne_load_string(uint16_t hinst, uint16_t id, char *out, int max) {
    ne_init();
    Mod *m = mod_by_hinst(hinst);
    int n = m ? load_string_from(m, id, out, max) : 0;
    if (n) return n;
    /* Fall back to any module: the engine sometimes passes the app (WAD)
     * instance for strings that actually live in CATZDLL, and vice-versa. */
    for (int i = 0; i < g_nmods; i++) {
        if (&g_mods[i] == m) continue;
        n = load_string_from(&g_mods[i], id, out, max);
        if (n) return n;
    }
    return 0;
}

uint16_t ne_find_resource(uint16_t hinst, int type_int, const char *type_str,
                          int name_int, const char *name_str) {
    ne_init();
    Mod *m = mod_by_hinst(hinst);
    if (!m) return 0;
    for (int i = 0; i < m->nres; i++) {
        Res *r = &m->res[i];
        int tmatch = (type_int >= 0) ? (r->type_int == type_int)
                                     : (r->type_int < 0 && type_str && ieq(r->type_name, type_str));
        if (!tmatch) continue;
        int nmatch = (name_int >= 0) ? (r->name_int == name_int)
                                     : (r->name_int < 0 && name_str && ieq(r->name_str, name_str));
        if (!nmatch) continue;
        if (g_nfound >= (int)(sizeof g_found / sizeof g_found[0])) return 0;
        g_found[g_nfound].m = m; g_found[g_nfound].r = r;
        return (uint16_t)(++g_nfound);            /* HRSRC = index+1 */
    }
    return 0;
}

const uint8_t *ne_resource_bytes(uint16_t hrsrc, uint32_t *out_len) {
    if (hrsrc == 0 || hrsrc > g_nfound) return NULL;
    Mod *m = g_found[hrsrc - 1].m;
    Res *r = g_found[hrsrc - 1].r;
    if (r->off + r->len > (uint32_t)m->size) return NULL;
    if (out_len) *out_len = r->len;
    return m->data + r->off;
}

/* tiny case-insensitive compare returning 1 on equal (avoid platform strcasecmp) */
static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return *a == *b;
}
