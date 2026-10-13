/*
 * machine.c - the PC around the CPU: PIC, PIT, keyboard controller, speaker
 * gate, and getting interrupts into lifted code.
 *
 * Lifted code runs to completion inside C calls, so nothing can interrupt it
 * the way an IRQ interrupts a real CPU. Instead every loop back-edge counts
 * down a budget (RECOMP_TICK in cpu.h) and recomp_tick() runs here: it works
 * out how much emulated time has passed, delivers the timer interrupts that
 * fell due in order -- rendering audio up to each one, so the music the
 * timer plays lands where it was written -- then keyboard and Sound Blaster
 * IRQs, and gives the host a frame. Delivering at back-edges is also where a
 * real 386 would most likely have taken them: a game that spins waiting on a
 * counter its own timer ISR advances only gets there through a back-edge.
 *
 * Started from the Blake Stone recomp's machine model (same author).
 *
 * See docs/architecture.md, "Time".
 */
#include "machine.h"
#ifdef _WIN32
#include <windows.h>
#ifdef SPACESIM_DEBUG
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#endif

CPU g_cpu;
int g_recomp_tick_budget = 1;
int g_tick_hold;                         /* no interrupts, no time: a widescreen side pass */

/* ---- host clock ------------------------------------------------------------ */

uint64_t host_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart * 1000000 + (c.QuadPart % f.QuadPart) * 1000000 / f.QuadPart);
#else
    return 0;
#endif
}

static uint64_t t_start;
static uint64_t emu_now;              /* PIT ticks, what the guest has been shown */

uint64_t pit_now(void) { return emu_now; }

/* ---- PIC ----------------------------------------------------------------- */

static uint8_t pic_mask[2] = { 0xB8, 0xFF };   /* timer, keyboard, cascade, IRQ 5/7? set by the game */
static int in_irq;                     /* one interrupt at a time; no nesting */

/* ---- PIT ----------------------------------------------------------------- */

static uint32_t pit_div[3] = { 65536, 65536, 65536 };
static uint8_t  pit_mode_access[3] = { 3, 3, 3 };
static int      pit_flip[3];
static uint16_t pit_latch[3];
static int      pit_latched[3];
static uint64_t next_t0;
static int      t0_pending;

static uint16_t pit_count(int ch)
{
    uint64_t period = pit_div[ch];
    /* Timer 0 counts down to the next IRQ 0, so a reading and the tick count
     * the game's handler keeps never disagree: the game adds the two up for
     * its clock, and a counter out of phase with the IRQ made that clock step
     * back and then leap by whole minutes. */
    if (ch == 0) return (uint16_t)(next_t0 > emu_now ? (next_t0 - emu_now < period ? next_t0 - emu_now : period) : 1);
    return (uint16_t)(period - (emu_now % period));
}

/* ---- keyboard controller ---------------------------------------------------- */

static uint8_t kq[64];
static int kq_head, kq_tail;
static uint8_t port60, port61 = 0x00;

void kbd_scancode(uint8_t sc)
{
    int n = (kq_tail + 1) & 63;
    if (n != kq_head) { kq[kq_tail] = sc; kq_tail = n; }
}

/* ---- ports ------------------------------------------------------------- */

#define JOY_CENTER 1000
static int16_t joy_pos[2];                     /* the stick, -32767..32767, from the host */
static unsigned joy_reads;                     /* reads of 201h since the last write */

int machine_port_in(uint16_t port, uint8_t *v)
{
    switch (port) {
    case 0x20: case 0xA0: *v = 0; return 1;
    case 0x21: *v = pic_mask[0]; return 1;
    case 0xA1: *v = pic_mask[1]; return 1;
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        uint16_t c = pit_latched[ch] ? pit_latch[ch] : pit_count(ch);
        int acc = pit_mode_access[ch];
        if (acc == 1) *v = (uint8_t)c;
        else if (acc == 2) *v = (uint8_t)(c >> 8);
        else { *v = pit_flip[ch] ? (uint8_t)(c >> 8) : (uint8_t)c; pit_flip[ch] ^= 1;
               if (!pit_flip[ch]) pit_latched[ch] = 0; }
        return 1;
    }
    case 0x60: *v = port60; return 1;
    case 0x61: {
        static uint8_t refresh;
        refresh ^= 0x10;
        *v = (uint8_t)((port61 & 0x0F) | refresh);
        return 1;
    }
    case 0x64: *v = (uint8_t)(0x14 | (kq_head != kq_tail ? 1 : 0)); return 1;
    case 0x201: {
        /* The game times each axis's one-shot by counting reads of this port
         * (fn_124D0, up to 4000), so the model counts reads too: an axis bit
         * stays high for JOY_CENTER reads give or take the stick. The game
         * takes the count it sees at startup as the centre. Axes 2 and 3 (a
         * second stick) are open and never fall; no button is pressed. */
        joy_reads++;
        *v = 0xF0 | 0x0C;
        for (int i = 0; i < 2; i++)
            if (joy_reads <= (unsigned)(JOY_CENTER + joy_pos[i] * (JOY_CENTER - 50) / 32767)) *v |= (uint8_t)(1 << i);
        return 1;
    }
    }
    return 0;
}

void machine_joystick(int x, int y) { joy_pos[0] = (int16_t)x; joy_pos[1] = (int16_t)y; }

int machine_port_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x20: case 0xA0: return 1;            /* EOI: nothing is held in service */
    case 0x21: pic_mask[0] = v; return 1;
    case 0xA1: pic_mask[1] = v; return 1;
    case 0x43: {
        int ch = v >> 6;
        if (ch == 3) return 1;                 /* read-back: not used */
        if (((v >> 4) & 3) == 0) { pit_latch[ch] = pit_count(ch); pit_latched[ch] = 1; pit_flip[ch] = 0; return 1; }
        pit_mode_access[ch] = (v >> 4) & 3;
        pit_flip[ch] = 0;
        return 1;
    }
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        static uint16_t acc[3];
        int a = pit_mode_access[ch];
        if (a == 1) acc[ch] = (acc[ch] & 0xFF00) | v;
        else if (a == 2) acc[ch] = (uint16_t)((acc[ch] & 0xFF) | v << 8);
        else if (!pit_flip[ch]) { acc[ch] = (acc[ch] & 0xFF00) | v; pit_flip[ch] = 1; return 1; }
        else { acc[ch] = (uint16_t)((acc[ch] & 0xFF) | v << 8); pit_flip[ch] = 0; }
        pit_div[ch] = acc[ch] ? acc[ch] : 65536;
        if (ch == 0) { next_t0 = emu_now + pit_div[0]; trace("[pit] timer 0 at %.1f Hz\n", PIT_HZ / pit_div[0]); }
        if (ch == 2) audio_speaker((port61 & 3) == 3, (uint16_t)pit_div[2]);
        return 1;
    }
    case 0x60: case 0x64: return 1;            /* LEDs, controller commands */
    case 0x61:
        port61 = v;
        audio_speaker((v & 3) == 3, (uint16_t)pit_div[2]);
        return 1;
    case 0x201: joy_reads = 0; return 1;        /* fire the one-shots */
    }
    return 0;
}

static int warned_ports[0x400];

uint8_t port_in8(CPU *cpu, uint16_t port)
{
    uint8_t v = 0xFF;
    (void)cpu;
    if (vga_port_in(port, &v) || audio_port_in(port, &v) || machine_port_in(port, &v)) return v;
    if (port < 0x400 && !warned_ports[port]++) trace("[port] in %03X\n", port);
    return 0xFF;
}

void port_out8(CPU *cpu, uint16_t port, uint8_t v)
{
    (void)cpu;
    if (vga_port_out(port, v) || audio_port_out(port, v) || machine_port_out(port, v)) return;
    if (port < 0x400 && !warned_ports[port]++) trace("[port] out %03X = %02X\n", port, v);
}

uint16_t port_in16(CPU *cpu, uint16_t port)
{
    return (uint16_t)(port_in8(cpu, port) | port_in8(cpu, (uint16_t)(port + 1)) << 8);
}

void port_out16(CPU *cpu, uint16_t port, uint16_t v)
{
    port_out8(cpu, port, (uint8_t)v);
    port_out8(cpu, (uint16_t)(port + 1), (uint8_t)(v >> 8));
}

/* ---- dispatch ----------------------------------------------------------- */

static void (*find_in(const RecompFunc *t, unsigned n, uint16_t load, uint16_t seg, uint16_t off))(CPU *)
{
    int32_t lin = ((int32_t)seg - (int32_t)load) * 16 + off;
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if ((int32_t)t[mid].addr < lin) lo = mid + 1; else hi = mid;
    }
    return (lo < n && (int32_t)t[lo].addr == lin) ? t[lo].fn : NULL;
}

/* The program an address belongs to: the companion (SS1.EXE) sits above the
 * game's memory, so anything from its load segment up is its. */
static const Program *owner(uint16_t seg)
{
    const Program *p = companion();
    return p && seg >= p->load_seg && !g_opt.no_ss1 ? p : NULL;
}

static void (*lookup(uint16_t seg, uint16_t off))(CPU *)
{
    const Program *p = owner(seg);
    if (p) return find_in(p->funcs, p->n, p->load_seg, seg, off);
    return find_in(g_funcs, g_func_count, g_load_seg, seg, off);
}

void dispatch_entry(CPU *cpu)
{
    void (*fn)(CPU *) = lookup(cpu->cs, cpu->ip);
    if (fn) fn(cpu);
    else fprintf(stderr, "[dos] no lifted entry point at %04X:%04X\n", cpu->cs, cpu->ip);
}

/* An address the guest jumped to that has no lifted function. Recorded so the
 * next `tools/lift.py` run makes it an entry point (load segment form; the
 * lifter converts it back). */
static void miss(const char *how, uint16_t seg, uint16_t off)
{
    static uint32_t seen[512]; static int nseen;
    uint32_t key = (uint32_t)seg << 16 | off;
    for (int i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 512) seen[nseen++] = key;
    const Program *prog = owner(seg);
    if (!prog) {   int drv_miss(uint32_t lin);          /* a Super VGA driver's own: drv.c */
        if (drv_miss((uint32_t)(((int32_t)seg - g_load_seg) * 16 + off))) return; }
    uint16_t load = prog ? prog->load_seg : g_load_seg;
    fprintf(stderr, "[miss] %s%s %04X:%04X (image %05X)\n", prog ? "ss1 " : "", how, seg, off,
            (unsigned)(((int32_t)seg - load) * 16 + off));
    { void host_backtrace(void); host_backtrace(); }
    char p[256]; snprintf(p, sizeof p, "work/%s/misses.txt", prog ? prog->id : g_game_id);
    FILE *f = fopen(p, "a");
    if (f) { fprintf(f, "%04X:%04X\n", seg, off); fclose(f); }
    /* SS1's C runtime does not survive a skipped routine: what it does next
     * is garbage, and garbage misses would land in the game's list. Stop;
     * the next lift has it (tools/converge.py relifts and runs again). */
    if (prog) { fprintf(stderr, "[miss] stopping: run tools/lift.py ss1 and rebuild\n"); dos_exit(3); }
}

/* `jmp far [old]` / `pushf; call far [old]` into the BIOS: chain to the
 * built-in handler and return through its IRET. */
static void stub_iret(CPU *cpu, uint16_t off)
{
    bios_builtin(cpu, off);
    cpu->sp += 4;
    cpu->flags = pop16(cpu);
}

void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off)
{
    if (seg == bios_stub_seg()) { stub_iret(cpu, off); return; }
    /* A routine that pops its own return address and jumps to it (Borland's
     * _setargv, which builds argv on the stack under itself) jumps to the
     * lifter's 0xFFFF sentinel: that is a return to the C caller, and the
     * guest has already taken the address off the stack. */
    if (off == 0xFFFF) return;
    void (*fn)(CPU *) = lookup(seg, off);
    if (fn) fn(cpu); else miss("jmp", seg, off);
}

void dispatch_near(CPU *cpu, uint16_t cs, uint16_t off)
{
    void (*fn)(CPU *) = lookup(cs, off);
    if (fn) fn(cpu); else { miss("call near", cs, off); cpu->sp += 2; }
}

void dispatch_far(CPU *cpu, uint16_t seg, uint16_t off)
{
    if (seg == bios_stub_seg() && off == 0x4F05) {       /* VESA window function: a far call */
        void vga_vbe_window(CPU *c);
        vga_vbe_window(cpu);
        cpu->sp += 4;
        return;
    }
    if (seg == bios_stub_seg()) { stub_iret(cpu, off); return; }
    void (*fn)(CPU *) = lookup(seg, off);
    if (fn) fn(cpu); else { miss("call far", seg, off); cpu->sp += 4; }
}

/* INT n: whatever the IVT holds, with the frame a real INT pushes. */
void call_vector(CPU *cpu, unsigned vec)
{
    uint8_t *v = &cpu->mem[vec * 4];
    uint16_t off = (uint16_t)(v[0] | v[1] << 8), seg = (uint16_t)(v[2] | v[3] << 8);
    if (seg == bios_stub_seg()) {
        if (!bios_builtin(cpu, off)) trace("[int] no handler for INT %02X\n", vec);
        return;
    }
    void (*fn)(CPU *) = lookup(seg, off);
    if (!fn) {
        static unsigned seen[256];
        if (!seen[vec & 255]++) fprintf(stderr, "[int] INT %02X points nowhere lifted\n", vec);
        miss("int", seg, off);
        return;
    }
    push16(cpu, cpu->flags);
    push16(cpu, seg);
    push16(cpu, 0xFFFF);
    cpu->flags &= ~(FLAG_IF | FLAG_TF);
    fn(cpu);                                   /* its IRET pops the frame */
}

void int_handler(CPU *cpu, unsigned num) { call_vector(cpu, num); }

static void irq(CPU *cpu, int line)
{
    in_irq = 1;
    call_vector(cpu, line < 8 ? 8 + line : 0x70 + line - 8);
    in_irq = 0;
}

static int irq_ok(CPU *cpu, int line)
{
    return !in_irq && (cpu->flags & FLAG_IF) && !(pic_mask[line >> 3] & (1 << (line & 7)));
}

int  sb_irq_pending(void);
int  sb_irq_line(void);
void sb_irq_ack(void);

/* Emulated time. Windowed it is the wall clock -- the game should feel like
 * a game. Headless it is deterministic: every interrupt poll (one per 2000
 * loop back-edges, or a DOS/BIOS call that waits) moves time on by POLL_US,
 * so a run depends on the guest's own progress and never on how busy the
 * host is. Scripted keys, frame grabs and recordings then land on the same
 * screens every time; with wall-clock time the conformance scripts raced
 * Planet Strike's title fade and lost. POLL_US is sized so the guest runs
 * at roughly the speed of a fast 386 (docs/architecture.md, "Time"). */
#define POLL_US 300
static uint64_t virt_us, polls;

/* The speed control (the settings menu) runs the wall clock faster or
 * slower from the moment it changes, so emulated time never jumps. */
static double speed = 1.0;
static uint64_t speed_emu;                /* emulated us when the speed last changed */

uint64_t emu_us(void)
{
    if (g_opt.headless && !g_opt.realtime) return virt_us;
    return speed_emu + (uint64_t)((host_us() - t_start) * speed);
}

void machine_set_speed(double s)
{
    speed_emu = emu_us();
    t_start = host_us();
    speed = s;
}

double machine_speed(void) { return speed; }

void machine_report(void)
{
    trace("[time] %llu polls, %.1f s emulated\n", (unsigned long long)polls, emu_us() / 1e6);
}

void recomp_tick(CPU *cpu)
{
    static int busy;
    g_recomp_tick_budget = 2000;
    if (busy || g_tick_hold) return;
    busy = 1;
    polls++;
    virt_us += POLL_US;

    uint64_t target = (uint64_t)((double)emu_us() * (PIT_HZ / 1e6));
    if (target > next_t0 + (uint64_t)(PIT_HZ / 10)) {
        /* far behind (a long load, a debugger): drop the backlog rather than
         * replay a storm of ticks the game would see as a time warp anyway */
        next_t0 = target - pit_div[0];
    }
    while (next_t0 <= target) {
        audio_advance(next_t0);
        emu_now = next_t0;
        next_t0 += pit_div[0];
        t0_pending = 1;
        if (irq_ok(cpu, 0)) { t0_pending = 0; irq(cpu, 0); }
        else break;                            /* masked or cli: wait, one pending at most */
    }
    if (t0_pending && irq_ok(cpu, 0)) { t0_pending = 0; irq(cpu, 0); }
    if (target > emu_now) { audio_advance(target); emu_now = target; }

    while (kq_head != kq_tail && irq_ok(cpu, 1)) {
        port60 = kq[kq_head];
        kq_head = (kq_head + 1) & 63;
        irq(cpu, 1);
    }
    if (sb_irq_pending() && irq_ok(cpu, sb_irq_line())) { sb_irq_ack(); irq(cpu, sb_irq_line()); }

    host_frame();
    busy = 0;
}

void machine_init(void)
{
    t_start = host_us();
    next_t0 = pit_div[0];
}

/* A divide that faulted (zero divisor or a quotient too big), with cpu->cs:ip
 * at the divide (pcrecomp lift16 sets them). The game's INT 0 handler reads
 * the instruction through the return address -- a 66h prefix means 32 bits
 * -- saturates the quotient, and steps the return address over it; so it gets
 * the frame the CPU would push. The lifted code then simply carries on after
 * the divide, which is where the handler's iret would have gone. */
void recomp_div0(const char *what)
{
    CPU *c = &g_cpu;
    uint8_t *v = &c->mem[0];
    uint16_t off = (uint16_t)(v[0] | v[1] << 8), seg = (uint16_t)(v[2] | v[3] << 8);
    void (*fn)(CPU *) = seg == bios_stub_seg() ? NULL : lookup(seg, off);
    if (!fn) { fprintf(stderr, "[cpu] divide fault (%s) at %04X:%04X, no handler\n", what, c->cs, c->ip); return; }
    trace("[cpu] divide fault (%s) at %04X:%04X\n", what, c->cs, c->ip);
    push16(c, c->flags);
    push16(c, c->cs);
    push16(c, c->ip);
    c->flags &= ~(FLAG_IF | FLAG_TF);
    fn(c);
}

int cpu_lar(CPU *cpu, uint16_t sel, uint16_t *out) { (void)cpu; (void)sel; (void)out; return 0; }
int cpu_lsl(CPU *cpu, uint16_t sel, uint16_t *out) { (void)cpu; (void)sel; (void)out; return 0; }

int cpu_alloc_mem(CPU *cpu)
{
    cpu->mem = (uint8_t *)calloc(1, MEM_SIZE);
    return cpu->mem ? 0 : -1;
}

/* ---- debugging: SPACESIM_WATCH=<linear hex> reports every write to that byte
 * with the lifted functions most recently entered (build with
 * -DSPACESIM_TRACE=ON so the lifted code records them). */
#ifdef RECOMP_TRACE
static const char *ring[64];
static unsigned ring_n;
/* SPACESIM_CALLS=from-to (ms of game time): each lifted function entered in
 * that window, once, in order of first entry -- diff two runs to find the
 * code a key press reaches */
/* SPACESIM_SNAP=<fn>: the whole machine at that lifted function's first and
 * second entry, to work/snap1.bin and work/snap2.bin -- registers, guest
 * memory, VGA planes and registers. tools/fdiff.py runs the original bytes
 * from snap1 under Unicorn and compares what they draw with snap2. */
static void snapshot(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    CPU *c = &g_cpu;
    uint32_t r[8] = { c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp };
    uint16_t s[7] = { c->cs, c->ds, c->es, c->ss, c->fs, c->gs, c->flags };
    fwrite(r, 4, 8, f); fwrite(s, 2, 7, f);
    fwrite(c->mem, 1, MEM_SIZE, f);
    void vga_snap(FILE *f); vga_snap(f);
    void ems_snap(FILE *f); ems_snap(f);
    fclose(f);
    fprintf(stderr, "[snap] %s\n", path);
}

static int snap_got;                     /* snapshots written so far */
static char snap_want[64];

/* LIFT_EXIT_HOOK'd functions call this as they return: with SPACESIM_SNAP
 * naming the same function, the second snapshot is taken here -- the state
 * the function itself left, which is what fdiff compares against. */
void recomp_exit_hook(const char *fn)
{
    if (snap_got == 1 && !strcmp(fn, snap_want)) { snap_got = 2; snapshot("work/snap2.bin"); }
}

void recomp_enter(const char *fn)
{
    ring[ring_n++ & 63] = fn;
    {   /* SPACESIM_SNAP=fn[:N]: the Nth entry (default 1), then its return if
         * the function was lifted with LIFT_EXIT_HOOK, else its next entry */
        static int init, skip;
        if (!init) {
            const char *e = getenv("SPACESIM_SNAP");
            init = 1;
            if (e) { snprintf(snap_want, sizeof snap_want, "%s", e); char *c = strchr(snap_want, ':'); if (c) { *c = 0; skip = atoi(c + 1) - 1; } }
        }
        if (*snap_want && snap_got < 2 && !strcmp(fn, snap_want)) {
            if (skip > 0) skip--;
            else snapshot(++snap_got == 1 ? "work/snap1.bin" : "work/snap2.bin");
        }
    }
    {   /* a runaway recursion: name the cycle before the 16 MB stack goes */
        static char *base;
        char here;
        if (!base) base = &here;
        if (base - &here > 12 * 1024 * 1024) {
            fprintf(stderr, "[stack] 12 MB deep; last 64 lifted entries:\n");
            for (unsigned i = ring_n - 64; i != ring_n; i++) fprintf(stderr, " %s", ring[i & 63]);
            fprintf(stderr, "\n");
            exit(3);
        }
    }
    static int init;
    static unsigned from, to;
    static const char *seen[8192];
    static int nseen;
    if (!init) { init = 1; const char *e = getenv("SPACESIM_CALLS"); if (!e || sscanf(e, "%u-%u", &from, &to) != 2) to = 0; }
    if (!to) return;
    uint64_t ms = emu_us() / 1000;
    if (ms < from || ms >= to) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == fn) return;
    if (nseen < 8192) seen[nseen++] = fn;
    fprintf(stderr, "[call] %s\n", fn);
}
void recomp_trace_dump(const char *why)
{
    fprintf(stderr, "  [%s] last functions:", why);
    for (unsigned i = ring_n > 12 ? ring_n - 12 : 0; i < ring_n; i++) fprintf(stderr, " %s", ring[i & 63]);
    fprintf(stderr, "\n");
}
#endif

/* The host stack, symbolised (debug builds carry a PDB): lifted functions
 * keep their fn_ names, so this is the guest call chain. */
void host_backtrace(void)
{
#if defined(SPACESIM_DEBUG) && defined(_WIN32)
    static int init;
    HANDLE p = GetCurrentProcess();
    if (!init) { init = 1; SymInitialize(p, NULL, TRUE); }
    void *fr[48]; USHORT n = CaptureStackBackTrace(1, 48, fr, NULL);
    char buf[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127;
    fprintf(stderr, "   ");
    for (USHORT i = 0; i < n; i++)
        if (SymFromAddr(p, (DWORD64)(size_t)fr[i], NULL, si)) fprintf(stderr, " %s", si->Name);
    fprintf(stderr, "\n");
#endif
}

void watch_write(uint32_t a, uint8_t v)
{
    static uint32_t w = 1;
    if (w == 1) { const char *e = getenv("SPACESIM_WATCH"); w = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
    if (!w || (a != w && a != w + 1)) return;
    fprintf(stderr, "[watch] %05X = %02X\n", a, v);
    host_backtrace();
}

/* SPACESIM_VGAPROF=1 (debug builds): which lifted functions write video memory,
 * by return address, symbolised at exit. Finds the wall scalers, the plane
 * drawer and the sprite scalers without reading every routine. */
#if defined(SPACESIM_DEBUG) && defined(_WIN32)
#include <intrin.h>
static struct { void *ra; unsigned long n; } vp[256];
static int vp_on = -1;
void vgaprof_note(void *ra)
{
    if (vp_on < 0) vp_on = getenv("SPACESIM_VGAPROF") != NULL;
    if (!vp_on) return;
    for (int i = 0; i < 256; i++) {
        if (vp[i].ra == ra) { vp[i].n++; return; }
        if (!vp[i].ra) { vp[i].ra = ra; vp[i].n = 1; return; }
    }
}
void vgaprof_report(void)
{
    if (vp_on <= 0) return;
    HANDLE p = GetCurrentProcess();
    SymInitialize(p, NULL, TRUE);
    char buf[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    unsigned long tot[256] = {0}; char names[256][64]; int nn = 0;
    for (int i = 0; i < 256 && vp[i].ra; i++) {
        si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127;
        const char *nm = SymFromAddr(p, (DWORD64)(size_t)vp[i].ra, NULL, si) ? si->Name : "?";
        int k = 0;
        for (; k < nn; k++) if (!strcmp(names[k], nm)) break;
        if (k == nn) { strncpy(names[nn], nm, 63); names[nn][63] = 0; nn++; }
        tot[k] += vp[i].n;
    }
    for (int k = 0; k < nn; k++) fprintf(stderr, "[vgaprof] %10lu %s\n", tot[k], names[k]);
}
#else
void vgaprof_report(void) {}
#endif

/* Save states: the PIC and the PIT's programming. Not the time: it runs on. */
void machine_state(void)
{
    STATE(pic_mask); STATE(in_irq); STATE(pit_div); STATE(pit_mode_access);
    STATE(pit_flip); STATE(pit_latch); STATE(pit_latched); STATE(port61);
    kq_head = kq_tail = 0;
}
