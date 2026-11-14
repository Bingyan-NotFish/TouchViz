#!/bin/bash
# 起虚拟多点触控屏（常驻），支持起第 2 台设备用于多指测试。
set -u
. "$(dirname "$0")/lib.sh"

RANGE="${MTINJ_RANGE:-65535}"
KEEP_MAP="${KEEP_MAP:-0}"
SECOND=0

usage() {
    cat <<'EOF'
用法: start_mtinject.sh [选项]
  起一块虚拟多点触控屏（常驻），供 TouchViz 与测试脚本使用。
  默认会把设备映射到整个桌面（touchprobe map --full），
  这样注入坐标等于屏幕像素坐标。
  --range N    坐标量程（默认 65535，可用 MTINJ_RANGE 覆盖）
  --second     起第二块设备（多指测试，FIFO 用 MTINJ_FIFO2 或 /tmp/mt2.fifo）
  --keep-map   跳过映射设置（保留当前系统映射，不执行 --full）
  --fifo PATH  指定 FIFO 路径（默认 /tmp/mtinject.fifo）
  -h, --help   显示本帮助
环境变量: MTINJ_RANGE MTINJ_FIFO MTINJ_FIFO2 KEEP_MAP
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --range) RANGE="$2"; shift 2 ;;
        --fifo)  TV_FIFO="$2"; shift 2 ;;
        --second) SECOND=1; shift ;;
        --keep-map) KEEP_MAP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

[ "$SECOND" = 1 ] && TV_FIFO="${MTINJ_FIFO2:-/tmp/mt2.fifo}"

tv_need_probe
tv_need_inject

W=$(tv_w); H=$(tv_h)
if [ -z "${W:-}" ] || [ -z "${H:-}" ]; then
    echo "拿不到屏幕尺寸（X 连不上）。" >&2
    exit 1
fi

# ---------- 1.权限检查 ----------
if [ ! -w /dev/uinput ]; then
    echo "/dev/uinput 不可写。请执行：" >&2
    echo "  sudo setfacl -m u:$(id -un):rw /dev/uinput" >&2
    exit 1
fi

# ---------- 2.清理旧实例 ----------
if [ "$SECOND" = 0 ]; then
    tv_kill_inject
    sleep 0.3
else
    pkill -f "mtinject daemon.*$TV_FIFO" 2>/dev/null
fi
BEFORE=$(tv_probe devices 2>/dev/null | grep -c '^设备 ')

# ---------- 3.启动守护进程 ----------
rm -f "$TV_FIFO"
mkfifo "$TV_FIFO" || exit 1
env MTINJ_W="$W" MTINJ_H="$H" MTINJ_RANGE="$RANGE" \
    setsid "$TV_INJ" daemon < "$TV_FIFO" > "${TV_TMP}/mtinject.out" 2> "${TV_TMP}/mtinject.err" &
setsid bash -c "exec 9<>'$TV_FIFO'; exec -a mtinject-fifo-holder sleep 2147483647" >/dev/null 2>&1 &

if ! tv_wait_device 10; then
    echo "虚拟设备没出现。" >&2
    exit 1
fi

# ---------- 4.检查设备数量（针对第二台） ----------
if [ "$SECOND" = 1 ]; then
    for i in $(seq 1 40); do
        NOW=$(tv_probe devices 2>/dev/null | grep -c '^设备 ')
        [ "${NOW:-0}" -gt "${BEFORE:-0}" ] && break
        sleep 0.25
    done
fi

# ---------- 5.映射设置：默认把设备铺满整个桌面 ----------
# 注：桌面环境（dde-daemon / KWin 等）可能在设备出现后立刻重写映射，
#     所以写完后回读验证，不通过就重试几次。
if [ "$KEEP_MAP" = 1 ]; then
    echo "跳过映射设置（--keep-map），保留当前系统映射。"
else
    echo "把虚拟触控屏映射到整个桌面（注入坐标 = 屏幕像素坐标）……"
    map_ids=$(tv_probe devices 2>/dev/null \
              | awk -F'[: ]+' '/^设备 / && /TouchInject/ {print $2}')
    if [ -z "$map_ids" ]; then
        echo "  ⚠ 没找到 TouchInject 设备，跳过映射。" >&2
    fi
    for id in $map_ids; do
        ok=0
        for attempt in 1 2 3 4 5; do
            if ! tv_probe map --dev "$id" --full >/dev/null 2>"${TV_TMP}/map.err"; then
                warn=$(cat "${TV_TMP}/map.err" 2>/dev/null)
                [ -n "$warn" ] && echo "  map 尝试 $attempt 失败: $warn" >&2
                sleep 0.3
                continue
            fi
            # 回读验证（touchprobe devices 输出里有 "STAT dev=N full=0/1"）
            sleep 0.2
            full=$(tv_probe devices 2>/dev/null \
                   | grep "^STAT dev=$id " \
                   | sed -n 's/.*full=\([01]\).*/\1/p')
            if [ "$full" = "1" ]; then
                echo "  设备 $id → 已铺满整个桌面"
                ok=1
                break
            fi
            sleep 0.3
        done
        if [ "$ok" != 1 ]; then
            echo "  ⚠ 设备 $id 映射未生效（可能被桌面环境覆盖）。" >&2
            echo "    可以手动再跑一次：tools/touchprobe map --dev $id --full" >&2
        fi
    done
fi

echo "--- 设备 ---"
tv_probe devices | sed 's/^/  /'
echo "[mtinject] 就绪。FIFO: $TV_FIFO"