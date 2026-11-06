// cursorblur.c - motion-blurred Windows cursor with a fading trail, auto-off in games.
// Hides the standard system cursors and draws them itself, blurred across each frame's motion,
// so the eye/camera sees one even smear instead of a row of crisp copies (phantom array).
// Mouse moves are timestamped by a low-level hook at the full mouse rate, and every frame shows the path at a fixed,
// vblank-aligned time, so each frame covers exactly one refresh of motion (no bright clumps).
// Build: gcc -O2 -mwindows cursorblur.c -o cursorblur.exe -ldwmapi -lgdi32 -luser32
// Quit:  Ctrl+Alt+Shift+Q (restores the real cursor)
#define _WIN32_WINNT 0x0A00 // Win10+ APIs (per-monitor DPI)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define TRAIL_MS 50.0  // recent cursor path that forms the trail
#define SMOOTH_MS 2.0  // averages the path over +-2 ms, hiding whole-pixel steps (blur length error 37% -> 7% in tests)
#define HIST     2048  // max path points (enough for 8 kHz mice over TRAIL_MS)
#define NS       4096  // mouse sample ring, power of two
#define STRENGTH 1.2f  // overall trail opacity (tune to taste)
#define STEP     2.0f  // px between ghost copies along the trail

typedef struct { HCURSOR id; int w, h, hx, hy; unsigned *px; } Sprite; // px = premultiplied BGRA
typedef struct { float x, y; } FP;
typedef struct { double t; POINT p; } Samp;

static HDC fbDC; static HBITMAP fbBmp; static unsigned *fb; static int capW, capH;

// ---- timestamped mouse samples (written by the input thread, read by the render loop) ----
static double qfreq;
static CRITICAL_SECTION cs; static Samp ring[NS]; static unsigned wr; static volatile double lastRaw = -1e9;
static volatile double rate = 1.0, jitter = 0.2; // learned ms between mouse reports, and its mean deviation

// How far in the past to show the cursor: far enough that the next mouse report has almost always
// arrived, so every frame interpolates instead of freezing. ~4 ms for 1000 Hz mice, ~14 ms for 125 Hz.
static double delay_ms(void) {
    double d = rate + 3 * jitter + SMOOTH_MS + 0.5;
    return d < 3 ? 3 : d > 30 ? 30 : d;
}

static double qpc_ms(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / qfreq; }
static int rnd(float v) { return (int)floorf(v + 0.5f); }

static void push(double t, POINT p) {
    EnterCriticalSection(&cs);
    if (wr && t < ring[(wr - 1) & (NS - 1)].t) t = ring[(wr - 1) & (NS - 1)].t; // keep the ring in time order
    ring[wr & (NS - 1)] = (Samp){t, p}; wr++;
    LeaveCriticalSection(&cs);
}

// Cursor position at time t (caller holds cs, wr > 0). Interpolates between reports,
// but holds still across long gaps so a resumed movement doesn't slide in from the past.
static FP at(double t) {
    unsigned beg = wr > NS ? wr - NS : 0, j = wr - 1;
    while (j > beg && ring[j & (NS - 1)].t > t) j--;
    Samp a = ring[j & (NS - 1)];
    FP r = {(float)a.p.x, (float)a.p.y};
    if (j + 1 < wr && a.t <= t) {
        Samp b = ring[(j + 1) & (NS - 1)];
        if (b.t > a.t && b.t - a.t < fmax(10, 3 * rate)) { // longer gap = the mouse had stopped
            float u = (float)((t - a.t) / (b.t - a.t));
            r.x += (b.p.x - a.p.x) * u; r.y += (b.p.y - a.p.y) * u;
        }
    }
    return r;
}

static FP sm(double t) {
    FP a = at(t - SMOOTH_MS), b = at(t), c = at(t + SMOOTH_MS);
    return (FP){(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3};
}

// Cursor path over [t0, t1], oldest first, plus the position at th. Returns the point count.
static int path(double t0, double t1, FP *out, int max, double th, FP *ph) {
    int n = 0;
    EnterCriticalSection(&cs);
    if (wr) {
        unsigned beg = wr > NS ? wr - NS : 0, i = wr;
        while (i > beg && ring[(i - 1) & (NS - 1)].t > t0) i--;
        out[n++] = sm(t0);
        for (; i < wr && ring[i & (NS - 1)].t < t1 && n < max - 1; i++)
            out[n++] = (FP){(float)ring[i & (NS - 1)].p.x, (float)ring[i & (NS - 1)].p.y};
        out[n++] = sm(t1);
        *ph = sm(th);
    }
    LeaveCriticalSection(&cs);
    return n;
}

// Low-level mouse hook: Windows calls it for every mouse move at the mouse's full rate, with the
// position the cursor is moving to. (Raw input to a background window gets batched to 125 Hz on
// Windows 11, which made the blur advance in 8 ms blocks.) Removed while a game runs.
static volatile LONG wantHook = 1; static DWORD inputTid;

static LRESULT CALLBACK mouse_hook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w == WM_MOUSEMOVE) {
        POINT p = ((const MSLLHOOKSTRUCT *)l)->pt;
        double t = qpc_ms(), dt = t - lastRaw;
        if (dt < 50) { rate += (dt - rate) * 0.05; jitter += (fabs(dt - rate) - jitter) * 0.05; } // learn the report rate
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN); // pt isn't clipped yet
        p.x = max(vx, min(p.x, vx + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
        p.y = max(vy, min(p.y, vy + GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
        push(t, p);
        lastRaw = t;
    }
    return CallNextHookEx(NULL, code, w, l);
}

static DWORD WINAPI input_thread(LPVOID hi) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); // every mouse move waits for this hook
    MSG m; HHOOK hk = NULL;
    PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE); // create the queue before reading wantHook, so no request is lost
    for (;;) {
        if (wantHook && !hk) hk = SetWindowsHookExA(WH_MOUSE_LL, mouse_hook, (HINSTANCE)hi, 0);
        if (!wantHook && hk) { UnhookWindowsHookEx(hk); hk = NULL; }
        if (GetMessageA(&m, NULL, 0, 0) <= 0) break;
        DispatchMessageA(&m);
    }
    return 0;
}

static void set_hook(int on) {
    if (wantHook == on) return;
    wantHook = on;
    PostThreadMessageA(inputTid, WM_NULL, 0, 0); // wake the input thread to apply it
}

// ---- system cursor takeover ----
// Static system cursors we take over. Wait/AppStarting stay real: they are animated.
static const int ids[] = {32512, 32513, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32651};
#define NIDS (int)(sizeof ids / sizeof ids[0])
static HCURSOR shared[NIDS], orig[NIDS];
static int hidden;

static void set_cursors(int blank) {
    if (blank == hidden) return;
    hidden = blank;
    BYTE andm[128], xorm[128]; memset(andm, 0xff, sizeof andm); memset(xorm, 0, sizeof xorm);
    for (int i = 0; i < NIDS; i++) // SetSystemCursor takes ownership, so hand it a fresh cursor each time
        SetSystemCursor(blank ? CreateCursor(GetModuleHandleA(NULL), 0, 0, 32, 32, andm, xorm) : CopyCursor(orig[i]), ids[i]);
}

static void restore_scheme(void) { SystemParametersInfoA(SPI_SETCURSORS, 0, NULL, 0); hidden = 0; } // reload user's scheme

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e) {
    (void)e; restore_scheme(); // never leave the user without a cursor
    return EXCEPTION_CONTINUE_SEARCH;
}

// Shell popups (Start, notifications, quick settings...) live in z-order bands above every normal
// topmost window, so our overlay can't cover them: show the real cursor there instead.
// Same-band topmost windows (taskbar, menus, tooltips) we simply stay above.
typedef BOOL (WINAPI *GetWindowBand_t)(HWND, DWORD *); // undocumented user32 export, read-only
static GetWindowBand_t pGetWindowBand;

static int popup_above(POINT p, HWND self) {
    HWND w = GetAncestor(WindowFromPoint(p), GA_ROOT); // our overlay is click-through, so never hit
    DWORD band = 1;
    if (!w) return 0;
    if (pGetWindowBand && pGetWindowBand(w, &band) && band > 1) return 1;
    if (GetWindowLongA(w, GWL_EXSTYLE) & WS_EX_TOPMOST) {
        HWND q = w;
        while ((q = GetWindow(q, GW_HWNDPREV)) && q != self) {}
        if (!q) SetWindowPos(self, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return 0;
}

// ---- per-monitor vblank (gdi32's D3DKMT, the driver-level wait) ----
// DWM timing only describes the primary monitor: on a 120 Hz second screen we drew 350 frames/s,
// it showed every third, and each shown frame covered a third of the motion (blur with gaps).
typedef struct { HDC hDc; UINT hAdapter; LUID luid; UINT source; } KmtOpen;
typedef struct { UINT hAdapter, hDevice, source; } KmtWait;
typedef LONG (APIENTRY *KmtOpen_t)(KmtOpen *);
typedef LONG (APIENTRY *KmtWait_t)(const KmtWait *);
typedef LONG (APIENTRY *KmtClose_t)(const UINT *);
static KmtOpen_t pKmtOpen; static KmtWait_t pKmtWait; static KmtClose_t pKmtClose;
static struct { HMONITOR mon; int failed; KmtWait w; double period, last; } vs;

// Blocks until the next vblank of mon; returns its time and the measured refresh period.
static int wait_vblank(HMONITOR mon, double *t, double *period) {
    if (!pKmtOpen || !pKmtWait) return 0;
    if (mon != vs.mon) {
        if (vs.w.hAdapter && pKmtClose) pKmtClose(&vs.w.hAdapter);
        memset(&vs, 0, sizeof vs);
        vs.mon = mon; vs.failed = 1; // until proven otherwise; don't retry every frame
        MONITORINFOEXA mi; mi.cbSize = sizeof mi;
        // Primary monitor: DwmFlush + DWM timing hit every refresh in tests; this wait missed ~1 in 4 at 350 Hz.
        if (!GetMonitorInfoA(mon, (MONITORINFO *)&mi) || (mi.dwFlags & MONITORINFOF_PRIMARY)) return 0;
        HDC dc = CreateDCA(NULL, mi.szDevice, NULL, NULL);
        if (!dc) return 0;
        KmtOpen o = {dc};
        LONG st = pKmtOpen(&o);
        DeleteDC(dc);
        if (st) return 0;
        DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
        vs.w.hAdapter = o.hAdapter; vs.w.source = o.source; vs.failed = 0;
        vs.period = 1000.0 / (dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60);
    }
    if (vs.failed || pKmtWait(&vs.w)) return 0;
    double now = qpc_ms(), d = now - vs.last;
    if (fabs(d - vs.period) < vs.period * 0.2) vs.period += (d - vs.period) * 0.02; // e.g. 349.9 Hz, not 350
    vs.last = *t = now; *period = vs.period;
    return 1;
}

// ---- drawing ----
static HBITMAP dib(HDC dc, int w, int h, unsigned **bits) {
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

// Pixel drawn on black vs on white -> premultiplied BGRA. Works for alpha, masked and XOR (I-beam) cursors.
static unsigned from_bw(unsigned b, unsigned w) {
    int a = 255 - ((int)(w >> 8 & 255) - (int)(b >> 8 & 255));
    a = a > 255 ? 255 : a < 0 ? 0 : a;
    unsigned r = (unsigned)a << 24;
    for (int sh = 0; sh < 24; sh += 8) { unsigned c = b >> sh & 255; r |= (c > (unsigned)a ? a : c) << sh; }
    return r;
}

// premultiplied "over"
static unsigned over(unsigned dp, unsigned sp) {
    unsigned inv = 255 - (sp >> 24), r = 0;
    for (int sh = 0; sh < 32; sh += 8) r |= ((sp >> sh & 255) + (dp >> sh & 255) * inv / 255) << sh;
    return r;
}

static int load_sprite(Sprite *s, HCURSOR c) {
    ICONINFO ii; BITMAP bm;
    if (!c || !GetIconInfo(c, &ii)) return 0;
    GetObject(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
    int w = bm.bmWidth, h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2; // mono cursors stack AND+XOR masks
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    HDC dc = CreateCompatibleDC(NULL); unsigned *blk, *wht;
    HBITMAP hb = dib(dc, w, h, &blk), hw = dib(dc, w, h, &wht);
    memset(blk, 0, w * h * 4); memset(wht, 0xff, w * h * 4);
    HGDIOBJ old = SelectObject(dc, hb); DrawIconEx(dc, 0, 0, c, w, h, 0, NULL, DI_NORMAL);
    SelectObject(dc, hw);                DrawIconEx(dc, 0, 0, c, w, h, 0, NULL, DI_NORMAL);
    SelectObject(dc, old);
    free(s->px); s->px = malloc(w * h * 4);
    for (int i = 0; i < w * h; i++) s->px[i] = from_bw(blk[i], wht[i]);
    DeleteObject(hb); DeleteObject(hw); DeleteDC(dc);
    s->id = c; s->w = w; s->h = h; s->hx = ii.xHotspot; s->hy = ii.yHotspot;
    return 1;
}

static void ensure_fb(int w, int h) {
    if (w <= capW && h <= capH) return;
    capW = max(w, capW); capH = max(h, capH);
    HBITMAP nb = dib(fbDC, capW, capH, &fb);
    SelectObject(fbDC, nb);
    if (fbBmp) DeleteObject(fbBmp);
    fbBmp = nb;
}

// sprite scaled by k (0..256), "over" the framebuffer at (ox,oy)
static void blit(const Sprite *s, int ox, int oy, unsigned k) {
    for (int y = 0; y < s->h; y++) {
        unsigned *d = fb + (oy + y) * capW + ox; const unsigned *p = s->px + y * s->w;
        for (int x = 0; x < s->w; x++) {
            unsigned sp = p[x], ss = 0; if (!sp) continue;
            for (int sh = 0; sh < 32; sh += 8) ss |= (((sp >> sh & 255) * k) >> 8) << sh;
            d[x] = over(d[x], ss);
        }
    }
}

// The cursor itself: box-filter average of the sprite along a->b (one refresh of motion).
// a == b gives a crisp cursor.
// Sub-pixel endpoints: samples every half pixel, so each frame's smear has its true length.
static void blit_head(const Sprite *s, float ax, float ay, float bx, float by) {
    static unsigned short *acc; static size_t cap;
    int hx = (int)floorf(fminf(ax, bx)), hy = (int)floorf(fminf(ay, by));
    int w = (int)ceilf(fmaxf(ax, bx)) - hx + s->w, h = (int)ceilf(fmaxf(ay, by)) - hy + s->h;
    float dx = bx - ax, dy = by - ay;
    int n = (int)ceilf(2 * sqrtf(dx * dx + dy * dy)) + 1; if (n > 64) n = 64; // 64*255 fits in u16
    size_t need = (size_t)w * h * 4;
    if (need > cap) { free(acc); acc = malloc(need * sizeof *acc); cap = need; }
    memset(acc, 0, need * sizeof *acc);
    for (int i = 0; i < n; i++) {
        float t = n > 1 ? (float)i / (n - 1) : 1;
        int px = rnd(ax + dx * t) - hx, py = rnd(ay + dy * t) - hy;
        for (int y = 0; y < s->h; y++) {
            unsigned short *a = acc + ((size_t)(py + y) * w + px) * 4; const unsigned *p = s->px + y * s->w;
            for (int x = 0; x < s->w; x++, a += 4) {
                unsigned sp = p[x]; if (!sp) continue;
                a[0] += sp & 255; a[1] += sp >> 8 & 255; a[2] += sp >> 16 & 255; a[3] += sp >> 24;
            }
        }
    }
    for (int y = 0; y < h; y++) {
        unsigned short *a = acc + (size_t)y * w * 4; unsigned *d = fb + (hy + y) * capW + hx;
        for (int x = 0; x < w; x++, a += 4) {
            if (!a[3]) continue;
            d[x] = over(d[x], (a[0] / n) | (a[1] / n) << 8 | (a[2] / n) << 16 | (unsigned)(a[3] / n) << 24);
        }
    }
}

// ---- one frame: what to draw (from the sample ring) and drawing it (into fb) ----
static FP hist[HIST];
typedef struct { FP a; int n, x0, y0, x1, y1, still; float len; POINT cur; } Plan;

// The path for render time T: trail polyline in hist, head start in a, bounding box, stillness.
static int plan_frame(double T, double period, Plan *p) {
    p->n = path(T - TRAIL_MS, T, hist, HIST, T - period, &p->a);
    if (!p->n) return 0;
    float fx0 = hist[0].x, fy0 = hist[0].y, fx1 = fx0, fy1 = fy0, len = 0;
    for (int i = 1; i < p->n; i++) {
        fx0 = fminf(fx0, hist[i].x); fy0 = fminf(fy0, hist[i].y);
        fx1 = fmaxf(fx1, hist[i].x); fy1 = fmaxf(fy1, hist[i].y);
        float dx = hist[i].x - hist[i - 1].x, dy = hist[i].y - hist[i - 1].y;
        len += sqrtf(dx * dx + dy * dy);
    }
    p->x0 = (int)floorf(fx0); p->y0 = (int)floorf(fy0); p->x1 = (int)ceilf(fx1); p->y1 = (int)ceilf(fy1);
    p->len = len; p->still = len < STEP;
    p->cur = (POINT){rnd(hist[p->n - 1].x), rnd(hist[p->n - 1].y)};
    return 1;
}

// Draws trail + head into fb (origin at x0,y0 minus the hotspot); returns the used size.
static void draw_frame(const Sprite *s, const Plan *p, int own, int *W, int *H) {
    *W = p->x1 - p->x0 + s->w; *H = p->y1 - p->y0 + s->h;
    ensure_fb(*W, *H);
    for (int y = 0; y < *H; y++) memset(fb + y * capW, 0, *W * 4);

    // Trail: walk the path backwards from the newest point, dropping fading ghosts.
    unsigned base = (unsigned)(256 * STRENGTH * STEP / s->w);
    int seg = p->n - 1; float segPos = 0;
    for (float d = STEP; !p->still && d < p->len; d += STEP) {
        float need = STEP; // advance STEP px along the polyline
        float sx, sy;
        for (;;) {
            float dx = hist[seg - 1].x - hist[seg].x, dy = hist[seg - 1].y - hist[seg].y;
            float sl = sqrtf(dx * dx + dy * dy);
            if (segPos + need <= sl || seg == 1) {
                segPos += need;
                float t = sl > 0 ? (segPos > sl ? 1 : segPos / sl) : 0;
                sx = hist[seg].x + dx * t; sy = hist[seg].y + dy * t;
                break;
            }
            need -= sl - segPos; segPos = 0; seg--;
        }
        float fade = 1 - d / p->len;
        unsigned k = (unsigned)(base * fade * fade);
        if (k > 256) k = 256;
        if (k) blit(s, rnd(sx) - p->x0, rnd(sy) - p->y0, k);
    }

    // Head: the cursor blurred over exactly one refresh of motion.
    if (own) {
        FP a = p->a, b = p->still ? (FP){(float)p->cur.x, (float)p->cur.y} : hist[p->n - 1];
        if (p->still) a = b;
        blit_head(s, a.x - p->x0, a.y - p->y0, b.x - p->x0, b.y - p->y0);
    }
}

static int game_mode(const CURSORINFO *ci) {
    if (!(ci->flags & CURSOR_SHOWING)) return 1;                 // game hid the cursor
    RECT clip; GetClipCursor(&clip);
    if (clip.left > GetSystemMetrics(SM_XVIRTUALSCREEN) || clip.top > GetSystemMetrics(SM_YVIRTUALSCREEN) ||
        clip.right - clip.left < GetSystemMetrics(SM_CXVIRTUALSCREEN) ||
        clip.bottom - clip.top < GetSystemMetrics(SM_CYVIRTUALSCREEN)) return 1; // game locked the cursor
    HWND f = GetForegroundWindow(); char cls[16] = "";
    if (!f || IsZoomed(f)) return 0;                             // maximized apps are not games
    GetClassNameA(f, cls, sizeof cls);
    if (!strcmp(cls, "Progman") || !strcmp(cls, "WorkerW")) return 0; // the desktop itself
    RECT r; MONITORINFO mi = {sizeof mi};
    GetWindowRect(f, &r); GetMonitorInfo(MonitorFromWindow(f, MONITOR_DEFAULTTONEAREST), &mi);
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom; // fullscreen / borderless
}

static void selftest(void) {
    assert(from_bw(0x00000000, 0x00ffffff) == 0x00000000);      // transparent
    assert(from_bw(0x00ff0000, 0x00ff0000) == 0xffff0000);      // opaque red
    assert(from_bw(0x00ffffff, 0x00000000) == 0xffffffff);      // XOR pixel -> opaque white
    assert(from_bw(0x00404040, 0x00bfbfbf) == 0x80404040);      // ~50% gray, premultiplied
    assert(over(0x00000000, 0xffffffff) == 0xffffffff);         // opaque src wins
    assert(over(0xff0000ff, 0x00000000) == 0xff0000ff);         // empty src keeps dst
    ring[0] = (Samp){100, {0, 0}}; ring[1] = (Samp){102, {10, 0}}; ring[2] = (Samp){500, {50, 0}}; wr = 3;
    assert(at(50).x == 0);                                      // before history: oldest point
    assert(at(101).x == 5);                                     // between reports: interpolated
    assert(at(300).x == 10);                                    // long gap: holds still
    assert(at(900).x == 50);                                    // after history: newest point
    FP h; FP pts[8]; int n = path(101, 400, pts, 8, 101, &h);
    assert(n == 3 && pts[0].x == 5 && pts[1].x == 10 && pts[2].x == 10 && h.x == 5);
    wr = 0;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    InitializeCriticalSection(&cs);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); qfreq = (double)f.QuadPart;
    selftest();
    CreateMutexA(NULL, TRUE, "cursorblur-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // physical pixel coords
    RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'Q');
    SetUnhandledExceptionFilter(on_crash);
    pGetWindowBand = (GetWindowBand_t)GetProcAddress(GetModuleHandleA("user32"), "GetWindowBand");
    HMODULE gdi = GetModuleHandleA("gdi32");
    pKmtOpen = (KmtOpen_t)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
    pKmtWait = (KmtWait_t)GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent");
    pKmtClose = (KmtClose_t)GetProcAddress(gdi, "D3DKMTCloseAdapter");

    restore_scheme(); // undo a previous run that was killed
    for (int i = 0; i < NIDS; i++) {
        shared[i] = LoadCursorA(NULL, MAKEINTRESOURCEA(ids[i])); // handle apps use; its image gets blanked
        orig[i] = CopyCursor(shared[i]);                          // our copy of the real image
    }
    CreateThread(NULL, 0, input_thread, hi, 0, &inputTid);

    WNDCLASSA wc = {0, DefWindowProcA, 0, 0, hi, 0, 0, 0, 0, "cursorblur"};
    RegisterClassA(&wc);
    HWND wnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               "cursorblur", "", WS_POPUP, 0, 0, 1, 1, NULL, NULL, hi, NULL);
    fbDC = CreateCompatibleDC(NULL);
    Sprite spr = {0};

    POINT lastp = {0}; int visible = 0, gm = 0, pa = 0;
    double nextGm = 0, nextPa = 0;
    HCURSOR drawn = NULL; // cursor shown crisp at lastp (lets us skip redraws while still)
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    for (;;) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_HOTKEY || m.message == WM_QUIT) goto done;
            DispatchMessageA(&m);
        }
        // Pace to the vblank of the monitor the cursor is on (DWM only reports the primary's timing).
        static POINT lastCur;
        double kvb, kperiod;
        int kmt = wait_vblank(MonitorFromPoint(lastCur, MONITOR_DEFAULTTONEAREST), &kvb, &kperiod);
        if (!kmt) DwmFlush();

        CURSORINFO ci = {sizeof ci};
        double now = qpc_ms();
        int ok = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING); // hidden-cursor check is free, so per frame
        if (ok) lastCur = ci.ptScreenPos;
        if (ok && now >= nextGm) { gm = game_mode(&ci); nextGm = now + 1000; }
        if (ok && now >= nextPa) { pa = popup_above(ci.ptScreenPos, wnd); nextPa = now + 10; }
        ok = ok && !gm;
        set_hook(ok); // no hook while a game runs: it would sit in every mouse move
        HCURSOR img = ci.hCursor;
        for (int i = 0; i < NIDS; i++) if (img == shared[i]) img = orig[i];
        int own = img != ci.hCursor && !pa; // a cursor we blanked, so we draw it
        if (ok && ci.hCursor != spr.id && (ok = load_sprite(&spr, img))) spr.id = ci.hCursor;
        if (!ok) {
            drawn = NULL;
            set_cursors(0);
            if (visible) { ShowWindow(wnd, SW_HIDE); visible = 0; }
            Sleep(50);
            continue;
        }
        set_cursors(!pa);

        // Mouse paused (its last position is only known now) or no raw input at all (touchpad, pen,
        // SetCursorPos): sample the cursor once per frame.
        if (now - lastRaw > 5) {
            EnterCriticalSection(&cs);
            int same = wr && ring[(wr - 1) & (NS - 1)].p.x == ci.ptScreenPos.x && ring[(wr - 1) & (NS - 1)].p.y == ci.ptScreenPos.y;
            LeaveCriticalSection(&cs);
            if (!same) push(now, ci.ptScreenPos);
        }

        // Render time: the latest vblank minus the delay. Evenly spaced, unlike our wake-up time.
        DWM_TIMING_INFO ti = {sizeof ti};
        double period = kmt ? kperiod : 1000.0 / 60, vb = kmt ? kvb : now;
        if (!kmt && SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &ti)) && ti.qpcRefreshPeriod) {
            period = ti.qpcRefreshPeriod * 1000.0 / qfreq;
            vb = ti.qpcVBlank * 1000.0 / qfreq;
            vb += floor((now - vb) / period) * period; // snap forward if the info is stale
        }
        Plan pl;
        if (!plan_frame(vb - delay_ms(), period, &pl)) continue;
        if (pl.still && !own) { // real cursor is visible and nothing to trail
            drawn = NULL;
            if (visible) { ShowWindow(wnd, SW_HIDE); visible = 0; }
            continue;
        }
        if (pl.still && drawn == spr.id && lastp.x == pl.cur.x && lastp.y == pl.cur.y) continue;
        drawn = pl.still ? spr.id : NULL; lastp = pl.cur;

        int W, H;
        draw_frame(&spr, &pl, own, &W, &H);
        POINT dst = {pl.x0 - spr.hx, pl.y0 - spr.hy}, src = {0, 0};
        SIZE sz = {W, H};
        UpdateLayeredWindow(wnd, NULL, &dst, &sz, fbDC, &src, 0, &bf, ULW_ALPHA);
        if (!visible) { ShowWindow(wnd, SW_SHOWNOACTIVATE); visible = 1; }
    }
done:
    restore_scheme();
    return 0;
}
