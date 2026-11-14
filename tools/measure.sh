#!/bin/bash
# 简易性能测量：CPU% / 上下文切换频率
set -u

usage() {
    cat <<'EOF'
用法: measure.sh [进程名] [秒数] [标签]
  读取 /proc 数据，测量进程的 CPU 占用与唤醒频率。
  默认: measure.sh TouchViz 5
  -h, --help  显示本帮助
EOF
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

name="${1:-TouchViz}"
secs="${2:-5}"
tag="${3:-$name}"

pid=$(pgrep -x "$name" | head -1)

if [ -z "$pid" ]; then
    echo "[$tag] 找不到进程 $name"
    exit 1
fi

# ---------- 1.读取初始状态 ----------
read -r u1 s1 <<< $(awk '{print $14, $15}' /proc/$pid/stat)
v1=$(awk '/^voluntary_ctxt_switches/ {print $2}' /proc/$pid/status)
nv1=$(awk '/^nonvoluntary_ctxt_switches/ {print $2}' /proc/$pid/status)

# ---------- 2.等待观测 ----------
sleep "$secs"

# ---------- 3.读取结束状态 ----------
read -r u2 s2 <<< $(awk '{print $14, $15}' /proc/$pid/stat)
v2=$(awk '/^voluntary_ctxt_switches/ {print $2}' /proc/$pid/status)
nv2=$(awk '/^nonvoluntary_ctxt_switches/ {print $2}' /proc/$pid/status)

# ---------- 4.计算并输出 ----------
hz=$(getconf CLK_TCK)
cpu=$(awk -v u1="$u1" -v s1="$s1" -v u2="$u2" -v s2="$s2" -v hz="$hz" -v secs="$secs" \
    'BEGIN {printf "%.2f", ((u2-u1)+(s2-s1))/hz/secs*100}')

wak=$(awk -v v1="$v1" -v v2="$v2" -v secs="$secs" \
    'BEGIN {printf "%.0f", (v2-v1)/secs}')

pas=$(awk -v n1="$nv1" -v n2="$nv2" -v secs="$secs" \
    'BEGIN {printf "%.1f", (n2-n1)/secs}')

echo "[$tag] pid=$pid  CPU=${cpu}%  主动唤醒=${wak}/s  被动切换=${pas}/s"