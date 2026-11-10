#!/bin/bash
# ============================================================================
# 全面回归（便携版）：稳定期/长按时序、位置精度、闲置全透明、拖尾、
# 隐藏指针、鼠标不触发、多指取消长按、资源占用。
#
# 用法: tools/verify_rewrite.sh
#
# 所有检查都不再依赖 KWin 的 qdbus 截图 / PIL / xdotool / 固定分辨率。
# 想看单一项目可以直接跑对应的分脚本（verify_offset / verify_lp / verify_visual）。
# ============================================================================
set -u
. "$(dirname "$0")/lib.sh"

tv_need_probe
[ -x "$TV_ROOT/TouchViz" ] || ( cd "$TV_ROOT" && make TouchViz >/dev/null )
if ! tv_has_inject; then
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "起不了虚拟触控屏" >&2; exit 1; }
fi
tv_start_clean >/dev/null 2>&1; sleep 1; tv_assert_single
read -r W H <<<"$(tv_screen)"
read -r PX PY <<<"$(tv_at 45 55)"
echo "桌面 ${W}x${H}，测试点 ($PX,$PY)"
FAIL=0

# ---- 1) 稳定期语义 + 长按时序（不截图，靠真实按键时间戳）-------------------
echo
echo "== 1) 稳定期 / 长按出右键时序 =="
run_latency() {   # $1=按住ms $2=观察秒（结果写到 $LG）
    LG="$TV_TMP/rw-lat.log"; : > "$LG"
    setsid "$TV_PROBE" latency -t "$2" > "$LG" 2>&1 &
    local pid=$!; sleep 0.5
    tv_inject "hold $PX $PY $1"
    wait $pid 2>/dev/null
    sed 's/^/     /' "$LG"
}
run_latency 130 2
if [ "$(tv_lat_btn3_count "$LG" 400)" -gt 0 ]; then echo "   ✗ 130ms 就出了右键"; FAIL=1; else echo "   ✓ 130ms（未过稳定期）不出右键"; fi
run_latency 420 2
if [ "$(tv_lat_btn3_count "$LG" 400)" -gt 0 ]; then echo "   ✗ 420ms 就出了右键（应到 600ms）"; FAIL=1; else echo "   ✓ 420ms（还在进度中）不出右键"; fi
run_latency 1000 3
TB=$(tv_lat_time "$LG" begin); RB=$(tv_lat_time "$LG" btn3)
if [ -n "${RB:-}" ] && [ -n "${TB:-}" ]; then
    D=$((RB - TB)); echo "   右键在触控开始后 ${D}ms（期望 ≈ 600ms）"
    if [ "$D" -ge 400 ] && [ "$D" -le 900 ]; then echo "   ✓ 时序正确"; else echo "   ✗ 时序不对"; FAIL=1; fi
else
    echo "   ✗ 1000ms 没出右键"; FAIL=1
fi

# ---- 2) 位置精度 -----------------------------------------------------------
echo
echo "== 2) 位置精度 =="
"$TV_TOOLS/verify_device.sh" 位置 || FAIL=1

# ---- 3) 闲置全透明 ---------------------------------------------------------
echo
echo "== 3) 闲置全透明 =="
sleep 1.2
tv_shot "$TV_TMP/rw-on.ppm"
tv_ctl stop >/dev/null 2>&1; sleep 0.5
tv_shot "$TV_TMP/rw-off.ppm"
tv_ctl start >/dev/null 2>&1; sleep 0.8
D=$(tv_probe diff "$TV_TMP/rw-on.ppm" "$TV_TMP/rw-off.ppm" --region $((PX - 200)) $((PY - 200)) 400 400 --thr 25)
echo "   $D"
echo "$D" | grep -q 'DIFF none' && echo "   ✓ 空闲时完全透明" || { echo "   ✗ 空闲时还有残留绘制"; FAIL=1; }

# ---- 4) 拖尾 ---------------------------------------------------------------
echo
echo "== 4) 滑动拖尾 =="
read -r X1 Y1 <<<"$(tv_at 25 55)"; read -r X2 Y2 <<<"$(tv_at 55 55)"
read -r EX1 EY1 <<<"$(tv_expect "$X1" "$Y1")"; read -r EX2 EY2 <<<"$(tv_expect "$X2" "$Y2")"
sleep 1.2; tv_shot "$TV_TMP/rw-tr-b.ppm"
setsid bash -c "sleep 0.2; printf 'drag %d %d %d %d 1200\n' $X1 $Y1 $X2 $Y2 > '$TV_FIFO'" >/dev/null 2>&1
sleep 0.5; tv_shot "$TV_TMP/rw-tr-a.ppm"; sleep 1.5
RX=$(( EX1 < EX2 ? EX1 : EX2 )); RXX=$(( EX1 > EX2 ? EX1 : EX2 ))
D=$(tv_probe diff "$TV_TMP/rw-tr-b.ppm" "$TV_TMP/rw-tr-a.ppm" --region "$RX" $((EY1 - 70)) $((RXX - RX)) 140 --thr 25)
WD=$(echo "$D" | awk -F'bbox=' '/bbox=/{split($2,a,","); print a[3]-a[1]}')
echo "   $D"
[ "${WD:-0}" -gt 100 ] && echo "   ✓ 拖尾长度 ${WD}px" || { echo "   ✗ 拖尾缺失/过短"; FAIL=1; }

# ---- 5) 隐藏指针 -----------------------------------------------------------
echo
echo "== 5) 触控时隐藏指针 =="
cursor_hidden() { tv_probe cursor 2>/dev/null | sed -n 's/.*SUMMARY hidden=//p'; }
hide_run() {
    pkill -x TouchViz; sleep 0.3
    setsid "$TV_ROOT/TouchViz" --no-overlay --long-press 0 $1 >/dev/null 2>&1 </dev/null & sleep 0.8
    tv_probe move "$PX" "$PY" >/dev/null; sleep 0.2
    local b d a
    b=$(cursor_hidden)
    tv_inject "hold $PX $PY 1200"; sleep 0.5
    d=$(cursor_hidden)
    sleep 1.2
    a=$(cursor_hidden)
    echo "$b $d $a"
}
H1=$(hide_run "" | tail -1)
H2=$(hide_run "--no-hide-cursor" | tail -1)
echo "   默认      (前 中 后) = $H1"
echo "   关隐藏    (前 中 后) = $H2"
set -- $H1
if [ "$1" = 0 ] && [ "$2" = 1 ] && [ "$3" = 0 ]; then echo "   ✓ 触控时指针隐藏、抬手恢复"
else echo "   ✗ 指针隐藏行为不对"; FAIL=1; fi
set -- $H2
[ "$2" = 0 ] && echo "   ✓ --no-hide-cursor 对照正常" || { echo "   ✗ 对照异常"; FAIL=1; }
tv_start_clean >/dev/null 2>&1; sleep 0.8

# ---- 6) 鼠标完全不触发 ------------------------------------------------------
echo
echo "== 6) 鼠标不触发触点 =="
sleep 1.0; tv_shot "$TV_TMP/rw-m-b.ppm"
tv_probe move $((PX - 250)) $((PY + 50)) >/dev/null
for i in 1 2 3 4 5; do tv_probe move $((PX - 250 + i * 50)) $((PY + 50)) >/dev/null; sleep 0.05; done
tv_probe click 1 1500 >/dev/null
sleep 0.6; tv_shot "$TV_TMP/rw-m-a.ppm"
D=$(tv_probe diff "$TV_TMP/rw-m-b.ppm" "$TV_TMP/rw-m-a.ppm" --region $((PX - 300)) $((PY - 30)) 700 160 --thr 25)
echo "   $D"
LPS=$(grep -c '长按 -> 右键' "$(tv_log)" 2>/dev/null); LPS=${LPS:-0}
echo "   日志里长按次数 = $LPS（鼠标按 1.5s 不应增加，注意 1) 里已经有过触控）"

# ---- 7) 两指同时按 -> 长按作废 ----------------------------------------------
echo
echo "== 7) 两指同时按，长按应作废 =="
N=$(tv_multitouch_cancel 1500 350 || echo "skip")
if [ "$N" = skip ]; then
    echo "   (跳过：第二台虚拟设备起不来)"
else
    echo "   两指期间右键次数 = $N（应为 0）"
    [ "$N" = 0 ] && echo "   ✓ 多指时没有误触发右键" || { echo "   ✗ 多指也触发了右键"; FAIL=1; }
fi

# ---- 8) 资源占用 -----------------------------------------------------------
echo
echo "== 8) 空闲资源占用 =="
tv_start_clean >/dev/null 2>&1; sleep 1; tv_assert_single
"$TV_TOOLS/measure.sh" TouchViz 5 空闲 | sed 's/^/   /'

echo
if [ "$FAIL" = 0 ]; then echo "结论: ✓ 全部通过"; exit 0; fi
echo "结论: ✗ 有项目不符合预期"; exit 1
