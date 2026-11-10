#!/bin/bash
# ============================================================================
# 用鼠标“当手指”：把左键按下 / 拖动 / 松开 转发给虚拟触控屏(mtinject)，
# 这样没有真触控屏的机器也能看触点圆 / 拖尾 / 长按进度环 / 长按右键。
#
# 用法:
#   tools/mouse_touch_test.sh          # 需要时会自动起虚拟触控屏和 TouchViz
#   tools/mouse_touch_test.sh --no-viz   # 只转发，不管 TouchViz
#
# 只做测试，不改 TouchViz 行为；Ctrl-C 结束后鼠标不会再产生任何触控。
# 便携性: 只依赖 libX11（touchprobe finger），不需要 xdotool / xinput / python。
# ============================================================================
set -u
. "$(dirname "$0")/lib.sh"

START_VIZ=1
[ "${1:-}" = "--no-viz" ] && START_VIZ=0

tv_need_probe

# 1) 虚拟触控屏：看 FIFO + 设备，而不是“有没有触控设备”
#    （后者在有真触控屏的机器上恒为真，会误以为注入器已经在跑）
if ! tv_has_inject; then
    echo "虚拟触控屏没在跑，先起一个……"
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1
    if ! tv_has_inject; then
        echo "起不来。手动跑 tools/start_mtinject.sh 看具体报错：" >&2
        echo "  （常见原因：/dev/uinput 不可写 → sudo setfacl -m u:$(id -un):rw /dev/uinput）" >&2
        exit 1
    fi
fi

# 2) TouchViz：不跑的话鼠标点了什么都看不到
if [ "$START_VIZ" = 1 ] && ! pgrep -x TouchViz >/dev/null; then
    echo "TouchViz 没在跑，先启一个（默认参数）……"
    tv_ctl start >/dev/null 2>&1
    sleep 0.5
    pgrep -x TouchViz >/dev/null || echo "  ⚠ TouchViz 没起来，只能看日志/不影响事件转发" >&2
fi

echo "现在用鼠标：左键按住=触点，拖动=拖尾，按住约 0.6s=右键，松开=淡出。Ctrl-C 退出。"
exec "$TV_PROBE" finger --fifo "$TV_FIFO"
