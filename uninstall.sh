#!/bin/bash
# 卸载 TouchViz：停进程 + 去掉自启动项 + 删程序

usage() {
    cat <<'EOF'
用法: uninstall.sh
  停止 TouchViz 进程、删除登录自启动项与 /usr/local/bin 下的程序。
  -h, --help  显示本帮助
EOF
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

pkill -x TouchViz 2>/dev/null
pkill -f '[t]ouchviz .*--long-press' 2>/dev/null
rm -f "$HOME/.config/autostart/TouchViz.desktop"
sudo rm -f /usr/local/bin/TouchViz /usr/local/bin/TouchViz-ctl
rm -f /tmp/TouchViz.log
echo "[uninstall] 已停止进程、删除自启动项与 /usr/local/bin 下的程序"