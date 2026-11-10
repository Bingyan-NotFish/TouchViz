#!/bin/bash
# 卸载 TouchViz：停进程 + 去掉自启动项 + 删程序
if [ "$(id -u)" = 0 ]; then SUDO=""; else SUDO="sudo"; fi
pkill -x TouchViz 2>/dev/null
pkill -f '[t]ouchviz .*--long-press' 2>/dev/null
rm -f "$HOME/.config/autostart/TouchViz.desktop"
$SUDO rm -f /usr/local/bin/TouchViz /usr/local/bin/TouchViz-ctl
rm -f /tmp/TouchViz.log
echo "[uninstall] 已停止进程、删除自启动项与 /usr/local/bin/TouchViz{, -ctl}"
echo "           本目录里的源码/脚本没有动。"
