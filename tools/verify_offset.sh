#!/bin/bash
# ============================================================================
# 触控偏移验证（核心工具，便携版）
#
#   tools/verify_offset.sh                 # 自动：用虚拟触控屏在 3 个位置注入，量偏移
#   tools/verify_offset.sh --real          # 真触控屏：按提示在屏幕上点 3 下
#   tools/verify_offset.sh --visual        # 再加截图交叉验证（触点圆 vs 指针）
#   tools/verify_offset.sh --fix           # 把“只覆盖部分桌面”的设备改成覆盖整个桌面
#   tools/verify_offset.sh --tol 5         # 允许的偏差像素（默认 3）
#
# 判断依据（正常路径不需要任何截图）：
#   触控落下时 X 会把指针移到落点，所以
#     ① TouchViz 用事件里的 valuator 算出的位置 == 指针位置 → TouchViz 没偏移
#     ② 设备映射矩形是否覆盖整个桌面                    → 系统层面有没有偏移
#   两者任一不成立，就是真实可感知的“触控偏移”。
#
# 可移植性：不写死 DISPLAY/分辨率/坐标，不需要 python/PIL/xdotool/xinput/qdbus/root。
# ============================================================================
set -u
. "$(dirname "$0")/lib.sh"

MODE=auto; VISUAL=0; FIX=0; TOL=3
while [ $# -gt 0 ]; do
    case "$1" in
        --real)   MODE=real; shift ;;
        --auto)   MODE=auto; shift ;;
        --visual) VISUAL=1; shift ;;
        --fix)    FIX=1; shift ;;
        --tol)    TOL="$2"; shift 2 ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

tv_need_probe
read -r W H <<<"$(tv_screen)"
[ -n "${W:-}" ] || { echo "连不上 X（DISPLAY='${DISPLAY:-}'）" >&2; exit 1; }
echo "会话: DISPLAY=$DISPLAY  桌面 ${W}x${H}"

# 自动模式：先把虚拟触控屏准备好，否则连设备都看不到
if [ "$MODE" = auto ] && ! tv_has_inject; then
    echo "没有触控设备，先起虚拟触控屏……"
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "起不来，手动跑 tools/start_mtinject.sh 看报错" >&2; exit 1; }
fi

# ---- 1. 设备与映射 ---------------------------------------------------------
echo
echo "== 1) 设备映射（系统层面）=="
DEVDUMP=$(tv_probe devices)
echo "$DEVDUMP" | grep -v '^STAT ' | sed 's/^/  /'
STATS=$(echo "$DEVDUMP" | grep '^STAT ' || true)
if [ -z "$STATS" ]; then
    echo "  没有触控设备。接一块触控屏，或先跑 tools/start_mtinject.sh 起虚拟设备" >&2
    exit 1
fi
BADMAP=$(echo "$STATS" | awk -F'full=' '{split($2,a," "); if (a[1]=="0") n++} END{print n+0}')
if [ "$BADMAP" -gt 0 ]; then
    echo "  ⚠ 有 $BADMAP 台设备的映射矩形不是整个桌面 —— 手指在别处时落点会跑到映射区里，"
    echo "    这就是最常见的“触控偏移”。加 --fix 可在本会话内改成覆盖整个桌面。"
    if [ "$FIX" = 1 ]; then
        for id in $(echo "$STATS" | awk '/full=0/{for(i=1;i<=NF;i++) if ($i ~ /^dev=/) {split($i,a,"="); print a[2]}}'); do
            tv_probe map --dev "$id" --full
        done
        echo "  ---- 改完后 ----"
        tv_probe devices | grep -v '^STAT ' | sed 's/^/  /'
        BADMAP=0
    fi
else
    echo "  ✓ 所有触控设备都覆盖整个桌面（系统层面没有缩放/平移造成的偏移）"
fi

# ---- 2. 偏移测量 -----------------------------------------------------------
echo
echo "== 2) 触点位置 vs 指针落点（TouchViz 公式是否与真实落点一致）=="
WLOG="$TV_TMP/verify_offset_watch.log"
: > "$WLOG"
RC=0

if [ "$MODE" = real ]; then
    echo "  请在屏幕上点 3 个位置：左上角附近、正中间、右下角附近（每次按住 0.2 秒即可）"
    tv_probe watch -n 3 -t 60 --tol "$TOL" 2>&1 | tee "$WLOG"
    RC=${PIPESTATUS[0]}
else
    if ! tv_has_inject; then
        "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "  没有触控设备，也起不来" >&2; exit 1; }
    fi
    setsid "$TV_PROBE" watch -n 3 -t 40 --tol "$TOL" > "$WLOG" 2>&1 &
    WPID=$!
    sleep 0.6
    for pct in "10 15" "50 50" "88 85"; do
        set -- $pct
        read -r x y <<<"$(tv_at "$1" "$2")"
        EXP=$(tv_probe expect "$x" "$y" | sed -n 's/.*screen=\(.*\)/\1/p')
        echo "  注入 ($x,$y) → 预期落点 ($EXP)"
        tv_inject "hold $x $y 600"
        sleep 1.2
    done
    wait $WPID 2>/dev/null; RC=$?
    sed 's/^/  /' "$WLOG"
fi

SUM=$(grep '^SUMMARY' "$WLOG" 2>/dev/null | tail -1)
NTOT=$(echo "${SUM:-}" | sed -n 's/.*n=\([0-9]*\).*/\1/p')
NBAD=$(echo "${SUM:-}" | sed -n 's/.*bad=\([0-9]*\).*/\1/p')
NTOT=${NTOT:-0}; NBAD=${NBAD:-0}

# ---- 3. 可选：截图交叉验证 -------------------------------------------------
if [ "$VISUAL" = 1 ]; then
    echo
    echo "== 3) 截图交叉验证（触点圆中心 vs 预期落点）=="
    pgrep -x TouchViz >/dev/null || { echo "  起 TouchViz……"; tv_ctl start >/dev/null 2>&1; }
    sleep 0.8
    for pct in "30 30" "70 70"; do
        set -- $pct
        read -r x y <<<"$(tv_at "$1" "$2")"
        read -r ex ey <<<"$(tv_probe expect "$x" "$y" | sed -n 's/.*screen=\(-\{0,1\}[0-9]*\),\(-\{0,1\}[0-9]*\).*/\1 \2/p')"
        sleep 1.0
        tv_shot "$TV_TMP/off-before.ppm"
        tv_inject "hold $x $y 1200"
        sleep 0.6
        tv_shot "$TV_TMP/off-during.ppm"
        D=$(tv_diff "$TV_TMP/off-before.ppm" "$TV_TMP/off-during.ppm" "$ex" "$ey" 140 25 --centroid)
        R=$(tv_ring "$TV_TMP/off-during.ppm" "$ex" "$ey" 90 || true)
        echo "  注入 ($x,$y) 预期落点 ($ex,$ey)"
        echo "    变化区域: $D"
        echo "    触点圆  : $R"
        sleep 1.2
    done
fi

# ---- 4. 结论 ---------------------------------------------------------------
echo
if [ "$NTOT" -gt 0 ] && [ "$NBAD" = 0 ]; then
    echo "结论: ✓ TouchViz 算出的触点位置与真实落点一致（≤${TOL}px），TouchViz 本身没有偏移。"
    if [ "${BADMAP:-0}" -gt 0 ]; then
        echo "      但系统把触控设备只映射到了桌面的一部分 → 手指与落点会整体错位。"
        echo "      修: touchprobe map --full（本会话生效），或到“控制中心 → 显示 → 触摸屏映射”长期修改。"
    fi
    exit 0
elif [ "$NTOT" -gt 0 ]; then
    echo "结论: ✗ 有 $NBAD 次触控的触点位置与指针落点差 >${TOL}px —— 这才是 TouchViz 的坐标问题。"
    exit 1
else
    echo "结论: ? 没测到触控事件（设备没反应，或事件被别的程序抓走了）。"
    exit 2
fi
