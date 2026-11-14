#!/bin/bash
# 视觉/行为测试：滑动拖尾、触控时隐藏指针、鼠标不触发
set -u
. "$(dirname "$0")/lib.sh"

usage() {
    cat <<'EOF'
用法: verify_visual.sh
  视觉/行为测试：滑动拖尾、触控时隐藏指针、鼠标不触发触点。
  -h, --help  显示本帮助
EOF
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

tv_need_probe
if ! tv_has_inject; then
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "没有触控设备" >&2; exit 1; }
fi

# 使用百分比坐标，适配所有屏幕分辨率
read -r W H <<<"$(tv_screen)"
read -r PX PY <<<"$(tv_at 40 60)" 
FAIL=0

# ---------- 1.滑动拖尾测试 ----------
echo "== 1) 滑动拖尾 =="
read -r X1 Y1 <<<"$(tv_at 30 60)"
read -r X2 Y2 <<<"$(tv_at 60 60)"
sleep 1.0
tv_shot "$TV_TMP/vis-before.ppm"

# 后台注入滑动，模拟拖尾
setsid bash -c "sleep 0.2; printf 'drag %d %d %d %d 1200\n' $X1 $Y1 $X2 $Y2 > '$TV_FIFO'" >/dev/null 2>&1
sleep 0.5
tv_shot "$TV_TMP/vis-during.ppm"

# 检查变化区域
D=$(tv_diff "$TV_TMP/vis-before.ppm" "$TV_TMP/vis-during.ppm" $((PX - 200)) $((PY - 200)) 400 25)
if echo "$D" | grep -q 'DIFF none'; then
    echo "   ✗ 拖动过程中没有绘制任何东西"
    FAIL=1
else
    echo "   ✓ 拖尾正常产生"
fi

# ---------- 2.触控时隐藏指针 ----------
echo "== 2) 触控时隐藏指针 =="
cursor_hidden() { tv_probe cursor 2>/dev/null | sed -n 's/.*SUMMARY hidden=//p'; }

# 启动一个无覆盖层的测试实例
pkill -x TouchViz; sleep 0.3
setsid "$TV_ROOT/TouchViz" --no-overlay --long-press 0 >/dev/null 2>&1 </dev/null &
sleep 0.8

before=$(cursor_hidden)
tv_inject "hold $PX $PY 1200"
sleep 0.5
during=$(cursor_hidden)
sleep 1.2
after=$(cursor_hidden)

if [ "$before" = "0" ] && [ "$during" = "1" ] && [ "$after" = "0" ]; then
    echo "   ✓ 指针隐藏与恢复正常"
else
    echo "   ✗ 指针状态异常 (前:$before 中:$during 后:$after)"
    FAIL=1
fi

# ---------- 3.鼠标不触发 ----------
echo "== 3) 鼠标不触发触点 =="
tv_ctl start >/dev/null 2>&1; sleep 0.8

# 模拟鼠标点击，看会不会生成触点圆
tv_probe click 1 1500 >/dev/null
sleep 0.4
tv_shot "$TV_TMP/vis-mouse.ppm"

# 寻找触点圆（特征颜色）
R=$(tv_ring "$TV_TMP/vis-mouse.ppm" "$PX" "$PY" 120 || true)
CNT=$(echo "$R" | sed -n 's/.*count=\([0-9]*\).*/\1/p'); CNT=${CNT:-0}

if [ "$CNT" -ge 100 ]; then
    echo "   ✗ 鼠标产生了触点 (检测到 $CNT 个像素)"
    FAIL=1
else
    echo "   ✓ 鼠标无触发 (检测到 $CNT 个像素，属背景噪声)"
fi

# ---------- 4.输出结果 ----------
echo
if [ "$FAIL" = 0 ]; then
    echo "结论: ✓ 视觉/行为全部符合预期"
    exit 0
else
    echo "结论: ✗ 有项目不符合预期"
    exit 1
fi