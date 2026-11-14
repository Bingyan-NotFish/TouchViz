#!/bin/bash
# 单点位置精度验证：注入点位，截屏找触点圆，计算真实偏移。
set -u
. "$(dirname "$0")/lib.sh"

usage() {
    cat <<'EOF'
用法: verify_device.sh [TAG] [PX] [PY] [EX] [EY]
  注入一个点，截屏找触点圆，计算它与期望落点的真实偏移。
  TAG        输出标签（默认 POS）
  PX PY      注入坐标（默认按百分比 40% 30%）
  EX EY      期望落点（默认用设备矩阵算）
  -h, --help 显示本帮助
EOF
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

TAG="${1:-POS}"
read -r PX PY <<<"$(tv_at ${2:-40} ${3:-30})"  # 默认用百分比，也可传确切坐标
EX="${4:-}"
EY="${5:-}"

tv_need_probe

# ---------- 1.准备环境 ----------
if ! tv_has_inject; then
    "$TV_TOOLS/start_mtinject.sh" >/dev/null 2>&1 || { echo "没有触控设备" >&2; exit 1; }
fi
[ -p "$TV_FIFO" ] || { echo "FIFO 不在：先跑 tools/start_mtinject.sh" >&2; exit 1; }

if [ -z "$EX" ]; then
    # 没给期望点，就用设备的变换矩阵算一遍真实落点
    read -r EX EY <<<"$(tv_expect "$PX" "$PY")"
fi

pgrep -x TouchViz >/dev/null || tv_ctl start >/dev/null 2>&1
sleep 1.5 # 等待上一次的拖尾淡出

# ---------- 2.注入并截图 ----------
tv_shot "$TV_TMP/dev-before.ppm"
tv_inject "hold $PX $PY 900"
sleep 0.45
tv_shot "$TV_TMP/dev-during.ppm"
sleep 1.5

# ---------- 3.比对与找圆 ----------
D=$(tv_diff "$TV_TMP/dev-before.ppm" "$TV_TMP/dev-during.ppm" "$EX" "$EY" 160 25 --centroid)
R=$(tv_ring "$TV_TMP/dev-during.ppm" "$EX" "$EY" 100 || true)

echo "[$TAG] 注入=($PX,$PY)  期望=($EX,$EY)"
echo "       变化: $D"
echo "       触点: $R"

# ---------- 4.计算偏移 ----------
echo "$R" | awk -v ex="$EX" -v ey="$EY" -v tag="$TAG" '
/^RING none/ { print "       ✗ 未找到触点圆"; exit 1 }
/^RING/ {
    for (i = 1; i <= NF; i++) {
        if ($i ~ /^center=/) { split($i, a, "[=,]"); cx = a[2]; cy = a[3] }
    }
    dx = cx - ex; dy = cy - ey;
    printf("       %s 偏移 = (%.1f, %.1f) 像素%s\n", tag, dx, dy,
           (dx > -3 && dx < 3 && dy > -3 && dy < 3) ? "  ✓ 位置正确" : "  ✗ 有偏移");
}'