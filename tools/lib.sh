#!/bin/bash
# ============================================================================
# tools/lib.sh —— 测试脚本的公共部分（用 source 引入，不直接执行）
#
# 可移植性约定（所有 tools/ 脚本都必须遵守）：
#   • 不写死 DISPLAY / 分辨率 / 坐标 / 设备号 / 用户名 / 安装路径
#   • 不依赖 python / PIL / ImageMagick / xdotool / xinput / qdbus / kwin
#   • 不需要 root 密码（只有 /dev/uinput 不可写时才尝试 sudo -n）
#   • 临时文件放 ${TMPDIR:-/tmp}，FIFO 可用 MTINJ_FIFO 覆盖
#   • 二进制优先用仓库里刚编的，其次已安装的
# ============================================================================

TV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TV_TOOLS="$TV_ROOT/tools"
TV_PROBE="$TV_ROOT/touchprobe"
TV_INJ="$TV_ROOT/mtinject"
TV_TMP="${TMPDIR:-/tmp}"
TV_FIFO="${MTINJ_FIFO:-/tmp/mtinject.fifo}"

export DISPLAY="${DISPLAY:-}"

# ---- 编译检查 --------------------------------------------------------------
tv_need_probe() {
    if [ ! -x "$TV_PROBE" ]; then
        ( cd "$TV_ROOT" && make touchprobe >/dev/null 2>&1 ) || {
            echo "缺少 touchprobe 且编译失败。请装 libx11-dev libxi-dev（或先 make）" >&2
            exit 1
        }
    fi
    # DISPLAY 没设/设错时，用 touchprobe 找出来的那个，免得脚本里打印空值
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

# ---- 会话信息 --------------------------------------------------------------
tv_probe() { "$TV_PROBE" "$@"; }

tv_screen() {   # 输出 "W H"
    tv_probe geom | sed -n 's/.*screen=\([0-9]*\)x\([0-9]*\).*/\1 \2/p'
}

tv_w() { tv_screen | cut -d' ' -f1; }
tv_h() { tv_screen | cut -d' ' -f2; }

# 屏幕尺寸的百分比坐标（0..100），任何分辨率都能用
tv_at() {   # tv_at 50 50  -> "1920 540"
    local w h; read -r w h <<<"$(tv_screen)"
    echo $(( w * $1 / 100 )) $(( h * $2 / 100 ))
}

# ---- TouchViz 控制（不依赖 /usr/local/bin） ---------------------------------
tv_ctl() {
    local bin="$TV_ROOT/TouchViz"
    [ -x "$bin" ] || { ( cd "$TV_ROOT" && make TouchViz >/dev/null 2>&1 ); }
    BIN="$bin" "$TV_ROOT/TouchViz-ctl" "$@"
}

tv_ctl_args() {   # 只重启不打印状态：tv_ctl_args restart
    local bin="$TV_ROOT/TouchViz"
    [ -x "$bin" ] || { ( cd "$TV_ROOT" && make TouchViz >/dev/null 2>&1 ); }
    BIN="$bin" "$TV_ROOT/TouchViz-ctl" "$@"
}

# 直接用指定参数起 TouchViz（测试某些选项时用）
tv_start_viz() {
    pkill -x TouchViz 2>/dev/null
    sleep 0.3
    setsid "$TV_ROOT/TouchViz" "$@" > "$TV_TMP/TouchViz.log" 2>&1 </dev/null &
    sleep 0.8
}

tv_log() { echo "${TV_LOG:-$TV_TMP/TouchViz.log}"; }

# 只应该有 1 个 TouchViz：多开会重复画触点、重复发右键，测试就会误判。
tv_viz_count() { pgrep -cx TouchViz 2>/dev/null || echo 0; }
tv_assert_single() {
    local n; n=$(tv_viz_count)
    if [ "$n" != 1 ]; then
        echo "⚠ 现在有 $n 个 TouchViz 在跑（只应该 1 个）" >&2
        pgrep -a TouchViz >&2
        return 1
    fi
}

# 干净启动（先按路径杀掉可能残留的实例，即使它的名字不是 TouchViz）
tv_start_clean() {
    pkill -x TouchViz 2>/dev/null
    pkill -f "$TV_ROOT/TouchViz " 2>/dev/null
    sleep 0.4
    tv_ctl start
}

# ---- latency 日志解析（同一时钟，不依赖墙钟） -------------------------------
tv_lat_time() {   # tv_lat_time <log> begin|end|btn3  -> 打印毫秒，无则空
    case "$2" in
        begin) awk '/^TOUCH/ && /kind=begin/ {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); print $i; exit}}' "$1" ;;
        end)   awk '/^TOUCH/ && /kind=end/   {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); print $i; exit}}' "$1" ;;
        btn3)  awk '/^BUTTON/ && /btn=3/ && /press=1/ {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); print $i; exit}}' "$1" ;;
    esac
}

# 在 [begin, begin+window_ms] 内出现的右键按下次数（用来排除环境自带的“长按出菜单”）
tv_lat_btn3_count() {   # tv_lat_btn3_count <log> <window_ms>
    awk -v win="$2" '
        /^TOUCH/ && /kind=begin/ && tb == "" {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); tb=$i; break}}
        /^BUTTON/ && /btn=3/ && /press=1/ {for(i=1;i<=NF;i++) if ($i ~ /^t=/) {sub("t=","",$i); if (tb != "" && $i - tb <= win) n++; break}}
        END { print n+0 }' "$1"
}

# ---- 截图 / 图像比较（内建 XGetImage，不依赖外部工具） ----------------------
tv_shot() {   # tv_shot <file.ppm> [x y w h]
    tv_probe shot "$@"
}

# 在 (x,y) 附近 pad 像素内比较两张图，打印 "DIFF ..."
tv_diff() {   # tv_diff a.ppm b.ppm x y pad [thr] [--centroid]
    local a="$1" b="$2" x="$3" y="$4" pad="$5" thr="${6:-25}" extra="${7:-}"
    tv_probe diff "$a" "$b" --region $((x-pad)) $((y-pad)) $((pad*2)) $((pad*2)) --thr "$thr" $extra
}

# 在 (x,y) 附近找触点圆（槽位 0 的青色）
tv_ring() {   # tv_ring file.ppm x y pad
    tv_probe ring "$1" "$2" "$3" "$4"
}

# 注入坐标 -> 真实落点（设备变换矩阵可能把注入坐标缩放/平移）
tv_expect() { tv_probe expect "$1" "$2" | sed -n 's/.*screen=\([-0-9]*\),\([-0-9]*\).*/\1 \2/p'; }

# ---- 虚拟触控屏 ------------------------------------------------------------
tv_inject() {   # 往 FIFO 写一条命令
    [ -p "$TV_FIFO" ] || { echo "FIFO $TV_FIFO 不存在：先跑 tools/start_mtinject.sh" >&2; return 1; }
    printf '%s\n' "$1" > "$TV_FIFO"
}

TV_FIFO2="${MTINJ_FIFO2:-/tmp/mt2.fifo}"
tv_inject2() {  # 第二台虚拟设备
    [ -p "$TV_FIFO2" ] || { echo "FIFO $TV_FIFO2 不存在：先跑 tools/start_mtinject2.sh" >&2; return 1; }
    printf '%s\n' "$1" > "$TV_FIFO2"
}

tv_has_device2() {   # 至少两台触控设备
    [ "$(tv_probe devices 2>/dev/null | grep -c '^设备 ')" -ge 2 ]
}

# 两根手指（两台设备）同时按 -> 长按应作废。输出实测到的右键次数。
tv_multitouch_cancel() {   # $1=按住ms $2=两指间隔ms
    local hold="${1:-1500}" gap="${2:-350}"
    "$TV_TOOLS/start_mtinject2.sh" >/dev/null 2>&1 || return 2
    tv_has_device2 || return 2
    local log="$TV_TMP/mt-cancel.log"; : > "$log"
    setsid "$TV_PROBE" latency -t 4 > "$log" 2>&1 &
    local pid=$!
    sleep 0.5
    tv_inject "hold 600 400 $hold"
    sleep "$(awk -v g="$gap" 'BEGIN{printf "%.2f", g/1000}')"
    tv_inject2 "hold 900 400 $hold"
    wait $pid 2>/dev/null
    local n; n=$(grep -c 'btn=3 press=1' "$log" 2>/dev/null)
    echo "${n:-0}"
}

tv_wait_device() {   # 等虚拟设备在 X 里出现（最多 $1 秒）
    local i=0 max=$((${1:-8} * 4))
    while [ $i -lt $max ]; do
        tv_probe devices 2>/dev/null | grep -q '^设备 ' && return 0
        sleep 0.25; i=$((i+1))
    done
    return 1
}

tv_has_device() { tv_probe devices 2>/dev/null | grep -q '^设备 '; }

# “我们自己起的虚拟触控屏”是否就绪：FIFO 在 + 设备列表里有 TouchInject
# （注意不能只看“有没有触控设备”——真触控屏也会让那个判断为真，
#   然后脚本就以为注入器已经跑着，结果 FIFO 根本没有）
tv_has_inject() {
    [ -p "$TV_FIFO" ] && tv_probe devices 2>/dev/null | grep -q 'TouchInject'
}

tv_kill_inject() {
    pkill -x mtinject 2>/dev/null                 # 虚拟触控屏守护进程
    pkill -f 'fifo-holder' 2>/dev/null            # FIFO 常开写端（否则会每次泄漏一个）
    pkill -f 'mtinject daemon' 2>/dev/null
    if [ -n "${TV_SUDO:-}" ]; then
        $TV_SUDO pkill -x mtinject 2>/dev/null
        $TV_SUDO pkill -f 'fifo-holder' 2>/dev/null
    fi
    rm -f "$TV_FIFO"
    return 0
}
