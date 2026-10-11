/*
 * machine.h - the DOS machine under the lifted game: shared state between
 * src/*.c. The generated code never includes this; it sees lifted.h only.
 */
#pragma once
#include "lifted.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern CPU g_cpu;

typedef struct {
    const char *datadir;      /* the game's files (read) */
    const char *savedir;      /* where CONFIG and saved games go (read+write) */
    int headless;             /* no window: render offscreen */
    const char *record;       /* --record out.mp4 */
    double seconds;           /* stop after this long (0 = never) */
    const char *keys;         /* scripted input: "ms:key,ms:key" */
    const char *shots;        /* --shot-at "ms:file.bmp,..." */
    const char *wav;          /* --wav out.wav: the audio, no ffmpeg needed */
    int mute;
    int trace;                /* RECOMP_TRACE-ish logging of DOS calls */
    const char *args;         /* game command line tail */
    int scale;                /* window scale factor */
    int fullscreen;
    const char *display;      /* scaling: sharp | smooth | crt | nearest | integer */
    int gdi;                  /* --gdi: no Direct3D */
    int no_ss1;               /* --no-ss1: boot the game, not its launcher */
    int realtime;             /* headless, but on the wall clock */
} Options;
extern Options g_opt;

uint64_t host_us(void);
void     trace(const char *fmt, ...);
void     fatal(const char *fmt, ...);

/* dos.c */
void dos_boot(CPU *cpu);
void dos_exit(int code);              /* longjmps back to main */
uint16_t bios_stub_seg(void);
int  bios_builtin(CPU *cpu, unsigned num);   /* the BIOS/DOS behind an unhooked vector */
void bios_key_push(uint8_t scan, uint8_t ascii);

/* drv.c: the 2D driver variants */
void drv_loaded(uint32_t linear, uint32_t n);

/* ems.c: expanded memory behind INT 67h */
void ems_int67(CPU *cpu);
void ems_report(void);
uint16_t ems_frame(void);

/* machine.c: PIC, PIT, keyboard controller, speaker, IRQ delivery */
void machine_init(void);
void kbd_scancode(uint8_t sc);        /* from the host: one byte of a make/break */
void mouse_host(int dx, int dy, int buttons);
int  machine_port_in(uint16_t port, uint8_t *v);
int  machine_port_out(uint16_t port, uint8_t v);
void call_vector(CPU *cpu, unsigned vec);    /* run whatever is in the IVT, as INT would */
uint64_t pit_now(void);
uint64_t emu_us(void);                /* see machine.c: wall clock, or deterministic */
void machine_set_speed(double s);     /* windowed: the wall clock times s */
double machine_speed(void);
void machine_report(void);               /* emulated time in PIT ticks (1.193182 MHz) */
#define PIT_HZ 1193182.0

/* vga.c */
void vga_init(void);
void vga_start(void);
void vga_set_mode(int m);
int  vga_mode(void);
int  vga_port_in(uint16_t port, uint8_t *v);
int  vga_port_out(uint16_t port, uint8_t v);
void vga_dac_set(int i, uint8_t r, uint8_t g, uint8_t b);
void vga_dac_get(int i, uint8_t *r, uint8_t *g, uint8_t *b);
void vga_compose(uint32_t *out, int *w, int *h);
extern int g_tick_hold, g_recomp_tick_budget;

/* audio.c: OPL2 (opl.cpp), Sound Blaster DSP + 8237 DMA, PC speaker */
void audio_init(void);
void audio_advance(uint64_t pit_ticks);    /* render up to this emulated time */
int  audio_port_in(uint16_t port, uint8_t *v);
int  audio_port_out(uint16_t port, uint8_t v);
void audio_speaker(int gate, uint16_t divisor);
void audio_shutdown(void);

/* host.c: window or offscreen, input, presentation, recording */
/* present_d3d.c: scale w x h pixels (0x00RRGGBB) into rect r of a cw x ch
 * client area (r is a RECT*, hw an HWND). Returns 0 without Direct3D 11. */
enum { SCALE_SHARP, SCALE_SMOOTH, SCALE_CRT, SCALE_NEAREST, SCALE_INTEGER, SCALE_COUNT };
int d3d_present(void *hw, const uint32_t *px, int w, int h, const void *r, int cw, int ch,
                int mode, int look, int vsync);
extern const char *snapshot_path;     /* the next d3d_present's back buffer to this BMP */

/* state.c: save states. Each module's X_state() passes its variables
 * through STATE(), which writes or reads them. */
void state_io(void *p, size_t n);
#define STATE(v) state_io(&(v), sizeof(v))
void state_point(void);               /* a save or load happens here, if one is due */
void state_init(void *main_frame);

void host_init(void);
void host_frame(void);                /* present + pump, at most once per refresh */
void host_audio(const int16_t *stereo, int frames);
void host_shutdown(void);
#define AUDIO_RATE 49716              /* the OPL2's own rate: 3.579545 MHz / 72 */
