#!/bin/bash
# 安装 TouchViz：二进制 + 控制脚本 + 登录自启动

usage() {
    cat <<'EOF'
用法: install.sh [选项]
  编译 + 安装到 /usr/local/bin + 写登录自启动 + 立即启动。
  --no-autostart  跳过写登录自启动项
  -h, --help      显示本帮助
EOF
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

cd "$(dirname "$0")" || exit 1

# ---------- 1.编译 ----------
if ! make >/dev/null; then
    echo "[install] 编译失败：先确认装了 libxi-dev libcairo2-dev libxtst-dev libxfixes-dev libxext-dev"
    exit 1
fi
echo "[install] 已编译成功"

# ---------- 2.装到 /usr/local/bin ----------
sudo install -m755 TouchViz /usr/local/bin/TouchViz
sudo install -m755 TouchViz-ctl /usr/local/bin/TouchViz-ctl
echo "[install] 程序已安装到 /usr/local/bin"

# ---------- 3.登录自启动 ----------
if [ "$1" != "--no-autostart" ]; then
    mkdir -p "$HOME/.config/autostart"
    cat > "$HOME/.config/autostart/TouchViz.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Touch Points (X11)
Name[zh_CN]=触控点显示与增强
Comment=Touch point circles, sliding trail and long-press ring on X11
Exec=/usr/local/bin/TouchViz-ctl start
TryExec=/usr/local/bin/TouchViz-ctl
Icon=input-touchscreen
Terminal=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
X-GNOME-Autostart-Delay=3
DESKTOP
    echo "[install] 登录自启动已写入: $HOME/.config/autostart/TouchViz.desktop"
else
    echo "[install] 按参数要求跳过自启动项"
fi

# ---------- 4.立刻启动一次 ----------
if [ -x /usr/local/bin/TouchViz-ctl ]; then
    /usr/local/bin/TouchViz-ctl start
fi