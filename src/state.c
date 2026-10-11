/*
 * state.c - save states (the settings menu, F11).
 *
 * A state is the whole machine: registers, the 1 MB address space, expanded
 * memory, the VGA and Super VGA memory and registers, the PIC and PIT, DOS's
 * memory blocks and open files. Guest time is not in it (it runs on) and the
 * input queues start empty.
 *
 * The lifted code keeps the guest's call chain on the host's stack as C
 * frames, so a state can only be loaded where those frames are the same ones
 * it was saved under. Both happen at one hook, state_point(), on the way into
 * every BIOS/DOS service (the simulator calls INT 67h many times a frame).
 * A state records the host's return addresses above the hook, the host stack
 * depth and the guest's SS:SP; a load waits for a call that matches all three
 * -- in practice the same place in the next frame -- and then overwrites the
 * machine and returns into the very same frames. A load requested elsewhere
 * in the game (a menu, when the state is from flight) waits for 10 seconds
 * and gives up.
 */
#include "machine.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif

#define STATE_MAGIC 0x53534D53u                  /* "SMSS" */
#define STATE_VERSION 1
#define FRAMES 48

volatile int g_state_request;                    /* host.c sets it: slot to save, -slot to load */
void host_note(const char *fmt, ...);
void ems_state(void); void audio_state(void); void vga_state(void); void machine_state(void); void dos_state(void); void drv_state(void);

static FILE *state_file;
static int saving, state_ok;
static char *base_frame;                         /* main()'s frame: depth is measured from it */
static uint64_t give_up_us;

void state_io(void *p, size_t n)
{
    if (!state_ok) return;
    if (saving) state_ok = fwrite(p, 1, n, state_file) == n;
    else state_ok = fread(p, 1, n, state_file) == n;
}

void state_path(int slot, char *out, size_t n)
{
    _mkdir("states");
    snprintf(out, n, "states/slot%d.sss", slot);
}

void state_init(void *main_frame) { base_frame = main_frame; }

typedef struct {
    uint32_t magic, version, depth;
    uint16_t ss, sp;
    uint64_t chain;                              /* hash of the host's return addresses */
} Head;

static void where_now(Head *h, void *here)
{
    void *ra[FRAMES];
    USHORT n = RtlCaptureStackBackTrace(2, FRAMES, ra, NULL);
    uint64_t x = 1469598103934665603ull;
    HMODULE exe = GetModuleHandle(NULL);
    for (USHORT i = 0; i < n; i++) {            /* relative to the exe: ASLR moves it */
        x ^= (uint64_t)((char *)ra[i] - (char *)exe);
        x *= 1099511628211ull;
    }
    h->magic = STATE_MAGIC; h->version = STATE_VERSION;
    h->depth = (uint32_t)(base_frame - (char *)here);
    h->ss = g_cpu.ss; h->sp = g_cpu.sp;
    h->chain = x;
}

static void machine_io(void)
{
    CPU regs = g_cpu;                            /* everything but the memory pointer */
    state_io(&regs, offsetof(CPU, mem));
    if (!saving && state_ok) memcpy(&g_cpu, &regs, offsetof(CPU, mem));
    state_io(&g_cpu.dir, sizeof g_cpu.dir);
    state_io(g_cpu.mem, MEM_SIZE);
    state_io(&g_x87, sizeof g_x87);
    ems_state(); audio_state(); vga_state(); machine_state(); dos_state(); drv_state();
}

static void save(int slot, const Head *h)
{
    char path[260];
    state_path(slot, path, sizeof path);
    if (!(state_file = fopen(path, "wb"))) { host_note("could not write %s", path); return; }
    saving = 1; state_ok = 1;
    state_io((void *)h, sizeof *h);
    machine_io();
    if (fclose(state_file) != 0) state_ok = 0;
    if (!state_ok) { remove(path); host_note("could not save slot %d", slot); }
    else host_note("saved slot %d", slot);
}

/* 1 when the load is done or abandoned, 0 to try again at the next call */
static int load(int slot, const Head *now)
{
    char path[260];
    Head h;
    state_path(slot, path, sizeof path);
    if (!(state_file = fopen(path, "rb"))) { host_note("slot %d is empty", slot); return 1; }
    if (fread(&h, sizeof h, 1, state_file) != 1 || h.magic != STATE_MAGIC || h.version != STATE_VERSION) {
        fclose(state_file); host_note("slot %d is from another build", slot); return 1;
    }
    if (h.depth != now->depth || h.ss != now->ss || h.sp != now->sp || h.chain != now->chain) {
        fclose(state_file);
        if (host_us() < give_up_us) return 0;
        host_note("slot %d was saved somewhere else in the game: load it from there", slot);
        return 1;
    }
    saving = 0; state_ok = 1;
    machine_io();
    fclose(state_file);
    if (!state_ok) fatal("%s is cut short; the machine is half loaded", path);
    host_note("loaded slot %d", slot);
    return 1;
}

void state_point(void)
{
    static int pending;                          /* a load waiting for its place */
    int req = g_state_request;
    if (req) { g_state_request = 0; pending = 0; }
    if (req < 0) { pending = -req; give_up_us = host_us() + 10000000; }
    if (!req && !pending) return;
    char here;
    Head h;
    where_now(&h, &here);
    if (req > 0) save(req, &h);
    else if (load(pending, &h)) pending = 0;
}
