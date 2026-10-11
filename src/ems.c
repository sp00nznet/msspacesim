/*
 * ems.c - expanded memory (LIM EMS 4.0) behind INT 67h.
 *
 * Space Simulator will not start without 512 KB of it ("Insufficient expanded
 * memory (EMS) is available."). It finds the driver the documented way --
 * open the device EMMXXXX0, IOCTL 4400h says character device, 4407h says
 * ready (dos.c answers those) -- then asks for the status (40h), the page
 * frame (41h) and the free page count (42h), allocates handles (43h) and maps
 * pages into the frame (44h). It maps a whole 64 KB bank at a time: logical
 * pages 0-3 of one handle into physical pages 0-3.
 *
 * Mapping copies. A real board switches which memory the frame decodes; copying
 * the page in, and the previous one back out, looks the same to a program that
 * only ever reads the frame as memory, and the rest of the machine never has
 * to know the window is special. Same design as DinoPark's EMS (sp00nznet/
 * dinopark), which first needed it.
 */
#include "machine.h"

#define PAGE     16384u
#define PHYS     4                     /* physical pages in the 64 KB frame */
#define HANDLES  32
#define TOTAL_KB 2048                  /* the readme: "more than 768K offers minimal benefit" */

static const uint16_t frame_seg = 0xE000;
static uint8_t *store;                 /* every logical page, host-side */
static unsigned total = TOTAL_KB * 1024u / PAGE, used;
static int mapped[PHYS] = { -1, -1, -1, -1 };   /* store page in each physical page */
static unsigned h_base[HANDLES], h_pages[HANDLES];
static uint8_t h_used[HANDLES];
static unsigned maps;
static int saved_map[HANDLES][PHYS];      /* 47h/48h, per handle */

uint16_t ems_frame(void) { return frame_seg; }

static uint8_t *phys_at(int p) { return &g_cpu.mem[(uint32_t)frame_seg * 16 + (uint32_t)p * PAGE]; }

static void unmap(int p)
{
    if (mapped[p] < 0) return;
    memcpy(store + (size_t)mapped[p] * PAGE, phys_at(p), PAGE);
    mapped[p] = -1;
}

/* Handles take store pages from a bump pointer; freeing the most recent one
 * gives its pages back, which is the only order the game frees in.
 * ponytail: no compaction; a program that frees out of order just leaks those
 * pages until exit -- add a free list if one ever runs short. */
static int alloc(unsigned n)
{
    if (n > total - used) return -1;
    for (int h = 1; h < HANDLES; h++)
        if (!h_used[h]) { h_used[h] = 1; h_base[h] = used; h_pages[h] = n; used += n; return h; }
    return -2;
}

void ems_int67(CPU *c)
{
    if (!store && !(store = calloc(total, PAGE))) { c->ah = 0x80; return; }
    int h = c->dx;
    int ok_h = h > 0 && h < HANDLES && h_used[h];
    switch (c->ah) {
    case 0x40: c->ah = 0; break;                                   /* status */
    case 0x41: c->bx = frame_seg; c->ah = 0; break;                /* page frame */
    case 0x42: c->bx = (uint16_t)(total - used); c->dx = (uint16_t)total; c->ah = 0; break;
    case 0x43: {                                                    /* allocate */
        int r = c->bx ? alloc(c->bx) : -3;
        if (r == -1) { c->ah = 0x88; break; }
        if (r == -2) { c->ah = 0x85; break; }
        if (r == -3) { c->ah = 0x89; break; }
        c->dx = (uint16_t)r; c->ah = 0;
        trace("[ems] handle %d: %u pages\n", r, h_pages[r]);
        break;
    }
    case 0x44: {                                                    /* map */
        int p = c->al, log = (int16_t)c->bx;
        if (p >= PHYS) { c->ah = 0x8B; break; }
        if (!ok_h) { c->ah = 0x83; break; }
        if (log == -1) { unmap(p); c->ah = 0; break; }
        if (log < 0 || (unsigned)log >= h_pages[h]) { c->ah = 0x8A; break; }
        int s = (int)(h_base[h] + (unsigned)log);
        if (mapped[p] != s) {
            for (int q = 0; q < PHYS; q++)                          /* one page, one place */
                if (q != p && mapped[q] == s) unmap(q);
            unmap(p);
            memcpy(phys_at(p), store + (size_t)s * PAGE, PAGE);
            mapped[p] = s;
            maps++;
        }
        c->ah = 0;
        break;
    }
    case 0x45:                                                      /* free */
        if (!ok_h) { c->ah = 0x83; break; }
        for (int p = 0; p < PHYS; p++)
            if (mapped[p] >= (int)h_base[h] && mapped[p] < (int)(h_base[h] + h_pages[h])) unmap(p), mapped[p] = -1;
        if (h_base[h] + h_pages[h] == used) used = h_base[h];
        h_used[h] = 0;
        c->ah = 0;
        break;
    case 0x46: c->al = 0x40; c->ah = 0; break;                     /* version 4.0 */
    case 0x47:                                                      /* save the page map */
        if (!ok_h) { c->ah = 0x83; break; }
        memcpy(saved_map[h], mapped, sizeof mapped);
        c->ah = 0;
        break;
    case 0x48:                                                      /* restore it */
        if (!ok_h) { c->ah = 0x83; break; }
        for (int p = 0; p < PHYS; p++) {
            int s = saved_map[h][p];
            if (mapped[p] == s) continue;
            for (int q = 0; q < PHYS; q++) if (q != p && mapped[q] == s && s >= 0) unmap(q);
            unmap(p);
            if (s >= 0) { memcpy(phys_at(p), store + (size_t)s * PAGE, PAGE); mapped[p] = s; maps++; }
        }
        c->ah = 0;
        break;
    case 0x4B: {                                                    /* handles in use */
        int n = 0;
        for (int i = 1; i < HANDLES; i++) n += h_used[i];
        c->bx = (uint16_t)n; c->ah = 0;
        break;
    }
    case 0x4C: c->bx = ok_h ? (uint16_t)h_pages[h] : 0; c->ah = ok_h ? 0 : 0x83; break;
    default:
        trace("[ems] function %02Xh not modelled\n", c->ah);
        c->ah = 0x84;
        break;
    }
}

void ems_report(void)
{
    if (store) fprintf(stderr, "[ems] %u of %u pages allocated, %u maps\n", used, total, maps);
}

/* for SPACESIM_SNAP (machine.c): the pages, the frame mapping and the handles */
void ems_snap(FILE *f)
{
    uint32_t n = store ? total : 0;
    fwrite(&n, 4, 1, f);
    if (n) fwrite(store, PAGE, n, f);
    int32_t m[PHYS]; for (int p = 0; p < PHYS; p++) m[p] = mapped[p];
    fwrite(m, 4, PHYS, f);
    uint32_t hb[HANDLES], hp[HANDLES]; for (int h = 0; h < HANDLES; h++) { hb[h] = h_base[h]; hp[h] = h_pages[h]; }
    fwrite(hb, 4, HANDLES, f); fwrite(hp, 4, HANDLES, f); fwrite(h_used, 1, HANDLES, f);
}

void ems_state(void)
{
    state_io(store, TOTAL_KB * 1024u);
    STATE(used); STATE(mapped); STATE(saved_map); STATE(h_base); STATE(h_pages); STATE(h_used); STATE(maps);
}
