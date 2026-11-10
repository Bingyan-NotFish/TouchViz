#!/bin/bash
# ============================================================================
# 单点位置精度：注入（或手指按）某个位置后，触点在屏幕上画在哪里。
#
# 用法:
#   tools/verify_device.sh [标签] [注入命令] [期望x] [期望y]
#     tools/verify_device.sh
#     tools/verify_device.sh 右屏 "hold 2500 500 900"
#     tools/verify_device.sh 自定义 "hold 800 600 900" 400 300   # 手动给期望落点
#
# 不给期望落点时会用 touchprobe expect 按设备变换矩阵算出来（注入坐标 -> 真实落点）。
# 便携性: 不用 PIL / ImageMagick / kwin 截图 / xdotool；坐标和分辨率都现场算。
# ============================================================================
set -u
. "$(dirname "$0")/lib.sh"

tv_need_probe

TAG="${1:-POS}"
CMD="${2:-}"
EX="${3:-}"
EY="${4:-}"

if ! tv_has_inject; then
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "没有触控设备" >&2; exit 1; }
fi
[ -p "$TV_FIFO" ] || { echo "FIFO 不在：先跑 tools/start_mtinject.sh" >&2; exit 1; }

# 默认：在屏幕中间偏上注入一下
if [ -z "$CMD" ]; then
    read -r x y <<<"$(tv_at 40 30)"
    CMD="hold $x $y 900"
fi
# 从命令里取注入坐标
INJ=($(echo "$CMD" | awk '{for(i=2;i<=NF;i++) if ($i ~ /^[0-9]+$/) {print $i; if (++n==2) exit}}'))
[ -n "$EX" ] || {
    read -r EX EY <<<"$(tv_probe expect "${INJ[0]}" "${INJ[1]}" | sed -n 's/.*screen=\([0-9-]*\),\([0-9-]*\).*/\1 \2/p')"
}

pgrep -x TouchViz >/dev/null || tv_ctl start >/dev/null 2>&1
sleep 0.8
sleep 1.0                                             # 等上一次的淡出结束
tv_shot "$TV_TMP/dev-before.ppm"
tv_inject "$CMD"
sleep 0.45
tv_shot "$TV_TMP/dev-during.ppm"
sleep 1.5

D=$(tv_diff "$TV_TMP/dev-before.ppm" "$TV_TMP/dev-during.ppm" "$EX" "$EY" 160 25 --centroid)
R=$(tv_ring "$TV_TMP/dev-during.ppm" "$EX" "$EY" 100 || true)

echo "[$TAG] 注入=$CMD  期望落点=($EX,$EY)"
echo "       变化区域: $D"
echo "       触点圆  : $R"

echo "$R" | awk -v ex="$EX" -v ey="$EY" -v tag="$TAG" '
/^RING none/ { print "       ✗ 没有找到触点圆"; exit }
/^RING/ {
    for (i = 1; i <= NF; i++) {
        if ($i ~ /^center=/) { split($i, a, "[=,]"); cx = a[2]; cy = a[3] }
    }
    dx = cx - ex; dy = cy - ey;
    printf("       %s 偏移 = (%.1f, %.1f) 像素%s\n", tag, dx, dy,
           (dx > -3 && dx < 3 && dy > -3 && dy < 3) ? "  ✓ 位置正确" : "  ✗ 有偏移");
}'
