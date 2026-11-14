#!/bin/bash
# 测试脚本的公共部分

# ---------- 1. 全局路径与基础变量 ----------
TV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TV_TOOLS="$TV_ROOT/tools"
TV_PROBE="$TV_ROOT/touchprobe"
TV_INJ="$TV_ROOT/mtinject"
TV_TMP="${TMPDIR:-/tmp}"
TV_FIFO="${MTINJ_FIFO:-/tmp/mtinject.fifo}"
TV_FIFO2="${MTINJ_FIFO2:-/tmp/mt2.fifo}"

export DISPLAY="${DISPLAY:-}"

# ---------- 2. 编译与依赖检查 ----------
tv_need_probe() {
    if [ ! -x "$TV_PROBE" ]; then
        ( cd "$TV_ROOT" && make touchprobe >/dev/null 2>&1 ) || {
            echo "缺少 touchprobe 且编译失败。请装 libx11-dev libxi-dev（或先 make）" >&2
            exit 1
        }
    fi
    if [ -z "${DISPLAY:-}" ]; then
        DISPLAY=$("$TV_PROBE" geom 2>/dev/null | sed -n 's/^DISPLAY=\([^ ]*\).*/\1/p')
        [ -n "${DISPLAY:-}" ] && export DISPLAY
    fi
}

tv_need_inject() {
    if [ ! -x "$TV_INJ" ]; then
        ( cd "$TV_ROOT" && make mtinject >/dev/null 2>&1 ) || {
            echo "缺少 mtinject 且编译失败（需要 libx11-dev）" >&2
            exit 1
        }
    fi
}

# ---------- 3. 会话信息与基础探测 ----------
tv_probe() { "$TV_PROBE" "$@"; }

tv_screen() {
    tv_probe geom | sed -n 's/.*screen=\([0-9]*\)x\([0-9]*\).*/\1 \2/p'
}

tv_w() { tv_screen | cut -d' ' -f1; }
tv_h() { tv_screen | cut -d' ' -f2; }

tv_at() {
    local w h
    read -r w h <<<"$(tv_screen)"
    echo $(( w * $1 / 100 )) $(( h * $2 / 100 ))
}

# ---------- 4. TouchViz 进程控制 ----------
tv_ctl() {
    local bin="$TV_ROOT/TouchViz"
    [ -x "$bin" ] || { ( cd "$TV_ROOT" && make TouchViz >/dev/null 2>&1 ); }
    BIN="$bin" "$TV_ROOT/TouchViz-ctl" "$@"
}

tv_ctl_args() {
    local bin="$TV_ROOT/TouchViz"
    [ -x "$bin" ] || { ( cd "$TV_ROOT" && make TouchViz >/dev/null 2>&1 ); }
    BIN="$bin" "$TV_ROOT/TouchViz-ctl" "$@"
}

tv_start_viz() {
    pkill -x TouchViz 2>/dev/null
    sleep 0.3
    setsid "$TV_ROOT/TouchViz" "$@" > "$(tv_log)" 2>&1 </dev/null &
    sleep 0.8
}

tv_log() { echo "${TV_LOG:-$TV_TMP/TouchViz.log}"; }

tv_viz_count() { pgrep -cx TouchViz 2>/dev/null || echo 0; }

tv_assert_single() {
    local n
    n=$(tv_viz_count)
    if [ "$n" != "1" ]; then
        echo "⚠ 现在有 $n 个 TouchViz 在跑（只应该 1 个）" >&2
        pgrep -a TouchViz >&2
        return 1
    fi
}

tv_start_clean() {
    pkill -x TouchViz 2>/dev/null
    pkill -f "$TV_ROOT/TouchViz " 2>/dev/null
    sleep 0.4
    tv_ctl start
}

# ---------- 5. latency 日志解析 ----------
tv_lat_time() {
    case "$2" in
        begin) awk '/^TOUCH/ && /kind=begin/ {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); print $i; exit}}' "$1" ;;
        end)   awk '/^TOUCH/ && /kind=end/   {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); print $i; exit}}' "$1" ;;
        btn3)  awk '/^BUTTON/ && /btn=3/ && /press=1/ {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); print $i; exit}}' "$1" ;;
    esac
}

tv_lat_btn3_count() {
    awk -v win="$2" '
        /^TOUCH/ && /kind=begin/ && tb == "" {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); tb=$i; break}}
        /^BUTTON/ && /btn=3/ && /press=1/ {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); if (tb != "" && $i - tb <= win) n++; break}}
        END { print n+0 }' "$1"
}

# ---------- 6. 截图与图像比较 ----------
tv_shot() {
    tv_probe shot "$@"
}

tv_diff() {
    local a="$1" b="$2" x="$3" y="$4" pad="$5" thr="${6:-25}" extra="${7:-}"
    tv_probe diff "$a" "$b" --region $((x-pad)) $((y-pad)) $((pad*2)) $((pad*2)) --thr "$thr" "$extra"
}

tv_ring() {
    tv_probe ring "$1" "$2" "$3" "$4"
}

tv_expect() {
    tv_probe expect "$1" "$2" | sed -n 's/.*screen=\([-0-9]*\),\([-0-9]*\).*/\1 \2/p'
}

# ---------- 7. 虚拟触控屏注入与设备控制 ----------
tv_inject() {
    [ -p "$TV_FIFO" ] || { echo "FIFO $TV_FIFO 不存在：先跑 tools/start_mtinject.sh" >&2; return 1; }
    printf '%s\n' "$1" > "$TV_FIFO"
}

tv_inject2() {
    [ -p "$TV_FIFO2" ] || { echo "FIFO $TV_FIFO2 不存在：先跑 tools/start_mtinject2.sh" >&2; return 1; }
    printf '%s\n' "$1" > "$TV_FIFO2"
}

tv_has_device2() {
    [ "$(tv_probe devices 2>/dev/null | grep -c '^设备 ')" -ge 2 ]
}

tv_has_device() { tv_probe devices 2>/dev/null | grep -q '^设备 '; }

tv_has_inject() {
    [ -p "$TV_FIFO" ] && tv_probe devices 2>/dev/null | grep -q 'TouchInject'
}

tv_wait_device() {
    local i=0 max=$(( ${1:-8} * 4 ))
    while [ "$i" -lt "$max" ]; do
        tv_has_device && return 0
        sleep 0.25
        i=$(( i + 1 ))
    done
    return 1
}

tv_multitouch_cancel() {
    local hold="${1:-1500}" gap="${2:-350}"
    "$TV_TOOLS/start_mtinject2.sh" >/dev/null 2>&1 || return 2
    tv_has_device2 || return 2
    
    local log="$TV_TMP/mt-cancel.log"
    : > "$log"
    setsid "$TV_PROBE" latency -t 4 > "$log" 2>&1 &
    local pid=$!
    
    sleep 0.5
    tv_inject "hold 600 400 $hold"
    sleep "$(awk -v g="$gap" 'BEGIN{printf "%.2f", g/1000}')"
    tv_inject2 "hold 900 400 $hold"
    wait "$pid" 2>/dev/null
    
    local n
    n=$(grep -c 'btn=3 press=1' "$log" 2>/dev/null)
    echo "${n:-0}"
}

tv_kill_inject() {
    pkill -x mtinject 2>/dev/null
    pkill -f 'fifo-holder' 2>/dev/null
    pkill -f 'mtinject daemon' 2>/dev/null
    
    # 如果普通权限杀不掉，交给交互式 sudo
    if pgrep -x mtinject >/dev/null || pgrep -f 'fifo-holder' >/dev/null; then
        echo "需要提权清理遗留的虚拟触屏进程..."
        sudo pkill -x mtinject 2>/dev/null
        sudo pkill -f 'fifo-holder' 2>/dev/null
    fi
    
    rm -f "$TV_FIFO"
    return 0
}