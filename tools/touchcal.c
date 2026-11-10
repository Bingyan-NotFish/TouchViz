/* ============================================================================
 * touchcal —— 触控偏移标定 / 验证（全屏，需要人配合点屏幕）
 *
 * 它做的事：
 *   1. 全屏显示黑底 + 若干十字目标（默认 5 个：正中 + 四角）；
 *   2. 你按提示用手指（或用鼠标当手指）点每个十字的正中心；
 *   3. 每次点数它记录三件事：
 *        • 目标位置 target          —— 你看到并瞄准的位置
 *        • 指针落点 pointer         —— 系统认为你点的位置（触控会移动指针）
 *        • 设备量程 + 变换矩阵      —— 现场从 X 里读
 *      屏幕上会用红线画出「目标 → 落点」的偏差，肉眼就能看到偏多少；
 *   4. 全部点完给出：每点偏差、平均/最大偏差、逐轴线性拟合，
 *      算出能把偏差抵掉的 Coordinate Transformation Matrix；
 *   5. --apply 直接写回设备（本会话生效），并可多轮验证改完的效果；
 *   6. 结果同时写到报告文件（默认 /tmp/touchcal-report.txt），可以贴给别人看。
 *
 * 用法：
 *   ./touchcal                      # 5 个点，只测量（不动系统设置）
 *   ./touchcal --points 9           # 3x3 九个点，更稳
 *   ./touchcal --apply              # 测完立刻应用，并自动再测一轮验证
 *   ./touchcal --device 21          # 指定设备（多块触控屏时）
 *   ./touchcal --rounds 1           # 只测一轮
 *   ./touchcal --report /tmp/x.txt  # 报告写到别处
 *
 * 按键：Esc / q 中止；r 重新开始本轮；space 跳过当前点。
 *
 * 依赖：libX11 + libXi（和 TouchViz 同一套），不需要 python/ImageMagick/root。
 * 注意：真触控屏请先把 TouchViz 停掉（TouchViz-ctl stop），它的覆盖层会盖住目标。
 * ==========================================================================*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XInput.h>

#define MAXPT 16

typedef struct {
    int    id;
    char   name[128];
    char   node[128];
    int    is_touch;
    int    vx, vy;
    double mnx, mxx, mny, mxy;
    double ctm[9];
    int    has_ctm;
} Dev;

typedef struct { double tx, ty; double px, py; int done; int skipped; int istouch; } Point;

static Display *dpy;
static int      scr, xi_opcode;
static int      W, H;          /* 根窗口尺寸 */
static Window   win;
static GC       gc;
static XFontStruct *font;
static int      have_touch = 0;
static Dev      dev;
static int      dev_ok = 0;

static int  npoints = 5;
static int  rounds = 1;
static int  apply  = 0;
static int  take_clicks = 1;   /* 鼠标左键也可以当“点”（--no-click 关掉） */
static int  win_timeout = 0;
static const char *report_path = "/tmp/touchcal-report.txt";
static FILE *rep;

static Point pts[MAXPT];
static int   cur = 0;        /* 当前目标下标 */
static int   round_no = 1;
static long  last_tap_ms = 0;
static volatile sig_atomic_t stop_flag = 0;
static int click_hinted = 0;

static void on_sig(int s) { (void)s; stop_flag = 1; }

static long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void say(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap); va_end(ap);
    printf("\n"); fflush(stdout);
    if (rep) { va_start(ap, fmt); vfprintf(rep, fmt, ap); va_end(ap); fputc('\n', rep); fflush(rep); }
}

/* ---------------------------------------------------------------- 设备 */

static int get_dev_prop(int devid, const char *name, unsigned char **data,
                        int *nitems, int *format) {
    Atom prop = XInternAtom(dpy, name, True), type = None;
    if (prop == None) return 0;
    XDevice *d = XOpenDevice(dpy, devid);
    if (!d) return 0;
    unsigned long n = 0, after = 0;
    int ok = (XGetDeviceProperty(dpy, d, prop, 0, 64, False, AnyPropertyType,
                                 &type, format, &n, &after, data) == Success);
    XCloseDevice(dpy, d);
    if (!ok || !*data || n == 0) { if (*data) { XFree(*data); *data = NULL; } return 0; }
    *nitems = (int)n;
    return 1;
}

static int read_ctm(int devid, double m[9]) {
    const double ident[9] = {1,0,0, 0,1,0, 0,0,1};
    memcpy(m, ident, sizeof ident);
    unsigned char *data = NULL; int n = 0, fmt = 0;
    if (!get_dev_prop(devid, "Coordinate Transformation Matrix", &data, &n, &fmt)) return 0;
    if (fmt == 32 && n >= 9) {
        const unsigned long *u = (const unsigned long *)data;
        for (int i = 0; i < 9; i++) {
            uint32_t bits = (uint32_t)u[i]; float f; memcpy(&f, &bits, 4);
            m[i] = f;
        }
    }
    XFree(data);
    return 1;
}

static int write_ctm(int devid, const double m[9]) {
    long v[9];
    for (int i = 0; i < 9; i++) {
        float f = (float)m[i]; uint32_t bits; memcpy(&bits, &f, 4); v[i] = (long)bits;
    }
    XDevice *d = XOpenDevice(dpy, devid);
    if (!d) return 0;
    XChangeDeviceProperty(dpy, d, XInternAtom(dpy, "Coordinate Transformation Matrix", False),
                          XInternAtom(dpy, "FLOAT", False), 32, PropModeReplace,
                          (unsigned char *)v, 9);
    XSync(dpy, False);
    XCloseDevice(dpy, d);
    return 1;
}

static void load_dev(Dev *d, XIDeviceInfo *info) {
    memset(d, 0, sizeof *d);
    d->id = info->deviceid;
    snprintf(d->name, sizeof d->name, "%s", info->name ? info->name : "?");
    d->vx = 0; d->vy = 1;
    d->mnx = 0; d->mxx = 65535; d->mny = 0; d->mxy = 65535;
    int sx = -1, sy = -1;
    for (int c = 0; c < info->num_classes; c++) {
        if (info->classes[c]->type == XITouchClass) { d->is_touch = 1; continue; }
        if (info->classes[c]->type != XIValuatorClass) continue;
        XIValuatorClassInfo *v = (XIValuatorClassInfo *)info->classes[c];
        if (!v->label || v->max <= v->min) continue;
        char *nm = XGetAtomName(dpy, v->label);
        if (!nm) continue;
        int L = (int)strlen(nm), last = L ? nm[L - 1] : 0;
        int sc = strstr(nm, "Position") ? 2 : 1;
        if (last == 'X' || last == 'x') {
            if (sc > sx) { sx = sc; d->vx = v->number; d->mnx = v->min; d->mxx = v->max; }
        } else if (last == 'Y' || last == 'y') {
            if (sc > sy) { sy = sc; d->vy = v->number; d->mny = v->min; d->mxy = v->max; }
        }
        XFree(nm);
    }
    d->has_ctm = read_ctm(d->id, d->ctm);
    unsigned char *data = NULL; int n = 0, fmt = 0;
    if (get_dev_prop(d->id, "Device Node", &data, &n, &fmt)) {
        snprintf(d->node, sizeof d->node, "%.*s", n, (char *)data);
        XFree(data);
    }
}

/* 挑一块真触控设备：优先名字里没有 TouchInject 的（虚拟测试设备排最后） */
static int pick_device(int want_id) {
    int n = 0, best = -1, best_score = -1, ntouch = 0;
    XIDeviceInfo *info = XIQueryDevice(dpy, XIAllDevices, &n);
    if (!info) return 0;
    for (int i = 0; i < n; i++) {
        if (!info[i].enabled) continue;
        if (info[i].use == XIMasterPointer || info[i].use == XIMasterKeyboard) continue;
        Dev d; load_dev(&d, &info[i]);
        if (!d.is_touch) continue;
        ntouch++;
        if (want_id > 0) {
            if (d.id == want_id) { dev = d; best = d.id; break; }
            continue;
        }
        int score = strstr(d.name, "TouchInject") ? 0 : 10;
        if (score > best_score) { best_score = score; best = d.id; dev = d; }
    }
    XIFreeDeviceInfo(info);
    if (want_id > 0 && best < 0) return 0;
    if (want_id <= 0 && best < 0) return 0;
    have_touch = (ntouch > 0);
    dev_ok = (best >= 0);
    return 1;
}

/* ---------------------------------------------------------------- 绘制 */

static void set_color(GC g, unsigned long px) { XSetForeground(dpy, g, px); }

static unsigned long alloc_color(const char *name, unsigned long fallback) {
    XColor c, exact;
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, scr), name, &c, &exact)) return c.pixel;
    return fallback;
}

static unsigned long COL_FG, COL_CUR, COL_DONE, COL_ERR;

static void draw_target(double x, double y, int idx, int is_cur) {
    int R = is_cur ? 26 : 14;
    set_color(gc, is_cur ? COL_CUR : COL_DONE);
    XDrawLine(dpy, win, gc, (int)x - R, (int)y, (int)x + R, (int)y);
    XDrawLine(dpy, win, gc, (int)x, (int)y - R, (int)x, (int)y + R);
    XDrawArc(dpy, win, gc, (int)x - R / 2, (int)y - R / 2, R, R, 0, 360 * 64);
    if (font) {
        char buf[16]; snprintf(buf, sizeof buf, "%d", idx + 1);
        XDrawString(dpy, win, gc, (int)x + R + 4, (int)y - 4, buf, (int)strlen(buf));
    }
}

static void draw(void) {
    XClearWindow(dpy, win);
    char head[256];
    int done = 0;
    for (int i = 0; i < npoints; i++) if (pts[i].done) done++;

    if (font) {
        set_color(gc, COL_FG);
        snprintf(head, sizeof head,
                 "touchcal  round %d/%d   tap the CENTER of the crosshair   (%d/%d)   Esc=abort  r=redo",
                 round_no, rounds, done, npoints);
        XDrawString(dpy, win, gc, 24, 28, head, (int)strlen(head));
        const char *sub = have_touch
            ? "touch device detected - use your finger"
            : "no touch device - left-click the target (mouse as finger)";
        XDrawString(dpy, win, gc, 24, 48, sub, (int)strlen(sub));
    }
    /* 已完成的点：目标(绿) + 落点(红) + 偏差线 */
    for (int i = 0; i < npoints; i++) {
        if (!pts[i].done) continue;
        if (!pts[i].skipped) {
            set_color(gc, COL_ERR);
            XDrawLine(dpy, win, gc, (int)pts[i].tx, (int)pts[i].ty, (int)pts[i].px, (int)pts[i].py);
            XFillArc(dpy, win, gc, (int)pts[i].px - 3, (int)pts[i].py - 3, 6, 6, 0, 360 * 64);
        }
        draw_target(pts[i].tx, pts[i].ty, i, 0);
    }
    if (cur < npoints) draw_target(pts[cur].tx, pts[cur].ty, cur, 1);
    XFlush(dpy);
}

static void make_points(void) {
    double xs[3] = {0.08, 0.5, 0.92}, ys[3] = {0.08, 0.5, 0.92};
    int n = 0;
    if (npoints <= 5) {
        /* 中心 + 四角（中心先点，便于确认设备是不是在工作） */
        pts[n].tx = 0.5 * W; pts[n].ty = 0.5 * H; n++;
        pts[n].tx = 0.08 * W; pts[n].ty = 0.08 * H; n++;
        pts[n].tx = 0.92 * W; pts[n].ty = 0.08 * H; n++;
        pts[n].tx = 0.08 * W; pts[n].ty = 0.92 * H; n++;
        pts[n].tx = 0.92 * W; pts[n].ty = 0.92 * H; n++;
    } else {
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++) {
                pts[n].tx = xs[i] * W; pts[n].ty = ys[j] * H; n++;
            }
    }
    for (int i = 0; i < n; i++) { pts[i].done = 0; pts[i].skipped = 0; pts[i].istouch = 0;
                                  pts[i].px = pts[i].py = -1; }
    cur = 0;
}

/* ---------------------------------------------------------------- 拟合 */

static void fit1(const double *x, const double *y, int n, double *a, double *b) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) { sx += x[i]; sy += y[i]; sxx += x[i]*x[i]; sxy += x[i]*y[i]; }
    double den = (double)n * sxx - sx * sx;
    if (fabs(den) < 1e-9) { *a = 1.0; *b = 0.0; return; }
    *a = ((double)n * sxy - sx * sy) / den;
    *b = (sy - (*a) * sx) / (double)n;
}

static void mat3_mul(const double A[9], const double B[9], double C[9]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            double s = 0;
            for (int k = 0; k < 3; k++) s += A[r*3+k] * B[k*3+c];
            C[r*3+c] = s;
        }
}

/* ---------------------------------------------------------------- 收尾计算 */

static int analyze(int do_apply, int show_fix) {
    int n = 0;
    double ox[MAXPT], oy[MAXPT], tx[MAXPT], ty[MAXPT];
    double maxe = 0, sumx = 0, sumy = 0;
    for (int i = 0; i < npoints; i++) {
        if (!pts[i].done || pts[i].skipped) continue;
        double dx = pts[i].px - pts[i].tx, dy = pts[i].py - pts[i].ty;
        double e = sqrt(dx*dx + dy*dy);
        if (e > maxe) maxe = e;
        sumx += dx; sumy += dy;
        say("  点%d  目标=(%7.1f,%7.1f)  落点=(%7.1f,%7.1f)  偏差=(%+7.1f,%+7.1f)  |偏差|=%.1f",
            i + 1, pts[i].tx, pts[i].ty, pts[i].px, pts[i].py, dx, dy, e);
        ox[n] = pts[i].px / (double)(W - 1); oy[n] = pts[i].py / (double)(H - 1);
        tx[n] = pts[i].tx / (double)(W - 1); ty[n] = pts[i].ty / (double)(H - 1);
        n++;
    }
    if (n == 0) { say("  没采到有效点。"); return 0; }

    int ntouch = 0;
    for (int i = 0; i < npoints; i++)
        if (pts[i].done && !pts[i].skipped && pts[i].istouch) ntouch++;

    double mx = sumx / n, my = sumy / n;
    double vx = 0, vy = 0;
    for (int i = 0; i < npoints; i++) {
        if (!pts[i].done || pts[i].skipped) continue;
        double dx = pts[i].px - pts[i].tx - mx, dy = pts[i].py - pts[i].ty - my;
        vx += dx*dx; vy += dy*dy;
    }
    double sdx = sqrt(vx / n), sdy = sqrt(vy / n);
    say("  平均偏差 = (%+.1f, %+.1f) px    最大 = %.1f px    离散度 = (%.1f, %.1f) px",
        mx, my, maxe, sdx, sdy);

    if (ntouch == 0) {
        say("");
        say("注意：这 %d 个点全是鼠标点击（[click]/[mouse]），点击位置本来就等于指针位置。", n);
        say("      所以这里量不出触控偏移，也不会生成校正矩阵。");
        say("      要测真实偏移：用指头点，或先跑 tools/mouse_touch_test.sh 让鼠标经由虚拟触控屏注入。");
        return 1;
    }
    if (ntouch < n)
        say("  （其中 %d/%d 个点是鼠标点击，只作参考，不参与后面的拟合判断）", n - ntouch, n);

    if (fabs(mx) > W / 8 && sdx < 30)
        say("  ⚠ 偏差主要是“整体平移”，像是设备被映射到了别的输出/半屏 —— "
            "这种情况更应该用 touchprobe map --rect 把设备映射到正确的屏，而不是标定。");

    /* 拟合：目标 = a*落点 + b （逐轴独立，稳） */
    double ax, bx, ay, by;
    fit1(ox, tx, n, &ax, &bx);
    fit1(oy, ty, n, &ay, &by);
    say("  拟合：目标x = %.6f*落点x%+.6f    目标y = %.6f*落点y%+.6f  （归一化坐标）",
        ax, bx, ay, by);

    /* 新矩阵 = F · 当前矩阵 */
    double F[9] = { ax, 0, bx, 0, ay, by, 0, 0, 1 };
    double M1[9];
    mat3_mul(F, dev.ctm, M1);

    say("  当前矩阵: %.6f %.6f %.6f | %.6f %.6f %.6f | %.6f %.6f %.6f",
        dev.ctm[0], dev.ctm[1], dev.ctm[2], dev.ctm[3], dev.ctm[4], dev.ctm[5],
        dev.ctm[6], dev.ctm[7], dev.ctm[8]);
    if (!show_fix) {
        say("  （这一轮只验证效果，不再改矩阵）");
        return 1;
    }
    say("  建议矩阵: %.6f %.6f %.6f | %.6f %.6f %.6f | %.6f %.6f %.6f",
        M1[0], M1[1], M1[2], M1[3], M1[4], M1[5], M1[6], M1[7], M1[8]);
    say("  等价命令: xinput set-prop %d \"Coordinate Transformation Matrix\" "
        "%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f", dev.id,
        M1[0], M1[1], M1[2], M1[3], M1[4], M1[5], M1[6], M1[7], M1[8]);

    if (fabs(ax) < 0.2 || fabs(ax) > 5 || fabs(ay) < 0.2 || fabs(ay) > 5)
        say("  ⚠ 拟合出的缩放很离谱（%.3f, %.3f），点可能没点准或设备没在“手指所在的那块屏”上；"
            "建议重点一次。", ax, ay);

    if (do_apply) {
        if (write_ctm(dev.id, M1)) {
            memcpy(dev.ctm, M1, sizeof M1);
            say("  已写入设备 %d 的矩阵（本会话生效）。", dev.id);
        } else {
            say("  ✗ 写入失败（设备可能已消失）。");
        }
    }

    /* 给一份能长期生效的 xorg 配置（按设备名匹配，不改系统文件也能先看看） */
    say("");
    say("  想长期生效（重启后仍在），把下面存成 /etc/X11/xorg.conf.d/99-touchcal.conf：");
    say("    Section \"InputClass\"");
    say("        Identifier \"touchcal-%d\"", dev.id);
    say("        MatchDevicePath \"/dev/input/event*\"");
    say("        MatchProduct \"%s\"", dev.name);
    say("        Option \"TransformationMatrix\" \"%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\"",
        M1[0], M1[1], M1[2], M1[3], M1[4], M1[5], M1[6], M1[7], M1[8]);
    say("    EndSection");
    return 1;
}

/* ---------------------------------------------------------------- 事件 */

static void pointer_pos(int *px, int *py) {
    Window r, c; int rx, ry, wx, wy; unsigned int m;
    if (!XQueryPointer(dpy, DefaultRootWindow(dpy), &r, &c, &rx, &ry, &wx, &wy, &m)) {
        *px = *py = -1; return;
    }
    *px = rx; *py = ry;
}

static void pointer_settled(int *px, int *py) {
    XSync(dpy, False);
    struct timespec ts = {0, 800000L};
    nanosleep(&ts, NULL);
    XSync(dpy, False);
    pointer_pos(px, py);
}

static void record_tap(double px, double py, const char *how) {
    if (cur >= npoints) return;
    long t = now_ms();
    if (t - last_tap_ms < 300) return;      /* 防抖：一次触摸会来好几个事件 */
    last_tap_ms = t;
    pts[cur].px = px; pts[cur].py = py;
    pts[cur].done = 1; pts[cur].skipped = 0;
    pts[cur].istouch = (how[0] == 't');
    double dx = px - pts[cur].tx, dy = py - pts[cur].ty;
    say("点%d/%d [%s] 目标=(%.0f,%.0f) 落点=(%.0f,%.0f) 偏差=(%+.0f,%+.0f)",
        cur + 1, npoints, how, pts[cur].tx, pts[cur].ty, px, py, dx, dy);
    cur++;
    draw();
    if (cur >= npoints) {
        say("");
        say("== 第 %d 轮结果 ==", round_no);
        int show_fix = (round_no == 1) || (round_no == rounds && !apply);
        analyze(apply && round_no == 1, show_fix);
        if (round_no < rounds) {
            round_no++;
            make_points();
            say("");
            say("== 开始第 %d/%d 轮（验证）==", round_no, rounds);
            draw();
        } else {
            stop_flag = 1;
        }
    }
}

int main(int argc, char **argv) {
    int want_dev = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--points") && i + 1 < argc) npoints = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) want_dev = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report_path = argv[++i];
        else if (!strcmp(argv[i], "--apply"))  apply = 1;
        else if (!strcmp(argv[i], "--click"))  take_clicks = 1;
        else if (!strcmp(argv[i], "--no-click")) take_clicks = 0;
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) win_timeout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fputs("用法: touchcal [--points 5|9] [--rounds N] [--device ID] [--apply] [--no-click] [--report 路径]\n"
                  "           [--timeout 秒]\n"
                  "  Esc/q 中止，r 重来，空格跳过当前点\n", stderr);
            return 2;
        } else { fprintf(stderr, "未知参数: %s\n", argv[i]); return 2; }
    }
    if (npoints < 3) npoints = 5;
    if (npoints > MAXPT) npoints = MAXPT;
    if (apply && rounds < 2) rounds = 2;      /* 应用后自动再测一轮 */

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "连不上 X（DISPLAY='%s'）\n", getenv("DISPLAY") ? getenv("DISPLAY") : ""); return 1; }
    int ev, err;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &ev, &err)) {
        fprintf(stderr, "这个 X server 没有 XInput 扩展\n"); return 1;
    }
    int major = 2, minor = 2;
    if (XIQueryVersion(dpy, &major, &minor) != Success) {
        fprintf(stderr, "需要 XInput 2.2\n"); return 1;
    }
    scr = DefaultScreen(dpy);
    W = DisplayWidth(dpy, scr); H = DisplayHeight(dpy, scr);

    rep = fopen(report_path, "w");
    say("touchcal  桌面 %dx%d  DISPLAY=%s  %s", W, H, DisplayString(dpy),
        rep ? report_path : "(报告文件打不开，只输出到屏幕)");

    if (!pick_device(want_dev)) {
        say("没找到触控设备（XITouchClass）。将用鼠标左键点目标（只验证流程/界面，测不出触控偏移）。");
    } else {
        say("设备 %d: %s", dev.id, dev.name);
        say("  通道 X=v%d[%.0f..%.0f] Y=v%d[%.0f..%.0f]%s%s", dev.vx, dev.mnx, dev.mxx, dev.vy, dev.mny, dev.mxy,
            dev.node[0] ? "  节点 " : "", dev.node);
        say("  变换矩阵: %.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f",
            dev.ctm[0], dev.ctm[1], dev.ctm[2], dev.ctm[3], dev.ctm[4], dev.ctm[5],
            dev.ctm[6], dev.ctm[7], dev.ctm[8]);
        if (!strstr(dev.name, "TouchInject"))
            say("提示：如果是真触控屏，先把 TouchViz 停掉（TouchViz-ctl stop），它的覆盖层会盖住目标。");
    }

    /* 全屏窗口 */
    XSetWindowAttributes at;
    at.override_redirect = True;
    at.background_pixel = BlackPixel(dpy, scr);
    at.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask;
    win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, W, H, 0,
                        DefaultDepth(dpy, scr), InputOutput, DefaultVisual(dpy, scr),
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &at);
    XMapRaised(dpy, win);
    XStoreName(dpy, win, "touchcal");
    gc = XCreateGC(dpy, win, 0, NULL);
    font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);

    COL_FG   = alloc_color("white",  WhitePixel(dpy, scr));
    COL_CUR  = alloc_color("cyan",   WhitePixel(dpy, scr));
    COL_DONE = alloc_color("green",  WhitePixel(dpy, scr));
    COL_ERR  = alloc_color("red",    WhitePixel(dpy, scr));

    /* 键盘（Esc/r/space） */
    set_color(gc, COL_FG);
    XGrabKeyboard(dpy, win, False, GrabModeAsync, GrabModeAsync, CurrentTime);

    /* 原始触控事件 */
    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    memset(mask, 0, sizeof mask);
    XISetMask(mask, XI_RawTouchBegin);
    XIEventMask em; em.deviceid = XIAllMasterDevices; em.mask_len = sizeof mask; em.mask = mask;
    XISelectEvents(dpy, DefaultRootWindow(dpy), &em, 1);
    XFlush(dpy);

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    make_points();
    draw();
    say("请依次点每个十字的**正中心**：目标 %d 个，共 %d 轮。", npoints, rounds);

    long t0 = now_ms();
    while (!stop_flag) {
        while (XPending(dpy)) {
            XEvent e; XNextEvent(dpy, &e);
            if (e.type == Expose && e.xexpose.count == 0) { XRaiseWindow(dpy, win); draw(); }
            else if (e.type == KeyPress) {
                KeySym ks = XLookupKeysym(&e.xkey, 0);
                if (ks == XK_Escape || ks == XK_q) { stop_flag = 1; }
                else if (ks == XK_r) { make_points(); say("重来本轮。"); draw(); }
                else if (ks == XK_space) {
                    if (cur < npoints) { pts[cur].done = 1; pts[cur].skipped = 1; cur++;
                                         say("跳过 点%d", cur); draw();
                                         if (cur >= npoints) { analyze(0, 1); stop_flag = 1; } }
                }
            } else if (e.type == ButtonPress && e.xbutton.button == 1 && (!have_touch || take_clicks)) {
                int px, py; pointer_settled(&px, &py);
                if (have_touch && !click_hinted) {
                    say("（提示：这次是鼠标点击。若你在用 tools/mouse_touch_test.sh 做“鼠标当手指”，"
                        "请加 --no-click，否则会先记下点击而不是触控）");
                    click_hinted = 1;
                }
                record_tap(px, py, have_touch ? "click" : "mouse");
            } else if (e.type == GenericEvent && e.xcookie.extension == xi_opcode) {
                if (!XGetEventData(dpy, &e.xcookie)) continue;
                if (e.xcookie.evtype == XI_RawTouchBegin) {
                    XIRawEvent *raw = (XIRawEvent *)e.xcookie.data;
                    int is_ours = dev_ok && raw->sourceid == dev.id;
                    if (is_ours &&
                        XIMaskIsSet(raw->valuators.mask, dev.vx) &&
                        XIMaskIsSet(raw->valuators.mask, dev.vy)) {
                        double nx = (raw->valuators.values[dev.vx] - dev.mnx) / (dev.mxx - dev.mnx) * (W - 1);
                        double ny = (raw->valuators.values[dev.vy] - dev.mny) / (dev.mxy - dev.mny) * (H - 1);
                        int px, py; pointer_settled(&px, &py);
                        (void)nx; (void)ny;
                        record_tap(px, py, "touch");
                    } else if (have_touch) {
                        /* 别的触控设备也接受，提示一下 */
                        int px, py; pointer_settled(&px, &py);
                        record_tap(px, py, "touch*");
                    }
                }
                XFreeEventData(dpy, &e.xcookie);
            }
        }
        if (win_timeout && now_ms() - t0 > win_timeout * 1000L) {
            say("超时（%d 秒），结束。", win_timeout);
            break;
        }
        struct pollfd pfd;
        pfd.fd = ConnectionNumber(dpy); pfd.events = POLLIN; pfd.revents = 0;
        poll(&pfd, 1, 200);
    }

    XUngrabKeyboard(dpy, CurrentTime);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    if (rep) { say("报告已写到 %s", report_path); fclose(rep); }
    return 0;
}
