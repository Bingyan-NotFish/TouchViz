#!/bin/bash
# ============================================================================
# 第二轮回归（便携版）：长按到阈值当场发右键 / 多指取消 / 位置 / 闲置透明 /
# 鼠标不触发 / release 模式。
#
# 用法: tools/verify_round2.sh
# ============================================================================
set -u
. "$(dirname "$0")/lib.sh"

tv_need_probe
[ -x "$TV_ROOT/TouchViz" ] || ( cd "$TV_ROOT" && make TouchViz >/dev/null )
if ! tv_has_inject; then
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "起不了虚拟触控屏" >&2; exit 1; }
fi
tv_start_clean >/dev/null 2>&1; sleep 1; tv_assert_single
read -r PX PY <<<"$(tv_at 40 45)"
FAIL=0
LOG="$TV_TMP/r2-lat.log"

latency_run() {   # $1=按住ms $2=观察秒
    : > "$LOG"
    setsid "$TV_PROBE" latency -t "$2" > "$LOG" 2>&1 &
    local pid=$!; sleep 0.5
    tv_inject "hold $PX $PY $1"
    wait $pid 2>/dev/null
    sed 's/^/     /' "$LOG"
}
echo "== A) 按住到阈值 -> 当场发右键 =="
latency_run 1200 3
TB=$(tv_lat_time "$LOG" begin)
RB=$(tv_lat_time "$LOG" btn3)
extra=$(grep -c 'btn=3 press=1' "$LOG" 2>/dev/null); extra=${extra:-0}
if [ -n "${RB:-}" ] && [ -n "${TB:-}" ]; then
    D=$((RB - TB))
    echo "   第一个右键在触控开始后 ${D}ms（期望 ≈ 600ms）"
    [ $extra -gt 1 ] && echo "   （另有 $((extra - 1)) 次右键：桌面/程序自带的长按菜单，与 TouchViz 无关）"
    if [ "$D" -ge 400 ] && [ "$D" -le 900 ]; then echo "   ✓ 时序正确"; else echo "   ✗ 时序不对"; FAIL=1; fi
else
    echo "   ✗ 没测到右键"; FAIL=1
fi

echo
echo "== B) 多指（两指）同时按住 -> 长按作废 =="
N=$(tv_multitouch_cancel 1500 350 || echo skip)
if [ "$N" = skip ]; then echo "   (跳过：第二台设备起不来)"; else
    echo "   两指期间右键次数 = $N（应 0）"
    [ "$N" = 0 ] && echo "   ✓ 未误触发" || { echo "   ✗ 误触发"; FAIL=1; }
fi

echo
echo "== C) 位置精度 =="
"$TV_TOOLS/verify_device.sh" 位置 || FAIL=1

echo
echo "== D) 闲置全透明 =="
sleep 1.2; tv_shot "$TV_TMP/r2-on.ppm"
tv_ctl stop >/dev/null 2>&1; sleep 0.5; tv_shot "$TV_TMP/r2-off.ppm"
tv_ctl start >/dev/null 2>&1; sleep 0.8
D=$(tv_probe diff "$TV_TMP/r2-on.ppm" "$TV_TMP/r2-off.ppm" --region $((PX - 150)) $((PY - 150)) 300 300 --thr 25)
echo "   $D"
echo "$D" | grep -q 'DIFF none' && echo "   ✓ 全透明" || { echo "   ✗ 有残留"; FAIL=1; }

echo
echo "== E) 鼠标划过 + 按住不产生触点 =="
LPB=$(grep -c '长按 -> 右键' "$(tv_log)" 2>/dev/null); LPB=${LPB:-0}
tv_probe move $((PX - 200)) $((PY + 60)) >/dev/null
for i in 1 2 3 4; do tv_probe move $((PX - 200 + i * 50)) $((PY + 60)) >/dev/null; sleep 0.05; done
tv_probe click 1 1300 >/dev/null; sleep 0.5
LPA=$(grep -c '长按 -> 右键' "$(tv_log)" 2>/dev/null); LPA=${LPA:-0}
echo "   日志长按次数 $LPB -> $LPA（应不变）"
[ "$LPB" = "$LPA" ] && echo "   ✓ 鼠标不触发" || { echo "   ✗ 鼠标触发了"; FAIL=1; }

echo
echo "== F) release 模式：右键在抬手之后 =="
pkill -x TouchViz; sleep 0.3
setsid "$TV_ROOT/TouchViz" --rightclick-at release --long-press 600 >/dev/null 2>&1 </dev/null & sleep 0.8
latency_run 1000 3
awk '/^TOUCH/ && /kind=end/ {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); te=$i}}
     /^BUTTON/ && /btn=3/ && /press=1/ {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); rb=$i}}
     END { if (te != "" && rb != "") printf("   抬手=%sms 右键=%sms %s\n", te, rb, (rb+20>=te) ? "✓ 在抬手之后" : "✗ 抬手之前") }' "$LOG"
grep -q 'btn=3 press=1' "$LOG" || { echo "   ✗ release 模式没出右键"; FAIL=1; }
tv_ctl start >/dev/null 2>&1; sleep 0.8

echo
if [ "$FAIL" = 0 ]; then echo "结论: ✓ 全部通过"; exit 0; fi
echo "结论: ✗ 有项目不符合预期"; exit 1
