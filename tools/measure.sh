#!/bin/bash
# ============================================================================
# 测进程资源：CPU%、每秒唤醒数(voluntary ctxt switches)、每 5s 被动切换
# 用法: tools/measure.sh <进程名> <秒数> [标签]
# 便携性: 只看 /proc，不需要 python / pgrep / top
# ============================================================================
set -u
name="$1"; secs="${2:-5}"; tag="${3:-$name}"

pid=""
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    if [ "$(cat "$p/comm" 2>/dev/null)" = "$name" ]; then pid="${p##*/}"; break; fi
done
if [ -z "$pid" ]; then
    echo "[$tag] 找不到进程 $name"
    exit 1
fi

stat_cpu() {   # 输出 "utime stime"（stat 里 comm 可能带空格/括号，从最后一个 ')' 之后取）
    local f; f=$(cat "/proc/$pid/stat" 2>/dev/null) || { echo "0 0"; return; }
    echo "${f#*) }" | awk '{print $12, $13}'
}
stat_ctxt() {
    awk '/^voluntary_ctxt_switches/ {v=$2} /^nonvoluntary_ctxt_switches/ {n=$2} END {print v+0, n+0}' \
        "/proc/$pid/status" 2>/dev/null || echo "0 0"
}

HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
read -r u1 s1 <<<"$(stat_cpu)"
read -r v1 nv1 <<<"$(stat_ctxt)"
sleep "$secs"
read -r u2 s2 <<<"$(stat_cpu)"
read -r v2 nv2 <<<"$(stat_ctxt)"

awk -v pid="$pid" -v s="$secs" -v hz="$HZ" -v tag="$tag" \
    -v u1="$u1" -v s1="$s1" -v u2="$u2" -v s2="$s2" -v v1="$v1" -v v2="$v2" -v nv1="$nv1" -v nv2="$nv2" \
'BEGIN {
    cpu = ((u2 - u1) + (s2 - s1)) / hz / s * 100;
    printf("[%s] pid=%s %gs  CPU=%.2f%%  唤醒=%.0f/s  被动切换=%.1f/s\n",
           tag, pid, s, cpu, (v2 - v1) / s, (nv2 - nv1) / s);
}'
