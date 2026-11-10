#!/bin/bash
# 安装 TouchViz：二进制 + 控制脚本 + 登录自启动（当前用户）
#   ./install.sh                全部安装
#   ./install.sh --no-autostart 只装程序，不写自启动项
# 注：不用 sudo 的部分（写自启动项）先做；需要 sudo 的部分失败只提示，不影响前者。
cd "$(dirname "$0")" || exit 1

# ---------- 1) 登录自启动（用户级，不需要 root） ----------
if [ "$1" != "--no-autostart" ]; then
    mkdir -p "$HOME/.config/autostart"
    cat > "$HOME/.config/autostart/TouchViz.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Touch Points (X11)
Name[zh_CN]=触控点显示与增强
Comment=Touch point circles, sliding trail and long-press ring on X11; long press = right click
Comment[zh_CN]=X11 下显示触点圆/滑动拖尾/长按进度环，长按出右键并隐藏鼠标指针
Exec=/usr/local/bin/TouchViz-ctl start
TryExec=/usr/local/bin/TouchViz-ctl
Icon=input-touchscreen
Terminal=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
X-GNOME-Autostart-Delay=3
DESKTOP
    echo "[install] 登录自启动: $HOME/.config/autostart/TouchViz.desktop"
else
    echo "[install] 按参数要求跳过自启动项"
fi

# ---------- 2) 编译 ----------
if ! make >/dev/null; then
    echo "[install] 编译失败：先确认装了 libxi-dev libcairo2-dev libxtst-dev libxfixes-dev libxext-dev"
    exit 1
fi
echo "[install] 已编译 TouchViz / TouchViz-ctl 依赖的 TouchViz / mtinject / touchprobe / touchcal"

# ---------- 3) 装到 /usr/local/bin（需要权限） ----------
if [ "$(id -u)" = 0 ]; then
    install -m755 TouchViz /usr/local/bin/TouchViz
    install -m755 TouchViz-ctl /usr/local/bin/TouchViz-ctl
    OK=1
elif sudo -n true 2>/dev/null; then
    sudo -n install -m755 TouchViz /usr/local/bin/TouchViz
    sudo -n install -m755 TouchViz-ctl /usr/local/bin/TouchViz-ctl
    OK=1
else
    echo "[install] 需要管理员权限写 /usr/local/bin，请在终端里执行下面两行（会提示输密码）："
    echo "          sudo install -m755 '$PWD/TouchViz'     /usr/local/bin/TouchViz"
    echo "          sudo install -m755 '$PWD/TouchViz-ctl' /usr/local/bin/TouchViz-ctl"
    OK=0
fi
[ "$OK" = 1 ] && echo "[install] 程序已安装: /usr/local/bin/TouchViz  /usr/local/bin/TouchViz-ctl"

# ---------- 4) 立刻启动一次（已经装了才做） ----------
if [ -x /usr/local/bin/TouchViz-ctl ]; then
    /usr/local/bin/TouchViz-ctl start
fi
echo
echo "常用: TouchViz-ctl start|stop|restart|status|toggle"
echo "没有触控屏？用鼠标当手指体验: ./tools/mouse_touch_test.sh   (左键=触点、拖动=拖尾、按住600ms=右键)"
echo "触控偏移排查: ./tools/verify_offset.sh     触摸标定: ./touchcal"
echo "卸载: ./uninstall.sh"
