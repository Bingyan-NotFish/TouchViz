#!/bin/bash
# 用鼠标“当手指”：把左键按下 / 拖动 / 松开 转发给虚拟触控屏(mtinject)，
set -u
. "$(dirname "$0")/lib.sh"

# ---------- 1. 参数解析 ----------
usage() {
    cat <<'EOF'
用法: mouse_touch_test.sh [选项]
  把鼠标左键的按下/拖动/松开 转发给虚拟触控屏，用来体验触点/拖尾/长按。
  --no-viz    只转发鼠标事件，不启动 TouchViz 界面
  -h, --help  显示本帮助
EOF
}

START_VIZ=1
case "${1:-}" in
    --no-viz) START_VIZ=0 ;;
    -h|--help) usage; exit 0 ;;
esac

tv_need_probe

# ---------- 2. 检查虚拟触控屏 ----------
# 看 FIFO + 设备，而不是“有没有触控设备”
# （后者在有真触控屏的机器上恒为真，会误以为注入器已经在跑）
if ! tv_has_inject; then
    echo "虚拟触控屏没在跑，先起一个……"
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1
    if ! tv_has_inject; then
        echo "起不来。手动跑 tools/start_mtinject.sh 看具体报错：" >&2
        echo "  （常见原因：/dev/uinput 不可写 → sudo setfacl -m u:$(id -un):rw /dev/uinput）" >&2
        exit 1
    fi
fi

# ---------- 3. 检查 TouchViz 进程 ----------
# 不跑的话鼠标点了什么都看不到
if [ "$START_VIZ" = 1 ] && ! pgrep -x TouchViz >/dev/null; then
    echo "TouchViz 没在跑，先启一个（默认参数）……"
    tv_ctl start >/dev/null 2>&1
    sleep 0.5
    pgrep -x TouchViz >/dev/null || echo "  ⚠ TouchViz 没起来，只能看日志/不影响事件转发" >&2
fi

# ---------- 4. 启动鼠标转发 ----------
echo "现在用鼠标：左键按住=触点，拖动=拖尾，按住约 0.6s=右键，松开=淡出。Ctrl-C 退出。"
exec "$TV_PROBE" finger --fifo "$TV_FIFO"