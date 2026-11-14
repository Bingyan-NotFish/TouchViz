#!/bin/bash
# 清理虚拟触控屏 + FIFO + holder，并恢复 TouchViz
set -u
. "$(dirname "$0")/lib.sh"

usage() {
    cat <<'EOF'
用法: cleanup_test.sh
  清理测试残留：杀掉虚拟触控屏(mtinject)、FIFO、holder 进程，并恢复 TouchViz 默认运行。
  -h, --help  显示本帮助
EOF
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

# ---------- 1.杀掉测试注入器 ----------
pkill -f 'mtinject daemon' 2>/dev/null
pkill -f 'mtinject-fifo-holder' 2>/dev/null
sleep 0.5
rm -f "$TV_FIFO" "${MTINJ_FIFO2:-/tmp/mt2.fifo}" "${TV_TMP}/mtinject.out" "${TV_TMP}/mtinject.err"

# ---------- 2.恢复默认运行 ----------
tv_ctl start 2>/dev/null || echo "[cleanup] 跳过 TouchViz 恢复"
echo "[cleanup] 测试环境已清理"