/*
 * host.c - the modern machine's side: a Win32 window (Direct3D 11 scaling,
 * a settings menu; see "window" below), waveOut audio, keyboard as raw PC scancodes -- or, with --headless,
 * none of that: frames are composed offscreen and, with --record, piped to
 * ffmpeg with the audio muxed in at the end. Headless never touches the
 * desktop, so it works over RDP and from CI (repo rules, section 13).
 *
 * Scripted input (--keys "ms:KEY,...") presses and releases keys at fixed
 * points in emulated time, which is how the harness walks the menus.
 */
#include "machine.h"
#include <ctype.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define FRAME_MAX (1024 * 768)                  /* the largest VESA mode offered (vga.c) */
static uint32_t frame[FRAME_MAX];
static uint32_t big[FRAME_MAX];
static int fw = 320, fh = 200;
static double frame_aspect = 4.0 / 3;

static void compose(void)
{
    vga_compose(frame, &fw, &fh);
    frame_aspect = 4.0 / 3;
}

/* ---- recording ------------------------------------------------------------ */

#define REC_FPS 35
static FILE *rec_video, *rec_audio;
static uint64_t rec_frames, rec_samples;
static char rec_vpath[512], rec_apath[512];

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, riff = 36 + data, rate = AUDIO_RATE, br = AUDIO_RATE * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static int rec_w = 640, rec_h = 400;

static void rec_open(void)
{
    snprintf(rec_vpath, sizeof rec_vpath, "%s.video.mp4", g_opt.record);
    snprintf(rec_apath, sizeof rec_apath, "%s.audio.wav", g_opt.record);
    char cmd[1200];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -y -loglevel error -f rawvideo -pixel_format bgra -video_size %dx%d "
             "-framerate %d -i - -vf scale=%d:%d:flags=neighbor -c:v libx264 -pix_fmt yuv420p "
             "-crf 18 \"%s\"", rec_w, rec_h, REC_FPS, rec_w < 1280 ? 1280 : rec_w,
             (rec_w < 1280 ? 1280 : rec_w) * 3 / 4, rec_vpath);
#ifdef _WIN32
    rec_video = _popen(cmd, "wb");
#endif
    if (!rec_video) fatal("cannot start ffmpeg (is it on PATH?)");
    rec_audio = fopen(rec_apath, "wb");
    if (rec_audio) wav_header(rec_audio, 0);
}

static void rec_close(void)
{
    if (!rec_video) return;
#ifdef _WIN32
    _pclose(rec_video);
#endif
    rec_video = NULL;
    if (rec_audio) { wav_header(rec_audio, (uint32_t)rec_samples); fclose(rec_audio); rec_audio = NULL; }
    char cmd[1600];
    snprintf(cmd, sizeof cmd, "ffmpeg -y -loglevel error -i \"%s\" -i \"%s\" -c:v copy -c:a aac -b:a 192k -shortest \"%s\"",
             rec_vpath, rec_apath, g_opt.record);
    if (system(cmd) == 0) { remove(rec_vpath); remove(rec_apath); }
    fprintf(stderr, "[rec] %s: %llu frames\n", g_opt.record, (unsigned long long)rec_frames);
}

/* Every frame to the encoder at one size, whatever mode the game is in */
static const uint32_t *as_rec(void)
{
    if (fw == rec_w && fh == rec_h) return frame;
    for (int y = 0; y < rec_h; y++)
        for (int x = 0; x < rec_w; x++)
            big[y * rec_w + x] = frame[(y * fh / rec_h) * fw + x * fw / rec_w];
    return big;
}

static FILE *wav_out;
static uint64_t wav_samples;

void host_snapshot(const char *path);

/* --shot-at "ms:file,...": frames at fixed points in emulated time, for the
 * conformance harness (tools/conformance.py) */
static void shots_due(uint64_t now)
{
    static const char *p;
    static int init;
    if (!init) { init = 1; p = g_opt.shots; }
    while (p && *p) {
        double ms; char path[260]; int n = 0;
        if (sscanf(p, "%lf:%259[^,]%n", &ms, path, &n) < 2) { p = NULL; return; }
        if (now < (uint64_t)(ms * PIT_HZ / 1000.0)) return;
        host_snapshot(path);
        p += n; if (*p == ',') p++;
    }
}

/* ---- scripted keys ----------------------------------------------------------- */

typedef struct { uint64_t at; uint8_t sc; uint8_t up; int8_t state; } KeyEv;   /* state: SAVEn / LOADn */
static KeyEv kev[512];
static int nkev, kev_i;

static int key_by_name(const char *s)
{
    static const struct { const char *n; int sc; } t[] = {
        {"ESC",1},{"ENTER",0x1C},{"SPACE",0x39},{"UP",0x48|0x100},{"DOWN",0x50|0x100},
        {"LEFT",0x4B|0x100},{"RIGHT",0x4D|0x100},{"CTRL",0x1D},{"ALT",0x38},{"SHIFT",0x2A},
        {"TAB",0x0F},{"BKSP",0x0E},{"Y",0x15},{"N",0x31},{"F1",0x3B},{"F2",0x3C},{"F3",0x3D},
        {"F10",0x44},{"1",2},{"2",3},{"3",4},{"4",5},{"5",6},{"6",7},{NULL,0}};
    for (int i = 0; t[i].n; i++) if (!_stricmp(s, t[i].n)) return t[i].sc;
    return (int)strtol(s, NULL, 16);
}

static void keys_parse(void)
{
    const char *p = g_opt.keys;
    while (p && *p && nkev < 500) {
        char name[32]; double ms; int hold = 120, n = 0;
        if (sscanf(p, "%lf:%31[^,]%n", &ms, name, &n) < 2) break;
        char *h = strchr(name, '+');            /* KEY+holdms */
        if (h) { *h = 0; hold = atoi(h + 1); }
        uint64_t at = (uint64_t)(ms * PIT_HZ / 1000.0);
        if (!_strnicmp(name, "SAVE", 4) || !_strnicmp(name, "LOAD", 4)) {
            int slot = atoi(name + 4);
            kev[nkev++] = (KeyEv){ at, 0, 0, (int8_t)(name[0] == 'S' || name[0] == 's' ? slot : -slot) };
            p += n; if (*p == ',') p++;
            continue;
        }
        int sc = key_by_name(name);
        kev[nkev++] = (KeyEv){ at, (uint8_t)sc, 0 };
        kev[nkev++] = (KeyEv){ at + (uint64_t)(hold * PIT_HZ / 1000.0), (uint8_t)sc, 1 };
        p += n; if (*p == ',') p++;
    }
    /* sort by time (insertion; small) */
    for (int i = 1; i < nkev; i++)
        for (int j = i; j > 0 && kev[j].at < kev[j - 1].at; j--) { KeyEv t = kev[j]; kev[j] = kev[j - 1]; kev[j - 1] = t; }
}

static int ext_of(int sc) { return sc == 0x48 || sc == 0x50 || sc == 0x4B || sc == 0x4D; }

static void keys_due(void)
{
    while (kev_i < nkev && kev[kev_i].at <= pit_now()) {
        int sc = kev[kev_i].sc;
        if (kev[kev_i].state) { extern volatile int g_state_request; g_state_request = kev[kev_i].state; kev_i++; continue; }
        if (ext_of(sc)) kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(sc | (kev[kev_i].up ? 0x80 : 0)));
        kev_i++;
    }
}

static char note[128];
static uint32_t note_until;

/* A line for the title bar (the window) or stderr (headless) */
void host_note(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(note, sizeof note, fmt, ap); va_end(ap);
#ifdef _WIN32
    note_until = GetTickCount() + 3000;
#endif
    fprintf(stderr, "[host] %s\n", note);
}

/* ---- window ---------------------------------------------------------------- */

/* The game uses F1-F10 and nearly every letter, so the window's own keys are
 * the two it leaves alone:
 *
 *   F11              the settings menu: save states, window size, scaling,
 *                    speed, vivid colour, fullscreen, mute, screenshot
 *   F12              next scaling mode; Shift+F12 a screenshot
 *   Alt+Enter        borderless fullscreen
 *
 * The picture is scaled by Direct3D 11 (present_d3d.c) or, without it, GDI.
 * Choices are kept in settings.ini beside the exe. */
#ifdef _WIN32
static HWND wnd;
static int quit_req;
static int captured;
static int cw, ch;
static RECT dst;                                /* where the picture lands */
static int scale = SCALE_SHARP, vivid, fullscreen, muted;
static char ini_path[MAX_PATH];
static WINDOWPLACEMENT saved_place = { sizeof saved_place };
static const char *scale_names[SCALE_COUNT] = { "sharp", "smooth", "crt", "nearest", "integer" };
static const double speeds[] = { 0.5, 1, 2, 4 };
void state_path(int slot, char *out, size_t n);
extern volatile int g_state_request;

static void save_settings(void)
{
    char v[16];
    WritePrivateProfileStringA("display", "scale", scale_names[scale], ini_path);
    WritePrivateProfileStringA("display", "vivid", vivid ? "1" : "0", ini_path);
    WritePrivateProfileStringA("display", "fullscreen", fullscreen ? "1" : "0", ini_path);
    WritePrivateProfileStringA("sound", "mute", muted ? "1" : "0", ini_path);
    snprintf(v, sizeof v, "%g", machine_speed());
    WritePrivateProfileStringA("game", "speed", v, ini_path);
}

static void load_settings(void)
{
    char buf[32];
    GetModuleFileNameA(NULL, ini_path, MAX_PATH);
    strcpy(strrchr(ini_path, '\\') + 1, "settings.ini");
    GetPrivateProfileStringA("display", "scale", "sharp", buf, sizeof buf, ini_path);
    for (int i = 0; i < SCALE_COUNT; i++) if (!_stricmp(buf, scale_names[i])) scale = i;
    vivid = GetPrivateProfileIntA("display", "vivid", 0, ini_path);
    fullscreen = GetPrivateProfileIntA("display", "fullscreen", 0, ini_path);
    muted = GetPrivateProfileIntA("sound", "mute", 0, ini_path);
    GetPrivateProfileStringA("game", "speed", "1", buf, sizeof buf, ini_path);
    if (atof(buf) > 0) machine_set_speed(atof(buf));
    /* the command line wins over the file */
    if (g_opt.display)
        for (int i = 0; i < SCALE_COUNT; i++) if (!_stricmp(g_opt.display, scale_names[i])) scale = i;
    if (g_opt.fullscreen) fullscreen = 1;
    if (g_opt.mute) muted = 1;
}

static void layout(void)
{
    int w, h;
    if (scale == SCALE_INTEGER) {                   /* whole multiples, square pixels */
        int k = cw / fw < ch / fh ? cw / fw : ch / fh;
        if (k < 1) k = 1;
        w = fw * k; h = fh * k;
    } else {                                        /* 4:3, as every mode was shown */
        w = cw; h = (int)(cw / frame_aspect);
        if (h > ch) { h = ch; w = (int)(ch * frame_aspect); }
    }
    dst.left = (cw - w) / 2; dst.top = (ch - h) / 2;
    dst.right = dst.left + w; dst.bottom = dst.top + h;
}

static void set_title(void)
{
    static char last[192];
    char t[192];
    if (GetTickCount() < note_until) snprintf(t, sizeof t, "Microsoft Space Simulator - %s", note);
    else if (!captured) snprintf(t, sizeof t, "Microsoft Space Simulator - F11 settings, click to use the mouse");
    else snprintf(t, sizeof t, "Microsoft Space Simulator");
    if (strcmp(t, last)) { strcpy(last, t); SetWindowTextA(wnd, t); }
}

static void apply_fullscreen(int on)
{
    DWORD st = GetWindowLong(wnd, GWL_STYLE);
    if (on) {
        MONITORINFO mi = { sizeof mi };
        if (st & WS_OVERLAPPEDWINDOW) GetWindowPlacement(wnd, &saved_place);
        GetMonitorInfo(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLong(wnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else if (!(st & WS_OVERLAPPEDWINDOW)) {
        SetWindowLong(wnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(wnd, &saved_place);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen = on;
}

/* Window sizes in 320x240 steps that fit the work area, borders and all */
static int max_window_k(void)
{
    RECT wa; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    int k = 8;
    for (; k > 1; k--) {
        RECT r = { 0, 0, 320 * k, 240 * k };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (r.right - r.left <= wa.right - wa.left && r.bottom - r.top <= wa.bottom - wa.top) break;
    }
    return k;
}

static void size_window(int k)
{
    RECT r = { 0, 0, 320 * k, 240 * k }, wa;
    if (fullscreen) apply_fullscreen(0);
    ShowWindow(wnd, SW_RESTORE);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(wnd, NULL, wa.left + (wa.right - wa.left - (r.right - r.left)) / 2,
                 wa.top + (wa.bottom - wa.top - (r.bottom - r.top)) / 2,
                 r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
}

/* Shift+F12: the current frame to screenshots\<game>-NNN.bmp */
static void screenshot_key(void)
{
    char p[256];
    CreateDirectoryA("screenshots", NULL);
    for (int n = 0; n < 1000; n++) {
        snprintf(p, sizeof p, "screenshots/%s-%03d.bmp", g_game_id, n);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) { host_snapshot(p); host_note("%s", p); return; }
    }
}

static void release_mouse(void)
{
    if (captured) { ClipCursor(NULL); ShowCursor(TRUE); captured = 0; mouse_host(0, 0, 0); }
}

/* F11: every setting in one native menu */
static void settings_menu(void)
{
    enum { ID_SCALE = 100, ID_SPEED = 200, ID_VIVID = 300, ID_FULL, ID_MUTE, ID_SHOT,
           ID_SIZE = 400, ID_SAVE = 500, ID_LOAD = 600 };
    static const char *scale_labels[SCALE_COUNT] = {
        "Sharp (crisp pixels, smooth edges)", "Smooth", "CRT", "Nearest", "Integer (whole multiples)" };
    release_mouse();
    HMENU m = CreatePopupMenu(), sc = CreatePopupMenu(), sp = CreatePopupMenu(), sz = CreatePopupMenu(),
          sv = CreatePopupMenu(), ld = CreatePopupMenu();
    for (int slot = 1; slot <= 9; slot++) {
        char path[MAX_PATH], t[64];
        WIN32_FILE_ATTRIBUTE_DATA a;
        SYSTEMTIME u, lt;
        int used;
        state_path(slot, path, sizeof path);
        if ((used = GetFileAttributesExA(path, GetFileExInfoStandard, &a)) != 0) {
            FileTimeToSystemTime(&a.ftLastWriteTime, &u);
            SystemTimeToTzSpecificLocalTime(NULL, &u, &lt);
            snprintf(t, sizeof t, "Slot %d\t%04d-%02d-%02d %02d:%02d", slot, lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute);
        } else snprintf(t, sizeof t, "Slot %d\t(empty)", slot);
        AppendMenuA(sv, MF_STRING, ID_SAVE + slot, t);
        AppendMenuA(ld, MF_STRING | (used ? 0 : MF_GRAYED), ID_LOAD + slot, t);
    }
    for (int k = 2; k <= max_window_k(); k++) {
        char t[48];
        snprintf(t, sizeof t, "%d x %d", 320 * k, 240 * k);
        AppendMenuA(sz, MF_STRING | (!fullscreen && cw == 320 * k && ch == 240 * k ? MF_CHECKED : 0), ID_SIZE + k, t);
    }
    for (int i = 0; i < SCALE_COUNT; i++)
        AppendMenuA(sc, MF_STRING | (scale == i ? MF_CHECKED : 0), ID_SCALE + i, scale_labels[i]);
    for (int i = 0; i < (int)(sizeof speeds / sizeof speeds[0]); i++) {
        char t[32];
        snprintf(t, sizeof t, "%gx%s", speeds[i], speeds[i] == 1 ? " (real time)" : "");
        AppendMenuA(sp, MF_STRING | (machine_speed() == speeds[i] ? MF_CHECKED : 0), ID_SPEED + i, t);
    }
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sv, "Save state");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)ld, "Load state");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sz, "Window size");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sc, "Scaling\tF12");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sp, "Speed");
    AppendMenuA(m, MF_STRING | (vivid ? MF_CHECKED : 0), ID_VIVID, "Vivid colour");
    AppendMenuA(m, MF_STRING | (fullscreen ? MF_CHECKED : 0), ID_FULL, "Fullscreen\tAlt+Enter");
    AppendMenuA(m, MF_STRING | (muted ? MF_CHECKED : 0), ID_MUTE, "Mute");
    AppendMenuA(m, MF_STRING, ID_SHOT, "Screenshot\tShift+F12");
    POINT p = { (dst.left + dst.right) / 2, (dst.top + dst.bottom) / 2 };
    ClientToScreen(wnd, &p);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, wnd, NULL);
    DestroyMenu(m);
    if (cmd > ID_SAVE && cmd <= ID_SAVE + 9) g_state_request = cmd - ID_SAVE;
    else if (cmd > ID_LOAD && cmd <= ID_LOAD + 9) g_state_request = -(cmd - ID_LOAD);
    else if (cmd >= ID_SIZE && cmd < ID_SAVE) size_window(cmd - ID_SIZE);
    else if (cmd >= ID_SCALE && cmd < ID_SCALE + SCALE_COUNT) { scale = cmd - ID_SCALE; layout(); }
    else if (cmd >= ID_SPEED && cmd < ID_VIVID) { machine_set_speed(speeds[cmd - ID_SPEED]); host_note("speed %gx", machine_speed()); }
    else if (cmd == ID_VIVID) vivid ^= 1;
    else if (cmd == ID_FULL) apply_fullscreen(!fullscreen);
    else if (cmd == ID_MUTE) muted ^= 1;
    else if (cmd == ID_SHOT) screenshot_key();
    if (cmd) save_settings();
}

static int mouse_buttons_now(void)
{
    return ((GetKeyState(VK_LBUTTON) < 0) ? 1 : 0) | ((GetKeyState(VK_RBUTTON) < 0) ? 2 : 0)
         | ((GetKeyState(VK_MBUTTON) < 0) ? 4 : 0);
}

/* GDI: the fallback when Direct3D 11 is unavailable */
static void gdi_paint(void)
{
    HDC dc = GetDC(wnd);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), fw, -fh, 1, 32, BI_RGB } };
    HBRUSH black = GetStockObject(BLACK_BRUSH);
    RECT bars[4] = { { 0, 0, cw, dst.top }, { 0, dst.bottom, cw, ch },
                     { 0, dst.top, dst.left, dst.bottom }, { dst.right, dst.top, cw, dst.bottom } };
    for (int i = 0; i < 4; i++) FillRect(dc, &bars[i], black);
    SetStretchBltMode(dc, scale == SCALE_SMOOTH ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
                  0, 0, fw, fh, frame, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(wnd, dc);
}

static void render(void)
{
    static int lw, lh;
    if (fw != lw || fh != lh) { lw = fw; lh = fh; layout(); }
    set_title();
    if (g_opt.gdi || !d3d_present(wnd, frame, fw, fh, &dst, cw, ch, scale, vivid, 0)) {
        g_opt.gdi = 1;
        gdi_paint();
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CLOSE: quit_req = 1; return 0;
    case WM_SIZE: cw = LOWORD(lp); ch = HIWORD(lp); layout(); return 0;
    case WM_PAINT: ValidateRect(h, NULL); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SYSKEYDOWN: case WM_KEYDOWN: case WM_SYSKEYUP: case WM_KEYUP: {
        int up = (m == WM_KEYUP || m == WM_SYSKEYUP);
        if (wp == VK_RETURN && (lp & (1 << 29))) { if (!up) { apply_fullscreen(!fullscreen); save_settings(); } return 0; }
        if (wp == VK_F4 && (lp & (1 << 29))) { quit_req = 1; return 0; }
        if (wp == VK_F11) { if (!up) settings_menu(); return 0; }
        if (wp == VK_F12) {
            if (up) return 0;
            if (GetKeyState(VK_SHIFT) < 0) { screenshot_key(); return 0; }
            scale = (scale + 1) % SCALE_COUNT;
            layout(); save_settings(); host_note("%s", scale_names[scale]);
            return 0;
        }
        if (!up && (lp & (1 << 30))) return 0;      /* the game does its own repeat */
        int sc = (lp >> 16) & 0xFF;
        if (lp & (1 << 24)) kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(sc | (up ? 0x80 : 0)));
        return 0;
    }
    case WM_SYSCHAR: return 0;                       /* no menu beep on Alt */

    /* Mouse: click in the window to capture it (the game turns with it when
     * MOUSE ENABLED is on in its options); losing focus or F11 releases it.
     * Raw input, so the motion is the device's, not the cursor's. */
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (!captured) {
            RAWINPUTDEVICE rid = { 1, 2, RIDEV_INPUTSINK, h };
            RECT r = dst; MapWindowPoints(h, NULL, (POINT *)&r, 2);
            RegisterRawInputDevices(&rid, 1, sizeof rid);
            ClipCursor(&r); ShowCursor(FALSE); captured = 1;
            host_note("Alt+Tab or F11 frees the mouse");
            return 0;
        }
        /* fall through: a click while captured is a button for the game */
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if (captured) mouse_host(0, 0, (int)(((wp & MK_LBUTTON) ? 1 : 0) | ((wp & MK_RBUTTON) ? 2 : 0) | ((wp & MK_MBUTTON) ? 4 : 0)));
        return 0;
    case WM_INPUT: {
        RAWINPUT ri; UINT sz = sizeof ri;
        if (captured && GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1
            && ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            mouse_host(ri.data.mouse.lLastX, ri.data.mouse.lLastY, mouse_buttons_now());
        break;
    }
    case WM_KILLFOCUS: release_mouse(); break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* waveOut: a ring the main thread fills and a few buffers cycling under it */
#define WBUF 8
#define WLEN 1024
static HWAVEOUT wout;
static WAVEHDR whdr[WBUF];
static int16_t wdata[WBUF][WLEN * 2];
static int wfill, wcur;

static void wave_open(void)
{
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, AUDIO_RATE, AUDIO_RATE * 4, 4, 16, 0 };
    if (waveOutOpen(&wout, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wout = NULL; return; }
    for (int i = 0; i < WBUF; i++) {
        whdr[i].lpData = (LPSTR)wdata[i]; whdr[i].dwBufferLength = WLEN * 4;
        waveOutPrepareHeader(wout, &whdr[i], sizeof whdr[i]);
        whdr[i].dwFlags |= WHDR_DONE;
    }
}

static void wave_push(const int16_t *s, int frames)
{
    while (frames > 0) {
        WAVEHDR *h = &whdr[wcur];
        if (!(h->dwFlags & WHDR_DONE)) return;       /* all queued: drop rather than block */
        int n = WLEN - wfill; if (n > frames) n = frames;
        memcpy(&wdata[wcur][wfill * 2], s, n * 4);
        wfill += n; s += n * 2; frames -= n;
        if (wfill == WLEN) {
            h->dwFlags &= ~WHDR_DONE;
            waveOutWrite(wout, h, sizeof *h);
            wfill = 0; wcur = (wcur + 1) % WBUF;
        }
    }
}
#endif

/* Gamepad: an XInput pad pressed as the keys the game already knows, so it
 * works in the menus and in flight with no joystick calibration (the game's
 * own joystick, port 201h, would want calibrating each session).
 *   stick / d-pad  arrows: yaw and pitch
 *   RT / LT        keypad + / -: thrust up and down
 *   RB / LB        F1 / F2: the time rate
 *   A  Enter       B / Start  Esc       X  Space       Back  Tab        */
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, void *);

static void pad_poll(void)
{
    static XInputGetState_t get;
    static int tried;
    static uint32_t held;
    static const struct { uint32_t bit; uint8_t sc; uint8_t ext; } map[] = {
        {1 << 0, 0x48, 1}, {1 << 1, 0x50, 1}, {1 << 2, 0x4B, 1}, {1 << 3, 0x4D, 1},
        {1 << 4, 0x4E, 0}, {1 << 5, 0x4A, 0}, {1 << 6, 0x1C, 0}, {1 << 7, 0x39, 0},
        {1 << 8, 0x3B, 0}, {1 << 9, 0x01, 0}, {1 << 10, 0x0F, 0}, {1 << 11, 0x3C, 0}};
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } s;

    if (!tried) {
        HMODULE m;
        tried = 1;
        m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
    }
    if (!get || get(0, &s) != ERROR_SUCCESS) return;

    uint32_t now = 0;
    WORD b = s.buttons;
    if ((b & 0x0001) || s.ly >  12000) now |= 1 << 0;   /* up */
    if ((b & 0x0002) || s.ly < -12000) now |= 1 << 1;   /* down */
    if ((b & 0x0004) || s.lx < -12000) now |= 1 << 2;   /* left */
    if ((b & 0x0008) || s.lx >  12000) now |= 1 << 3;   /* right */
    if (s.rt > 64) now |= 1 << 4;
    if (s.lt > 64) now |= 1 << 5;
    if (b & 0x1000) now |= 1 << 6;                       /* A */
    if (b & 0x4000) now |= 1 << 7;                       /* X */
    if (b & 0x0200) now |= 1 << 8;                       /* RB */
    if (b & 0x0100) now |= 1 << 11;                      /* LB */
    if (b & (0x2000 | 0x0010)) now |= 1 << 9;            /* B, Start */
    if (b & 0x0020) now |= 1 << 10;                      /* Back */

    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        uint32_t m = map[i].bit;
        if ((now ^ held) & m) {
            if (map[i].ext) kbd_scancode(0xE0);
            kbd_scancode((uint8_t)(map[i].sc | ((now & m) ? 0 : 0x80)));
        }
    }
    held = now;
}

void host_audio(const int16_t *s, int frames)
{
    if (rec_audio) { fwrite(s, 4, frames, rec_audio); rec_samples += frames; }
    if (wav_out) { fwrite(s, 4, frames, wav_out); wav_samples += frames; }
#ifdef _WIN32
    if (wout && !muted) wave_push(s, frames);
#endif
}

void host_init(void)
{
    keys_parse();
    if (g_opt.record) rec_open();
    if (g_opt.wav && (wav_out = fopen(g_opt.wav, "wb")) != NULL) wav_header(wav_out, 0);
#ifdef _WIN32
    if (g_opt.headless) return;
    SetProcessDPIAware();
    load_settings();
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "spacesim";
    wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
    RegisterClassA(&wc);
    wnd = CreateWindowA("spacesim", "Microsoft Space Simulator", WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL, wc.hInstance, NULL);
    /* --scale, else the largest 320x240 multiple that fits (3x on 1080p) */
    int k = g_opt.scale ? g_opt.scale : max_window_k(), fs = fullscreen;
    size_window(k);
    ShowWindow(wnd, SW_SHOW);
    if (fs) apply_fullscreen(1);
    wave_open();
#endif
}

void host_frame(void)
{
    static uint64_t last_present;
    keys_due();
    uint64_t now = pit_now();
    shots_due(now);
    if (g_opt.seconds > 0 && now >= (uint64_t)(g_opt.seconds * PIT_HZ)) {
        void host_backtrace(void);
        host_backtrace();                 /* debug builds: where the time ran out */
        dos_exit(0);
    }
    if (rec_video) {
        uint64_t per = (uint64_t)(PIT_HZ / REC_FPS);
        if ((rec_frames + 1) * per <= now) {
            compose();
            const uint32_t *f = as_rec();
            while ((rec_frames + 1) * per <= now) { fwrite(f, 4, (size_t)rec_w * rec_h, rec_video); rec_frames++; }
        }
    }
#ifdef _WIN32
    if (!wnd) return;
    uint64_t t = host_us();                 /* 70 a second of real time, whatever the speed */
    if (t - last_present < 1000000 / 70) return;
    last_present = t;
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (GetForegroundWindow() == wnd) pad_poll();
    if (quit_req) dos_exit(0);
    compose();
    render();
#endif
}

/* The last thing on screen, as a BMP -- a run nobody watched still says where
 * it got to. */
void host_snapshot(const char *path)
{
    compose();
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t sz = 54 + fw * fh * 4, off = 54, hs = 40, zero = 0, img = fw * fh * 4;
    int32_t w = fw, h = -fh; uint16_t planes = 1, bpp = 32;
    fwrite("BM", 1, 2, f); fwrite(&sz, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&off, 4, 1, f);
    fwrite(&hs, 4, 1, f); fwrite(&w, 4, 1, f); fwrite(&h, 4, 1, f); fwrite(&planes, 2, 1, f);
    fwrite(&bpp, 2, 1, f); fwrite(&zero, 4, 1, f); fwrite(&img, 4, 1, f);
    fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f);
    fwrite(frame, 4, fw * fh, f);
    fclose(f);
}

void host_shutdown(void)
{
    rec_close();
    if (wav_out) { wav_header(wav_out, (uint32_t)wav_samples); fclose(wav_out); wav_out = NULL; }
#ifdef _WIN32
    if (wout) { waveOutReset(wout); waveOutClose(wout); }
    if (wnd) DestroyWindow(wnd);
#endif
}
