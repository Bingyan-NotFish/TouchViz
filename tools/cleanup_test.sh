#!/bin/bash
# ============================================================================
# 收尾：清掉测试用的虚拟触控屏 + FIFO + holder，并把 TouchViz 恢复成默认配置。
# 任何机器/任何 DISPLAY 都能跑；不需要 root 密码（只有 daemon 是 root 起的才用 sudo -n）。
# ============================================================================
set -u
. "$(dirname "$0")/lib.sh"

TV_SUDO=""
if [ ! -w /dev/uinput ] && sudo -n true 2>/dev/null; then TV_SUDO="sudo -n"; fi

pkill -f 'mtinject daemon' 2>/dev/null
pkill -f 'mtinject-fifo-holder' 2>/dev/null
[ -n "$TV_SUDO" ] && {
    $TV_SUDO pkill -f 'mtinject daemon' 2>/dev/null
    $TV_SUDO pkill -f 'mtinject-fifo-holder' 2>/dev/null
}
sleep 1.0
rm -f "$TV_FIFO" "${MTINJ_FIFO2:-/tmp/mt2.fifo}" \
      "${TV_TMP}/mtinject.out" "${TV_TMP}/mtinject.err" "${TV_TMP}/mt2.out" "${TV_TMP}/mt2.err"

echo "--- 残留虚拟设备 ---"
if tv_has_device; then tv_probe devices | grep '^设备 ' || echo 无; else echo 无; fi
echo "--- 内核设备 ---"
grep -i touchinject /proc/bus/input/devices 2>/dev/null || echo 无

echo "--- TouchViz 恢复默认 ---"
tv_ctl start 2>/dev/null || echo "（没装/没编译 TouchViz，跳过）"
