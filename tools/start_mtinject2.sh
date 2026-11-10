#!/bin/bash
# 起第 2 台虚拟触控屏（模拟“另一个厂商”的另一台设备），用于多设备同时触控测试。
# 实现就是把 start_mtinject.sh --second 包一层，保持旧用法不变。
exec "$(dirname "$0")/start_mtinject.sh" --second "$@"
