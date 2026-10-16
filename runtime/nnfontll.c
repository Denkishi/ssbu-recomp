/* nn::fontll::ScalableFontEngine: the SDK's scalable-font rasteriser (a wrapper around Monotype iType).
 *
 * What the real one does was read from the SDK module itself (binaries/13.0.4/sdk.elf has symbols; the
 * wrappers at 0x19cfa0..0x19db50 tail-call FSS_ / FS_ functions of iType). Facts used here, with the SDK address
 * they were read at:
 *
 *   LoadFont(nameOut, data, index, nameLen) -> FSS_load_font(state, NULL, data, index, nameLen, nameOut).
 *     The name is a property of the font: the game loads every font into several engines (one for plotting,
 *     one per worker thread) and keeps ONE name per font, written by whichever engine loaded it last.
 *   SetFont(name)        -> FSS_set_font, then FSS_set_cmap(3, 10) and, if that fails, (3, 1)      (0x19d990)
 *   CheckGlyphExist(c)   -> FSS_map_char(c) != 0, i.e. the cmap has a glyph index for c            (0x19da00)
 *   SetScale(a, b, c, d) -> FSS_set_scale: 2x2 matrix in 16.16, a and d = pixels per em
 *   SetBoldWeight(v)     -> FS_set_bold_pct(v), 16.16, 0..1.0                                      (0x1a18d0)
 *   SetOutlineWidth(n)   -> FS_set_outline_width, 1..10, error 0x266 otherwise                     (0x1a1910)
 *   SetFlags(e)          -> FS_set_flags(table[e]), table at 0xa3b3ec = { clear effects, 0x80, 0x40 }:
 *                           0 = no effect, 1 = outlined unfilled (the ring only), 2 = outlined filled
 *   SetAutoHint(b)       -> FS_set_flags(b ? 0x1000000 : ~0x1000000)
 *   GetError()           -> error of the last call (state + 0x48), 0 = none
 *   GetAdvance(&idx, &idy, &dx, &dy, code, type) -> FSS_get_advance: dx, dy = advance in 16.16 pixels,
 *                           idx = (dx + 0x8000) >> 16 (0x1a6398); returns the error (0xc9 = out of memory)
 *   AcquireGlyphmap(code, type) -> FSS_get_glyphmap; type 8 = 8-bit gray map (the only one the game asks for)
 *
 * Metrics (0x93c bytes, filled at 0x1a47b0; font units):
 *   +0 u16 unitsPerEm, +2 u16 head.macStyle, +4 s16 OS/2 winAscent, +6 winDescent, +8 typoAscender,
 *   +0xa typoDescender, +0xc fsSelection, +0xe typoLineGap, +0x10 hhea ascender, +0x12 descender,
 *   +0x14 lineGap, +0x16 OS/2 fsType, +0x18 s16 head xMin, +0x1a yMin, +0x1c xMax, +0x1e yMax,
 *   +0x20 u16 font type flags, +0x22 u16 number of cmap subtables (at most 20), +0x24 + 8n { u16 platform,
 *   u16 encoding }, +0xc4 u32 fonts in collection, +0xc8 u32 index in collection, +0xcc char[50] family,
 *   +0xfe char[50] font name, +0x130 char[0x800] copyright, +0x930 u16 metricsResolution (= unitsPerEm),
 *   +0x934 u32 numGlyphs, +0x938 u32 0.
 *   The game (0x710007ea1c) reads +4, +6, +0x1a, +0x1e and divides them by +0x930.
 *
 * GlyphMap (make_graymap 0x1c3e30):
 *   +0 pointer (cache entry, 0 here), +8 s32 size, +0xc s16 lo_x = floor(xMin), +0xe s16 hi_y = ceil(yMax) - 1
 *   (pixel coordinates of the left column and of the TOP ROW, y up), +0x10 s16 i_dx, +0x12 s16 i_dy,
 *   +0x14 s32 dx (16.16), +0x18 s32 dy, +0x1c s16 width, +0x1e s16 height, +0x20 s16 bytes per line (= width
 *   for 8 bits), +0x22 u8 embedded, +0x24 s16 bitsPerPixel, +0x26 u16 type, +0x28 bits: one byte of coverage
 *   0..255 per pixel, top row first. A glyph without contours has width = height = 0 and size 0x28.
 *
 * Outline effect (outline_graymap 0x1af6b0): the map grows by n + 1 pixels on every side (lo_x -= n + 1,
 *   hi_y += n + 1), then n dilation passes: a pixel that is non-zero becomes 255, any other takes the
 *   maximum of its neighbours (8 neighbours when the remaining pass count is even, 4 when odd). "Unfilled"
 *   then subtracts the original glyph. The game plots a bordered glyph as filled - unfilled / 2
 *   (0x710007ffd0): 255 inside, about 128 on the border.
 *
 * OtfKerningTable (InitializeOtfKerningTable 0x19d1e0, allocated with the caller's allocator):
 *   +0 s32 count, +8 entries, +0x10 float 1 / unitsPerEm; entry = { u16 glyph1, u16 glyph2 (0xffff = single
 *   adjustment), s16 xAdvance, s16 xPlacement }, sorted. Pairs come from GPOS feature "kern" (lookup type 2,
 *   first value record only), singles from "palt" (lookup type 1); script latn, kana or DFLT.
 *   AcquireOtfKerning(table, c1, c2, size) = floor((single1.adv - single1.plc + single2.plc + pair.adv)
 *   / unitsPerEm * size + 0.5); First = single.plc, Last = single.adv - single.plc, scaled the same way.
 *
 * What is NOT the real thing: the outlines come from Windows GDI (GetGlyphOutline, unhinted Bezier outlines
 * at one font unit per pixel, glyph indices from our own cmap lookup) and are scan-converted here with exact
 * area coverage. iType's own hinting / grid fitting, its bold algorithm and its exact fixed-point rounding
 * are not reproduced: advances are advanceWidth * pixels / unitsPerEm rounded to 16.16.
 *
 * Debug: SSBU_FONTLOG=<n> prints the first n calls of every kind; SSBU_FONTDUMP=<dir> writes each distinct
 * font file as <dir>/<name>.ttf. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#ifndef FONTLL_TEST
#include "runtime.h"
#include "hostrt.h"
#endif

#define MAX_ENGINES 256
#define MAX_FONTS 32
#define MAX_FONTDATA 64
#define METRICS_SIZE 0x93c
#define GLYPH_HEADER 0x28
#define BFTTF_KEY 0x49621806u                     /* big-endian words of a .bfttf are XORed with this */
#define ENGINE_MAGIC 0x6c6c746e6f666e6eULL        /* stored in the guest object so the engine is found without a search */

#define ERR_NO_FONT 0x131                         /* iType's codes where known */
#define ERR_FONT_NOT_FOUND 0x12e
#define ERR_BAD_OUTLINE_WIDTH 0x266
#define ERR_BAD_BOLD 0x265
#define ERR_BAD_FLAG 0x264

#define FLAG_OUTLINED_FILLED 0x40u
#define FLAG_OUTLINED_UNFILLED 0x80u

/* One distinct font file (shared by every engine that loads the same bytes). */
typedef struct {
    uint8_t *data;                                /* our own copy */
    const uint8_t *dir;                           /* table directory of the chosen face */
    uint32_t size, hash, face_index;
    WCHAR face[LF_FACESIZE];
    char name[50];
    int upem, glyphs, hmetrics, cff;
    const uint8_t *head, *hhea, *os2, *hmtx, *cmap, *sub, *kern, *gpos, *nametab;
    uint32_t kern_len, gpos_len, cmap_len;
    int sub_format;
    HFONT hfont;
    int gdi;                                      /* 0 not tried, 1 usable, -1 unusable */
} FontData;

typedef struct { FontData *fd; char name[50]; } Font;

typedef struct {
    uint64_t key;                                 /* guest address of the engine object */
    uint64_t work;
    Font fonts[MAX_FONTS];
    int count, current, error;
    int32_t scale[4];
    uint32_t flags;
    int outline, bold;
} Engine;

static Engine g_engines[MAX_ENGINES];
static FontData g_fontdata[MAX_FONTDATA];
static int g_fontdata_count;
static CRITICAL_SECTION g_lock;                   /* GDI, the font list and the engine table */
static HDC g_dc;
static const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int16_t s16(const uint8_t *p) { return (int16_t)be16(p); }
static void put16(uint8_t *p, int v) { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); }
static void put32(uint8_t *p, int32_t v) { memcpy(p, &v, 4); }

static int g_log = -1;
static volatile LONG g_counts[16];
enum { L_CHECK, L_SETFONT, L_SCALE, L_ADVANCE, L_GLYPH, L_METRICS, L_BOLD, L_OUTLINE, L_FLAGS, L_KERN, L_OTF, L_LOAD };
static int flog(int what) {                       /* SSBU_FONTLOG=<n>: log the first n calls of each kind */
    if (g_log < 0) { const char *v = getenv("SSBU_FONTLOG"); g_log = v ? atoi(v) : 0; }
    return g_log > 0 && InterlockedIncrement(&g_counts[what]) <= g_log;
}

/* ---------------------------------------------------------------- font files */
static const uint8_t *table(const FontData *f, const char *tag, uint32_t *len) {
    int n = (int)be16(f->dir + 4);
    for (int i = 0; i < n; i++) {
        const uint8_t *rec = f->dir + 12 + i * 16;
        if (!memcmp(rec, tag, 4) && be32(rec + 8) < f->size) { if (len) *len = be32(rec + 12); return f->data + be32(rec + 8); }
    }
    if (len) *len = 0;
    return NULL;
}

/* Size of the whole font file, from its table directories (LoadFont is not told the size). */
static uint32_t file_size(const uint8_t *data) {
    uint32_t end = 0, faces = 1;
    int collection = !memcmp(data, "ttcf", 4);
    if (collection) faces = be32(data + 8);
    for (uint32_t k = 0; k < faces; k++) {
        const uint8_t *dir = collection ? data + be32(data + 12 + k * 4) : data;
        int n = (int)be16(dir + 4);
        if ((uint32_t)(dir - data) + 12 + n * 16 > end) end = (uint32_t)(dir - data) + 12 + n * 16;
        for (int i = 0; i < n; i++) {
            uint32_t e = be32(dir + 12 + i * 16 + 8) + be32(dir + 12 + i * 16 + 12);
            if (e > end) end = e;
        }
    }
    return (end + 3) & ~3u;
}

/* A string of the name table as ASCII (Windows platform, English preferred). */
static int name_string(const FontData *f, int id, char *out, int room, WCHAR *wide, int wroom) {
    const uint8_t *name = f->nametab, *best = NULL;
    if (room) out[0] = 0;
    if (wide && wroom) wide[0] = 0;
    if (!name) return 0;
    int n = (int)be16(name + 2);
    const uint8_t *strings = name + be16(name + 4);
    for (int i = 0; i < n; i++) {
        const uint8_t *rec = name + 6 + i * 12;
        if (be16(rec) == 3 && (int)be16(rec + 6) == id && (!best || be16(rec + 4) == 0x409)) best = rec;
    }
    if (!best) return 0;
    int len = (int)be16(best + 8) / 2, k = 0;
    for (int i = 0; i < len; i++) {
        unsigned ch = be16(strings + be16(best + 10) + i * 2);
        if (wide && i < wroom - 1) { wide[i] = (WCHAR)ch; wide[i + 1] = 0; }
        if (k < room - 1) out[k++] = ch >= 32 && ch < 127 ? (char)ch : '?';
    }
    if (room) out[k] = 0;
    return len;
}

/* The cmap subtable SetFont selects: Windows UCS-4 (3, 10), else Windows BMP (3, 1), else any Unicode one. */
static void pick_cmap(FontData *f) {
    f->sub = NULL;
    if (!f->cmap) return;
    int n = (int)be16(f->cmap + 2), best = -1;
    for (int i = 0; i < n; i++) {
        const uint8_t *rec = f->cmap + 4 + i * 8;
        int plat = (int)be16(rec), enc = (int)be16(rec + 2), score = plat == 3 && enc == 10 ? 3 : plat == 3 && enc == 1 ? 2 : plat == 0 ? 1 : 0;
        int fmt = (int)be16(f->cmap + be32(rec + 4));
        if (fmt != 4 && fmt != 12 && fmt != 6 && fmt != 0) continue;
        if (score > best) { best = score; f->sub = f->cmap + be32(rec + 4); f->sub_format = fmt; }
    }
}

/* FSS_map_char: character code -> glyph index, 0 when the font has no glyph for it. */
static uint32_t map_char(const FontData *f, uint32_t code) {
    const uint8_t *s = f->sub;
    if (!s) return 0;
    if (f->sub_format == 12) {
        uint32_t lo = 0, hi = be32(s + 12);
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            const uint8_t *g = s + 16 + mid * 12;
            if (code < be32(g)) hi = mid;
            else if (code > be32(g + 4)) lo = mid + 1;
            else { uint32_t id = be32(g + 8) + (code - be32(g)); return id < (uint32_t)f->glyphs ? id : 0; }
        }
        return 0;
    }
    if (f->sub_format == 4) {
        if (code > 0xffff) return 0;
        uint32_t segs = be16(s + 6) / 2, lo = 0, hi = segs;
        const uint8_t *end = s + 14, *start = end + segs * 2 + 2, *delta = start + segs * 2, *range = delta + segs * 2;
        while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (be16(end + mid * 2) < code) lo = mid + 1; else hi = mid; }
        if (lo == segs || be16(start + lo * 2) > code) return 0;
        uint32_t ro = be16(range + lo * 2), id;
        if (!ro) id = (code + be16(delta + lo * 2)) & 0xffff;
        else {
            id = be16(range + lo * 2 + ro + (code - be16(start + lo * 2)) * 2);
            if (id) id = (id + be16(delta + lo * 2)) & 0xffff;
        }
        return id < (uint32_t)f->glyphs ? id : 0;
    }
    if (f->sub_format == 6) {
        uint32_t first = be16(s + 6), count = be16(s + 8);
        return code >= first && code < first + count ? be16(s + 10 + (code - first) * 2) : 0;
    }
    return code < 256 ? s[6 + code] : 0;
}

static int advance_units(const FontData *f, uint32_t glyph) {
    if (!f->hmtx || !f->hmetrics) return 0;
    if (glyph >= (uint32_t)f->hmetrics) glyph = (uint32_t)f->hmetrics - 1;
    return (int)be16(f->hmtx + glyph * 4);
}

/* Finds or registers the font file at `data` (callers hold g_lock). */
static FontData *font_data(const uint8_t *data, uint32_t index) {
    uint32_t size = file_size(data), hash = 2166136261u;
    for (uint32_t i = 0; i < size; i++) hash = (hash ^ data[i]) * 16777619u;
    for (int i = 0; i < g_fontdata_count; i++)
        if (g_fontdata[i].size == size && g_fontdata[i].hash == hash && g_fontdata[i].face_index == index) return &g_fontdata[i];
    if (g_fontdata_count == MAX_FONTDATA) return NULL;
    FontData *f = &g_fontdata[g_fontdata_count];
    memset(f, 0, sizeof *f);
    f->data = malloc(size + 16);
    if (!f->data) return NULL;
    memcpy(f->data, data, size);
    f->size = size;
    f->hash = hash;
    f->face_index = index;
    f->dir = !memcmp(data, "ttcf", 4) ? f->data + be32(f->data + 12 + index * 4) : f->data;
    f->head = table(f, "head", NULL);
    f->hhea = table(f, "hhea", NULL);
    f->os2 = table(f, "OS/2", NULL);
    f->hmtx = table(f, "hmtx", NULL);
    f->cmap = table(f, "cmap", &f->cmap_len);
    f->kern = table(f, "kern", &f->kern_len);
    f->gpos = table(f, "GPOS", &f->gpos_len);
    f->nametab = table(f, "name", NULL);
    f->cff = table(f, "CFF ", NULL) != NULL;
    const uint8_t *maxp = table(f, "maxp", NULL);
    if (!f->head || !f->hhea || !maxp) { free(f->data); return NULL; }
    f->upem = (int)be16(f->head + 18);
    f->glyphs = (int)be16(maxp + 4);
    f->hmetrics = (int)be16(f->hhea + 34);
    pick_cmap(f);
    char family[41];
    name_string(f, 1, family, sizeof family, f->face, LF_FACESIZE);
    int n = 0;
    for (; family[n]; n++) f->name[n] = family[n] > 32 && family[n] != '?' ? family[n] : '_';
    if (!n) n = snprintf(f->name, sizeof f->name, "font");
    for (int i = 0, dup = 0; i < g_fontdata_count; i++)          /* same family name, different bytes */
        if (!strncmp(g_fontdata[i].name, f->name, (size_t)n) && (g_fontdata[i].name[n] == 0 || g_fontdata[i].name[n] == '~'))
            snprintf(f->name + n, sizeof f->name - (size_t)n, "~%d", ++dup);
    g_fontdata_count++;
    printf("nn::fontll: font \"%s\": %u bytes, %s, %d units per em, %d glyphs, cmap format %d%s%s\n", f->name, size, f->cff ? "CFF outlines" : "TrueType outlines",
           f->upem, f->glyphs, f->sub ? f->sub_format : -1, f->kern ? ", kern table" : "", f->gpos ? ", GPOS table" : "");
    const char *dump = getenv("SSBU_FONTDUMP");
    if (dump) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s.%s", dump, f->name, f->cff ? "otf" : "ttf");
        FILE *out = fopen(path, "wb");
        if (out) { fwrite(f->data, 1, size, out); fclose(out); }
    }
    return f;
}

/* ---------------------------------------------------------------- outlines (from GDI, in font units) */
typedef struct { double x, y; } Pt;
typedef struct { Pt *p; int n, cap; int *end; int contours, ccap; } Path;       /* flattened contours */

static void path_point(Path *path, double x, double y) {
    if (path->n == path->cap) { path->cap = path->cap ? path->cap * 2 : 256; path->p = realloc(path->p, (size_t)path->cap * sizeof(Pt)); }
    path->p[path->n].x = x;
    path->p[path->n++].y = y;
}
static void path_close(Path *path) {
    if (path->contours == path->ccap) { path->ccap = path->ccap ? path->ccap * 2 : 16; path->end = realloc(path->end, (size_t)path->ccap * sizeof(int)); }
    path->end[path->contours++] = path->n;
}
static double fx(FIXED v) { return v.value + v.fract / 65536.0; }

/* Whether `face` is one of the font's Windows family names (name ID 1) in any language. GetTextFace answers with the
 * name in the user's display language when the font has one: on a Chinese Windows "DFHeiGB Std W12" is reported as
 * its 0x804 name (U+534E U+5EB7 U+9ED1 U+4F53 " Std W12"), found 2026-10-15. */
static int has_family_name(const FontData *f, const WCHAR *face) {
    const uint8_t *name = f->nametab;
    if (!name) return 0;
    int n = (int)be16(name + 2), want = lstrlenW(face);
    const uint8_t *strings = name + be16(name + 4);
    for (int i = 0; i < n; i++) {
        const uint8_t *rec = name + 6 + i * 12;
        if (be16(rec) != 3 || be16(rec + 6) != 1 || (int)be16(rec + 8) / 2 != want) continue;
        int k = 0;
        for (; k < want; k++) {
            WCHAR a = (WCHAR)be16(strings + be16(rec + 10) + k * 2), b = face[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (k == want) return 1;
    }
    return 0;
}

/* Makes GDI able to draw from this font file; verifies that the face GDI selects really is this file. */
static int gdi_prepare(FontData *f) {             /* callers hold g_lock */
    if (f->gdi) return f->gdi > 0;
    DWORD installed = 0;
    f->gdi = -1;
    if (!f->face[0] || !AddFontMemResourceEx(f->data, f->size, NULL, &installed)) { printf("nn::fontll: font \"%s\" REJECTED BY GDI: its glyphs will be blank\n", f->name); return 0; }
    f->hfont = CreateFontW(-f->upem, 0, 0, 0, FW_DONTCARE, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, f->face);
    if (!f->hfont) return 0;
    SelectObject(g_dc, f->hfont);
    WCHAR got[LF_FACESIZE];
    uint8_t head[54];
    GetTextFaceW(g_dc, LF_FACESIZE, got);
    DWORD n = GetFontData(g_dc, 0x64616568 /* "head" */, 0, head, sizeof head);
    int same = n == sizeof head && !memcmp(head, f->head, sizeof head) && (!lstrcmpiW(got, f->face) || has_family_name(f, got));
    if (!same) {
        char shown[LF_FACESIZE * 6 + 1];          /* printf's %ls stops at the first character the C locale cannot convert */
        int k = 0;
        for (const WCHAR *p = got; *p && k < (int)sizeof shown - 7; p++)
            k += *p >= 32 && *p < 127 ? snprintf(shown + k, sizeof shown - (size_t)k, "%c", (char)*p) : snprintf(shown + k, sizeof shown - (size_t)k, "\\u%04x", *p);
        shown[k] = 0;
        printf("nn::fontll: font \"%s\": GDI selected another font (\"%s\"): its glyphs will be blank\n", f->name, shown);
        return 0;
    }
    f->gdi = 1;
    return 1;
}

/* Appends the outline of a glyph, transformed by the 16.16 matrix m / unitsPerEm, as polylines (y up). */
static void glyph_path(FontData *f, uint32_t glyph, const int32_t *m, Path *path) {
    static uint8_t *buf;
    static DWORD cap;
    GLYPHMETRICS gm;
    EnterCriticalSection(&g_lock);
    if (gdi_prepare(f)) {
        SelectObject(g_dc, f->hfont);
        const UINT fmt = GGO_BEZIER | GGO_UNHINTED | GGO_GLYPH_INDEX;
        DWORD size = GetGlyphOutlineW(g_dc, glyph, fmt, &gm, 0, NULL, &identity);
        if (size != GDI_ERROR && size) {
            if (size > cap) { cap = size + 4096; buf = realloc(buf, cap); }
            size = GetGlyphOutlineW(g_dc, glyph, fmt, &gm, size, buf, &identity);
        }
        if (size != GDI_ERROR && size) {
            double a = m[0] / 65536.0 / f->upem, b = m[1] / 65536.0 / f->upem, c2 = m[2] / 65536.0 / f->upem, d = m[3] / 65536.0 / f->upem;
            double pixels = fabs(a) > fabs(d) ? fabs(a) : fabs(d);
#define TX(px, py) ((px) * a + (py) * b)
#define TY(px, py) ((px) * c2 + (py) * d)
            for (DWORD at = 0; at + sizeof(TTPOLYGONHEADER) <= size;) {
                const TTPOLYGONHEADER *ph = (const TTPOLYGONHEADER *)(buf + at);
                DWORD end = at + ph->cb;
                if (ph->cb < sizeof *ph || end > size) break;
                double x0 = fx(ph->pfxStart.x), y0 = fx(ph->pfxStart.y);
                path_point(path, TX(x0, y0), TY(x0, y0));
                for (at += sizeof *ph; at + 4 <= end;) {
                    const TTPOLYCURVE *pc = (const TTPOLYCURVE *)(buf + at);
                    int n = pc->cpfx;
                    if (pc->wType == TT_PRIM_LINE) {
                        for (int i = 0; i < n; i++) { x0 = fx(pc->apfx[i].x); y0 = fx(pc->apfx[i].y); path_point(path, TX(x0, y0), TY(x0, y0)); }
                    } else if (pc->wType == TT_PRIM_CSPLINE) {
                        for (int i = 0; i + 2 < n; i += 3) {
                            double x1 = fx(pc->apfx[i].x), y1 = fx(pc->apfx[i].y), x2 = fx(pc->apfx[i + 1].x), y2 = fx(pc->apfx[i + 1].y);
                            double x3 = fx(pc->apfx[i + 2].x), y3 = fx(pc->apfx[i + 2].y);
                            double len = (fabs(x1 - x0) + fabs(y1 - y0) + fabs(x2 - x1) + fabs(y2 - y1) + fabs(x3 - x2) + fabs(y3 - y2)) * pixels;
                            int steps = 2 + (int)(len / 3.0);
                            if (steps > 32) steps = 32;
                            for (int k = 1; k <= steps; k++) {
                                double t = (double)k / steps, u = 1 - t;
                                double px = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
                                double py = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
                                path_point(path, TX(px, py), TY(px, py));
                            }
                            x0 = x3; y0 = y3;
                        }
                    } else {                      /* TT_PRIM_QSPLINE: control points with implied on-curve midpoints */
                        for (int i = 0; i + 1 < n; i++) {
                            double x1 = fx(pc->apfx[i].x), y1 = fx(pc->apfx[i].y), x2 = fx(pc->apfx[i + 1].x), y2 = fx(pc->apfx[i + 1].y);
                            if (i + 2 < n) { x2 = (x1 + x2) / 2; y2 = (y1 + y2) / 2; }
                            double len = (fabs(x1 - x0) + fabs(y1 - y0) + fabs(x2 - x1) + fabs(y2 - y1)) * pixels;
                            int steps = 2 + (int)(len / 3.0);
                            if (steps > 32) steps = 32;
                            for (int k = 1; k <= steps; k++) {
                                double t = (double)k / steps, u = 1 - t, px = u * u * x0 + 2 * u * t * x1 + t * t * x2, py = u * u * y0 + 2 * u * t * y1 + t * t * y2;
                                path_point(path, TX(px, py), TY(px, py));
                            }
                            x0 = x2; y0 = y2;
                        }
                    }
                    at += 4 + (DWORD)n * sizeof(POINTFX);
                }
                path_close(path);
                at = end;
            }
#undef TX
#undef TY
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* ---------------------------------------------------------------- scan conversion (exact area coverage) */
static void cover_line(float *acc, int w, int h, double x0, double y0, double x1, double y1) {
    if (y0 == y1) return;
    double dir = 1;
    if (y0 > y1) { double t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1; }
    double dxdy = (x1 - x0) / (y1 - y0), x = x0;
    int ya = (int)floor(y0), yb = (int)ceil(y1);
    if (ya < 0) { x -= y0 * dxdy; ya = 0; y0 = 0; }
    if (yb > h) yb = h;
    for (int y = ya; y < yb; y++) {
        float *row = acc + (size_t)y * (size_t)(w + 2);
        double top = y > y0 ? y : y0, bottom = y + 1 < y1 ? y + 1 : y1, dy = bottom - top;
        if (dy <= 0) continue;
        double xn = x + dxdy * dy, d = dy * dir;
        double xa = x < xn ? x : xn, xb = x < xn ? xn : x;
        if (xa < 0) xa = 0;
        if (xb > w) xb = w;
        if (xb < xa) xb = xa;
        double fa = floor(xa), cb = ceil(xb);
        int ia = (int)fa, ib = (int)cb;
        if (ib <= ia + 1) {
            double mid = 0.5 * (xa + xb) - fa;
            row[ia] += (float)(d - d * mid);
            row[ia + 1] += (float)(d * mid);
        } else {
            double s = 1.0 / (xb - xa), fa0 = xa - fa, a0 = 0.5 * s * (1 - fa0) * (1 - fa0), fb1 = xb - cb + 1, am = 0.5 * s * fb1 * fb1;
            row[ia] += (float)(d * a0);
            if (ib == ia + 2) row[ia + 1] += (float)(d * (1 - a0 - am));
            else {
                double a1 = s * (1.5 - fa0);
                row[ia + 1] += (float)(d * (a1 - a0));
                for (int xi = ia + 2; xi < ib - 1; xi++) row[xi] += (float)(d * s);
                double a2 = a1 + (ib - ia - 3) * s;
                row[ib - 1] += (float)(d * (1 - a2 - am));
            }
            row[ib] += (float)(d * am);
        }
        x = xn;
    }
}

typedef struct { int lo_x, hi_y, w, h; uint8_t *bits; } Gray;

/* Coverage map of a path: lo_x = floor(xMin), top row = ceil(yMax) - 1, as iType's make_graymap. */
static void rasterise(const Path *path, Gray *g) {
    memset(g, 0, sizeof *g);
    if (!path->n) return;
    double xmin = path->p[0].x, xmax = xmin, ymin = path->p[0].y, ymax = ymin;
    for (int i = 1; i < path->n; i++) {
        if (path->p[i].x < xmin) xmin = path->p[i].x;
        if (path->p[i].x > xmax) xmax = path->p[i].x;
        if (path->p[i].y < ymin) ymin = path->p[i].y;
        if (path->p[i].y > ymax) ymax = path->p[i].y;
    }
    int left = (int)floor(xmin), top = (int)ceil(ymax), w = (int)ceil(xmax) - left, h = top - (int)floor(ymin);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    float *acc = calloc((size_t)(w + 2) * (size_t)h, sizeof(float));
    g->bits = malloc((size_t)w * (size_t)h);
    if (!acc || !g->bits) { free(acc); free(g->bits); g->bits = NULL; return; }
    for (int k = 0, from = 0; k < path->contours; from = path->end[k++]) {
        int to = path->end[k];
        for (int i = from; i < to; i++) {
            const Pt *p = &path->p[i], *q = &path->p[i + 1 < to ? i + 1 : from];
            cover_line(acc, w, h, p->x - left, top - p->y, q->x - left, top - q->y);
        }
    }
    for (int y = 0; y < h; y++) {
        double sum = 0;
        for (int x = 0; x < w; x++) {
            sum += acc[(size_t)y * (size_t)(w + 2) + (size_t)x];
            double v = fabs(sum);
            g->bits[y * w + x] = (uint8_t)(v >= 1 ? 255 : (int)(v * 255.0 + 0.5));
        }
    }
    free(acc);
    g->lo_x = left;
    g->hi_y = top - 1;
    g->w = w;
    g->h = h;
}

/* iType's outline_graymap for 8-bit maps (see the header comment). */
static void outline_effect(Gray *g, int n, int unfilled) {
    if (!g->w || !g->h || n < 1) return;
    int pad = n + 1, w = g->w + 2 * pad, h = g->h + 2 * pad;
    uint8_t *buf = calloc((size_t)w, (size_t)h), *tmp = malloc((size_t)w * (size_t)h);
    if (!buf || !tmp) { free(buf); free(tmp); return; }
    for (int y = 0; y < g->h; y++) memcpy(buf + (size_t)(y + pad) * (size_t)w + pad, g->bits + y * g->w, (size_t)g->w);
    for (int pass = n; pass > 0; pass--) {
        memcpy(tmp, buf, (size_t)w * (size_t)h);
        for (int y = 1; y < h - 1; y++) {
            const uint8_t *up = tmp + (size_t)(y - 1) * (size_t)w, *cur = up + w, *down = cur + w;
            uint8_t *out = buf + (size_t)y * (size_t)w;
            for (int x = 1; x < w - 1; x++) {
                if (cur[x]) { out[x] = 255; continue; }
                uint8_t m = up[x];
                if (down[x] > m) m = down[x];
                if (cur[x - 1] > m) m = cur[x - 1];
                if (cur[x + 1] > m) m = cur[x + 1];
                if (!(pass & 1)) {
                    if (up[x - 1] > m) m = up[x - 1];
                    if (up[x + 1] > m) m = up[x + 1];
                    if (down[x - 1] > m) m = down[x - 1];
                    if (down[x + 1] > m) m = down[x + 1];
                }
                out[x] = m;
            }
        }
    }
    if (unfilled)
        for (int y = 0; y < g->h; y++)
            for (int x = 0; x < g->w; x++) buf[(size_t)(y + pad) * (size_t)w + (size_t)(x + pad)] -= g->bits[y * g->w + x];
    free(tmp);
    free(g->bits);
    g->bits = buf;
    g->w = w;
    g->h = h;
    g->lo_x -= pad;
    g->hi_y += pad;
}

/* ---------------------------------------------------------------- engine operations (no guest types) */
static FontData *current(Engine *e) { return e->current >= 0 ? e->fonts[e->current].fd : NULL; }

static int32_t advance_fixed(const Engine *e, const FontData *f, uint32_t glyph) {      /* 16.16 pixels */
    return (int32_t)(((int64_t)advance_units(f, glyph) * e->scale[0] + f->upem / 2) / f->upem);
}

/* Builds a glyph map (malloc) for a character; never NULL unless memory runs out. */
static uint8_t *make_glyphmap(Engine *e, uint32_t code, unsigned type) {
    FontData *f = current(e);
    Gray g;
    Path path;
    int32_t dx = 0;
    memset(&g, 0, sizeof g);
    memset(&path, 0, sizeof path);
    if (f) {
        uint32_t glyph = map_char(f, code);
        dx = advance_fixed(e, f, glyph);
        glyph_path(f, glyph, e->scale, &path);
        rasterise(&path, &g);
        free(path.p);
        free(path.end);
        if (e->flags & (FLAG_OUTLINED_FILLED | FLAG_OUTLINED_UNFILLED)) outline_effect(&g, e->outline, (e->flags & FLAG_OUTLINED_UNFILLED) != 0);
    }
    size_t size = GLYPH_HEADER + (size_t)g.w * (size_t)g.h + 7;
    uint8_t *map = calloc(1, size + 8);
    if (map) {
        put32(map + 8, g.bits ? (int32_t)size : GLYPH_HEADER);
        put16(map + 0xc, g.lo_x);
        put16(map + 0xe, g.hi_y);
        put16(map + 0x10, (dx + 0x8000) >> 16);
        put32(map + 0x14, dx);
        put16(map + 0x1c, g.w);
        put16(map + 0x1e, g.h);
        put16(map + 0x20, g.w);
        put16(map + 0x24, 8);
        put16(map + 0x26, (int)type);
        if (g.bits) memcpy(map + GLYPH_HEADER, g.bits, (size_t)g.w * (size_t)g.h);
    }
    free(g.bits);
    return map;
}

static int fill_metrics(Engine *e, uint8_t *m) {
    FontData *f = current(e);
    memset(m, 0, METRICS_SIZE);
    if (!f) return ERR_NO_FONT;
    put16(m, f->upem);
    put16(m + 2, (int)be16(f->head + 44));
    if (f->os2) {
        put16(m + 4, (int)be16(f->os2 + 74));
        put16(m + 6, (int)be16(f->os2 + 76));
        put16(m + 8, s16(f->os2 + 68));
        put16(m + 0xa, s16(f->os2 + 70));
        put16(m + 0xc, (int)be16(f->os2 + 62));
        put16(m + 0xe, s16(f->os2 + 72));
        put16(m + 0x16, (int)be16(f->os2 + 8));
    }
    put16(m + 0x10, s16(f->hhea + 4));
    put16(m + 0x12, s16(f->hhea + 6));
    put16(m + 0x14, s16(f->hhea + 8));
    for (int i = 0; i < 4; i++) put16(m + 0x18 + i * 2, s16(f->head + 36 + i * 2));
    put16(m + 0x20, f->cff ? 0x80 : 1);
    if (f->cmap) {
        int n = (int)be16(f->cmap + 2);
        if (n > 20) n = 20;
        put16(m + 0x22, n);
        for (int i = 0; i < n; i++) { put16(m + 0x24 + i * 8, (int)be16(f->cmap + 4 + i * 8)); put16(m + 0x26 + i * 8, (int)be16(f->cmap + 6 + i * 8)); }
    }
    name_string(f, 1, (char *)m + 0xcc, 50, NULL, 0);
    name_string(f, 4, (char *)m + 0xfe, 50, NULL, 0);
    name_string(f, 0, (char *)m + 0x130, 0x800, NULL, 0);
    put16(m + 0x930, f->upem);
    put32(m + 0x934, f->glyphs);
    return 0;
}

/* FSS_get_kerning: the TrueType kern table (format 0, horizontal), in 16.16 pixels. */
static int32_t kern_fixed(const Engine *e, const FontData *f, uint32_t code1, uint32_t code2) {
    if (!f->kern || f->kern_len < 4 || be16(f->kern)) return 0;
    uint32_t g1 = map_char(f, code1), g2 = map_char(f, code2), key = g1 << 16 | g2, tables = be16(f->kern + 2);
    const uint8_t *t = f->kern + 4;
    int units = 0;
    if (!g1 || !g2) return 0;
    for (uint32_t i = 0; i < tables && t + 14 <= f->kern + f->kern_len; i++, t += be16(t + 2)) {
        if ((be16(t + 4) & 0xff07) != 1) continue;               /* format 0, horizontal, not cross-stream */
        uint32_t lo = 0, hi = be16(t + 6);
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, k = be32(t + 14 + mid * 6);
            if (k < key) lo = mid + 1; else if (k > key) hi = mid; else { units += s16(t + 14 + mid * 6 + 4); break; }
        }
    }
    return (int32_t)((int64_t)units * e->scale[0] / f->upem);
}

/* ---------------------------------------------------------------- GPOS kerning (OtfKerningTable) */
typedef struct { uint16_t g1, g2; int16_t adv, plc; } KernPair;
typedef struct { KernPair *v; int n, cap; } KernList;

static void kern_add(KernList *l, unsigned g1, unsigned g2, int adv, int plc) {
    if (!(adv & 0xffff) && !(plc & 0xffff)) return;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 1024; l->v = realloc(l->v, (size_t)l->cap * sizeof(KernPair)); }
    l->v[l->n].g1 = (uint16_t)g1;
    l->v[l->n].g2 = (uint16_t)g2;
    l->v[l->n].adv = (int16_t)adv;
    l->v[l->n++].plc = (int16_t)plc;
}
static int kern_compare(const void *a, const void *b) {
    const KernPair *p = a, *q = b;
    return p->g1 != q->g1 ? (int)p->g1 - (int)q->g1 : (int)p->g2 - (int)q->g2;
}
static int value_size(unsigned fmt) { int n = 0; for (; fmt; fmt >>= 1) n += (int)(fmt & 1) * 2; return n; }
static void value_read(const uint8_t *p, unsigned fmt, int *adv, int *plc) {     /* xAdvance and xPlacement of a ValueRecord */
    *adv = *plc = 0;
    if (fmt & 1) { *plc = s16(p); p += 2; }
    if (fmt & 2) p += 2;
    if (fmt & 4) *adv = s16(p);
}
/* Calls fn(glyph, coverage index) for every glyph of a Coverage table. */
typedef void (*CoverFn)(void *ctx, unsigned glyph, unsigned index);
static void coverage_each(const uint8_t *cov, CoverFn fn, void *ctx) {
    unsigned n = be16(cov + 2);
    if (be16(cov) == 1) for (unsigned i = 0; i < n; i++) fn(ctx, be16(cov + 4 + i * 2), i);
    else if (be16(cov) == 2)
        for (unsigned i = 0; i < n; i++) {
            const uint8_t *r = cov + 4 + i * 6;
            for (unsigned g = be16(r); g <= be16(r + 2) && g <= 0xffff; g++) fn(ctx, g, be16(r + 4) + (g - be16(r)));
        }
}
static unsigned class_of(const uint8_t *def, unsigned glyph) {
    if (be16(def) == 1) { unsigned first = be16(def + 2), n = be16(def + 4); return glyph >= first && glyph < first + n ? be16(def + 6 + (glyph - first) * 2) : 0; }
    if (be16(def) == 2) for (unsigned i = 0, n = be16(def + 2); i < n; i++) { const uint8_t *r = def + 4 + i * 6; if (glyph >= be16(r) && glyph <= be16(r + 2)) return be16(r + 4); }
    return 0;
}
typedef struct { KernList *list; const uint8_t *sub; uint16_t *second; int nsecond; } PairCtx;
static void pair_glyphs(void *ctx, unsigned glyph, unsigned index) {             /* PairPos format 1 */
    PairCtx *x = ctx;
    const uint8_t *s = x->sub;
    unsigned f1 = be16(s + 4), f2 = be16(s + 6);
    if (index >= be16(s + 8)) return;
    const uint8_t *set = s + be16(s + 10 + index * 2);
    int rec = 2 + value_size(f1) + value_size(f2), adv, plc;
    for (unsigned i = 0, n = be16(set); i < n; i++) {
        const uint8_t *r = set + 2 + i * (unsigned)rec;
        value_read(r + 2, f1, &adv, &plc);
        kern_add(x->list, glyph, be16(r), adv, plc);
    }
}
static void pair_classes(void *ctx, unsigned glyph, unsigned index) {            /* PairPos format 2 */
    PairCtx *x = ctx;
    const uint8_t *s = x->sub;
    unsigned f1 = be16(s + 4), f2 = be16(s + 6), c1 = class_of(s + be16(s + 8), glyph), n1 = be16(s + 12), n2 = be16(s + 14);
    int rec = value_size(f1) + value_size(f2), adv, plc;
    (void)index;
    if (c1 >= n1) return;
    for (int i = 0; i < x->nsecond; i++) {
        unsigned g2 = x->second[i * 2], c2 = x->second[i * 2 + 1];
        if (c2 >= n2) continue;
        value_read(s + 16 + (c1 * n2 + c2) * (unsigned)rec, f1, &adv, &plc);
        kern_add(x->list, glyph, g2, adv, plc);
    }
}
typedef struct { KernList *list; const uint8_t *sub; } SingleCtx;
static void single_values(void *ctx, unsigned glyph, unsigned index) {           /* SinglePos formats 1 and 2 */
    SingleCtx *x = ctx;
    const uint8_t *s = x->sub;
    unsigned fmt = be16(s + 4);
    int adv, plc;
    if (be16(s) == 1) value_read(s + 6, fmt, &adv, &plc);
    else if (be16(s) == 2 && index < be16(s + 6)) value_read(s + 8 + index * (unsigned)value_size(fmt), fmt, &adv, &plc);
    else return;
    kern_add(x->list, glyph, 0xffff, adv, plc);
}
/* Offset of a feature table (from the feature list), found through script latn / kana / DFLT as the SDK does. */
static const uint8_t *gpos_feature(const uint8_t *gpos, const char *tag) {
    const uint8_t *scripts = gpos + be16(gpos + 4), *features = gpos + be16(gpos + 6);
    for (unsigned i = 0, n = be16(scripts); i < n; i++) {
        const uint8_t *rec = scripts + 2 + i * 6;
        if (memcmp(rec, "latn", 4) && memcmp(rec, "kana", 4) && memcmp(rec, "DFLT", 4)) continue;
        const uint8_t *script = scripts + be16(rec + 4);
        unsigned off = be16(script);
        if (!off) { if (!be16(script + 2)) continue; off = be16(script + 8); }
        const uint8_t *lang = script + off;
        for (unsigned k = 0, count = be16(lang + 4); k < count; k++) {
            unsigned index = be16(lang + 6 + k * 2);
            if (index < be16(features) && !memcmp(features + 2 + index * 6, tag, 4)) return features + be16(features + 2 + index * 6 + 4);
        }
        return NULL;
    }
    return NULL;
}
static void gpos_collect(const FontData *f, KernList *list, int kern_only) {
    const uint8_t *gpos = f->gpos;
    if (!gpos || f->gpos_len < 10) return;
    const uint8_t *lookups = gpos + be16(gpos + 8);
    for (int pass = 0; pass < (kern_only ? 1 : 2); pass++) {
        const uint8_t *feature = gpos_feature(gpos, pass ? "palt" : "kern");
        if (!feature) continue;
        for (unsigned i = 0, n = be16(feature + 2); i < n; i++) {
            unsigned index = be16(feature + 4 + i * 2);
            if (index >= be16(lookups)) continue;
            const uint8_t *lookup = lookups + be16(lookups + 2 + index * 2);
            if (be16(lookup) != (pass ? 1u : 2u)) continue;      /* pair adjustment for kern, single adjustment for palt */
            for (unsigned k = 0, subs = be16(lookup + 4); k < subs; k++) {
                const uint8_t *sub = lookup + be16(lookup + 6 + k * 2);
                unsigned fmt = be16(sub);
                if (pass) { SingleCtx ctx = {list, sub}; coverage_each(sub + be16(sub + 2), single_values, &ctx); continue; }
                PairCtx ctx = {list, sub, NULL, 0};
                if (fmt == 1) coverage_each(sub + be16(sub + 2), pair_glyphs, &ctx);
                else if (fmt == 2) {                              /* second glyphs: the ones the class definition names */
                    const uint8_t *def = sub + be16(sub + 10);
                    int cap = 0;
                    if (be16(def) == 1) cap = (int)be16(def + 4);
                    else if (be16(def) == 2) for (unsigned r = 0; r < be16(def + 2); r++) cap += (int)(be16(def + 4 + r * 6 + 2) - be16(def + 4 + r * 6)) + 1;
                    if (cap <= 0) continue;
                    ctx.second = malloc((size_t)cap * 2 * sizeof(uint16_t));
                    if (be16(def) == 1) for (unsigned g = 0; g < be16(def + 4); g++) { ctx.second[ctx.nsecond * 2] = (uint16_t)(be16(def + 2) + g); ctx.second[ctx.nsecond++ * 2 + 1] = (uint16_t)be16(def + 6 + g * 2); }
                    else for (unsigned r = 0; r < be16(def + 2); r++)
                        for (unsigned g = be16(def + 4 + r * 6); g <= be16(def + 4 + r * 6 + 2) && ctx.nsecond < cap; g++) { ctx.second[ctx.nsecond * 2] = (uint16_t)g; ctx.second[ctx.nsecond++ * 2 + 1] = (uint16_t)be16(def + 4 + r * 6 + 4); }
                    coverage_each(sub + be16(sub + 2), pair_classes, &ctx);
                    free(ctx.second);
                }
            }
        }
    }
    if (list->n) qsort(list->v, (size_t)list->n, sizeof(KernPair), kern_compare);
}
/* Entry with these two glyph indices, or NULL. */
static const KernPair *kern_find(const KernPair *v, int n, unsigned g1, unsigned g2) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (v[mid].g1 < g1 || (v[mid].g1 == g1 && v[mid].g2 < g2)) lo = mid + 1; else hi = mid;
    }
    return lo < n && v[lo].g1 == g1 && v[lo].g2 == g2 ? &v[lo] : NULL;
}
static int otf_raw(const FontData *f, const KernPair *v, int n, uint32_t code1, uint32_t code2) {
    unsigned g1 = map_char(f, code1) & 0xffff, g2 = map_char(f, code2) & 0xffff;
    const KernPair *a = kern_find(v, n, g1, 0xffff), *b = kern_find(v, n, g2, 0xffff), *p = kern_find(v, n, g1, g2);
    return (int16_t)((a ? a->adv - a->plc : 0) + (b ? b->plc : 0) + (p ? p->adv : 0));
}
static int otf_scale(int units, float per_unit, uint32_t size) { return (int)floorf((float)units * per_unit * (float)size + 0.5f); }

#ifndef FONTLL_TEST
/* ---------------------------------------------------------------- the imported functions */
#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define A6 (c->x[6])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HOST(a) ((uint8_t *)(uintptr_t)(a))

static Engine *engine(uint64_t key) {
    uint64_t slot, magic;
    memcpy(&slot, HOST(key), 8);
    memcpy(&magic, HOST(key) + 8, 8);
    if (magic == ENGINE_MAGIC && slot < MAX_ENGINES && g_engines[slot].key == key) return &g_engines[slot];
    EnterCriticalSection(&g_lock);
    Engine *e = NULL;
    for (int i = 0; i < MAX_ENGINES && !e; i++) if (g_engines[i].key == key) e = &g_engines[i];
    for (int i = 0; i < MAX_ENGINES && !e; i++) if (!g_engines[i].key) {
        e = &g_engines[i];
        memset(e, 0, sizeof *e);
        e->key = key;
        e->current = -1;
        e->scale[0] = e->scale[3] = 32 << 16;
        e->outline = 1;
    }
    LeaveCriticalSection(&g_lock);
    if (!e) { printf("nn::fontll: more than %d engines\n", MAX_ENGINES); report_and_exit(3); }
    slot = (uint64_t)(e - g_engines);
    magic = ENGINE_MAGIC;
    memcpy(HOST(key), &slot, 8);
    memcpy(HOST(key) + 8, &magic, 8);
    return e;
}

static void fe_ctor(Ctx *c) {
    uint64_t self = A0;
    memset(HOST(self), 0, 0xb8);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_ENGINES; i++) if (g_engines[i].key == self) g_engines[i].key = 0;      /* a new object at a reused address */
    LeaveCriticalSection(&g_lock);
    engine(self);
    RET(self);
}
static void fe_Initialize(Ctx *c) { Engine *e = engine(A0); e->work = A1; e->error = 0; memcpy(HOST(A0) + 0xb0, &e->work, 8); RET(0); }
static void fe_Finalize(Ctx *c) { Engine *e = engine(A0); uint64_t self = A0; EnterCriticalSection(&g_lock); e->key = 0; LeaveCriticalSection(&g_lock); memset(HOST(self), 0, 16); RET(0); }
static void fe_GetPointerToWorkBuffer(Ctx *c) { RET(engine(A0)->work); }
static void fe_GetError(Ctx *c) { RET((uint32_t)engine(A0)->error); }

/* (char *nameOut, const void *data, uint faceIndex, uint nameBufferLength) -> error */
static void fe_LoadFont(Ctx *c) {
    const uint8_t *data = HOST(A2);
    Engine *e = engine(A0);
    char *out = (char *)HOST(A1);
    size_t room = (size_t)(uint32_t)A4;
    if (!A2) { e->error = ERR_FONT_NOT_FOUND; RET(e->error); return; }
    if (e->count == MAX_FONTS) { printf("nn::fontll: more than %d fonts in one engine\n", MAX_FONTS); report_and_exit(3); }
    EnterCriticalSection(&g_lock);
    FontData *fd = font_data(data, (uint32_t)A3);
    LeaveCriticalSection(&g_lock);
    if (!fd) { printf("nn::fontll: LoadFont: not a usable font file at %p\n", (void *)data); e->error = ERR_FONT_NOT_FOUND; RET(e->error); return; }
    Font *f = &e->fonts[e->count];
    f->fd = fd;
    snprintf(f->name, sizeof f->name, "%s", fd->name);
    if (room > 0 && room < sizeof f->name) f->name[room - 1] = 0;
    if (flog(L_LOAD)) printf("fontll: LoadFont engine %llx data %p face %u -> \"%s\"\n", (unsigned long long)A0, (void *)data, (unsigned)A3, f->name);
    e->count++;
    if (out) { if (room == 0 || room > sizeof f->name) room = sizeof f->name; memcpy(out, f->name, room - 1); out[room - 1] = 0; }
    e->error = 0;
    RET(0);
}

static void fe_SetFont(Ctx *c) {
    const char *name = (const char *)HOST(A1);
    Engine *e = engine(A0);
    int found = -1;
    for (int i = 0; i < e->count; i++) if (!strcmp(e->fonts[i].name, name)) { found = i; break; }
    if (flog(L_SETFONT)) printf("fontll: SetFont engine %llx \"%s\" -> %d\n", (unsigned long long)A0, name, found);
    if (found < 0) { e->error = ERR_FONT_NOT_FOUND; RET(e->error); return; }
    e->current = found;
    e->error = 0;
    RET(0);
}

static void fe_SetScale(Ctx *c) {
    Engine *e = engine(A0);
    if (flog(L_SCALE)) printf("fontll: SetScale engine %llx %x %x %x %x\n", (unsigned long long)A0, (unsigned)A1, (unsigned)A2, (unsigned)A3, (unsigned)A4);
    e->scale[0] = (int32_t)A1;
    e->scale[1] = (int32_t)A2;
    e->scale[2] = (int32_t)A3;
    e->scale[3] = (int32_t)A4;
    e->error = current(e) ? 0 : ERR_NO_FONT;
    RET(e->error);
}

static void fe_SetBoldWeight(Ctx *c) {
    Engine *e = engine(A0);
    static volatile LONG warned;
    if (flog(L_BOLD)) printf("fontll: SetBoldWeight engine %llx %x\n", (unsigned long long)A0, (unsigned)A1);
    if ((uint32_t)A1 > 0x10000) { e->error = ERR_BAD_BOLD; RET(e->error); return; }
    if ((uint32_t)A1 && !InterlockedExchange(&warned, 1)) printf("nn::fontll: SetBoldWeight(%#x): emboldening is not implemented, glyphs stay regular\n", (unsigned)A1);
    e->bold = (int)A1;
    e->error = 0;
    RET(0);
}

static void fe_SetOutlineWidth(Ctx *c) {
    Engine *e = engine(A0);
    unsigned n = (unsigned)A1 & 0xffff;
    if (flog(L_OUTLINE)) printf("fontll: SetOutlineWidth engine %llx %u\n", (unsigned long long)A0, n);
    if (n < 1 || n > 10) { e->error = ERR_BAD_OUTLINE_WIDTH; RET(e->error); return; }
    e->outline = (int)n;
    e->error = 0;
    RET(0);
}

static void fe_SetFlags(Ctx *c) {                 /* Flags: 0 no effect, 1 outlined unfilled, 2 outlined filled */
    Engine *e = engine(A0);
    unsigned v = (unsigned)A1;
    if (flog(L_FLAGS)) printf("fontll: SetFlags engine %llx %u\n", (unsigned long long)A0, v);
    e->error = 0;
    if (v == 0) e->flags = 0;
    else if (v == 1) e->flags = FLAG_OUTLINED_UNFILLED;
    else if (v == 2) e->flags = FLAG_OUTLINED_FILLED;
    else e->error = ERR_BAD_FLAG;
    RET(e->error);
}
static void fe_SetAutoHint(Ctx *c) { engine(A0)->error = 0; RET(0); }

static void fe_GetFontMetrics(Ctx *c) {
    Engine *e = engine(A0);
    uint8_t *m = HOST(A1);
    e->error = fill_metrics(e, m);
    if (flog(L_METRICS)) {
        int16_t v[6];
        memcpy(v, m + 4, 4); memcpy(v + 2, m + 0x18, 8);
        printf("fontll: GetFontMetrics engine %llx font %d \"%s\" -> error %#x, unitsPerEm %u win %d %d bbox %d %d %d %d\n", (unsigned long long)A0, e->current,
               e->current >= 0 ? e->fonts[e->current].name : "", e->error, current(e) ? (unsigned)current(e)->upem : 0, v[0], v[1], v[2], v[3], v[4], v[5]);
    }
    RET(e->error);
}

static void fe_CheckGlyphExist(Ctx *c) {
    Engine *e = engine(A0);
    FontData *f = current(e);
    int yes = f && (map_char(f, (uint32_t)A1) & 0xffff) != 0;
    e->error = f ? 0 : ERR_NO_FONT;
    if (flog(L_CHECK)) printf("fontll: CheckGlyphExist engine %llx font %d code %x -> %d\n", (unsigned long long)A0, e->current, (unsigned)A1, yes);
    RET(yes);
}

/* (short *idx, short *idy, int *dx, int *dy, uint code, ushort type) -> error */
static void fe_GetAdvance(Ctx *c) {
    Engine *e = engine(A0);
    FontData *f = current(e);
    int32_t dx = f ? advance_fixed(e, f, map_char(f, (uint32_t)A5)) : 0;
    e->error = f ? 0 : ERR_NO_FONT;
    if (A1) put16(HOST(A1), (dx + 0x8000) >> 16);
    if (A2) put16(HOST(A2), 0);
    if (A3) put32(HOST(A3), dx);
    if (A4) put32(HOST(A4), 0);
    if (flog(L_ADVANCE)) printf("fontll: GetAdvance engine %llx font %d code %x type %x -> %.3f\n", (unsigned long long)A0, e->current, (unsigned)A5, (unsigned)A6, dx / 65536.0);
    RET(e->error);
}

/* (uint code, ushort type) -> GlyphMap *, released with ReleasesGlyph */
static void fe_AcquireGlyphmap(Ctx *c) {
    Engine *e = engine(A0);
    uint8_t *map = make_glyphmap(e, (uint32_t)A1, (unsigned)A2 & 0xffff);
    e->error = map ? (current(e) ? 0 : ERR_NO_FONT) : 0xc9;
    if (map && flog(L_GLYPH)) {
        int16_t v[4];
        memcpy(v, map + 0xc, 4); memcpy(v + 2, map + 0x1c, 4);
        printf("fontll: AcquireGlyphmap engine %llx font %d size %d code %x flags %x outline %d -> %dx%d lo_x %d hi_y %d\n", (unsigned long long)A0, e->current,
               e->scale[0] >> 16, (unsigned)A1, e->flags, e->outline, v[2], v[3], v[0], v[1]);
    }
    RET((uint64_t)(uintptr_t)map);
}
static void fe_ReleasesGlyph(Ctx *c) { free(HOST(A1)); engine(A0)->error = 0; RET(0); }

/* (int *dx, int *dy, uint code1, uint code2) -> error; 16.16 pixels */
static void fe_GetKerning(Ctx *c) {
    Engine *e = engine(A0);
    FontData *f = current(e);
    int32_t dx = f ? kern_fixed(e, f, (uint32_t)A3, (uint32_t)A4) : 0;
    if (A1) put32(HOST(A1), dx);
    if (A2) put32(HOST(A2), 0);
    e->error = f ? 0 : ERR_NO_FONT;
    if (flog(L_KERN)) printf("fontll: GetKerning engine %llx font %d codes %x %x -> %.3f\n", (unsigned long long)A0, e->current, (unsigned)A3, (unsigned)A4, dx / 65536.0);
    RET(e->error);
}

/* (void *(*alloc)(size_t size, size_t align, void *user), void *user, bool kernOnly) -> OtfKerningTable * or NULL */
static void fe_InitializeOtfKerningTable(Ctx *c) {
    Engine *e = engine(A0);
    FontData *f = current(e);
    uint64_t alloc = A1, user = A2;
    int kern_only = (int)(A3 & 1);
    KernList list = {NULL, 0, 0};
    if (!f || !f->gpos) { RET(0); return; }
    gpos_collect(f, &list, kern_only);
    c->x[0] = 0x18; c->x[1] = 4; c->x[2] = user;
    call_guest(c, alloc);
    uint64_t tab = c->x[0], entries = 0;
    if (tab) {
        c->x[0] = (uint64_t)list.n * 8; c->x[1] = 4; c->x[2] = user;
        call_guest(c, alloc);
        entries = c->x[0];
        float per_unit = 1.0f / (float)f->upem;
        int32_t count = entries ? list.n : 0;
        memset(HOST(tab), 0, 0x18);
        memcpy(HOST(tab), &count, 4);
        memcpy(HOST(tab) + 8, &entries, 8);
        memcpy(HOST(tab) + 0x10, &per_unit, 4);
        if (entries && list.n) memcpy(HOST(entries), list.v, (size_t)list.n * 8);
    }
    printf("nn::fontll: OTF kerning table of \"%s\": %d entries%s\n", f->name, list.n, kern_only ? " (kern only)" : "");
    free(list.v);
    RET(tab);
}
/* (OtfKerningTable *, void (*free)(void *, void *user), void *user) */
static void fe_FinalizeOtfKerningTable(Ctx *c) {
    uint64_t tab = A1, fn = A2, user = A3, entries;
    if (!tab) return;
    memcpy(&entries, HOST(tab) + 8, 8);
    if (entries) { c->x[0] = entries; c->x[1] = user; call_guest(c, fn); }
    c->x[0] = tab; c->x[1] = user;
    call_guest(c, fn);
}
static int otf_table(uint64_t tab, const KernPair **v, float *per_unit) {
    int32_t n;
    uint64_t entries;
    if (!tab) return -1;
    memcpy(&n, HOST(tab), 4);
    memcpy(&entries, HOST(tab) + 8, 8);
    memcpy(per_unit, HOST(tab) + 0x10, 4);
    *v = (const KernPair *)HOST(entries);
    return n;
}
/* (table, code1, code2, size) -> pixels */
static void fe_AcquireOtfKerning(Ctx *c) {
    Engine *e = engine(A0);
    FontData *f = current(e);
    const KernPair *v;
    float per_unit;
    int n = otf_table(A1, &v, &per_unit), r = 0;
    if (f && n >= 0) r = otf_scale(otf_raw(f, v, n, (uint32_t)A2, (uint32_t)A3), per_unit, (uint32_t)A4);
    if (flog(L_OTF)) printf("fontll: AcquireOtfKerning font %d codes %x %x size %u -> %d\n", e->current, (unsigned)A2, (unsigned)A3, (unsigned)A4, r);
    RET((uint32_t)r);
}
/* (table, code, size): placement of the first glyph of a line */
static void fe_AcquireOtfKerningFirst(Ctx *c) {
    Engine *e = engine(A0);
    FontData *f = current(e);
    const KernPair *v, *p;
    float per_unit;
    int n = otf_table(A1, &v, &per_unit), r = 0;
    if (n >= 0) { p = f ? kern_find(v, n, map_char(f, (uint32_t)A2) & 0xffff, 0xffff) : NULL; r = otf_scale(p ? p->plc : 0, per_unit, (uint32_t)A3); }
    RET((uint32_t)r);
}
/* (table, code, size): what remains after the last glyph of a line */
static void fe_AcquireOtfKerningLast(Ctx *c) {
    Engine *e = engine(A0);
    FontData *f = current(e);
    const KernPair *v, *p;
    float per_unit;
    int n = otf_table(A1, &v, &per_unit), r = 0;
    if (n >= 0) { p = f ? kern_find(v, n, map_char(f, (uint32_t)A2) & 0xffff, 0xffff) : NULL; r = otf_scale(p ? (int16_t)(p->adv - p->plc) : 0, per_unit, (uint32_t)A3); }
    RET((uint32_t)r);
}

/* ScalableFontEngineHelper::Decode(const void *bfttf, uint size): decodes in place, returns the TrueType data. */
static void fe_Decode(Ctx *c) {
    uint8_t *p = HOST(A0);
    uint32_t size = (uint32_t)A1 & ~3u;
    static const uint8_t key[4] = {BFTTF_KEY >> 24, (BFTTF_KEY >> 16) & 0xff, (BFTTF_KEY >> 8) & 0xff, BFTTF_KEY & 0xff};
    if (!p || size < 12) { RET(0); return; }
    for (uint32_t i = 0; i < size; i++) p[i] ^= key[i & 3];
    RET((uint64_t)(uintptr_t)(p + 8));
}

#define F(n) "_ZN2nn6fontll18ScalableFontEngine" n
static const ImportDef fontll_imports[] = {
    {F("C1Ev"), fe_ctor}, {F("C2Ev"), fe_ctor}, {F("10InitializeEPvj"), fe_Initialize}, {F("8FinalizeEv"), fe_Finalize}, {F("8GetErrorEv"), fe_GetError},
    {F("22GetPointerToWorkBufferEv"), fe_GetPointerToWorkBuffer}, {F("8LoadFontEPcPKvjj"), fe_LoadFont}, {F("7SetFontEPc"), fe_SetFont},
    {F("8SetScaleEiiii"), fe_SetScale}, {F("13SetBoldWeightEi"), fe_SetBoldWeight}, {F("15SetOutlineWidthEt"), fe_SetOutlineWidth},
    {F("8SetFlagsENS1_5FlagsE"), fe_SetFlags}, {F("11SetAutoHintEb"), fe_SetAutoHint}, {F("14GetFontMetricsEPNS0_7MetricsE"), fe_GetFontMetrics},
    {F("15CheckGlyphExistEj"), fe_CheckGlyphExist}, {F("10GetAdvanceEPsS2_PiS3_jt"), fe_GetAdvance}, {F("15AcquireGlyphmapEjt"), fe_AcquireGlyphmap},
    {F("13ReleasesGlyphEPv"), fe_ReleasesGlyph}, {F("10GetKerningEPiS2_jj"), fe_GetKerning},
    {F("25InitializeOtfKerningTableEPFPvmmS2_ES2_b"), fe_InitializeOtfKerningTable},
    {F("23FinalizeOtfKerningTableEPNS0_15OtfKerningTableEPFvPvS4_ES4_"), fe_FinalizeOtfKerningTable},
    {F("17AcquireOtfKerningEPKNS0_15OtfKerningTableEjjj"), fe_AcquireOtfKerning},
    {F("21AcquireOtfKerningLastEPKNS0_15OtfKerningTableEjj"), fe_AcquireOtfKerningLast},
    {F("22AcquireOtfKerningFirstEPKNS0_15OtfKerningTableEjj"), fe_AcquireOtfKerningFirst},
    {"_ZN2nn6fontll24ScalableFontEngineHelper6DecodeEPKvj", fe_Decode},
};

void register_fontll_imports(void) {
    InitializeCriticalSection(&g_lock);
    g_dc = CreateCompatibleDC(NULL);
    register_imports(fontll_imports, (int)(sizeof fontll_imports / sizeof fontll_imports[0]));
}

#else
/* ---------------------------------------------------------------- offline test: clang -DFONTLL_TEST nnfontll.c -lgdi32
 * fonttest <font file> <pixels> <text or U+hex ...> [outline n]: metrics, advances, glyph maps as text, kerning. */
int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: fonttest <font> <pixels> <text|U+xxxx> [outline width] [pgm prefix]\n"); return 1; }
    InitializeCriticalSection(&g_lock);
    g_dc = CreateCompatibleDC(NULL);
    FILE *in = fopen(argv[1], "rb");
    if (!in) return 1;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size + 16);
    fread(data, 1, (size_t)size, in);
    fclose(in);
    static Engine e;
    e.fonts[0].fd = font_data(data, 0);
    if (!e.fonts[0].fd) { printf("not a font\n"); return 1; }
    FontData *f = e.fonts[0].fd;
    e.count = 1;
    e.current = 0;
    e.scale[0] = e.scale[3] = atoi(argv[2]) << 16;
    e.outline = argc > 4 ? atoi(argv[4]) : 0;
    static uint8_t m[METRICS_SIZE];
    fill_metrics(&e, m);
    int16_t v[16];
    memcpy(v, m, 32);
    printf("metrics: upem %u macStyle %x win %d %d typo %d %d %d hhea %d %d %d bbox %d %d %d %d family \"%s\" name \"%s\"\n", (uint16_t)v[0], (uint16_t)v[1], v[2], v[3], v[4], v[5],
           v[7], v[8], v[9], v[10], v[12], v[13], v[14], v[15], (char *)m + 0xcc, (char *)m + 0xfe);
    KernList list = {NULL, 0, 0};
    gpos_collect(f, &list, 0);
    int singles = 0;
    for (int i = 0; i < list.n; i++) singles += list.v[i].g2 == 0xffff;
    printf("GPOS: %d entries (%d single adjustments)\n", list.n, singles);
    uint32_t codes[256];
    int n = 0;
    if (!strncmp(argv[3], "U+", 2)) codes[n++] = (uint32_t)strtoul(argv[3] + 2, NULL, 16);
    else for (const char *p = argv[3]; *p && n < 256; p++) codes[n++] = (uint8_t)*p;
    for (int i = 0; i < n; i++) {
        uint32_t glyph = map_char(f, codes[i]);
        GLYPHMETRICS gm;
        memset(&gm, 0, sizeof gm);
        gdi_prepare(f);
        SelectObject(g_dc, f->hfont);
        GetGlyphOutlineW(g_dc, glyph, GGO_METRICS | GGO_GLYPH_INDEX, &gm, 0, NULL, &identity);
        WCHAR ch = (WCHAR)codes[i];
        WORD gi = 0xffff;
        GetGlyphIndicesW(g_dc, &ch, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS);
        printf("U+%04X glyph %u (GDI says %u)  hmtx advance %d units (GDI at em size: %d) = %.4f px -> i_dx %d", codes[i], glyph, gi, advance_units(f, glyph), gm.gmCellIncX,
               advance_fixed(&e, f, glyph) / 65536.0, (advance_fixed(&e, f, glyph) + 0x8000) >> 16);
        if (i + 1 < n) printf("  kern with next: table %.3f px, GPOS raw %d units -> %d px", kern_fixed(&e, f, codes[i], codes[i + 1]) / 65536.0, otf_raw(f, list.v, list.n, codes[i], codes[i + 1]),
                              otf_scale(otf_raw(f, list.v, list.n, codes[i], codes[i + 1]), 1.0f / (float)f->upem, (uint32_t)atoi(argv[2])));
        printf("\n");
        for (int pass = 0; pass < (e.outline ? 3 : 1); pass++) {
            e.flags = pass == 1 ? FLAG_OUTLINED_FILLED : pass == 2 ? FLAG_OUTLINED_UNFILLED : 0;
            uint8_t *map = make_glyphmap(&e, codes[i], 8);
            int16_t g[4];
            memcpy(g, map + 0xc, 4); memcpy(g + 2, map + 0x1c, 4);
            printf("  %s: %dx%d lo_x %d hi_y %d\n", pass == 1 ? "outlined filled" : pass == 2 ? "outlined unfilled" : "plain", g[2], g[3], g[0], g[1]);
            if (argc > 5) {
                char path[260];
                snprintf(path, sizeof path, "%s_%04x_%d.pgm", argv[5], codes[i], pass);
                FILE *out = fopen(path, "wb");
                fprintf(out, "P5\n%d %d\n255\n", g[2] ? g[2] : 1, g[3] ? g[3] : 1);
                fwrite(map + GLYPH_HEADER, 1, g[2] && g[3] ? (size_t)(g[2] * g[3]) : 1, out);
                fclose(out);
            } else if (g[2] <= 100)
                for (int y = 0; y < g[3]; y++) { printf("    "); for (int x = 0; x < g[2]; x++) putchar(" .:-=+*#%@"[map[GLYPH_HEADER + y * g[2] + x] * 9 / 255]); putchar('\n'); }
            free(map);
        }
    }
    return 0;
}
#endif
