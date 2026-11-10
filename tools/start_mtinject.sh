#!/bin/bash
# ============================================================================
# 起一台虚拟多点触控屏（常驻），用于没有真触控屏的机器上验证 TouchViz。
#
# 用法:
#   tools/start_mtinject.sh                 # 覆盖整个桌面
#   tools/start_mtinject.sh --range 32767   # 模拟别的厂商的量程
#   tools/start_mtinject.sh --fifo /tmp/f   # 换 FIFO 路径（同 MTINJ_FIFO）
#
# 发命令: echo "tap X Y" > /tmp/mtinject.fifo    （X Y 是根窗口像素坐标）
# 收尾  : tools/cleanup_test.sh
#
# 可移植性: 不用 sudo 密码；屏幕尺寸现场查询；FIFO/临时文件可覆盖；
#           /dev/uinput 只有本用户不可写时才用 sudo -n。
# ============================================================================
set -u
. "$(dirname "$0")/lib.sh"

RANGE="${MTINJ_RANGE:-65535}"
KEEP_MAP="${KEEP_MAP:-0}"
SECOND=0
while [ $# -gt 0 ]; do
    case "$1" in
        --range) RANGE="$2"; shift 2 ;;
        --fifo)  TV_FIFO="$2"; shift 2 ;;
        --second) SECOND=1; shift ;;         # 起第 2 台（模拟“另一个厂商”的设备）
        --keep-map) KEEP_MAP=1; shift ;;      # 保留系统给的映射（默认会改成覆盖整屏）
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done
[ "$SECOND" = 1 ] && TV_FIFO="${MTINJ_FIFO2:-/tmp/mt2.fifo}"

tv_need_probe
tv_need_inject

W=$(tv_w); H=$(tv_h)
if [ -z "${W:-}" ] || [ -z "${H:-}" ]; then
    echo "拿不到屏幕尺寸（X 连不上）。" >&2
    echo "  普通用户: DISPLAY 应为 :0 之类；root 下还要带 XAUTHORITY=\$HOME/.Xauthority" >&2
    exit 1
fi

# ---- /dev/uinput 权限：优先当前用户，其次 sudo -n（不喂密码） ----------------
TV_SUDO=""
if [ ! -w /dev/uinput ]; then
    if sudo -n true 2>/dev/null; then
        TV_SUDO="sudo -n"
    else
        echo "/dev/uinput 不可写。任选一种：" >&2
        echo "  sudo setfacl -m u:$(id -un):rw /dev/uinput     # 一次即可，之后不用 root" >&2
        echo "  sudo $0 $*                                     # 或整条命令用 root 跑" >&2
        exit 1
    fi
fi

# ---- 清掉旧的 ---------------------------------------------------------------
if [ "$SECOND" = 0 ]; then
    tv_kill_inject
    sleep 0.3
else
    pkill -f "mtinject daemon.*$TV_FIFO" 2>/dev/null
fi
BEFORE=$(tv_probe devices 2>/dev/null | grep -c '^设备 ')

# ---- FIFO + 守护进程 --------------------------------------------------------
rm -f "$TV_FIFO"
mkfifo "$TV_FIFO" || exit 1
$TV_SUDO env MTINJ_W="$W" MTINJ_H="$H" MTINJ_RANGE="$RANGE" \
    setsid "$TV_INJ" daemon < "$TV_FIFO" > "${TV_TMP}/mtinject.out" 2> "${TV_TMP}/mtinject.err" &
# 常开写端：否则脚本退出后 daemon 读到 EOF 就退出了
setsid bash -c "exec 9<>'$TV_FIFO'; exec -a mtinject-fifo-holder sleep 2147483647" >/dev/null 2>&1 &

for i in $(seq 1 40); do
    [ -s "${TV_TMP}/mtinject.err" ] && grep -q 'ready' "${TV_TMP}/mtinject.err" && break
    sleep 0.25
done
sed -n '1,3p' "${TV_TMP}/mtinject.err" 2>/dev/null | sed 's/^/  /'

if ! tv_wait_device 10; then
    echo "虚拟设备没出现。看 ${TV_TMP}/mtinject.err，或确认 X 有热插拔（udev/input 组）" >&2
    exit 1
fi
# 第二台设备时，已有一台存在，所以要等“数量变多”
for i in $(seq 1 40); do
    NOW=$(tv_probe devices 2>/dev/null | grep -c '^设备 ')
    [ "${NOW:-0}" -gt "${BEFORE:-0}" ] && break
    sleep 0.25
done

# 虚拟设备默认映射到整个桌面：否则注入的像素坐标会被系统的变换矩阵打折
# （多屏时常见 0.5），测试会误判成“触控偏移”。想复现那种情况就加 --keep-map。
if [ "$KEEP_MAP" != 1 ]; then
    for id in $(tv_probe devices | awk -F'[: ]+' '/^设备 / && /TouchInject/ {print $2}'); do
        if tv_probe map --dev "$id" --full >/dev/null 2>&1; then
            echo "已把设备 $id 映射到整个桌面（注入坐标 = 屏幕坐标）"
        fi
    done
fi

echo "--- 设备 ---"
tv_probe devices | sed 's/^/  /'
cat <<EOF

已就绪。发命令（像素坐标，根窗口 ${W}x${H}）：
  echo "tap X Y"                 > $TV_FIFO
  echo "hold X Y 800"            > $TV_FIFO
  echo "drag X1 Y1 X2 Y2 1200"   > $TV_FIFO
  echo "down X Y" / "move X Y" / "up"   > $TV_FIFO

⚠ 若上面“映射矩形”不是整个桌面，注入的坐标会被系统变换打折 —— 这不是 TouchViz 的问题。
  用 tools/verify_offset.sh 看偏移，或 touchprobe map --full 让设备覆盖整个桌面。
收尾: tools/cleanup_test.sh
EOF
