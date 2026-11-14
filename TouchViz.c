// TouchViz：主程序，X11 触控可视化与长按增强

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <poll.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

// ---------- 1. 常量 ----------

#define MAX_TOUCH      16
#define TRAIL_MAX      40
#define TRAIL_MIN_DIST 2.0
#define DEV_CACHE      8
#define RECT_MAX       40

// ---------- 2. 配置 ----------

typedef struct {
    int    overlay;
    int    trail;
    int    rightclick;
    int    hide_cursor;
    int    scroll;
    int    long_press_ms;
    int    lp_arm_ms;
    double lp_move_tol;
    int    rc_on_press;
    int    rc_release_ms;
    int    frame_ms;
    int    fade_ms;
    int    trail_ms;
    int    stale_ms;
    int    raise_ms;
    int    cursor_restore_ms;
    int    cursor_recheck_ms;
} Config;

static Config cfg = {
    .overlay = 1, .trail = 1, .rightclick = 1, .hide_cursor = 1, .scroll = 0,
    .long_press_ms = 600, .lp_arm_ms = 200, .lp_move_tol = 18.0,
    .rc_on_press = 1, .rc_release_ms = 150,
    .frame_ms = 33, .fade_ms = 450, .trail_ms = 420, .stale_ms = 10000,
    .raise_ms = 2000, .cursor_restore_ms = 120, .cursor_recheck_ms = 500,
};

#define SCROLL_STEP 24.0

// ---------- 3. 基础工具 ----------

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }
static double clampd(double v, double lo, double hi) { return dmax(lo, dmin(hi, v)); }

// ---------- 4. 屏幕 / 显示 / 覆盖层 ----------

static Display *dpy;
static int      scr;
static int      scr_w, scr_h;
static int      xi_opcode;
static Window   overlay_win = 0;
static cairo_surface_t *ovl_surf;
static cairo_t         *ovl_cr;
static int      overlay_dirty = 0;
static int      overlay_lost = 0;
static int      overlay_painted = 0;

typedef struct { int x, y, w, h; } Rect;
static Rect prev_rects[RECT_MAX];
static int  prev_nrects = 0;
static int  rect_overflow = 0, prev_rect_overflow = 0;

static long raise_at = 0;
static long render_at = 0;
static long last_render_ms = 0;

static volatile sig_atomic_t got_signal = 0;

// ---------- 5. 触点 / 拖尾 ----------

typedef enum {
    PH_NONE = 0,
    PH_SETTLE,
    PH_CHARGE,
    PH_ARMED,
    PH_DEAD,
} Phase;

typedef struct {
    int    active;
    int    dev, id;
    int    have_pos;
    double x, y;
    double sx, sy;
    long   t_down, t_last, t_end;
    Phase  phase;
    long   arm_at;
    int    fired;
    double scroll_acc;
    double tx[TRAIL_MAX], ty[TRAIL_MAX];
    long   tt[TRAIL_MAX];
    int    tn;
} Touch;

static Touch touches[MAX_TOUCH];
static long  no_touch_since = 0;

static long pending_click_at = 0;
static int  pending_click_x = 0, pending_click_y = 0;

static int active_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_TOUCH; i++) if (touches[i].active) n++;
    return n;
}

static Touch *touch_find(int dev, int id) {
    for (int i = 0; i < MAX_TOUCH; i++)
        if (touches[i].active && touches[i].dev == dev && touches[i].id == id)
            return &touches[i];
    return NULL;
}

static Touch *touch_begin(int dev, int id) {
    Touch *t = touch_find(dev, id);
    if (t) return t;
    for (int i = 0; i < MAX_TOUCH; i++) {
        if (!touches[i].active) {
            memset(&touches[i], 0, sizeof(Touch));
            touches[i].active = 1;
            touches[i].dev = dev;
            touches[i].id = id;
            return &touches[i];
        }
    }
    return NULL;
}

static void touch_end(Touch *t, long now) {
    t->active = 0;
    t->t_end = now;
    t->phase = PH_NONE;
}

static void trail_push(Touch *t, double x, double y, long now) {
    if (t->tn > 0) {
        double dx = x - t->tx[t->tn - 1], dy = y - t->ty[t->tn - 1];
        if (dx * dx + dy * dy < TRAIL_MIN_DIST * TRAIL_MIN_DIST) return;
    }
    if (t->tn == TRAIL_MAX) {
        memmove(t->tx, t->tx + 1, sizeof(double) * (TRAIL_MAX - 1));
        memmove(t->ty, t->ty + 1, sizeof(double) * (TRAIL_MAX - 1));
        memmove(t->tt, t->tt + 1, sizeof(long)   * (TRAIL_MAX - 1));
        t->tn--;
    }
    t->tx[t->tn] = x; t->ty[t->tn] = y; t->tt[t->tn] = now;
    t->tn++;
}

static void trail_prune(Touch *t, long now) {
    int drop = 0;
    while (drop < t->tn && now - t->tt[drop] > cfg.trail_ms) drop++;
    if (drop > 0) {
        memmove(t->tx, t->tx + drop, sizeof(double) * (t->tn - drop));
        memmove(t->ty, t->ty + drop, sizeof(double) * (t->tn - drop));
        memmove(t->tt, t->tt + drop, sizeof(long)   * (t->tn - drop));
        t->tn -= drop;
    }
}

// ---------- 6. 设备信息 ----------

typedef struct {
    int    used;
    int    id;
    int    is_touch;
    int    vx, vy;
    double mnx, mxx, mny, mxy;
} DeviceInfo;

static DeviceInfo dev_cache[DEV_CACHE];
static int dev_cache_next = 0;
static int cur_dev = -1;

static const DeviceInfo *dev_lookup(int devid) {
    if (devid == cur_dev) {
        for (int i = 0; i < DEV_CACHE; i++)
            if (dev_cache[i].used && dev_cache[i].id == devid) return &dev_cache[i];
    }
    int ndev = 0;
    XIDeviceInfo *info = XIQueryDevice(dpy, devid, &ndev);
    if (!info) return NULL;

    DeviceInfo d;
    memset(&d, 0, sizeof(d));
    d.used = 1; d.id = devid; d.vx = 0; d.vy = 1;
    d.mnx = 0; d.mxx = 65535; d.mny = 0; d.mxy = 65535;
    int score_x = -1, score_y = -1, num0 = -1, num1 = -1;
    double mn0 = 0, mx0 = 65535, mn1 = 0, mx1 = 65535;

    for (int c = 0; c < info->num_classes; c++) {
        if (info->classes[c]->type == XITouchClass) { d.is_touch = 1; continue; }
        if (info->classes[c]->type != XIValuatorClass) continue;
        XIValuatorClassInfo *v = (XIValuatorClassInfo *)info->classes[c];
        if (v->number == 0) { num0 = 0; mn0 = v->min; mx0 = v->max; }
        if (v->number == 1) { num1 = 1; mn1 = v->min; mx1 = v->max; }
        if (!v->label || v->max <= v->min) continue;
        char *name = XGetAtomName(dpy, v->label);
        if (!name) continue;
        int L = strlen(name);
        char last = L ? name[L - 1] : 0;
        int score = strstr(name, "Position") ? 2 : 1;
        if (last == 'X' || last == 'x') {
            if (score > score_x) { score_x = score; d.vx = v->number; d.mnx = v->min; d.mxx = v->max; }
        } else if (last == 'Y' || last == 'y') {
            if (score > score_y) { score_y = score; d.vy = v->number; d.mny = v->min; d.mxy = v->max; }
        }
        XFree(name);
    }
    XIFreeDeviceInfo(info);
    if (score_x < 0 && num0 == 0 && mx0 > mn0) { d.vx = 0; d.mnx = mn0; d.mxx = mx0; }
    if (score_y < 0 && num1 == 1 && mx1 > mn1) { d.vy = 1; d.mny = mn1; d.mxy = mx1; }
    if (d.mxx <= d.mnx) { d.mnx = 0; d.mxx = 65535; }
    if (d.mxy <= d.mny) { d.mny = 0; d.mxy = 65535; }

    DeviceInfo *slot = &dev_cache[dev_cache_next++ % DEV_CACHE];
    *slot = d;
    cur_dev = devid;
    fprintf(stderr, "[TouchViz] 设备 %d: %s  X=v%d[%.0f..%.0f] Y=v%d[%.0f..%.0f]\n",
            devid, d.is_touch ? "触控设备" : "非触控(忽略)",
            d.vx, d.mnx, d.mxx, d.vy, d.mny, d.mxy);
    return slot;
}

static int xerr_handler(Display *d, XErrorEvent *e) {
    if (e->error_code == BadWindow || e->error_code == BadDrawable || e->error_code == BadCursor) {
        if (cfg.overlay && overlay_win && (Window)e->resourceid == overlay_win) overlay_lost = 1;
        return 0;
    }
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    fprintf(stderr, "[TouchViz] X 错误: %s (request %d.%d)\n", buf, e->request_code, e->minor_code);
    return 0;
}

// ---------- 7. 鼠标指针隐藏 ----------

static Cursor blank_cursor = None;
static Window blanked_win = None;
static int    cursor_hidden = 0;
static int    last_hide_x = -1000, last_hide_y = -1000;
static long   last_hide_ms = 0;
static long   cursor_recheck_at = 0;

static void make_blank_cursor(void) {
    Window root = DefaultRootWindow(dpy);
    Pixmap pm = XCreatePixmap(dpy, root, 1, 1, 1);
    XGCValues gcv; memset(&gcv, 0, sizeof(gcv));
    GC gc = XCreateGC(dpy, pm, 0, &gcv);
    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, pm, gc, 0, 0, 1, 1);
    XColor zero; memset(&zero, 0, sizeof(zero));
    blank_cursor = XCreatePixmapCursor(dpy, pm, pm, &zero, &zero, 0, 0);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, pm);
    if (blank_cursor == None) {
        fprintf(stderr, "[TouchViz] 无法创建透明光标，禁用隐藏指针\n");
        cfg.hide_cursor = 0;
    }
}

static Window window_under_pointer(void) {
    Window root = DefaultRootWindow(dpy), w = root, child = None;
    int rx, ry, wx, wy; unsigned int mask;
    for (int d = 0; d < 6; d++) {
        child = None;
        XQueryPointer(dpy, w, &root, &child, &rx, &ry, &wx, &wy, &mask);
        if (child == None) break;
        w = child;
    }
    return w;
}

static void cursor_hide(int x, int y) {
    Window root = DefaultRootWindow(dpy);
    if (!cursor_hidden) {
        XFixesHideCursor(dpy, root);
        cursor_hidden = 1;
        XFlush(dpy);
    }
    long now = now_ms();
    int moved = (x < 0) || (abs(x - last_hide_x) + abs(y - last_hide_y) > 4);
    if (!moved && now - last_hide_ms < cfg.cursor_recheck_ms) return;
    last_hide_x = x; last_hide_y = y; last_hide_ms = now;
    Window w = window_under_pointer();
    if (w != blanked_win) {
        if (blanked_win != None && blanked_win != root) XDefineCursor(dpy, blanked_win, None);
        blanked_win = w;
        if (w != None && w != root) XDefineCursor(dpy, w, blank_cursor);
        XFlush(dpy);
    }
}

static void cursor_show(void) {
    Window root = DefaultRootWindow(dpy);
    if (blanked_win != None && blanked_win != root) XDefineCursor(dpy, blanked_win, None);
    blanked_win = None;
    if (cursor_hidden) {
        XFixesShowCursor(dpy, root);
        cursor_hidden = 0;
    }
    XFlush(dpy);
}

// ---------- 8. 输入动作（XTEST） ----------

static void synth_right_click(int x, int y) {
    fprintf(stderr, "[TouchViz] 长按 -> 右键 (%d,%d)\n", x, y);
    XTestFakeMotionEvent(dpy, scr, x, y, CurrentTime);
    XTestFakeButtonEvent(dpy, 3, True, CurrentTime);
    XTestFakeButtonEvent(dpy, 3, False, CurrentTime);
    XFlush(dpy);
}

static void synth_wheel(int button) {
    XTestFakeButtonEvent(dpy, button, True, CurrentTime);
    XTestFakeButtonEvent(dpy, button, False, CurrentTime);
    XFlush(dpy);
}

// ---------- 9. 长按状态机 ----------

static int lp_candidate(Touch *t, int n_active) {
    if (!cfg.rightclick || cfg.long_press_ms <= 0) return 0;
    if (!t->active || !t->have_pos || t->t_down <= 0) return 0;
    if (t->phase == PH_DEAD || t->phase == PH_ARMED || t->fired) return 0;
    if (n_active != 1) return 0;
    double dx = t->x - t->sx, dy = t->y - t->sy;
    return dx * dx + dy * dy <= cfg.lp_move_tol * cfg.lp_move_tol;
}

static int lp_progress(Touch *t, long now, double *progress) {
    *progress = 0.0;
    if (t->phase == PH_ARMED) { *progress = 1.0; return 1; }
    if (t->phase != PH_SETTLE && t->phase != PH_CHARGE) return 0;
    if (t->t_down <= 0) return 0;
    if (now < t->arm_at) return 0;
    long span = cfg.long_press_ms - cfg.lp_arm_ms;
    if (span <= 0) span = 1;
    *progress = clampd((double)(now - t->arm_at) / (double)span, 0.0, 1.0);
    return 1;
}

static int lp_tick(Touch *t, long now, int n_active) {
    if (!t->active || !cfg.rightclick || cfg.long_press_ms <= 0) return 0;

    if (n_active >= 2 && t->phase != PH_DEAD) {
        t->phase = PH_DEAD;
        overlay_dirty = 1;
        return 1;
    }
    if ((t->phase == PH_SETTLE || t->phase == PH_CHARGE) && !lp_candidate(t, n_active)) {
        t->phase = PH_DEAD;
        overlay_dirty = 1;
        return 1;
    }
    if (!t->have_pos) return 0;

    if (t->phase == PH_SETTLE && now >= t->arm_at) {
        t->phase = PH_CHARGE;
        overlay_dirty = 1;
    }
    if ((t->phase == PH_SETTLE || t->phase == PH_CHARGE) &&
        now - t->t_down >= cfg.long_press_ms) {
        t->fired = 1;
        if (cfg.rc_on_press) {
            t->phase = PH_DEAD;
            synth_right_click((int)t->x, (int)t->y);
        } else {
            t->phase = PH_ARMED;
        }
        overlay_dirty = 1;
        return 1;
    }
    return 0;
}

// ---------- 10. 绘制 ----------

static const double PALETTE[][3] = {
    {0.20, 0.75, 1.00}, {1.00, 0.45, 0.30}, {0.45, 0.95, 0.45}, {1.00, 0.85, 0.25},
    {0.80, 0.55, 1.00}, {0.35, 1.00, 0.90}, {1.00, 0.55, 0.80}, {0.95, 0.95, 0.95},
};

#define CIRCLE_R_BASE 24.0
#define CIRCLE_R_MAX  (CIRCLE_R_BASE + 12.0 + 4.0)
#define LP_RING_R     32.0
#define LP_RING_W     4.0

static int touch_visible(const Touch *t, long now) {
    if (t->active) return 1;
    return t->t_end && now - t->t_end <= cfg.fade_ms;
}

static void rect_add(Rect *rs, int *n, int x, int y, int w, int h) {
    if (*n >= RECT_MAX) { rect_overflow = 1; return; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w <= 0 || h <= 0) return;
    rs[*n].x = x; rs[*n].y = y; rs[*n].w = w; rs[*n].h = h;
    (*n)++;
}

static void clear_rect(Rect r) {
    cairo_save(ovl_cr);
    cairo_rectangle(ovl_cr, r.x, r.y, r.w, r.h);
    cairo_clip(ovl_cr);
    cairo_set_operator(ovl_cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(ovl_cr, 0, 0, 0, 0);
    cairo_paint(ovl_cr);
    cairo_restore(ovl_cr);
}

static void draw_touch(cairo_t *cr, const Touch *t, long now, double alpha, const double *col) {
    const double base_r = CIRCLE_R_BASE;

    if (cfg.trail && t->tn >= 2) {
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        for (int k = 1; k < t->tn; k++) {
            double f   = (double)k / t->tn;
            double age = (double)(now - t->tt[k]) / cfg.trail_ms;
            cairo_set_source_rgba(cr, col[0], col[1], col[2],
                                  alpha * 0.75 * f * (1.0 - clampd(age, 0.0, 1.0)));
            cairo_set_line_width(cr, 2.0 + 10.0 * f);
            cairo_move_to(cr, t->tx[k - 1], t->ty[k - 1]);
            cairo_line_to(cr, t->tx[k], t->ty[k]);
            cairo_stroke(cr);
        }
    }

    double r = base_r + (t->active ? 0.0 : (1.0 - alpha) * 12.0);
    cairo_set_line_width(cr, 3.0);
    cairo_set_source_rgba(cr, col[0], col[1], col[2], 0.95 * alpha);
    cairo_arc(cr, t->x, t->y, r, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, col[0], col[1], col[2], 0.20 * alpha);
    cairo_arc(cr, t->x, t->y, r - 2, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, col[0], col[1], col[2], 0.95 * alpha);
    cairo_arc(cr, t->x, t->y, 4.5, 0, 2 * M_PI);
    cairo_fill(cr);

    double p = 0;
    if (lp_progress((Touch *)t, now, &p)) {
        cairo_set_line_width(cr, LP_RING_W);
        cairo_set_source_rgba(cr, col[0], col[1], col[2], 0.20 * alpha);
        cairo_arc(cr, t->x, t->y, LP_RING_R, 0, 2 * M_PI);
        cairo_stroke(cr);
        if (p > 0.002) {
            double a0 = -M_PI / 2;
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_source_rgba(cr, 0.97, 0.97, 0.97, 0.95 * alpha);
            cairo_arc(cr, t->x, t->y, LP_RING_R, a0, a0 + 2 * M_PI * p);
            cairo_stroke(cr);
        }
        if (t->phase == PH_ARMED) {
            cairo_set_line_width(cr, 2.0);
            cairo_set_source_rgba(cr, col[0], col[1], col[2], 0.95 * alpha);
            cairo_arc(cr, t->x, t->y, LP_RING_R + LP_RING_W + 1.5, 0, 2 * M_PI);
            cairo_stroke(cr);
        }
    }
}

static void overlay_render(void) {
    if (!ovl_cr || !ovl_surf) return;
    long now = now_ms();
    for (int i = 0; i < MAX_TOUCH; i++) if (touches[i].active) trail_prune(&touches[i], now);

    Rect nr[RECT_MAX];
    int nn = 0, drew = 0;
    for (int i = 0; i < MAX_TOUCH; i++) {
        Touch *t = &touches[i];
        if (!touch_visible(t, now)) {
            if (t->t_end && now - t->t_end > cfg.fade_ms) { t->t_end = 0; t->tn = 0; }
            continue;
        }
        double rr = CIRCLE_R_MAX;
        double p = 0;
        if (lp_progress(t, now, &p)) rr = dmax(rr, LP_RING_R + LP_RING_W + 4.0);
        rect_add(nr, &nn, (int)(t->x - rr), (int)(t->y - rr), (int)(2 * rr), (int)(2 * rr));
        if (cfg.trail && t->tn >= 2) {
            double x0 = t->tx[0], x1 = t->tx[0], y0 = t->ty[0], y1 = t->ty[0];
            for (int k = 1; k < t->tn; k++) {
                x0 = dmin(x0, t->tx[k]); x1 = dmax(x1, t->tx[k]);
                y0 = dmin(y0, t->ty[k]); y1 = dmax(y1, t->ty[k]);
            }
            rect_add(nr, &nn, (int)(x0 - 12), (int)(y0 - 12),
                     (int)(x1 - x0 + 24), (int)(y1 - y0 + 24));
        }
        drew = 1;
    }

    if (rect_overflow || prev_rect_overflow) {
        Rect full = {0, 0, scr_w, scr_h};
        clear_rect(full);
    } else {
        for (int k = 0; k < prev_nrects; k++) clear_rect(prev_rects[k]);
        for (int k = 0; k < nn; k++) clear_rect(nr[k]);
    }
    prev_rect_overflow = rect_overflow;
    rect_overflow = 0;

    if (drew) {
        cairo_save(ovl_cr);
        for (int k = 0; k < nn; k++)
            cairo_rectangle(ovl_cr, nr[k].x, nr[k].y, nr[k].w, nr[k].h);
        cairo_clip(ovl_cr);
        for (int i = 0; i < MAX_TOUCH; i++) {
            Touch *t = &touches[i];
            if (!touch_visible(t, now)) continue;
            double alpha = t->active ? 1.0
                                     : 1.0 - (double)(now - t->t_end) / cfg.fade_ms;
            draw_touch(ovl_cr, t, now, clampd(alpha, 0.0, 1.0),
                       PALETTE[i % (int)(sizeof(PALETTE) / sizeof(PALETTE[0]))]);
        }
        cairo_restore(ovl_cr);
    }

    memcpy(prev_rects, nr, sizeof(Rect) * nn);
    prev_nrects = nn;
    overlay_painted = drew;
    cairo_surface_flush(ovl_surf);
    XFlush(dpy);
}

static void overlay_make_cairo(void) {
    if (!overlay_win) return;
    if (ovl_cr)   { cairo_destroy(ovl_cr); ovl_cr = NULL; }
    if (ovl_surf) { cairo_surface_destroy(ovl_surf); ovl_surf = NULL; }
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, scr, 32, TrueColor, &vinfo)) {
        fprintf(stderr, "[TouchViz] 找不到 32 位 ARGB visual，无法显示\n");
        return;
    }
    ovl_surf = cairo_xlib_surface_create(dpy, overlay_win, vinfo.visual, scr_w, scr_h);
    ovl_cr = cairo_create(ovl_surf);
    cairo_set_operator(ovl_cr, CAIRO_OPERATOR_SOURCE);
}

static void overlay_create(void) {
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, scr, 32, TrueColor, &vinfo)) return;
    Window root = RootWindow(dpy, scr);
    XSetWindowAttributes at;
    memset(&at, 0, sizeof(at));
    at.override_redirect = True;
    at.colormap  = XCreateColormap(dpy, root, vinfo.visual, AllocNone);
    at.background_pixel = 0;
    at.border_pixel = 0;
    overlay_win = XCreateWindow(dpy, root, 0, 0, scr_w, scr_h, 0, vinfo.depth,
                                InputOutput, vinfo.visual,
                                CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel, &at);
    XShapeCombineRectangles(dpy, overlay_win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
    XStoreName(dpy, overlay_win, "TouchViz-overlay");
    XMapRaised(dpy, overlay_win);
    XFlush(dpy);
    overlay_make_cairo();
    overlay_dirty = 1;
}

static void overlay_destroy(void) {
    if (ovl_cr)   { cairo_destroy(ovl_cr); ovl_cr = NULL; }
    if (ovl_surf) { cairo_surface_destroy(ovl_surf); ovl_surf = NULL; }
    if (overlay_win) { XDestroyWindow(dpy, overlay_win); overlay_win = 0; }
    prev_nrects = 0;
    overlay_painted = 0;
}

static void overlay_check_size(void) {
    int nw = DisplayWidth(dpy, scr), nh = DisplayHeight(dpy, scr);
    if (nw == scr_w && nh == scr_h) return;
    scr_w = nw; scr_h = nh;
    if (overlay_win) {
        XMoveResizeWindow(dpy, overlay_win, 0, 0, scr_w, scr_h);
        overlay_make_cairo();
    }
    prev_nrects = 0; overlay_painted = 0; overlay_dirty = 1;
    XFlush(dpy);
    fprintf(stderr, "[TouchViz] 屏幕尺寸变化 -> %dx%d\n", scr_w, scr_h);
}

// ---------- 11. 渲染调度 ----------

static int render_needed(long now) {
    if (overlay_dirty) return 1;
    int any = 0;
    for (int i = 0; i < MAX_TOUCH; i++) {
        Touch *t = &touches[i];
        if (t->active) {
            any = 1;
            if (cfg.trail && t->tn >= 2) return 1;
            double p;
            if (lp_progress(t, now, &p) && p < 1.0) return 1;
        } else if (touch_visible(t, now)) {
            return 1;
        }
    }
    return (!any && overlay_painted);
}

static int next_wait_ms(long now) {
    long best = -1;
#define NEED(deadline) do { long d_ = (deadline); \
        if (d_ > now) { long w_ = d_ - now; if (best < 0 || w_ < best) best = w_; } } while (0)

    int act = 0;
    for (int i = 0; i < MAX_TOUCH; i++) {
        Touch *t = &touches[i];
        if (t->active) {
            act = 1;
            if (cfg.overlay) {
                if (cfg.trail && t->tn >= 2) NEED(now + cfg.frame_ms);
                double p;
                if (lp_progress(t, now, &p) && p < 1.0) NEED(now + cfg.frame_ms);
                if (t->phase == PH_SETTLE) NEED(t->arm_at);
                NEED(raise_at);
            }
            if (cfg.rightclick && cfg.long_press_ms > 0 && !t->fired &&
                t->phase != PH_DEAD && t->phase != PH_ARMED)
                NEED(t->t_down + cfg.long_press_ms);
            if (t->t_last) NEED(t->t_last + cfg.stale_ms + 1);
            if (cursor_hidden) NEED(cursor_recheck_at);
        } else if (touch_visible(t, now)) {
            NEED(now + cfg.frame_ms);
        }
    }
    if (!act && cursor_hidden) NEED(no_touch_since + cfg.cursor_restore_ms + 1);
    if (pending_click_at) {
        if (pending_click_at <= now) return 0;
        NEED(pending_click_at);
    }
    if (render_at) {
        if (render_at <= now) return 0;
        NEED(render_at);
    }
    if (best < 0) return -1;
    return (int)best;
#undef NEED
}

// ---------- 12. 事件处理 ----------

static void handle_motion_from_touch(Touch *t, double nx, double ny, int is_begin, long now, int n_active) {
    if (is_begin) {
        t->have_pos = 1;
        t->x = t->sx = nx;
        t->y = t->sy = ny;
        t->t_down = now;
        t->arm_at = now + cfg.lp_arm_ms;
        t->phase = (cfg.rightclick && cfg.long_press_ms > 0) ? PH_SETTLE : PH_DEAD;
        t->tn = 0;
        if (cfg.trail) trail_push(t, nx, ny, now);
        if (cfg.hide_cursor) cursor_hide((int)nx, (int)ny);
    } else {
        t->have_pos = 1;
        if (cfg.trail) trail_push(t, nx, ny, now);
        if (cfg.scroll && n_active == 2) {
            t->scroll_acc += ny - t->y;
            while (t->scroll_acc <= -SCROLL_STEP) { synth_wheel(5); t->scroll_acc += SCROLL_STEP; }
            while (t->scroll_acc >=  SCROLL_STEP) { synth_wheel(4); t->scroll_acc -= SCROLL_STEP; }
        }
        t->x = nx;
        t->y = ny;
    }
}

static void handle_event(XEvent *ev) {
    if (ev->type == Expose) { overlay_dirty = 1; return; }
    if (ev->type != GenericEvent || ev->xcookie.extension != xi_opcode) return;
    if (!XGetEventData(dpy, &ev->xcookie)) return;

    int et = ev->xcookie.evtype;
    if (et != XI_RawTouchBegin && et != XI_RawTouchUpdate && et != XI_RawTouchEnd) {
        XFreeEventData(dpy, &ev->xcookie);
        return;
    }

    // 本轮清理：XGetEventData 返回 True 即保证 cookie.data 非 NULL，故不再判 raw。
    XIRawEvent *raw = (XIRawEvent *)ev->xcookie.data;
    const DeviceInfo *dev = dev_lookup(raw->sourceid);
    if (!dev || !dev->is_touch) {
        XFreeEventData(dpy, &ev->xcookie);
        return;
    }

    long now = now_ms();
    int  id  = raw->detail;
    Touch *t = (et == XI_RawTouchEnd) ? touch_find(raw->sourceid, id)
                                      : touch_begin(raw->sourceid, id);
    if (t) {
        // valuators.mask / .values 在 num_valuators==0 时确实可能为 NULL，保留判空。
        int have_xy = raw->valuators.mask && raw->valuators.values &&
                      XIMaskIsSet(raw->valuators.mask, dev->vx) &&
                      XIMaskIsSet(raw->valuators.mask, dev->vy);
        if (et == XI_RawTouchBegin) overlay_check_size();
        if (have_xy) {
            double nx = (raw->valuators.values[dev->vx] - dev->mnx) / (dev->mxx - dev->mnx) * (scr_w - 1);
            double ny = (raw->valuators.values[dev->vy] - dev->mny) / (dev->mxy - dev->mny) * (scr_h - 1);
            nx = clampd(nx, 0, scr_w - 1);
            ny = clampd(ny, 0, scr_h - 1);
            t->t_last = now;
            int n_active = active_count();
            handle_motion_from_touch(t, nx, ny, et == XI_RawTouchBegin, now, n_active);
        }
        if (et == XI_RawTouchEnd) {
            if (t->phase == PH_ARMED && cfg.rightclick) {
                pending_click_x = (int)t->x;
                pending_click_y = (int)t->y;
                pending_click_at = now + cfg.rc_release_ms;
            }
            touch_end(t, now);
        }
        overlay_dirty = 1;
    }
    XFreeEventData(dpy, &ev->xcookie);
}

// ---------- 13. 主循环 ----------

static void on_signal(int sig) {
    (void)sig;
    got_signal = 1;
}

static void tick_touches(long now) {
    for (int i = 0; i < MAX_TOUCH; i++) {
        Touch *t = &touches[i];
        if (!t->active) continue;
        if (t->t_last && now - t->t_last > cfg.stale_ms) {
            touch_end(t, now);
            overlay_dirty = 1;
        }
    }
    int n_active = active_count();
    for (int i = 0; i < MAX_TOUCH; i++)
        if (touches[i].active) lp_tick(&touches[i], now, n_active);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--no-overlay"))     cfg.overlay = 0;
        else if (!strcmp(argv[i], "--no-trail"))       cfg.trail = 0;
        else if (!strcmp(argv[i], "--no-rightclick"))  cfg.rightclick = 0;
        else if (!strcmp(argv[i], "--no-hide-cursor")) cfg.hide_cursor = 0;
        else if (!strcmp(argv[i], "--scroll"))         cfg.scroll = 1;
        else if (!strcmp(argv[i], "--long-press") && i + 1 < argc) cfg.long_press_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lp-arm")     && i + 1 < argc) cfg.lp_arm_ms     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lp-move")    && i + 1 < argc) cfg.lp_move_tol   = atof(argv[++i]);
        else if (!strcmp(argv[i], "--idle-ms")    && i + 1 < argc) cfg.frame_ms      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rightclick-at") && i + 1 < argc)
            cfg.rc_on_press = !strcmp(argv[++i], "press");
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("用法: %s [选项]\n"
                   "  触控点显示与增强（X11）：触点圆点、滑动拖尾、长按进度环、长按出右键、触控时隐藏指针。\n"
                   "选项:\n"
                   "  --no-overlay         不显示触点(只做长按/隐藏指针)\n"
                   "  --no-trail           不画滑动拖尾\n"
                   "  --no-rightclick      关闭长按->右键\n"
                   "  --no-hide-cursor     触控时不隐藏鼠标指针\n"
                   "  --scroll             双指同向滑动 -> 滚轮(默认关)\n"
                   "  --long-press MS      长按总时长(从按下算)，默认 %d\n"
                   "  --lp-arm MS          稳定期(按下后多久才开始算进度)，默认 %d\n"
                   "  --lp-move PX         长按允许漂移像素，默认 %.0f\n"
                   "  --rightclick-at V    press(默认,当场发) | release(抬手后发)\n"
                   "  --idle-ms MS         动画帧间隔，默认 %d\n",
                   argv[0], cfg.long_press_ms, cfg.lp_arm_ms, cfg.lp_move_tol, cfg.frame_ms);
            return 0;
        }
        else {
            fprintf(stderr, "未知参数: %s（--help 看用法）\n", argv[i]);
            return 2;
        }
    }
    if (cfg.long_press_ms < 0) cfg.long_press_ms = 0;
    if (cfg.lp_arm_ms < 0) cfg.lp_arm_ms = 0;
    if (cfg.long_press_ms > 0 && cfg.lp_arm_ms >= cfg.long_press_ms)
        cfg.lp_arm_ms = cfg.long_press_ms / 3;
    if (cfg.lp_move_tol < 1) cfg.lp_move_tol = 1;
    if (cfg.frame_ms < 4) cfg.frame_ms = 4;

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "无法连接 X server（DISPLAY 对没?）\n"); return 1; }
    XSetErrorHandler(xerr_handler);
    scr   = DefaultScreen(dpy);
    scr_w = DisplayWidth(dpy, scr);
    scr_h = DisplayHeight(dpy, scr);

    int xi_ev, xi_err, major = 2, minor = 2;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &xi_ev, &xi_err) ||
        XIQueryVersion(dpy, &major, &minor) != Success) {
        fprintf(stderr, "需要 XInput2 >= 2.2\n");
        return 1;
    }

    if (cfg.hide_cursor) make_blank_cursor();
    if (cfg.overlay)     overlay_create();

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGHUP,  on_signal);

    unsigned char mask[(XI_LASTEVENT + 7) / 8];
    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_RawTouchBegin);
    XISetMask(mask, XI_RawTouchUpdate);
    XISetMask(mask, XI_RawTouchEnd);
    XIEventMask em;
    em.deviceid = XIAllMasterDevices;
    em.mask_len = sizeof(mask);
    em.mask     = mask;
    XISelectEvents(dpy, DefaultRootWindow(dpy), &em, 1);
    XFlush(dpy);

    fprintf(stderr, "[TouchViz] 就绪 %dx%d  显示=%d 拖尾=%d 长按=%dms(稳定期 %dms + 进度 %dms, %s) "
                    "滚动=%d 隐藏指针=%d\n",
            scr_w, scr_h, cfg.overlay, cfg.trail, cfg.long_press_ms, cfg.lp_arm_ms,
            cfg.long_press_ms - cfg.lp_arm_ms, cfg.rc_on_press ? "当场发右键" : "抬手后发",
            cfg.scroll, cfg.hide_cursor);

    for (;;) {
        if (got_signal) {
            cursor_show();
            fprintf(stderr, "[TouchViz] 退出（已恢复鼠标指针）\n");
            XCloseDisplay(dpy);
            return 0;
        }

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            handle_event(&ev);
        }

        if (overlay_lost) {
            overlay_lost = 0;
            if (cfg.overlay) {
                fprintf(stderr, "[TouchViz] 覆盖层丢失，重建\n");
                overlay_destroy();
                overlay_create();
            }
        }

        long now = now_ms();
        tick_touches(now);

        if (pending_click_at && now >= pending_click_at) {
            pending_click_at = 0;
            synth_right_click(pending_click_x, pending_click_y);
        }

        if (active_count() == 0) {
            if (no_touch_since == 0) no_touch_since = now;
            if (cursor_hidden && now - no_touch_since >= cfg.cursor_restore_ms) cursor_show();
        } else {
            no_touch_since = 0;
            if (cfg.overlay && overlay_win && now >= raise_at) {
                raise_at = now + cfg.raise_ms;
                XRaiseWindow(dpy, overlay_win);
                XFlush(dpy);
            }
            if (cursor_hidden && now >= cursor_recheck_at) {
                cursor_recheck_at = now + cfg.cursor_recheck_ms;
                int px = 0, py = 0;
                for (int i = 0; i < MAX_TOUCH; i++)
                    if (touches[i].active && touches[i].have_pos) {
                        px = (int)touches[i].x; py = (int)touches[i].y;
                    }
                cursor_hide(px, py);
            }
        }

        if (cfg.overlay && render_needed(now)) {
            if (now - last_render_ms >= cfg.frame_ms / 2) {
                overlay_dirty = 0;
                render_at = 0;
                last_render_ms = now;
                overlay_render();
            } else {
                render_at = last_render_ms + cfg.frame_ms / 2;
            }
        }

        int wait = next_wait_ms(now_ms());
        struct pollfd pfd;
        pfd.fd = ConnectionNumber(dpy);
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, wait);
    }
}