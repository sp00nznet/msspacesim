/*
 * vga.c - the VGA, programmed directly.
 *
 * Space Simulator's built-in VGA driver runs 320x400: BIOS mode 13h, chain-4
 * off and the CRTC's double scan off, so one byte address at A000 covers four
 * pixels, one per plane, and the Map Mask picks which planes a write lands in.
 * It also uses the parts of the card a flat framebuffer does not have:
 *
 *   - latches: a read loads one byte from every plane, and write mode 1 stores
 *     those four bytes back;
 *   - the CRTC start address, which is how pages flip (nothing is copied);
 *   - the Graphics Controller's set/reset, bit mask and logical ops.
 *
 * (This model came from the Blake Stone recomp, which needed the same card.)
 *
 * VRAM is stored interleaved, vram[offset*4 + plane], which also makes chain-4
 * mode a plain linear store (plane = addr & 3, offset = addr >> 2).
 */
#include "machine.h"
#include "recomp16/platform/font8x8.h"
#ifdef SPACESIM_DEBUG
#include <intrin.h>
#endif

static uint8_t vram[0x40000];
static uint8_t latch[4];
static uint8_t seq[8], gc[16], crtc[32], attr[32];
static uint8_t seq_i, gc_i, crtc_i, attr_i, attr_flip;
static uint8_t dac[256][3], dac_wi, dac_ri, dac_wc, dac_rc, dac_mask = 0xFF;
static int mode = 3;                 /* BIOS mode: 3 text, 0x13 graphics */

/* Super VGA, through the VESA BIOS Extensions 1.2: one linear megabyte seen
 * through a 64 KB window at A000 whose position the driver sets (function
 * 05h, or the window function's far pointer), and a display start it moves to
 * flip pages (07h). SVGA.DRV (640x400) and SVGA8.DRV (800x600) take this path
 * when SETUP's chipset is "VESA 1.2 Compatible". */
static uint8_t svga[1 << 20];
static int svga_mode;                /* 0, or the VESA mode set */
static int svga_w, svga_h;
static unsigned svga_bank;           /* window position, in 64 KB units */
static unsigned svga_start;          /* display start, in bytes */
static const struct { uint16_t mode, w, h; } vbe_modes[] = {
    { 0x100, 640, 400 }, { 0x101, 640, 480 }, { 0x103, 800, 600 }, { 0x105, 1024, 768 } };
static int chain4 = 0;
static uint8_t misc_out = 0x63;

uint8_t *vga_text_mem(void) { return g_cpu.mem + 0xB8000; }

static inline int chained(void) { return (seq[4] & 8) != 0; }

void vga_set_mode(int m)
{
    svga_mode = 0;
    mode = m & 0x7F;
    memset(seq, 0, sizeof seq); memset(gc, 0, sizeof gc); memset(crtc, 0, sizeof crtc);
    seq[2] = 0x0F;
    gc[8] = 0xFF;
    gc[6] = 0x05;
    if (mode == 0x13) {
        seq[4] = 0x0E;               /* chain-4, odd/even off, extended memory */
        crtc[0x09] = 0x41;           /* double scan: 200 rows on 400 lines */
        crtc[0x12] = 0x8F;           /* vertical display end 399 (with overflow bit) */
        crtc[0x07] = 0x1F;
        crtc[0x13] = 40;             /* 320 bytes per row in dword mode */
        crtc[0x14] = 0x40;           /* dword mode */
        crtc[0x17] = 0xA3;
        gc[5] = 0x40;
        if (!(m & 0x80)) memset(vram, 0, sizeof vram);
    } else {
        crtc[0x09] = 0x0F;
        if (!(m & 0x80)) {
            uint8_t *t = vga_text_mem();
            for (int i = 0; i < 80 * 25; i++) { t[i * 2] = ' '; t[i * 2 + 1] = 7; }
        }
    }
    for (int i = 0; i < 16; i++) attr[i] = (uint8_t)i;
    attr[0x10] = (mode == 0x13) ? 0x41 : 0x0C;
    g_cpu.mem[0x449] = (uint8_t)mode;
    chain4 = chained();
}

int vga_mode(void) { return mode; }

void vga_init(void)
{
    /* The default VGA palette's first 16 entries, as text mode shows them. */
    static const uint8_t ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};
    memset(dac, 0, sizeof dac);
    memcpy(dac, ega, sizeof ega);
    /* text mode reads attr -> DAC through the EGA-compatible mapping */
    vga_set_mode(3);
}

/* ---- CPU access to A000-AFFF ------------------------------------------- */

int recomp_mem_write8(CPU *cpu, uint32_t a, uint8_t v)
{
#ifdef SPACESIM_DEBUG
    { void watch_write(uint32_t, uint8_t); watch_write(a, v); }
#endif
    if (a - 0xA0000u >= 0x10000u) return 0;
#ifdef SPACESIM_DEBUG
    { void vgaprof_note(void *ra); vgaprof_note(_ReturnAddress()); }
#endif
    (void)cpu;
    uint32_t off = a - 0xA0000u;
    if (svga_mode) { svga[((svga_bank << 16) + off) & 0xFFFFF] = v; return 1; }
    if (chain4) { vram[off] = v; return 1; }
    uint8_t mask = seq[2] & 0x0F;
    uint8_t *p = &vram[off * 4];
    int wmode = gc[5] & 3;
    if (wmode == 0 && !(gc[1] & 0x0F) && !(gc[3]) && gc[8] == 0xFF) {
        if (mask & 1) p[0] = v;
        if (mask & 2) p[1] = v;
        if (mask & 4) p[2] = v;
        if (mask & 8) p[3] = v;
        return 1;
    }
    if (wmode == 1) {
        for (int i = 0; i < 4; i++) if (mask & (1 << i)) p[i] = latch[i];
        return 1;
    }
    uint8_t rot = gc[3] & 7, fn = (gc[3] >> 3) & 3, bm = gc[8];
    uint8_t r = (uint8_t)((v >> rot) | (v << (8 - rot)));
    if (wmode == 3) { bm &= r; }
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1 << i))) continue;
        uint8_t d;
        if (wmode == 2)       d = (v & (1 << i)) ? 0xFF : 0;
        else if (wmode == 3)  d = (gc[0] & (1 << i)) ? 0xFF : 0;
        else                  d = (gc[1] & (1 << i)) ? ((gc[0] & (1 << i)) ? 0xFF : 0) : r;
        switch (fn) {
        case 1: d &= latch[i]; break;
        case 2: d |= latch[i]; break;
        case 3: d ^= latch[i]; break;
        }
        p[i] = (uint8_t)((d & bm) | (latch[i] & ~bm));
    }
    return 1;
}

int recomp_mem_read8(CPU *cpu, uint32_t a, uint8_t *out)
{
    if (a - 0xA0000u >= 0x10000u) return 0;
    (void)cpu;
    uint32_t off = a - 0xA0000u;
    if (svga_mode) { *out = svga[((svga_bank << 16) + off) & 0xFFFFF]; return 1; }
    if (chain4) { *out = vram[off]; return 1; }
    uint8_t *p = &vram[off * 4];
    latch[0] = p[0]; latch[1] = p[1]; latch[2] = p[2]; latch[3] = p[3];
    if (gc[5] & 8) {                                   /* read mode 1: colour compare */
        uint8_t m = 0xFF;                              /* bits where every cared-about plane matched */
        for (int i = 0; i < 4; i++)
            if (gc[7] & (1 << i)) m &= (uint8_t)~(latch[i] ^ ((gc[2] & (1 << i)) ? 0xFF : 0));
        *out = m;
        return 1;
    }
    *out = latch[gc[4] & 3];
    return 1;
}

/* ---- ports 3C0-3DA --------------------------------------------------------- */

static uint64_t t0_us;

/* 70 Hz refresh, the last ~10% of it in vertical retrace. Real time, because
 * VL_WaitVBL is how the engine paces its fades and the intro. */
static uint8_t input_status(void)
{
    static uint8_t de;
    uint64_t us = emu_us() - t0_us;
    uint64_t ph = us % 14286;
    de ^= 1;                                           /* display enable flickers per line */
    return (uint8_t)(((ph >= 12900) ? 0x08 : 0) | ((ph >= 12900) ? 1 : de));
}

int vga_port_in(uint16_t port, uint8_t *v)
{
    switch (port) {
    case 0x3C0: *v = attr_i; return 1;
    case 0x3C1: *v = attr[attr_i & 0x1F]; return 1;
    case 0x3C2: *v = 0x10; return 1;
    case 0x3C4: *v = seq_i; return 1;
    case 0x3C5: *v = seq[seq_i & 7]; return 1;
    case 0x3C6: *v = dac_mask; return 1;
    case 0x3C7: *v = 3; return 1;
    case 0x3C8: *v = dac_wi; return 1;
    case 0x3C9: *v = dac[dac_ri][dac_rc]; if (++dac_rc == 3) { dac_rc = 0; dac_ri++; } return 1;
    case 0x3CC: *v = misc_out; return 1;
    case 0x3CE: *v = gc_i; return 1;
    case 0x3CF: *v = gc[gc_i & 15]; return 1;
    case 0x3D4: *v = crtc_i; return 1;
    case 0x3D5: *v = crtc[crtc_i & 31]; return 1;
    case 0x3DA: attr_flip = 0; *v = input_status(); return 1;
    }
    return 0;
}

int vga_port_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x3C0:
        if (!attr_flip) attr_i = v; else attr[attr_i & 0x1F] = v;
        attr_flip ^= 1; return 1;
    case 0x3C2: misc_out = v; return 1;
    case 0x3C4: seq_i = v; return 1;
    case 0x3C5:
        if ((seq_i & 7) != 2) trace("[vga] seq %d = %02X\n", seq_i & 7, v);
        seq[seq_i & 7] = v; if ((seq_i & 7) == 4) chain4 = chained(); return 1;
    case 0x3C6: dac_mask = v; return 1;
    case 0x3C7: dac_ri = v; dac_rc = 0; return 1;
    case 0x3C8: dac_wi = v; dac_wc = 0; return 1;
    case 0x3C9: dac[dac_wi][dac_wc] = v & 0x3F; if (++dac_wc == 3) { dac_wc = 0; dac_wi++; } return 1;
    case 0x3CE: gc_i = v; return 1;
    case 0x3CF: gc[gc_i & 15] = v; return 1;
    case 0x3D4: crtc_i = v; return 1;
    case 0x3D5:
        crtc[crtc_i & 31] = v;
        return 1;
    }
    return 0;
}

/* INT 10h palette services land here too. */
void vga_dac_set(int i, uint8_t r, uint8_t g, uint8_t b) { dac[i & 255][0] = r & 63; dac[i & 255][1] = g & 63; dac[i & 255][2] = b & 63; }
void vga_dac_get(int i, uint8_t *r, uint8_t *g, uint8_t *b) { *r = dac[i & 255][0]; *g = dac[i & 255][1]; *b = dac[i & 255][2]; }

/* ---- scanout --------------------------------------------------------------- */

static inline uint32_t rgb(int i)
{
    i &= dac_mask;
    return 0xFF000000u | ((uint32_t)(dac[i][0] * 255 / 63) << 16)
         | ((uint32_t)(dac[i][1] * 255 / 63) << 8) | (uint32_t)(dac[i][2] * 255 / 63);
}

/* What the monitor shows right now, as 0xAARRGGBB. Graphics: 320 x rows, rows
 * from the CRTC: 200, or 400 with double scan off. Text: 640x400. */
void vga_compose(uint32_t *out, int *w, int *h)
{
    if (svga_mode) {
        *w = svga_w; *h = svga_h;
        for (int y = 0; y < svga_h; y++) {
            const uint8_t *row = &svga[(svga_start + (unsigned)y * svga_w) & 0xFFFFF];
            for (int x = 0; x < svga_w; x++) out[y * svga_w + x] = rgb(row[x]);
        }
        return;
    }
    if (mode != 0x13) {
        static uint32_t blink;
        const uint8_t *t = vga_text_mem();
        int cx = g_cpu.mem[0x450], cy = g_cpu.mem[0x451];
        blink++;
        *w = 640; *h = 400;
        for (int r = 0; r < 25; r++)
            for (int c = 0; c < 80; c++) {
                uint8_t ch = t[(r * 80 + c) * 2], at = t[(r * 80 + c) * 2 + 1];
                uint32_t fg = rgb(attr[at & 15] & 0x3F), bg = rgb(attr[(at >> 4) & 7] & 0x3F);
                if ((at & 0x80) && (blink & 32)) fg = bg;
                for (int y = 0; y < 16; y++) {
                    uint8_t bits = font8x8_cp437[ch][y >> 1];
                    if (r == cy && c == cx && y >= 13 && (blink & 16)) bits = 0xFF;
                    uint32_t *o = out + (r * 16 + y) * 640 + c * 8;
                    for (int x = 0; x < 8; x++) o[x] = (bits & (0x80 >> x)) ? fg : bg;
                }
            }
        return;
    }
    unsigned start = ((unsigned)crtc[0x0C] << 8) | crtc[0x0D];
    unsigned vde = crtc[0x12] | ((crtc[0x07] & 2) << 7) | ((crtc[0x07] & 0x40) << 3);
    unsigned scan = (crtc[0x09] & 0x1F) + 1;
    int rows = (int)((vde + 1) / scan);
    if (rows < 200 || rows > 480) rows = 200;
    *w = 320; *h = rows;
    if (chain4) {
        /* dword mode: the start address and the row offset count 4-byte units */
        unsigned row = crtc[0x13] ? crtc[0x13] * 8u : 320u;
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < 320; x++)
                out[y * 320 + x] = rgb(vram[(start * 4 + y * row + x) & 0x3FFFF]);
        return;
    }
    unsigned row = crtc[0x13] ? crtc[0x13] * 2u : 80u;
    for (int y = 0; y < rows; y++) {
        unsigned base = start + y * row;
        for (int x = 0; x < 320; x++)
            out[y * 320 + x] = rgb(vram[(((base + (x >> 2)) & 0xFFFF) << 2) | (x & 3)]);
    }
}

void vga_start(void) { t0_us = emu_us(); }

int vga_svga_active(void) { return svga_mode != 0; }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* The window function (ModeInfoBlock.WinFuncPtr): a far call with BH = 0 set
 * (DX = position) or 1 get, BL = window. machine.c routes F000:4F05 here. */
void vga_vbe_window(CPU *c)
{
    if (c->bh == 0) svga_bank = c->dx & 0x0F;
    else c->dx = (uint16_t)svga_bank;
}

/* INT 10h AH=4Fh */
void vga_vbe(CPU *c)
{
    uint8_t *es_di = &g_cpu.mem[(((uint32_t)c->es << 4) + c->di) & 0xFFFFF];
    switch (c->al) {
    case 0x00: {                                        /* controller information */
        memset(es_di, 0, 256);
        memcpy(es_di, "VESA", 4);
        put16(es_di + 4, 0x0102);
        /* the strings and the mode list live in the block's reserved area */
        memcpy(es_di + 0x100 - 32, "pcrecomp VGA", 13);
        put16(es_di + 6, (uint16_t)(c->di + 0x100 - 32)); put16(es_di + 8, c->es);
        uint16_t list = (uint16_t)(c->di + 0x100 - 64);
        put16(es_di + 14, list); put16(es_di + 16, c->es);
        for (int i = 0; i < 4; i++) put16(es_di + 0x100 - 64 + 2 * i, vbe_modes[i].mode);
        put16(es_di + 0x100 - 64 + 8, 0xFFFF);
        put16(es_di + 18, 16);                          /* 16 x 64 KB = 1 MB */
        c->ax = 0x004F;
        return;
    }
    case 0x01: {                                        /* mode information */
        int k = -1;
        for (int i = 0; i < 4; i++) if (vbe_modes[i].mode == (c->cx & 0x1FF)) k = i;
        if (k < 0) { c->ax = 0x014F; return; }
        memset(es_di, 0, 256);
        put16(es_di + 0, 0x001B);                       /* supported, extended info, colour, graphics */
        es_di[2] = 0x07; es_di[3] = 0;                  /* window A: exists, readable, writable */
        put16(es_di + 4, 64); put16(es_di + 6, 64);     /* granularity and size, KB */
        put16(es_di + 8, 0xA000); put16(es_di + 10, 0);
        put16(es_di + 12, 0x4F05); put16(es_di + 14, bios_stub_seg());   /* the window function */
        put16(es_di + 16, vbe_modes[k].w);              /* bytes per scan line */
        put16(es_di + 18, vbe_modes[k].w); put16(es_di + 20, vbe_modes[k].h);
        es_di[22] = 8; es_di[23] = 16; es_di[24] = 1; es_di[25] = 8; es_di[26] = 1;
        es_di[27] = 4;                                  /* packed pixel */
        es_di[29] = (uint8_t)((1u << 20) / ((unsigned)vbe_modes[k].w * vbe_modes[k].h) - 1);
        es_di[30] = 1;
        c->ax = 0x004F;
        return;
    }
    case 0x02: {                                        /* set mode */
        uint16_t m = c->bx & 0x1FF;
        if (m < 0x100) { vga_set_mode(m | (c->bx & 0x8000 ? 0x80 : 0)); c->ax = 0x004F; return; }
        for (int i = 0; i < 4; i++)
            if (vbe_modes[i].mode == m) {
                vga_set_mode(0x13 | 0x80);              /* the DAC and sequencer as mode 13h leaves them */
                svga_mode = m; svga_w = vbe_modes[i].w; svga_h = vbe_modes[i].h;
                svga_bank = 0; svga_start = 0;
                if (!(c->bx & 0x8000)) memset(svga, 0, sizeof svga);
                trace("[vga] VESA mode %03Xh, %dx%d\n", m, svga_w, svga_h);
                c->ax = 0x004F;
                return;
            }
        c->ax = 0x014F;
        return;
    }
    case 0x03: c->bx = (uint16_t)(svga_mode ? svga_mode : mode); c->ax = 0x004F; return;
    case 0x05: vga_vbe_window(c); c->ax = 0x004F; return;
    case 0x07:
        if ((c->bl & 0x7F) == 0) svga_start = (unsigned)c->dx * (svga_w ? svga_w : 320) + c->cx;
        else { c->cx = (uint16_t)(svga_w ? svga_start % svga_w : 0); c->dx = (uint16_t)(svga_w ? svga_start / svga_w : 0); }
        c->ax = 0x004F;
        return;
    }
    trace("[vga] VESA function %02Xh not modelled\n", c->al);
    c->ax = 0x014F;
}

/* for SPACESIM_SNAP (machine.c): planes, then seq, gc, latches */
void vga_snap(FILE *f)
{
    fwrite(vram, 1, sizeof vram, f);
    fwrite(seq, 1, 8, f); fwrite(gc, 1, 16, f); fwrite(latch, 1, 4, f);
    fputc(seq_i, f); fputc(gc_i, f);
}

/* --trace: the registers that decide what vga_compose shows */
void vga_report(void)
{
    trace("[vga] mode %02X seq2 %02X seq4 %02X gc5 %02X gc6 %02X misc %02X | crtc 09 %02X 12 %02X 07 %02X "
          "13 %02X 14 %02X 17 %02X start %02X%02X | dacmask %02X\n",
          mode, seq[2], seq[4], gc[5], gc[6], misc_out, crtc[9], crtc[0x12], crtc[7],
          crtc[0x13], crtc[0x14], crtc[0x17], crtc[0x0C], crtc[0x0D], dac_mask);
    const char *d = getenv("SPACESIM_VRAMDUMP");     /* raw planes + DAC, for offline looking */
    FILE *f = d ? fopen(d, "wb") : NULL;
    if (f) { fwrite(vram, 1, sizeof vram, f); fwrite(dac, 1, sizeof dac, f); fclose(f); }
}

void vga_state(void)
{
    STATE(vram); STATE(latch); STATE(seq); STATE(gc); STATE(crtc); STATE(attr);
    STATE(seq_i); STATE(gc_i); STATE(crtc_i); STATE(attr_i); STATE(attr_flip);
    STATE(dac); STATE(dac_wi); STATE(dac_ri); STATE(dac_wc); STATE(dac_rc); STATE(dac_mask);
    STATE(mode); STATE(svga); STATE(svga_mode); STATE(svga_w); STATE(svga_h);
    STATE(svga_bank); STATE(svga_start); STATE(chain4); STATE(misc_out);
}
