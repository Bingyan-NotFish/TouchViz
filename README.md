# TouchViz

在 **X11** 会话里给任意触控屏加上：触点圆点、滑动拖尾、长按进度环、长按出右键、触控时隐藏鼠标指针。设备无关、空闲零开销，并附带一套可移植的**触控偏移排查与标定工具**。

<sub>GPL-3.0 · C + shell · 只依赖 X11 库（无需 Python/ImageMagick/xdotool）</sub>

> 本项目由氛围编程 (Vibe Coding) 方式编写。如果您对此类项目有排斥感，请无视此项目，谢谢。

## 目录

- [特性](#特性)
- [快速开始](#快速开始)
- [环境变量与参数](#环境变量与参数)
- [目录结构](#目录结构)
- [测试工具及参数](#测试工具及参数)
- [依赖](#依赖)
- [触控偏移解决](#触控偏移解决)
- [已知限制](#已知限制)
- [许可证](#许可证)

## 特性

| 功能 | 说明 | 开关 |
|:--|:--|:--|
| 触点圆点 | 每个触点一个圆（按槽位换色），抬手后 450ms 淡出 | `--no-overlay` 关全部绘制 |
| 滑动拖尾 | 手指划过留下渐细渐淡的轨迹，停止 420ms 后自然消失 | `--no-trail` |
| 长按进度环 | 稳定期过后，触点圆外侧出现暗底环 + 白色进度弧 | 随长按开关 |
| 长按出右键 | 按住到阈值 → XTEST 发右键（桌面右键菜单等） | `--no-rightclick`、`--long-press 0` |
| 触控时隐藏指针 | 触控期间指针隐藏，抬手 120ms 后恢复 | `--no-hide-cursor` |
| 双指滑动转滚轮 | 两指同向滑动 → 滚轮 4/5 | `--scroll`（默认关） |

**鼠标永远不会触发**：只订阅 `XI_RawTouch*`（鼠标不产生这类事件），且来源设备必须有 `XITouchClass`。

## 快速开始

```bash
make                 # 编译 4 个程序
sudo make install    # 只装 TouchViz + TouchViz-ctl 到 /usr/local/bin
./install.sh         # 编译 + 安装 + 写登录自启动 + 立即启动
./uninstall.sh       # 卸载

TouchViz-ctl status  # 看运行状态
```

## 环境变量与参数

### 启动控制 (TouchViz-ctl)

```bash
TouchViz-ctl start | stop | restart | status | toggle
```

### 环境变量 (供 TouchViz-ctl 使用)

可以通过在命令前设置环境变量，来改变 TouchViz 的行为（例如 `LONG_PRESS=800 TouchViz-ctl start`）：

* `LONG_PRESS=600`：长按总时长(ms)，`0` = 关闭长按
* `LP_ARM=200`：稳定期(ms)：按下后多久才开始算进度
* `LP_MOVE=18`：长按允许的手指漂移(px)，超出即作废
* `RIGHTCLICK=0`：关闭长按发右键
* `RIGHTCLICK_AT=press`：`press`（默认，到阈值当场发）| `release`（抬手后 150ms 发）
* `SCROLL=1`：双指滑动转滚轮
* `TRAIL=0`：关闭拖尾
* `OVERLAY=0`：只做长按/隐藏指针，不显示触点
* `HIDE_CURSOR=0`：触控时不隐藏指针
* `BIN=/path/to/TouchViz`：指定二进制的路径

### 直接运行 (TouchViz)

如果不通过 `TouchViz-ctl`，也可以直接运行 `TouchViz` 并传入参数：

```bash
TouchViz [--no-overlay] [--no-trail] [--no-rightclick] [--no-hide-cursor]
         [--scroll] [--long-press MS] [--lp-arm MS] [--lp-move PX]
         [--rightclick-at press|release] [--idle-ms MS]
```

## 目录结构

```text
TouchViz.c           主程序（单文件 C，约 900 行）
TouchViz-ctl         启动/停止/状态控制脚本（安装在 /usr/local/bin）
install.sh           编译 + 安装 + 写登录自启动
uninstall.sh         卸载
tools/*              测试工具，详见下文【测试工具及参数】
Makefile             make / sudo make install / make clean
```

编译产物：`TouchViz`、`mtinject`、`touchprobe`、`touchcal`（后三个供测试，`make` 一并编译）。

## 测试工具及参数

如果你没有真实的触控屏，或者需要对 TouchViz 进行深度调试，可以使用 `tools/` 目录下的测试工具。

### 虚拟触控屏 (`start_mtinject.sh`)

利用 Linux 的 `uinput` 模块，在系统里模拟出一块或多块（至多两块）虚拟触控屏。

```bash
./tools/start_mtinject.sh               # 起一块虚拟触控屏
./tools/start_mtinject.sh --second      # 起第二块
./tools/start_mtinject.sh --range 32767 # 模拟不同厂商的坐标量程
./tools/start_mtinject.sh --keep-map    # 保留系统默认的映射
./tools/start_mtinject.sh --help        # 看全部参数

```

### 注入命令 (`mtinject`)

`start_mtinject.sh` 底层的 uinput 注入器，也可以单独调用；坐标用屏幕像素，量程可用环境变量 `MTINJ_RANGE` 覆盖。

```bash
./mtinject tap 960 540              # 点击一下
./mtinject hold 960 540 600         # 按住 600ms
./mtinject drag 100 100 500 500 800 # 从 (100,100) 拖到 (500,500)
./mtinject scroll 960 540 3         # 双指上下滚动
./mtinject daemon                   # 从 stdin 循环读命令
./mtinject --help                   # 看全部命令
```

### 便携诊断 (`touchprobe`)

查看设备映射与量程，量偏移、截图找触点，是最常用的排查工具。

```bash
./touchprobe devices              # 看设备量程与变换矩阵
./touchprobe map --full           # 把触控屏映射到整个桌面
./touchprobe watch -n 3 -t 10     # 量注入点与落点偏移
./touchprobe --help               # 看全部子命令
```

### 鼠标模拟触控 (`mouse_touch_test.sh`)

把鼠标左键的按下、拖动、松开，转发给虚拟触控屏。这样即使没有触控屏，也能直观体验触点圆、拖尾、长按进度环。

```bash
./tools/mouse_touch_test.sh          # 开启鼠标模拟
./tools/mouse_touch_test.sh --no-viz # 只转发鼠标事件，不启动 TouchViz 界面
./tools/mouse_touch_test.sh --help   # 看全部参数
```

### 偏移排查与标定 (`verify_offset.sh` 和 `touchcal`)

```bash
./tools/verify_offset.sh             # 自动量偏移
./tools/verify_offset.sh --real      # 用真实触控屏测
./tools/verify_offset.sh --tol 5     # 偏移阈值设为 5 像素
./tools/verify_offset.sh --help      # 看全部参数

./touchcal                           # 5个点，只测量不修改
./touchcal --points 9 --apply        # 3x3标定，并自动应用校正矩阵
./touchcal --help                    # 看全部参数
```

### 单点精度 (`verify_device.sh`)

注入一个点，截屏找触点圆，计算它与期望落点的真实偏移。

```bash
./tools/verify_device.sh            # 默认注入 (40%,30%)，期望落点按矩阵算
./tools/verify_device.sh POS 50 50  # 注入屏幕 50%,50%
./tools/verify_device.sh --help     # 看全部参数
```

### 长按时序 (`verify_lp.sh`)

验证长按语义与出右键的时序是否正确。

```bash
./tools/verify_lp.sh          # 用默认 600ms / 200ms
./tools/verify_lp.sh 800 250  # 自定义长按时长与稳定期
./tools/verify_lp.sh --help   # 看全部参数
```

### 视觉行为 (`verify_visual.sh`)

自动检查滑动拖尾、触控时隐藏指针、鼠标不触发触点。

```bash
./tools/verify_visual.sh        # 一键跑完三项
./tools/verify_visual.sh --help # 看全部参数
```

### 性能测量 (`measure.sh`)

读取 `/proc` 下的数据，测量 TouchViz 的 CPU 占用和唤醒频率。

```bash
./tools/measure.sh TouchViz 5 # 测量 TouchViz 在 5 秒内的资源占用
./tools/measure.sh --help     # 看全部参数
```

### 环境清理 (`cleanup_test.sh`)

如果你用过了测试工具，运行这个脚本清理虚拟设备、FIFO 管道，并将 TouchViz 恢复默认状态。

```bash
./tools/cleanup_test.sh        # 清理并恢复
./tools/cleanup_test.sh --help # 看全部参数
```

## 依赖

Debian / Ubuntu 包名：

| 用途 | 包 |
|:--|:--|
| 运行 | `libxi6` `libcairo2` `libxtst6` `libxfixes3` `libx11-6` |
| 编译 | `libxi-dev` `libcairo2-dev` `libxtst-dev` `libxfixes-dev` `libxext-dev` `libx11-dev` |
| 测试工具 | 同上，无需其它依赖 |
| 虚拟触控屏 | `/dev/uinput` 写权限：`sudo setfacl -m u:$USER:rw /dev/uinput` |

## 触控偏移解决

### 排查

```bash
make && ./tools/verify_offset.sh            # 自动：注入 3 个点量偏移
./tools/verify_offset.sh --real             # 真触控屏：按提示点 3 下
./touchprobe devices                        # 只看设备：量程、变换矩阵
```

判据有两条：
1. **TouchViz 公式 vs 指针落点**：差 >3px 才是 TouchViz 的坐标问题；
2. **设备映射矩形 vs 整个桌面**：只覆盖一部分 ⇒ 手指与落点整体错位。

两种修法不要混：

| 现象 | 解决方法 |
|:--|:--|
| 整体偏一个屏宽（双屏时差一半） | 改**映射**：（部分系统）在「控制中心」修改 或 `./touchprobe map --full` |
| 中心准、边缘偏二三十像素 | 做**标定**：`./touchcal --points 9 --apply` |

### 标定

需要人配合点击：

```bash
TouchViz-ctl stop                 # 先停掉 TouchViz
./touchcal --points 9 --apply     # 测完写回设备
```

## 已知限制

- 只对**被 X 认成触控设备**（有 `XITouchClass`）的硬件生效。
- 完全依赖 X11 的 XI2 原始事件，**Wayland 原生会话不适用**。
- 被 `SIGKILL` 强杀时，透明光标可能残留；遇到时 `TouchViz-ctl restart` 即可。
- `touchprobe map` 与 `touchcal --apply` 的改动只在**当前会话**有效。

## 许可证

GPL-3.0，全文见 [LICENSE](LICENSE)。`TouchViz.c`、`tools/*.c` 与各脚本均以此协议分发，再分发时请保留许可证与来源声明。