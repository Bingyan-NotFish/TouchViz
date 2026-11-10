#!/bin/bash
# ============================================================================
# 视觉/行为回归：拖尾、触控时隐藏指针、鼠标完全不触发、空闲全透明。
#
# 用法: tools/verify_visual.sh
#
# 全部用 touchprobe 内建的 XGetImage 截图 + PPM 比对，不依赖 PIL / ImageMagick /
# kwin 截图 / xdotool。坐标按屏幕百分比取，任何分辨率都能跑。
# ============================================================================
set -u
. "$(dirname "$0")/lib.sh"

tv_need_probe
if ! tv_has_inject; then
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "没有触控设备" >&2; exit 1; }
fi
read -r W H <<<"$(tv_screen)"
read -r PX PY <<<"$(tv_at 40 60)"        # 安静区域
B="$TV_TMP/vis-before.ppm" ; A="$TV_TMP/vis-during.ppm"
FAIL=0

shot() { tv_shot "$1"; }
diff_at() { tv_diff "$1" "$2" "$3" "$4" "${5:-80}" "${6:-25}" "${7:-}"; }

# ---------------------------------------------------------------- 0) 基线
echo "== 0) 基线：不碰屏幕时覆盖层应完全透明 =="
REG="--region $((PX - 200)) $((PY - 200)) 400 400 --thr 25"
# 先量这块区域自己会不会动（时钟、动画、光标闪烁…），会动就不下结论，避免误判
tv_ctl stop >/dev/null 2>&1; sleep 0.6
shot "$A"; sleep 0.5; shot "$B"
NOISE=$(tv_probe diff "$A" "$B" $REG)
if echo "$NOISE" | grep -q 'DIFF none'; then
    tv_ctl start >/dev/null 2>&1; sleep 1.0
    shot "$B"
    BASE=$(tv_probe diff "$B" "$A" $REG)
    echo "   TouchViz 运行/停止 在该区域的差异: $BASE"
    if echo "$BASE" | grep -q 'DIFF none'; then
        echo "   ✓ 空闲时覆盖层完全透明"
    else
        echo "   ✗ 空闲时不是全透明（会看到覆盖层）"; FAIL=1
    fi
else
    echo "   ⚠ 该区域背景自己在变化（$NOISE），跳过基线判断"
fi
tv_ctl start >/dev/null 2>&1; sleep 0.8

# ---------------------------------------------------------------- 1) 拖尾
echo
echo "== 1) 滑动拖尾 =="
read -r X1 Y1 <<<"$(tv_at 30 60)"
read -r X2 Y2 <<<"$(tv_at 60 60)"
read -r EX1 EY1 <<<"$(tv_expect "$X1" "$Y1")"     # 真实落点（设备变换矩阵可能把注入点平移/缩放）
read -r EX2 EY2 <<<"$(tv_expect "$X2" "$Y2")"
sleep 1.0
shot "$B"
setsid bash -c "sleep 0.2; printf 'drag %d %d %d %d 1200\n' $X1 $Y1 $X2 $Y2 > '$TV_FIFO'" >/dev/null 2>&1
sleep 0.5
shot "$A"
RX=$(( EX1 < EX2 ? EX1 : EX2 )); RXX=$(( EX1 > EX2 ? EX1 : EX2 ))
D=$(tv_probe diff "$B" "$A" --region "$RX" $((EY1 - 70)) $((RXX - RX)) 140 --thr 25)
echo "   $D"
W_DRAWN=$(echo "$D" | awk -F'bbox=' '/bbox=/{split($2,a,","); print a[3]-a[1]}')
if echo "$D" | grep -q 'DIFF none'; then
    echo "   ✗ 拖动过程中什么都没画"; FAIL=1
elif [ "${W_DRAWN:-0}" -gt 100 ]; then
    echo "   ✓ 有拖尾（画出 ${W_DRAWN}px，拖动实际跨度 $((RXX - RX))px）"
else
    echo "   ✗ 拖尾太短（${W_DRAWN}px）"; FAIL=1
fi
sleep 1.5

# ---------------------------------------------------------------- 2) 隐藏指针
echo
echo "== 2) 触控时隐藏指针（看光标状态，根窗口截图里本来就没有光标）=="
cursor_hidden() { tv_probe cursor 2>/dev/null | sed -n 's/.*SUMMARY hidden=//p'; }

test_hide() {   # $1=额外参数（空 或 --no-hide-cursor）
    pkill -x TouchViz; sleep 0.3
    setsid "$TV_ROOT/TouchViz" --no-overlay --long-press 0 $1 >/dev/null 2>&1 </dev/null &
    sleep 0.8
    tv_probe move "$PX" "$PY" >/dev/null; sleep 0.2
    local before during after
    before=$(cursor_hidden)
    tv_inject "hold $PX $PY 1200"
    sleep 0.5
    during=$(cursor_hidden)
    sleep 1.2
    after=$(cursor_hidden)
    echo "   触控前=$before  触控中=$during  抬手后=$after" >&2
    echo "$before $during $after"
}

R1=$(test_hide "")
D1=$(echo "$R1" | tail -1)
R2=$(test_hide "--no-hide-cursor")
D2=$(echo "$R2" | tail -1)
echo "   默认:     触控前/中/后 hidden = $D1"
echo "   关隐藏:   触控前/中/后 hidden = $D2"
echo
set -- $D1
if [ "$2" = "1" ] && [ "$1" = "0" ] && [ "$3" = "0" ]; then
    echo "   ✓ 默认：触控期间指针隐藏，抬手恢复"
else
    echo "   ✗ 默认：指针隐藏行为不对"; FAIL=1
fi
set -- $D2
if [ "$2" = "0" ]; then
    echo "   ✓ --no-hide-cursor：触控期间指针保持显示（对照）"
else
    echo "   ✗ 对照反而隐藏了"; FAIL=1
fi

# ---------------------------------------------------------------- 3) 鼠标不触发
echo
echo "== 3) 鼠标只动/按住，不应产生触点 =="
tv_ctl start >/dev/null 2>&1; sleep 0.8
LPBEFORE=$(grep -c '长按 -> 右键' "$(tv_log)" 2>/dev/null); LPBEFORE=${LPBEFORE:-0}
read -r MX MY <<<"$(tv_at 40 62)"          # 在这附近划动 + 按住 1.5 秒
tv_probe move "$MX" "$MY" >/dev/null; sleep 0.2
for i in 1 2 3 4 5 6; do
    tv_probe move $((MX - 120 + i * 40)) "$MY" >/dev/null
    sleep 0.06
done
tv_probe move "$MX" "$MY" >/dev/null
tv_probe click 1 1500 >/dev/null
sleep 0.4
shot "$A"
R=$(tv_ring "$A" "$MX" "$MY" 120 || true)
CNT=$(echo "$R" | sed -n 's/.*count=\([0-9]*\).*/\1/p'); CNT=${CNT:-0}
LPAFTER=$(grep -c '长按 -> 右键' "$(tv_log)" 2>/dev/null); LPAFTER=${LPAFTER:-0}
echo "   该处触点圆检测: $R"
echo "   长按右键次数: $LPBEFORE -> $LPAFTER（应不变）"
[ "$LPBEFORE" = "$LPAFTER" ] || { echo "   ✗ 鼠标按住触发了长按"; FAIL=1; }
# 触点圆是槽位 0 的亮青色，比“整区域有没有变化”可靠得多（背景噪声不影响）
if [ "$CNT" -ge 100 ]; then
    echo "   ✗ 鼠标位置出现了 $CNT 个触点圆像素"; FAIL=1
else
    echo "   ✓ 鼠标没有画出触点圆（检测到 $CNT 个相近像素，属背景噪声）"
fi

echo
if [ "$FAIL" = 0 ]; then echo "结论: ✓ 视觉/行为全部符合预期"; exit 0; fi
echo "结论: ✗ 有项目不符合预期（见上面的 ✗）"; exit 1
