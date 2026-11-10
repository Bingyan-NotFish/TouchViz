/* ============================================================================
 * touchprobe —— TouchViz 的便携诊断/验证工具
 *
 * 设计目标：**绝对的可移植性**
 *   • 只依赖 libX11 + libXi（项目本身就要这两个），不用 python/PIL/ImageMagick、
 *     不用 xdotool/xinput/qdbus、不读 /tmp 下的固定文件名、不需要 root。
 *   • DISPLAY 没设或设错时自动在 /tmp/.X11-unix/X* 里找可用的。
 *   • 屏幕尺寸、设备号、量程、变换矩阵全部现场查询，不写死任何坐标/分辨率。
 *   • 判断“有没有偏移”不需要截图：触控点落下时指针会被移到该点，
 *     所以把「按 TouchViz 公式算出的位置」和「指针真实位置」一比即可。
 *     需要看图形时才用内建的 XGetImage 截图 + PPM 比对（同样零外部依赖）。
 *
 * 子命令
 *   geom                       打印 DISPLAY / 屏幕尺寸
 *   devices                    列出所有直接触控设备：量程、变换矩阵、映射矩形
 *   expect <x> <y>             注入(或触摸)屏幕像素坐标 x,y 后，实际会落在哪里
 *   watch [-n N] [-t SEC] [--tol PX] [-q]
 *                              实时测量每次触控的偏移（TouchViz 公式 vs 指针）
 *   shot <out.ppm> [x y w h]   截取根窗口（PPM/P6，供 diff/ring 用）
 *   diff <a.ppm> <b.ppm> [x y w h [thr]] [--centroid]
 *                              两图差异：bbox / 最大差值 / 超阈值像素数 / 质心
 *   ring <a.ppm> <x> <y> <pad> [--tol N] [--color r,g,b]
 *                              在 (x,y) 附近找触点圆环，输出圆心与半径
 *   finger [--fifo P] [--max-hz N]
 *                              “鼠标当手指”：把左键按下/拖动/松开转发给 mtinject
 *   map [--dev ID] --full | --rect X Y W H | --reset | --show
 *                              读写设备的 Coordinate Transformation Matrix
 *
 * 退出码：watch 有超差返回 1；其余命令成功 0、失败非 0。
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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XInput.h>    /* XGetDeviceProperty / XChangeDeviceProperty */
#include <X11/extensions/XTest.h>      /* XTestFakeButtonEvent（TouchViz 本身也依赖它） */
#include <X11/extensions/Xfixes.h>     /* XFixesGetCursorImage：判断指针是否被隐藏 */

static Display *dpy;
static int      xi_opcode = -1;
static int      scr_w, scr_h;

/* ------------------------------------------------------------------ 基础 */

static void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("touchprobe: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(2);
}

static long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 打开显示：DISPLAY 无效时自动扫描本地 X socket（:0..:9 / :10..） */
static Display *open_display(void) {
    const char *d = getenv("DISPLAY");
    Display *x = d && *d ? XOpenDisplay(NULL) : NULL;
    if (x) return x;
    char buf[32];
    /* 先看 /tmp/.X11-unix 里有哪些 socket，再按号码试 */
    for (int n = 0; n < 64; n++) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", n);
        if (access(path, F_OK) != 0) continue;
        snprintf(buf, sizeof(buf), ":%d", n);
        setenv("DISPLAY", buf, 1);
        if ((x = XOpenDisplay(buf))) return x;
    }
    if (d && *d) setenv("DISPLAY", d, 1);
    return NULL;
}

/* 设备可能在枚举后就消失（热插拔/残留项）：忽略这类 X 错误，不要直接退出 */
static int x_error_handler(Display *d, XErrorEvent *e) {
    (void)d;
    if (e->request_code == xi_opcode) return 0;          /* XInput 请求失败：自己往下走 */
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "touchprobe: X 错误 %s (request %d.%d)\n", buf, e->request_code, e->minor_code);
    return 0;
}

static void open_xi(void) {
    dpy = open_display();
    if (!dpy) fatal("连不上 X server（DISPLAY='%s'）。root 下请先 export DISPLAY/XAUTHORITY",
                    getenv("DISPLAY") ? getenv("DISPLAY") : "");
    XSetErrorHandler(x_error_handler);
    int ev, err;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &ev, &err))
        fatal("这个 X server 没有 XInput 扩展");
    int major = 2, minor = 2;
    if (XIQueryVersion(dpy, &major, &minor) != Success)
        fatal("需要 XInput 2.2（原始触控事件）");
    scr_w = DisplayWidth(dpy, DefaultScreen(dpy));
    scr_h = DisplayHeight(dpy, DefaultScreen(dpy));
}

/* 设备属性读取（XI1 的 XGetDeviceProperty 对 XI2 设备同样有效） */
static int get_dev_prop(int devid, const char *name, Atom req_type,
                        unsigned char **data, int *nitems, Atom *type, int *format) {
    Atom prop = XInternAtom(dpy, name, True);
    if (prop == None) return 0;
    XDevice *dev = XOpenDevice(dpy, devid);      /* XI1 API 需要 XDevice* */
    if (!dev) return 0;
    unsigned long n = 0, after = 0;
    int ok = (XGetDeviceProperty(dpy, dev, prop, 0, 64, False, req_type,
                                 type, format, &n, &after, data) == Success);
    XCloseDevice(dpy, dev);
    if (!ok || !*data || n == 0) { if (*data) { XFree(*data); *data = NULL; } return 0; }
    *nitems = (int)n;
    return 1;
}

static int get_ctm(int devid, double m[9]) {
    const double ident[9] = {1,0,0, 0,1,0, 0,0,1};
    memcpy(m, ident, sizeof(ident));
    unsigned char *data = NULL; int n = 0, fmt = 0; Atom type = None;
    if (!get_dev_prop(devid, "Coordinate Transformation Matrix", AnyPropertyType,
                      &data, &n, &type, &fmt)) return 0;
    /* format==32 的属性按 long 数组返回，FLOAT 要把 32 位位模式重解释成 float */
    if (fmt == 32 && n >= 9) {
        const unsigned long *u = (const unsigned long *)data;
        for (int i = 0; i < 9; i++) {
            uint32_t bits = (uint32_t)u[i];
            float f;
            memcpy(&f, &bits, sizeof f);
            m[i] = f;
        }
    }
    XFree(data);
    return 1;
}

static char *get_dev_node(int devid) {
    unsigned char *data = NULL; int n = 0, fmt = 0; Atom type = None;
    static char buf[256];
    if (!get_dev_prop(devid, "Device Node", XA_STRING, &data, &n, &type, &fmt)) return NULL;
    snprintf(buf, sizeof(buf), "%.*s", n, (char *)data);
    XFree(data);
    return buf;
}

/* ------------------------------------------------------------------ 设备信息 */

typedef struct {
    int    id;
    char   name[128];
    int    is_touch;
    int    vx, vy;
    double mnx, mxx, mny, mxy;
    double ctm[9];
    int    has_ctm;
} Dev;

static Dev *devs = NULL;
static int  ndevs = 0;

static int score_label(const char *name) {
    return strstr(name, "Position") ? 2 : 1;   /* “Position”优先，避开 Tilt X/Y */
}

static void load_dev(Dev *d, XIDeviceInfo *info) {
    memset(d, 0, sizeof(*d));
    d->id = info->deviceid;
    snprintf(d->name, sizeof(d->name), "%s", info->name ? info->name : "?");
    d->vx = 0; d->vy = 1;
    d->mnx = 0; d->mxx = 65535; d->mny = 0; d->mxy = 65535;
    int sx = -1, sy = -1, n0 = -1, n1 = -1;
    double mn0 = 0, mx0 = 65535, mn1 = 0, mx1 = 65535;
    for (int c = 0; c < info->num_classes; c++) {
        if (info->classes[c]->type == XITouchClass) { d->is_touch = 1; continue; }
        if (info->classes[c]->type != XIValuatorClass) continue;
        XIValuatorClassInfo *v = (XIValuatorClassInfo *)info->classes[c];
        if (v->number == 0) { n0 = 0; mn0 = v->min; mx0 = v->max; }
        if (v->number == 1) { n1 = 1; mn1 = v->min; mx1 = v->max; }
        if (!v->label || v->max <= v->min) continue;
        char *nm = XGetAtomName(dpy, v->label);
        if (!nm) continue;
        int L = (int)strlen(nm), last = L ? nm[L - 1] : 0, sc = score_label(nm);
        if (last == 'X' || last == 'x') {
            if (sc > sx) { sx = sc; d->vx = v->number; d->mnx = v->min; d->mxx = v->max; }
        } else if (last == 'Y' || last == 'y') {
            if (sc > sy) { sy = sc; d->vy = v->number; d->mny = v->min; d->mxy = v->max; }
        }
        XFree(nm);
    }
    if (sx < 0 && n0 == 0 && mx0 > mn0) { d->vx = 0; d->mnx = mn0; d->mxx = mx0; }
    if (sy < 0 && n1 == 1 && mx1 > mn1) { d->vy = 1; d->mny = mn1; d->mxy = mx1; }
    if (d->mxx <= d->mnx) { d->mnx = 0; d->mxx = 65535; }
    if (d->mxy <= d->mny) { d->mny = 0; d->mxy = 65535; }
    d->has_ctm = get_ctm(info->deviceid, d->ctm);
}

static void load_devices(void) {
    int n = 0;
    XIDeviceInfo *info = XIQueryDevice(dpy, XIAllDevices, &n);
    if (!info) fatal("XIQueryDevice 失败");
    devs = calloc(n ? n : 1, sizeof(Dev));
    ndevs = 0;
    for (int i = 0; i < n; i++) {
        if (!info[i].enabled) continue;
        if (info[i].use == XIMasterPointer || info[i].use == XIMasterKeyboard) continue;
        Dev d; load_dev(&d, &info[i]);
        /* 主设备也声明 XITouchClass，但它们不产生 raw 触控事件（sourceid 总是从设备） */
        if (!d.is_touch) continue;
        /* 已经消失但还在设备列表里的“幽灵”打不开，跳过，避免重复统计 */
        XDevice *odev = XOpenDevice(dpy, d.id);
        if (!odev) continue;
        XCloseDevice(dpy, odev);
        devs[ndevs++] = d;
    }
    XIFreeDeviceInfo(info);
}

static const Dev *find_dev(int id) {
    for (int i = 0; i < ndevs; i++) if (devs[i].id == id) return &devs[i];
    return NULL;
}

/* 事件里出现的新设备（热插拔）：现场查询并加入缓存 */
static const Dev *ensure_dev(int id) {
    const Dev *d = find_dev(id);
    if (d) return d;
    int n = 0;
    XIDeviceInfo *info = XIQueryDevice(dpy, id, &n);
    if (!info) return NULL;
    Dev nd; memset(&nd, 0, sizeof nd);
    load_dev(&nd, &info[0]);
    XIFreeDeviceInfo(info);
    Dev *tmp = realloc(devs, (ndevs + 1) * sizeof(Dev));
    if (!tmp) return NULL;
    devs = tmp; devs[ndevs++] = nd;
    return &devs[ndevs - 1];
}

/* TouchViz 的坐标公式：valuator(已套过变换矩阵) -> 屏幕像素。
 * X server 把设备量程 [min,max] 映射到 [0, W-1]，所以这里也用 W-1/H-1。 */
static void overlay_pos(const Dev *d, double vx, double vy, double *ox, double *oy) {
    double nx = (vx - d->mnx) / (d->mxx - d->mnx) * (scr_w - 1);
    double ny = (vy - d->mny) / (d->mxy - d->mny) * (scr_h - 1);
    if (nx < 0) { nx = 0; }
    if (nx > scr_w - 1) { nx = scr_w - 1; }
    if (ny < 0) { ny = 0; }
    if (ny > scr_h - 1) { ny = scr_h - 1; }
    *ox = nx; *oy = ny;
}

/* 设备映射矩形（由 CTM 决定）：设备覆盖桌面的哪一块 */
static void mapped_rect(const Dev *d, double *x0, double *y0, double *x1, double *y1) {
    double a = d->ctm[0], tx = d->ctm[2], e = d->ctm[4], ty = d->ctm[5];
    double W = scr_w - 1, H = scr_h - 1;
    double sx0 = tx * W, sx1 = (a + tx) * W;
    double sy0 = ty * H, sy1 = (e + ty) * H;
    if (sx1 < sx0) { double t = sx0; sx0 = sx1; sx1 = t; }
    if (sy1 < sy0) { double t = sy0; sy0 = sy1; sy1 = t; }
    *x0 = sx0; *y0 = sy0; *x1 = sx1; *y1 = sy1;
}

static void pointer_pos(int *px, int *py) {
    Window r, c; int rx, ry, wx, wy; unsigned int m;
    if (!XQueryPointer(dpy, DefaultRootWindow(dpy), &r, &c, &rx, &ry, &wx, &wy, &m)) {
        *px = *py = -1; return;
    }
    *px = rx; *py = ry;
}

/* 指针位置可能要等 server 处理完这次触控动作，做一次往返 + 1ms */
static void pointer_pos_settled(int *px, int *py) {
    XSync(dpy, False);
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
    XSync(dpy, False);
    pointer_pos(px, py);
}

/* ------------------------------------------------------------------ geom */

static int cmd_geom(void) {
    printf("DISPLAY=%s  screen=%dx%d  root=%#lx\n",
           DisplayString(dpy), scr_w, scr_h, DefaultRootWindow(dpy));
    load_devices();
    int nt = 0;
    for (int i = 0; i < ndevs; i++) nt++;
    printf("touch_devices=%d\n", nt);
    return 0;
}

/* --------------------------------------------------------------- devices */

static int cmd_devices(void) {
    load_devices();
    if (!ndevs) {
        printf("没有发现直接触控设备（XITouchClass）。\n");
        return 0;
    }
    for (int i = 0; i < ndevs; i++) {
        const Dev *d = &devs[i];
        char *node = get_dev_node(d->id);
        printf("设备 %d: %s\n", d->id, d->name);
        printf("  通道: X=v%d[%.0f..%.0f]  Y=v%d[%.0f..%.0f]\n",
               d->vx, d->mnx, d->mxx, d->vy, d->mny, d->mxy);
        if (node) printf("  节点: %s\n", node);
        if (!d->has_ctm) {
            printf("  变换矩阵: (无) => 覆盖整个桌面 %dx%d\n", scr_w, scr_h);
            printf("  结论: 映射正常（整个桌面）\n");
            continue;
        }
        printf("  变换矩阵: %.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f\n",
               d->ctm[0], d->ctm[1], d->ctm[2], d->ctm[3], d->ctm[4], d->ctm[5],
               d->ctm[6], d->ctm[7], d->ctm[8]);
        double x0, y0, x1, y1; mapped_rect(d, &x0, &y0, &x1, &y1);
        int full = (fabs(x0) < 1 && fabs(y0) < 1 &&
                    fabs(x1 - (scr_w - 1)) < 1 && fabs(y1 - (scr_h - 1)) < 1);
        printf("  映射矩形: (%.0f,%.0f)-(%.0f,%.0f) 即 %.0fx%.0f 个像素\n",
               x0, y0, x1, y1, x1 - x0, y1 - y0);
        if (full) {
            printf("  结论: 映射正常（整个桌面）。若手指与落点仍不符，多半是面板标定/边框问题，\n"
                   "        可用 map --rect 或在系统设置里做触摸屏校准。\n");
        } else {
            printf("  结论: **只覆盖桌面的一部分**！触摸会整体缩放/平移 ——\n"
                   "        这是“触控偏移”的典型原因（多屏时被自动映射到主屏，或校准矩阵被改过）。\n"
                   "        修复: touchprobe map --dev %d --full             # 覆盖整个桌面\n"
                   "              touchprobe map --dev %d --rect %.0f %.0f %.0f %.0f   # 只映射到某块屏\n",
                   d->id, d->id, x0, y0, x1 - x0, y1 - y0);
        }
        printf("STAT dev=%d full=%d rect=%.0f,%.0f,%.0f,%.0f\n",
               d->id, full, x0, y0, x1, y1);
    }
    return 0;
}

/* 注入（mtinject 的像素坐标，等价于“手指在设备上按下的位置”）x,y 之后，
 * 事件里 valuator 会被套上 CTM，最终落点 = (a*x + tx*W, e*y + ty*H) */
static int cmd_expect(int argc, char **argv) {
    if (argc < 2) fatal("expect 需要 x y");
    double x = atof(argv[0]), y = atof(argv[1]);
    load_devices();
    if (!ndevs) fatal("没有触控设备");
    const Dev *d = &devs[0];
    /* mtinject 按根窗口像素 -> 设备量程；libinput 归一化后套 CTM；server 再 ×(W-1) */
    double sx = (d->ctm[0] * x / scr_w + d->ctm[2]) * (scr_w - 1);
    double sy = (d->ctm[4] * y / scr_h + d->ctm[5]) * (scr_h - 1);
    printf("inject=%.0f,%.0f -> screen=%.0f,%.0f\n", x, y, sx, sy);
    return 0;
}

/* ----------------------------------------------------------------- watch */

static volatile sig_atomic_t stop_flag = 0;
static void on_sig(int s) { (void)s; stop_flag = 1; }

static int cmd_watch(int argc, char **argv) {
    int want = 0, timeout = 0;      /* want=0 表示不限次数 */
    double tol = 3.0; int quiet = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) want = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) timeout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tol") && i + 1 < argc) tol = atof(argv[++i]);
        else if (!strcmp(argv[i], "-q")) quiet = 1;
    }
    load_devices();
    if (!ndevs) fatal("没有触控设备：先跑 tools/start_mtinject.sh，或接一块触控屏");
    if (!quiet) {
        printf("# 触摸屏幕任意位置（或用 mtinject 注入）。每次触控输出一行：\n");
        printf("# dev slot  valuators      TouchViz位置      指针位置        偏移\n");
        fflush(stdout);
    }
    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_RawTouchBegin);
    XISetMask(mask, XI_RawTouchUpdate);
    XISetMask(mask, XI_RawTouchEnd);
    XIEventMask em; em.deviceid = XIAllMasterDevices; em.mask_len = sizeof(mask); em.mask = mask;
    XISelectEvents(dpy, DefaultRootWindow(dpy), &em, 1);
    XFlush(dpy);

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    long t0 = now_ms();
    int got = 0, bad = 0;
    while (!stop_flag) {
        if (timeout && now_ms() - t0 >= timeout * 1000L) break;
        if (want && got >= want) break;
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (ev.type != GenericEvent || ev.xcookie.extension != xi_opcode) continue;
            if (!XGetEventData(dpy, &ev.xcookie)) continue;
            int et = ev.xcookie.evtype;
            if (et == XI_RawTouchBegin || et == XI_RawTouchUpdate) {
                XIRawEvent *raw = (XIRawEvent *)ev.xcookie.data;
                const Dev *d = ensure_dev(raw->sourceid);
                if (d && d->is_touch &&
                    XIMaskIsSet(raw->valuators.mask, d->vx) &&
                    XIMaskIsSet(raw->valuators.mask, d->vy)) {
                    double vx = raw->valuators.values[d->vx];
                    double vy = raw->valuators.values[d->vy];
                    double ox, oy; overlay_pos(d, vx, vy, &ox, &oy);
                    int px, py; pointer_pos_settled(&px, &py);
                    double dx = ox - px, dy = oy - py;
                    if (et == XI_RawTouchBegin) got++;
                    int is_bad = (fabs(dx) > tol || fabs(dy) > tol);
                    if (is_bad && et == XI_RawTouchBegin) bad++;
                    if (!quiet || is_bad)
                        printf("TOUCH dev=%d slot=%d v=(%.0f,%.0f) overlay=(%.1f,%.1f) "
                               "pointer=(%d,%d) delta=(%.1f,%.1f) %s\n",
                               d->id, raw->detail, vx, vy, ox, oy, px, py, dx, dy,
                               is_bad ? "OFFSET" : "ok");
                    fflush(stdout);
                }
            }
            XFreeEventData(dpy, &ev.xcookie);
        }
        struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
        poll(&pfd, 1, 200);
    }
    if (!quiet) {
        printf("# 共 %d 次触控，超差(>%.0fpx) %d 次\n", got, tol, bad);
        if (got && !bad) printf("# 结论: TouchViz 公式与真实落点一致，无偏移\n");
    }
    printf("SUMMARY n=%d bad=%d tol=%.1f\n", got, bad, tol);
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------ PPM */

typedef struct { int w, h; unsigned char *p; } Img;

static int ppm_token(FILE *f, char *buf, int n) {
    int c;
    do { c = fgetc(f); if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); } } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    if (c == EOF) return 0;
    int i = 0;
    while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r' && i < n - 1) {
        buf[i++] = (char)c; c = fgetc(f);
    }
    buf[i] = 0;
    return 1;
}

static int img_read(const char *path, Img *im) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "touchprobe: 打不开 %s: %s\n", path, strerror(errno)); return 0; }
    char tok[64];
    if (!ppm_token(f, tok, sizeof tok) || strcmp(tok, "P6")) { fclose(f); return 0; }
    if (!ppm_token(f, tok, sizeof tok)) { fclose(f); return 0; } im->w = atoi(tok);
    if (!ppm_token(f, tok, sizeof tok)) { fclose(f); return 0; } im->h = atoi(tok);
    if (!ppm_token(f, tok, sizeof tok)) { fclose(f); return 0; }
    im->p = malloc((size_t)im->w * im->h * 3);
    if (!im->p) { fclose(f); return 0; }
    size_t got = fread(im->p, 1, (size_t)im->w * im->h * 3, f);
    fclose(f);
    if (got != (size_t)im->w * im->h * 3) { free(im->p); im->p = NULL; return 0; }
    return 1;
}

static int cmd_shot(int argc, char **argv) {
    if (argc < 1) fatal("shot 需要输出文件名");
    int x = 0, y = 0, w = scr_w, h = scr_h;
    if (argc >= 5) { x = atoi(argv[1]); y = atoi(argv[2]); w = atoi(argv[3]); h = atoi(argv[4]); }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w <= 0 || h <= 0) fatal("截图区域为空");
    XImage *img = XGetImage(dpy, DefaultRootWindow(dpy), x, y, w, h, AllPlanes, ZPixmap);
    if (!img) fatal("XGetImage 失败");
    FILE *f = fopen(argv[0], "wb");
    if (!f) fatal("写不了 %s", argv[0]);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    unsigned char *row = malloc((size_t)w * 3);
    const int bpp = img->bits_per_pixel;
    for (int j = 0; j < h; j++) {
        const unsigned char *src = (const unsigned char *)img->data + (size_t)j * img->bytes_per_line;
        if ((bpp == 32 || bpp == 24) && img->byte_order == LSBFirst) {
            /* 直接读内存：比 XGetPixel 快一两个数量级（截 4K 屏不会误帧） */
            const int step = bpp / 8;
            for (int i = 0; i < w; i++) {
                unsigned long p = 0;
                memcpy(&p, src + (size_t)i * step, step);
                row[i*3+0] = (unsigned char)(((p & img->red_mask)   * 255) / img->red_mask);
                row[i*3+1] = (unsigned char)(((p & img->green_mask) * 255) / img->green_mask);
                row[i*3+2] = (unsigned char)(((p & img->blue_mask)  * 255) / img->blue_mask);
            }
        } else {
            for (int i = 0; i < w; i++) {
                unsigned long p = XGetPixel(img, i, j);
                row[i*3+0] = (unsigned char)((p & img->red_mask)   * 255 / img->red_mask);
                row[i*3+1] = (unsigned char)((p & img->green_mask) * 255 / img->green_mask);
                row[i*3+2] = (unsigned char)((p & img->blue_mask)  * 255 / img->blue_mask);
            }
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row); fclose(f);
    XDestroyImage(img);
    return 0;
}

/* 区域裁剪到图像范围内 */
static void clip(int *x, int *y, int *w, int *h, int iw, int ih) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > iw) *w = iw - *x;
    if (*y + *h > ih) *h = ih - *y;
    if (*w <= 0 || *h <= 0) { *w = *h = 0; }
}

/* ---------------------------------------------------------------- diff */

static int cmd_diff(int argc, char **argv) {
    if (argc < 2) fatal("diff 需要两张 PPM");
    Img a = {0}, b = {0};
    if (!img_read(argv[0], &a)) fatal("读不了 %s", argv[0]);
    if (!img_read(argv[1], &b)) fatal("读不了 %s", argv[1]);
    int x = 0, y = 0, w = a.w < b.w ? a.w : b.w, h = a.h < b.h ? a.h : b.h;
    int thr = 25, centroid = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--centroid")) centroid = 1;
        else if (!strcmp(argv[i], "--thr") && i + 1 < argc) thr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--region") && i + 4 < argc) {
            x = atoi(argv[++i]); y = atoi(argv[++i]); w = atoi(argv[++i]); h = atoi(argv[++i]);
        }
    }
    clip(&x, &y, &w, &h, a.w, a.h);
    if (!w || !h) fatal("比较区域为空");
    long n = 0; int maxd = 0;
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
    double sx = 0, sy = 0;
    for (int j = y; j < y + h; j++) {
        const unsigned char *pa = a.p + ((size_t)j * a.w + x) * 3;
        const unsigned char *pb = b.p + ((size_t)j * b.w + x) * 3;
        for (int i = 0; i < w; i++) {
            int d = 0;
            for (int c = 0; c < 3; c++) {
                int v = abs((int)pa[i*3+c] - (int)pb[i*3+c]);
                if (v > d) d = v;
            }
            if (d > maxd) maxd = d;
            if (d > thr) {
                n++; sx += x + i; sy += j;
                if (x + i < bx0) { bx0 = x + i; }
                if (x + i > bx1) { bx1 = x + i; }
                if (j < by0) { by0 = j; }
                if (j > by1) { by1 = j; }
            }
        }
    }
    if (!n) { printf("DIFF none max=%d\n", maxd); return 0; }
    printf("DIFF count=%ld max=%d bbox=%d,%d,%d,%d", n, maxd, bx0, by0, bx1, by1);
    if (centroid) printf(" centroid=%.1f,%.1f", sx / n, sy / n);
    printf("\n");
    free(a.p); free(b.p);
    return 0;
}

/* ---------------------------------------------------------------- ring */

/* 在区域内找触点圆环：颜色接近给定 RGB 的像素的质心与包围盒 */
static int cmd_ring(int argc, char **argv) {
    if (argc < 4) fatal("ring <a.ppm> <x> <y> <pad>");
    Img a = {0};
    const char *file = argv[0];
    int cx = atoi(argv[1]), cy = atoi(argv[2]), pad = atoi(argv[3]);
    int tol = 70;
    int cr = 51, cg = 191, cb = 255;              /* 槽位 0 的默认颜色 */
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--tol") && i + 1 < argc) tol = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--color") && i + 1 < argc) {
            sscanf(argv[++i], "%d,%d,%d", &cr, &cg, &cb);
        }
    }
    if (!img_read(file, &a)) fatal("读不了 %s", file);
    int x = cx - pad, y = cy - pad, w = pad * 2, h = pad * 2;
    clip(&x, &y, &w, &h, a.w, a.h);
    long n = 0; double sx = 0, sy = 0;
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
    for (int j = y; j < y + h; j++) {
        const unsigned char *p = a.p + ((size_t)j * a.w + x) * 3;
        for (int i = 0; i < w; i++) {
            int dr = (int)p[i*3+0] - cr, dg = (int)p[i*3+1] - cg, db = (int)p[i*3+2] - cb;
            if (dr*dr + dg*dg + db*db > tol * tol) continue;
            n++; sx += x + i; sy += j;
            if (x + i < bx0) { bx0 = x + i; }
            if (x + i > bx1) { bx1 = x + i; }
            if (j < by0) { by0 = j; }
            if (j > by1) { by1 = j; }
        }
    }
    if (!n) { printf("RING none\n"); free(a.p); return 1; }
    double mx = sx / n, my = sy / n;
    double rad = (bx1 - bx0 + by1 - by0) / 4.0;
    printf("RING count=%ld center=%.1f,%.1f bbox=%d,%d,%d,%d radius≈%.1f dx=%.1f dy=%.1f\n",
           n, mx, my, bx0, by0, bx1, by1, rad, mx - cx, my - cy);
    free(a.p);
    return 0;
}

/* ---------------------------------------------------------------- finger */

/* 把命令写进 mtinject 的 FIFO；daemon 重启过就自动重连一次 */
static int  fifo_fd = -1;
static const char *fifo_path = NULL;
static int  fifo_warned = 0;

static int fifo_send(const char *s) {
    if (fifo_fd < 0) {
        fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
        if (fifo_fd < 0) {
            if (!fifo_warned) {
                fprintf(stderr, "touchprobe: 打不开 FIFO %s (%s)\n"
                                "  虚拟触控屏没在跑？跑一下 tools/start_mtinject.sh\n",
                        fifo_path, strerror(errno));
                fifo_warned = 1;
            }
            return -1;
        }
        fifo_warned = 0;
    }
    ssize_t n = write(fifo_fd, s, strlen(s));
    if (n < 0) {
        close(fifo_fd);
        fifo_fd = -1;                      /* 下次自动重开 */
        if (!fifo_warned) {
            fprintf(stderr, "touchprobe: 写 FIFO 失败（daemon 重启过？），会自动重连\n");
            fifo_warned = 1;
        }
        return -1;
    }
    return 0;
}

static int cmd_finger(int argc, char **argv) {
    fifo_path = getenv("MTINJ_FIFO");
    if (!fifo_path || !*fifo_path) fifo_path = "/tmp/mtinject.fifo";
    int maxhz = 60;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--fifo") && i + 1 < argc) fifo_path = argv[++i];
        else if (!strcmp(argv[i], "--max-hz") && i + 1 < argc) maxhz = atoi(argv[++i]);
    }
    if (fifo_send("") < 0) return 1;       /* 先确认 FIFO 能写 */

    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_RawMotion);
    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawButtonRelease);
    XIEventMask em; em.deviceid = XIAllMasterDevices; em.mask_len = sizeof(mask); em.mask = mask;
    XISelectEvents(dpy, DefaultRootWindow(dpy), &em, 1);
    XFlush(dpy);

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    int down = 0; long last_move = 0;
    char buf[128];
    fprintf(stderr, "touchprobe: 鼠标当手指已就绪（FIFO %s）。左键按住=触点、拖动=拖尾、松开=抬手。Ctrl-C 退出。\n",
            fifo_path);
    while (!stop_flag) {
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (ev.type != GenericEvent || ev.xcookie.extension != xi_opcode) continue;
            if (!XGetEventData(dpy, &ev.xcookie)) continue;
            int et = ev.xcookie.evtype;
            /* 触控设备自己的按键/移动不要再转发（否则真触控屏会被当成鼠标再灌一遍） */
            const Dev *src = ensure_dev(((XIRawEvent *)ev.xcookie.data)->sourceid);
            if (src && src->is_touch) {
                XFreeEventData(dpy, &ev.xcookie);
                continue;
            }
            if (et == XI_RawButtonPress || et == XI_RawButtonRelease) {
                XIRawEvent *raw = (XIRawEvent *)ev.xcookie.data;
                if (raw->detail == 1) {
                    int px, py; pointer_pos_settled(&px, &py);
                    if (et == XI_RawButtonPress && !down) {
                        snprintf(buf, sizeof buf, "down %d %d\n", px, py);
                        fifo_send(buf);
                        down = 1;
                        fprintf(stderr, "  按下 -> (%d,%d)\n", px, py);
                    } else if (et == XI_RawButtonRelease && down) {
                        fifo_send("up\n");
                        down = 0;
                        fprintf(stderr, "  松开\n");
                    }
                }
            } else if (et == XI_RawMotion && down) {
                long t = now_ms();
                if (t - last_move < 1000 / (maxhz > 0 ? maxhz : 60)) { XFreeEventData(dpy, &ev.xcookie); continue; }
                last_move = t;
                int px, py; pointer_pos(&px, &py);
                snprintf(buf, sizeof buf, "move %d %d\n", px, py);
                fifo_send(buf);
            }
            XFreeEventData(dpy, &ev.xcookie);
        }
        struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
        poll(&pfd, 1, 200);
    }
    if (down) fifo_send("up\n");         /* 退出时别把触点留在按下状态 */
    if (fifo_fd >= 0) close(fifo_fd);
    return 0;
}

/* ------------------------------------------------------------------ map */

static void set_ctm(int devid, const double m[9]) {
    /* format 32 的属性必须按 long 数组传；FLOAT 要把 float 的位模式塞进 long */
    long v[9];
    for (int i = 0; i < 9; i++) {
        float f = (float)m[i];
        uint32_t bits;
        memcpy(&bits, &f, sizeof bits);
        v[i] = (long)bits;
    }
    Atom prop = XInternAtom(dpy, "Coordinate Transformation Matrix", False);
    Atom type = XInternAtom(dpy, "FLOAT", False);
    XDevice *dev = XOpenDevice(dpy, devid);
    if (!dev) fatal("打不开设备 %d", devid);
    XChangeDeviceProperty(dpy, dev, prop, type, 32, PropModeReplace, (unsigned char *)v, 9);
    XSync(dpy, False);
    XCloseDevice(dpy, dev);
}

/* 把“桌面上的哪块矩形”写成变换矩阵 */
static void rect_to_ctm(double x, double y, double w, double h, double m[9]) {
    double ident[9] = {1,0,0, 0,1,0, 0,0,1};
    memcpy(m, ident, sizeof ident);
    m[0] = w / (scr_w - 1.0);  m[2] = x / (scr_w - 1.0);
    m[4] = h / (scr_h - 1.0);  m[5] = y / (scr_h - 1.0);
}

static int cmd_map(int argc, char **argv) {
    load_devices();
    if (!ndevs) fatal("没有触控设备");
    int id = -1, mode = 0;      /* 1=full 2=rect 3=reset 4=show */
    double rx = 0, ry = 0, rw = 0, rh = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dev") && i + 1 < argc) id = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--full")) mode = 1;
        else if (!strcmp(argv[i], "--reset")) mode = 3;
        else if (!strcmp(argv[i], "--show")) mode = 4;
        else if (!strcmp(argv[i], "--rect") && i + 4 < argc) {
            mode = 2;
            rx = atof(argv[++i]); ry = atof(argv[++i]); rw = atof(argv[++i]); rh = atof(argv[++i]);
        }
    }
    if (id < 0) id = devs[0].id;
    const Dev *d = find_dev(id);
    if (!d) fatal("设备 %d 不是触控设备", id);
    double m[9];
    memcpy(m, d->ctm, sizeof m);
    if (mode == 4 || mode == 0) {
        printf("设备 %d CTM: %.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f\n", id,
               m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
        return mode == 0 ? 2 : 0;
    }
    if (mode == 1) rect_to_ctm(0, 0, scr_w, scr_h, m);
    else if (mode == 2) rect_to_ctm(rx, ry, rw, rh, m);
    else if (mode == 3) rect_to_ctm(0, 0, scr_w, scr_h, m);
    set_ctm(id, m);
    double x0, y0, x1, y1;
    Dev tmp = *d; memcpy(tmp.ctm, m, sizeof m); tmp.has_ctm = 1;
    mapped_rect(&tmp, &x0, &y0, &x1, &y1);
    printf("已设置设备 %d：覆盖桌面 (%.0f,%.0f)-(%.0f,%.0f)\n", id, x0, y0, x1, y1);
    printf("注意：会话内的设置在注销/重启后会回到系统默认（例如 dde-daemon 会把触控屏\n"
           "      自动映射到主屏）。要长期生效，请在“控制中心 → 显示 → 触摸屏映射”里改。\n");
    return 0;
}

/* ---------------------------------------------------------------- buttons */

/* 监听真实鼠标按键（含 TouchViz 用 XTEST 合成的右键），带时间戳。
 * 用来验长按出右键的时序：不需要截图，只要看什么时候来了一个 button 3。 */
static int cmd_buttons(int argc, char **argv) {
    int want = 0, timeout = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) want = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) timeout = atoi(argv[++i]);
    }
    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawButtonRelease);
    XIEventMask em; em.deviceid = XIAllMasterDevices; em.mask_len = sizeof(mask); em.mask = mask;
    XISelectEvents(dpy, DefaultRootWindow(dpy), &em, 1);
    XFlush(dpy);
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    long t0 = now_ms();
    int press = 0, got = 0;
    while (!stop_flag) {
        if (timeout && now_ms() - t0 >= timeout * 1000L) break;
        if (want && got >= want) break;
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (ev.type != GenericEvent || ev.xcookie.extension != xi_opcode) continue;
            if (!XGetEventData(dpy, &ev.xcookie)) continue;
            int et = ev.xcookie.evtype;
            if (et == XI_RawButtonPress || et == XI_RawButtonRelease) {
                XIRawEvent *raw = (XIRawEvent *)ev.xcookie.data;
                int is_press = (et == XI_RawButtonPress);
                if (is_press) { press++; got++; }
                printf("BUTTON t=%ld btn=%d press=%d dev=%d\n",
                       now_ms() - t0, raw->detail, is_press, raw->sourceid);
                fflush(stdout);
            }
            XFreeEventData(dpy, &ev.xcookie);
        }
        struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
        poll(&pfd, 1, 100);
    }
    printf("SUMMARY press=%d\n", press);
    return 0;
}


/* ----------------------------------------------------------------- cursor */

/* 打印当前显示的光标信息。TouchViz 隐藏指针时会把光标换成 1x1 透明，
 * 所以用这个判断“触控期间指针是否真的没了”——根窗口截图里根本看不到光标。 */
static int cmd_cursor(void) {
    XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
    if (!ci) { printf("CURSOR none\n"); return 1; }
    long opaque = 0;
    for (int i = 0; i < ci->width * ci->height; i++)
        if ((ci->pixels[i] >> 24) > 128) opaque++;
    printf("CURSOR w=%d h=%d opaque=%ld hotspot=%d,%d\n",
           ci->width, ci->height, opaque, ci->xhot, ci->yhot);
    int hidden = (ci->width * ci->height <= 1) || opaque == 0;
    printf("SUMMARY hidden=%d\n", hidden);
    XFree(ci);
    return 0;
}

/* ---------------------------------------------------------------- latency */

/* 同一个时钟下同时打印「触控开始」与「真实按键」的时间戳。
 * 用来测长按多久才发出右键，不需要截图、也不依赖墙钟。 */
static int cmd_latency(int argc, char **argv) {
    int timeout = 15;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "-t") && i + 1 < argc) timeout = atoi(argv[++i]);
    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_RawTouchBegin);
    XISetMask(mask, XI_RawTouchEnd);
    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawButtonRelease);
    XIEventMask em; em.deviceid = XIAllMasterDevices; em.mask_len = sizeof(mask); em.mask = mask;
    XISelectEvents(dpy, DefaultRootWindow(dpy), &em, 1);
    XFlush(dpy);
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    long t0 = now_ms();
    int ntouch = 0, nbtn = 0;
    while (!stop_flag) {
        if (now_ms() - t0 >= timeout * 1000L) break;
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (ev.type != GenericEvent || ev.xcookie.extension != xi_opcode) continue;
            if (!XGetEventData(dpy, &ev.xcookie)) continue;
            int et = ev.xcookie.evtype;
            long t = now_ms() - t0;
            XIRawEvent *raw = (XIRawEvent *)ev.xcookie.data;
            if (et == XI_RawTouchBegin) {
                ntouch++;
                printf("TOUCH t=%ld kind=begin dev=%d slot=%d\n", t, raw->sourceid, raw->detail);
            } else if (et == XI_RawTouchEnd) {
                printf("TOUCH t=%ld kind=end dev=%d slot=%d\n", t, raw->sourceid, raw->detail);
            } else if (et == XI_RawButtonPress || et == XI_RawButtonRelease) {
                nbtn++;
                printf("BUTTON t=%ld btn=%d press=%d dev=%d\n", t, raw->detail,
                       et == XI_RawButtonPress, raw->sourceid);
            }
            fflush(stdout);
            XFreeEventData(dpy, &ev.xcookie);
        }
        struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
        poll(&pfd, 1, 100);
    }
    printf("SUMMARY touches=%d buttons=%d\n", ntouch, nbtn);
    return 0;
}

/* ------------------------------------------------------------ 指针 / 点击 */

static int cmd_move(int argc, char **argv) {
    /* 用 XTest 移动：和真鼠标一样走“设备 -> 原始事件”这条路
     * （XWarpPointer 是服务器端直接挪指针，很多地方看不到 raw motion） */
    if (argc < 2) fatal("move 需要 x y");
    XTestFakeMotionEvent(dpy, DefaultScreen(dpy), atoi(argv[0]), atoi(argv[1]), CurrentTime);
    XSync(dpy, False);
    int px, py; pointer_pos(&px, &py);
    printf("MOVE pointer=%d,%d\n", px, py);
    return 0;
}

static int cmd_click(int argc, char **argv) {  /* XTest 合成真实按键，和手指点击一样 */
    int btn = argc > 0 ? atoi(argv[0]) : 1;
    int ms  = argc > 1 ? atoi(argv[1]) : 60;
    XTestFakeButtonEvent(dpy, btn, True, CurrentTime);
    XSync(dpy, False);
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    XTestFakeButtonEvent(dpy, btn, False, CurrentTime);
    XSync(dpy, False);
    printf("CLICK btn=%d ms=%d\n", btn, ms);
    return 0;
}

/* ------------------------------------------------------------------ main */

static void usage(void) {
    fputs(
        "用法:\n"
        "  touchprobe geom\n"
        "  touchprobe devices\n"
        "  touchprobe expect <x> <y>\n"
        "  touchprobe watch [-n N] [-t SEC] [--tol PX] [-q]\n"
        "  touchprobe shot <out.ppm> [x y w h]\n"
        "  touchprobe diff <a.ppm> <b.ppm> [x y w h [thr]] [--centroid]\n"
        "  touchprobe ring <a.ppm> <x> <y> <pad> [--tol N] [--color r,g,b]\n"
        "  touchprobe finger [--fifo P] [--max-hz N]\n"
        "  touchprobe buttons [-n N] [-t SEC]\n"
        "  touchprobe latency [-t SEC]\n"
        "  touchprobe move <x> <y>\n"
        "  touchprobe click <button> [ms]\n"
        "  touchprobe cursor\n"
        "  touchprobe map [--dev ID] --full|--rect X Y W H|--reset|--show\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    const char *cmd = argv[1];
    open_xi();
    int r;
    if      (!strcmp(cmd, "geom"))    r = cmd_geom();
    else if (!strcmp(cmd, "devices")) r = cmd_devices();
    else if (!strcmp(cmd, "expect"))  r = cmd_expect(argc - 2, argv + 2);
    else if (!strcmp(cmd, "watch"))   r = cmd_watch(argc - 2, argv + 2);
    else if (!strcmp(cmd, "shot"))    r = cmd_shot(argc - 2, argv + 2);
    else if (!strcmp(cmd, "diff"))    r = cmd_diff(argc - 2, argv + 2);
    else if (!strcmp(cmd, "ring"))    r = cmd_ring(argc - 2, argv + 2);
    else if (!strcmp(cmd, "finger"))  r = cmd_finger(argc - 2, argv + 2);
    else if (!strcmp(cmd, "buttons")) r = cmd_buttons(argc - 2, argv + 2);
    else if (!strcmp(cmd, "latency")) r = cmd_latency(argc - 2, argv + 2);
    else if (!strcmp(cmd, "move"))    r = cmd_move(argc - 2, argv + 2);
    else if (!strcmp(cmd, "click"))   r = cmd_click(argc - 2, argv + 2);
    else if (!strcmp(cmd, "cursor"))  r = cmd_cursor();
    else if (!strcmp(cmd, "map"))     r = cmd_map(argc - 2, argv + 2);
    else usage();
    XCloseDisplay(dpy);
    return r;
}
