// mtinject：虚拟多点触控屏（uinput），用于在 X11 下验证/复现触控。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <linux/uinput.h>
#include <X11/Xlib.h>

// ---------- 1. 全局变量与宏定义 ----------

#define ABSMAX 65535

static int SW = 1920, SH = 1080;      // 由 X 屏幕实际尺寸决定（可用 MTINJ_W/MTINJ_H 覆盖）
static int rmax = ABSMAX;             // 可用环境变量 MTINJ_RANGE 模拟不同厂商的量程
static int fd = -1;

// ---------- 2. 底层 uinput 操作 ----------

static void emit(int type, int code, int val) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = val;
    ssize_t n = write(fd, &ev, sizeof(ev));
    if (n < 0 && errno != EAGAIN) {
        perror("write /dev/uinput");
    }
}

static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

static inline int toabs(int px, int max) {
    if (px < 0) px = 0;
    if (px > max) px = max;
    return (int)((long)px * rmax / max);
}

static void msleep(int ms) {
    if (ms <= 0) return;
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// ---------- 3. 设备初始化 ----------

static void detect_screen(void) {
    const char *w = getenv("MTINJ_W"), *h = getenv("MTINJ_H");
    if (w && h && atoi(w) > 0 && atoi(h) > 0) {
        SW = atoi(w); SH = atoi(h);
    } else {
        Display *d = XOpenDisplay(NULL);      // root 下可能因 XAUTHORITY 失败，所以支持环境变量
        if (d) {
            SW = DisplayWidth(d, DefaultScreen(d));
            SH = DisplayHeight(d, DefaultScreen(d));
            XCloseDisplay(d);
        }
    }
    fprintf(stderr, "[mtinject] screen %dx%d, abs range 0..%d\n", SW, SH, rmax);
}

static void setup_device(void) {
    const char *r = getenv("MTINJ_RANGE");
    if (r && atoi(r) > 0) rmax = atoi(r);
    detect_screen();

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); exit(1); }

    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_DOUBLETAP);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_abs_setup abs;
    int codes[] = {ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_TRACKING_ID,
                   ABS_MT_POSITION_X, ABS_MT_POSITION_Y};
    for (unsigned i = 0; i < sizeof(codes)/sizeof(codes[0]); i++) {
        memset(&abs, 0, sizeof(abs));
        abs.code = codes[i];
        abs.absinfo.minimum = 0;
        abs.absinfo.maximum = (codes[i] == ABS_MT_SLOT) ? 9 :
                              (codes[i] == ABS_MT_TRACKING_ID) ? ABSMAX : rmax;
        ioctl(fd, UI_ABS_SETUP, &abs);
    }

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    strcpy(us.name, "TouchInject (virtual)");
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1ff7;
    us.id.product = 0x9999;
    us.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); exit(1); }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); exit(1); }
    msleep(1200); // 等 X server/libinput 热插拔
    fprintf(stderr, "[mtinject] virtual touchscreen ready\n");
}

// ---------- 4. 触控动作封装 ----------

static void touch_down(int slot, int id, int x, int y) {
    emit(EV_ABS, ABS_MT_SLOT, slot);
    emit(EV_ABS, ABS_MT_TRACKING_ID, id);
    emit(EV_ABS, ABS_MT_POSITION_X, toabs(x, SW));
    emit(EV_ABS, ABS_MT_POSITION_Y, toabs(y, SH));
    emit(EV_ABS, ABS_X, toabs(x, SW));
    emit(EV_ABS, ABS_Y, toabs(y, SH));
    emit(EV_KEY, BTN_TOUCH, 1);
    emit(EV_KEY, BTN_TOOL_FINGER, 1);
    syn();
}

static void touch_move(int slot, int x, int y) {
    emit(EV_ABS, ABS_MT_SLOT, slot);
    emit(EV_ABS, ABS_MT_POSITION_X, toabs(x, SW));
    emit(EV_ABS, ABS_MT_POSITION_Y, toabs(y, SH));
    emit(EV_ABS, ABS_X, toabs(x, SW));
    emit(EV_ABS, ABS_Y, toabs(y, SH));
    syn();
}

static void touch_up(int slot) {
    emit(EV_ABS, ABS_MT_SLOT, slot);
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    syn();
}

static void all_up(int nslots) {
    for (int s = 0; s < nslots; s++) touch_up(s);
    emit(EV_KEY, BTN_TOUCH, 0);
    emit(EV_KEY, BTN_TOOL_FINGER, 0);
    emit(EV_KEY, BTN_TOOL_DOUBLETAP, 0);
    syn();
}

// ---------- 5. 命令解析与主函数 ----------

static void do_tap(int x, int y) {
    touch_down(0, 1, x, y); msleep(80);
    touch_up(0); all_up(1); msleep(60);
}

static void do_hold(int x, int y, int ms) {
    touch_down(0, 1, x, y); msleep(ms);
    touch_up(0); all_up(1); msleep(60);
}

static void do_drag(int x1, int y1, int x2, int y2, int ms) {
    touch_down(0, 1, x1, y1); msleep(60);
    int steps = ms / 10;
    if (steps < 4) steps = 4;
    int step_ms = ms / steps;
    if (step_ms < 1) step_ms = 1;
    for (int i = 1; i <= steps; i++) {
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        touch_move(0, x, y); msleep(step_ms);
    }
    touch_up(0); all_up(1); msleep(60);
}

static void do_scroll(int x, int y, int step) {
    // 双指上下滑动：两根手指同向平移
    int d = (step >= 0) ? 1 : -1;
    int n = abs(step) * 6;
    if (n < 6) n = 6;
    touch_down(0, 1, x - 80, y);
    touch_down(1, 2, x + 80, y);
    msleep(40);
    for (int i = 1; i <= n; i++) {
        touch_move(0, x - 80, y - d * i * 12);
        touch_move(1, x + 80, y - d * i * 12);
        msleep(12);
    }
    touch_up(0); touch_up(1); all_up(2); msleep(60);
}

static void usage(const char *prog) {
    printf("用法: %s <命令> [参数...]\n"
           "  虚拟多点触控屏注入（uinput），也可由 start_mtinject.sh 调用。\n"
           "命令:\n"
           "  tap <x> <y>                   点击一下\n"
           "  hold <x> <y> <ms>             按住 ms 毫秒后松开\n"
           "  drag <x1> <y1> <x2> <y2> <ms> 从 (x1,y1) 拖到 (x2,y2)，耗时 ms\n"
           "  scroll <x> <y> <step>         双指上下滚动，step 正负决定方向\n"
           "  daemon                         从 stdin 循环读命令（tap/hold/drag/scroll/down/move/up/quit）\n"
           "环境变量:\n"
           "  MTINJ_W/MTINJ_H               覆盖屏幕尺寸（拿不到 X 时用）\n"
           "  MTINJ_RANGE                   覆盖绝对坐标量程（默认 65535）\n",
           prog);
}

int main(int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(argv[0]);
        return 0;
    }
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    setup_device();

    if (!strcmp(argv[1], "daemon")) {
        char line[256];
        while (fgets(line, sizeof(line), stdin)) {
            int a, b, c, d, e;
            if (sscanf(line, "tap %d %d", &a, &b) == 2) do_tap(a, b);
            else if (sscanf(line, "hold %d %d %d", &a, &b, &c) == 3) do_hold(a, b, c);
            else if (sscanf(line, "drag %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) do_drag(a, b, c, d, e);
            else if (sscanf(line, "scroll %d %d %d", &a, &b, &c) == 3) do_scroll(a, b, c);
            else if (sscanf(line, "down %d %d", &a, &b) == 2) touch_down(0, 1, a, b);
            else if (sscanf(line, "move %d %d", &a, &b) == 2) touch_move(0, a, b);
            else if (!strncmp(line, "up", 2)) { touch_up(0); all_up(1); }
            else if (!strncmp(line, "quit", 4)) break;
            else if (line[0] != '\n') fprintf(stderr, "? %s", line);
            fflush(stdout);
        }
    } else if (!strcmp(argv[1], "tap") && argc == 4) {
        do_tap(atoi(argv[2]), atoi(argv[3]));
    } else if (!strcmp(argv[1], "hold") && argc == 5) {
        do_hold(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    } else if (!strcmp(argv[1], "drag") && argc == 7) {
        do_drag(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
    } else if (!strcmp(argv[1], "scroll") && argc == 5) {
        do_scroll(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    } else {
        fprintf(stderr, "bad args\n");
        return 2;
    }

    msleep(300);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}