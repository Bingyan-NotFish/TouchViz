#!/bin/bash
# 长按语义 + 出右键时序测试
set -u
. "$(dirname "$0")/lib.sh"

usage() {
    cat <<'EOF'
用法: verify_lp.sh [LP] [ARM]
  长按语义 + 出右键时序测试。
  LP         长按总时长 ms（默认 600）
  ARM        稳定期 ms（默认 200）
  -h, --help 显示本帮助
EOF
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

LP="${1:-600}"
ARM="${2:-200}"
tv_need_probe

if ! tv_has_inject; then
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "无触控设备" >&2; exit 1; }
fi

# ---------- 1.初始化与准备 ----------
( export LONG_PRESS="$LP" LP_ARM="$ARM"; tv_start_clean >/dev/null 2>&1 )
tv_assert_single || exit 1
read -r PX PY <<<"$(tv_at 45 40)"
LOG="$TV_TMP/verify_lp.log"
FAIL=0

run_latency() {
    : > "$LOG"
    setsid "$TV_PROBE" latency -t "$2" > "$LOG" 2>&1 &
    local pid=$!
    sleep 0.5
    tv_inject "hold $PX $PY $1"
    wait $pid 2>/dev/null
}

# ---------- 2.短按测试 ----------
echo "== 1) 短按 =="
run_latency $((ARM - 20)) 2
CNT=$(tv_lat_btn3_count "$LOG" $((ARM + 150)))
if [ "$CNT" -gt 0 ]; then
    echo "   ✗ 短按误触发右键"; FAIL=1
else
    echo "   ✓ 短按正常"
fi

# ---------- 3.长按时序测试 ----------
echo "== 2) 长按时序 =="
run_latency $((LP + 400)) 3
TB=$(tv_lat_time "$LOG" begin)
RB=$(tv_lat_time "$LOG" btn3)
if [ -n "${RB:-}" ] && [ -n "${TB:-}" ]; then
    DELAY=$((RB - TB))
    if [ "$DELAY" -ge $((LP - 150)) ] && [ "$DELAY" -le $((LP + 150)) ]; then
        echo "   ✓ 时序正确 (${DELAY}ms)"
    else
        echo "   ✗ 时序不对 (${DELAY}ms)"; FAIL=1
    fi
else
    echo "   ✗ 没测到右键"; FAIL=1
fi

# ---------- 4.结果 ----------
[ "$FAIL" = 0 ] && echo "结论: ✓ 通过" || echo "结论: ✗ 失败"
exit "$FAIL"