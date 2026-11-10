#!/bin/bash
# ============================================================================
# 长按语义 + 出右键时序：
#   • 短按（未过稳定期）不该出右键
#   • 按住到阈值（默认 600ms）应准时出一次右键
#
# 用 touchprobe latency 在同一个时钟下记录「触控开始/结束」和「真实按键」，
# 不需要截图、不需要 python、不依赖屏幕内容 —— 任何桌面上都能量。
#
# 注意：某些桌面（或某些程序）自己也会在长按时合成右键，
# 所以这里只认“触控开始后 ${window}ms 内”的第一次右键，并会报告多出来的次数。
#
# 用法: tools/verify_lp.sh [总阈值ms] [稳定期ms]
# ============================================================================
set -u
. "$(dirname "$0")/lib.sh"

LP="${1:-600}"
ARM="${2:-200}"
tv_need_probe

if ! tv_has_inject; then
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "没有触控设备" >&2; exit 1; }
fi

# 用指定参数干净地重启 TouchViz（保证阈值就是我们要测的）
( export LONG_PRESS="$LP" LP_ARM="$ARM"; tv_start_clean >/dev/null 2>&1 )
tv_assert_single || exit 1

read -r PX PY <<<"$(tv_at 45 40)"
LOG="$TV_TMP/verify_lp.log"

run_latency() {   # $1=按住毫秒 $2=观察秒
    : > "$LOG"
    setsid "$TV_PROBE" latency -t "$2" > "$LOG" 2>&1 &
    local pid=$!
    sleep 0.5
    tv_inject "hold $PX $PY $1"
    wait $pid 2>/dev/null
    sed 's/^/     /' "$LOG"
}

FAIL=0
WINDOW=$((LP + 300))

echo "== 1) 短按 ${ARM}ms 以内（按住 $((ARM - 20))ms）不应出右键 =="
run_latency $((ARM - 20)) 2
CNT=$(tv_lat_btn3_count "$LOG" $((ARM + 150)))
if [ "$CNT" -gt 0 ]; then
    echo "   ✗ 短按也出了 $CNT 次右键"; FAIL=1
else
    echo "   ✓ 短按没有右键"
fi

echo
echo "== 2) 按住 $((LP + 400))ms：应在约 ${LP}ms 时出一次右键 =="
run_latency $((LP + 400)) 3
TB=$(tv_lat_time "$LOG" begin)
RB=$(tv_lat_time "$LOG" btn3)
TOTAL=$(grep -c 'btn=3 press=1' "$LOG" 2>/dev/null); TOTAL=${TOTAL:-0}
if [ -n "${RB:-}" ] && [ -n "${TB:-}" ]; then
    DELAY=$((RB - TB))
    echo "   第一次右键在触控开始后 ${DELAY}ms（期望 ≈ ${LP}ms，允许 ±150ms）"
    if [ "$DELAY" -ge $((LP - 150)) ] && [ "$DELAY" -le $((LP + 150)) ]; then
        echo "   ✓ 时序正确"
    else
        echo "   ✗ 时序不对"; FAIL=1
    fi
    [ "$TOTAL" -gt 1 ] && echo "   （另外还收到 $((TOTAL - 1)) 次右键，多半是桌面/程序自带的长按菜单，与 TouchViz 无关）"
else
    echo "   ✗ 没测到右键（或没测到触控开始）"; FAIL=1
fi

echo
if [ "$FAIL" = 0 ]; then echo "结论: ✓ 长按语义与时序都正确"; exit 0; fi
echo "结论: ✗ 长按行为不符合预期"; exit 1
