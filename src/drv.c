/*
 * drv.c - which 2D driver is loaded, and calls into it.
 *
 * The game's built-in VGA driver and the Super VGA driver files (SVGA.DRV,
 * SVGA8.DRV) occupy the same addresses: the game reads a driver file over the
 * built-in one at 293C:D010 when SPACESIM.INI names it. tools/lift.py lifts
 * each driver as a variant of that region and gives every address in it a
 * trampoline that calls drv_call(); this sends the call to the variant whose
 * code is loaded now. Which one that is, is decided after every DOS read that
 * lands in the region, by matching the first 32 bytes of its code against each
 * variant's (see docs/lifting.md, "Super VGA").
 */
#include "machine.h"

static unsigned active;                      /* index into g_drv_variants */

static void (*find(const DrvVariant *v, uint32_t lin))(CPU *)
{
    unsigned lo = 0, hi = v->n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (v->funcs[mid].addr < lin) lo = mid + 1;
        else hi = mid;
    }
    return lo < v->n && v->funcs[lo].addr == lin ? v->funcs[lo].fn : NULL;
}

void drv_call(CPU *cpu, uint32_t lin)
{
    const DrvVariant *v = &g_drv_variants[active];
    void (*fn)(CPU *) = find(v, lin);
    if (fn) { fn(cpu); return; }
    /* Not lifted for this driver: recorded for this variant's next lift
     * (work/spacesim/misses_<driver>.txt), the way machine.c records others. */
    static uint32_t seen[256]; static unsigned nseen;
    for (unsigned i = 0; i < nseen; i++) if (seen[i] == lin) return;
    if (nseen < 256) seen[nseen++] = lin;
    uint32_t seg = (uint32_t)g_load_seg + g_drv_seg, off = lin - (uint32_t)g_drv_seg * 16;
    fprintf(stderr, "[miss] %s driver %04X:%04X\n", v->name, (unsigned)seg, (unsigned)off);
    if (active == 0) return;                 /* the built-in one: machine.c's list covers it */
    char p[256];
    snprintf(p, sizeof p, "work/%s/misses_%s.txt", g_game_id, v->name);
    FILE *f = fopen(p, "a");
    if (f) { fprintf(f, "%04X:%04X\n", (unsigned)seg, (unsigned)off); fclose(f); }
}

/* A dispatch miss inside the driver region while a Super VGA driver is
 * loaded belongs to that driver's lift, not the main one: machine.c asks. */
int drv_miss(uint32_t lin)
{
    if (!g_drv_count || active == 0 || lin < g_drv_lo || lin >= g_drv_lo + g_drv_len) return 0;
    drv_call(&g_cpu, lin);                   /* not in the table: records it */
    return 1;
}

/* After a read of n bytes to linear address a: if it touched the driver
 * region, see which driver is there now. */
void drv_loaded(uint32_t a, uint32_t n)
{
    if (!g_drv_count) return;
    uint32_t lo = (uint32_t)g_load_seg * 16 + g_drv_lo;
    if (a + n <= lo || a >= lo + g_drv_len) return;
    for (unsigned i = 0; i < g_drv_count; i++)
        if (!memcmp(&g_cpu.mem[lo], g_drv_variants[i].sig, sizeof g_drv_variants[i].sig)) {
            if (active != i) trace("[drv] %s driver loaded\n", g_drv_variants[i].name);
            active = i;
            return;
        }
    fprintf(stderr, "[drv] a 2D driver was loaded that tools/lift.py did not lift; "
                    "the built-in VGA driver's code will run in its place\n");
}

void drv_state(void) { STATE(active); }
