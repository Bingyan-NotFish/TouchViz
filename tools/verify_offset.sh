#!/bin/bash
# 触控偏移验证
set -u
. "$(dirname "$0")/lib.sh"

MODE=auto; TOL=3

usage() {
    cat <<'EOF'
用法: verify_offset.sh [选项]
  自动量偏移：注入 3 个点，用 watch 对比 TouchViz 位置与真实落点。
  --real      用真实触控屏测（人工点 3 下）
  --tol N     偏移阈值像素（默认 3）
  -h, --help  显示本帮助
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --real) MODE=real; shift ;;
        --tol)  TOL="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) shift ;;
    esac
done

tv_need_probe
read -r W H <<<"$(tv_screen)"
echo "会话: DISPLAY=$DISPLAY  桌面 ${W}x${H}"

# ---------- 1) 检查设备映射 ----------
echo "== 1) 检查设备映射 =="
DEVDUMP=$(tv_probe devices)
echo "$DEVDUMP" | grep -v '^STAT ' | sed 's/^/  /'
BADMAP=$(echo "$DEVDUMP" | grep '^STAT ' | awk -F'full=' '{split($2,a," "); if (a[1]=="0") n++} END{print n+0}')
if [ "$BADMAP" -gt 0 ]; then
    echo "  ⚠ 有 $BADMAP 台设备映射不是整个桌面，这就是偏移的元凶。"
else
    echo "  ✓ 所有设备都覆盖整个桌面。"
fi

# ---------- 2) 测量偏移 ----------
echo "== 2) 测量触点位置 =="
WLOG="$TV_TMP/verify_offset_watch.log"
: > "$WLOG"
RC=0

if [ "$MODE" = real ]; then
    echo "  请在屏幕上点 3 个位置（按住 0.2 秒）"
    tv_probe watch -n 3 -t 60 --tol "$TOL" 2>&1 | tee "$WLOG"
    RC=${PIPESTATUS[0]}
else
    if ! tv_has_inject; then
        "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || exit 1
    fi
    setsid "$TV_PROBE" watch -n 3 -t 40 --tol "$TOL" > "$WLOG" 2>&1 &
    WPID=$!
    sleep 0.6
    for pct in "10 15" "50 50" "88 85"; do
        set -- $pct
        read -r x y <<<"$(tv_at "$1" "$2")"
        tv_inject "hold $x $y 600"
        sleep 1.2
    done
    wait $WPID 2>/dev/null; RC=$?
    sed 's/^/  /' "$WLOG"
fi

SUM=$(grep '^SUMMARY' "$WLOG" 2>/dev/null | tail -1)
NTOT=$(echo "${SUM:-}" | sed -n 's/.*n=\([0-9]*\).*/\1/p'); NTOT=${NTOT:-0}
NBAD=$(echo "${SUM:-}" | sed -n 's/.*bad=\([0-9]*\).*/\1/p'); NBAD=${NBAD:-0}

# ---------- 3) 结论 ----------
if [ "$NTOT" -gt 0 ] && [ "$NBAD" = 0 ]; then
    echo "结论: ✓ TouchViz 位置一致。"
    exit 0
else
    echo "结论: ✗ 有 $NBAD 次触控偏移过大。"
    exit 1
fi