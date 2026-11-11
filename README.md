# TouchViz

在 **X11** 会话里给任意触控屏加上：触点圆点、滑动拖尾、长按进度环、长按出右键、触控时隐藏鼠标指针。设备无关、空闲零开销，并附带一套可移植的**触控偏移排查与标定工具**。

<sub>GPL-3.0 · C + shell · 只依赖 X11 库（无需 Python/ImageMagick/xdotool） · 已在 Deepin 25 + KWin X11 实测</sub>

> 代码与文档由 AI 辅助编写，作者实测验收。

## 目录

- [特性](#特性)
- [快速开始](#快速开始)
- [目录结构](#目录结构)
- [依赖](#依赖)
- [用法](#用法)
- [长按时序](#长按时序)
- [工作原理](#工作原理)
- [可移植性](#可移植性)
- [触控偏移：先量，再修](#触控偏移先量再修)
- [测试工具](#测试工具)
- [性能](#性能)
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
make                 # 编译 4 个程序，-Wall -Wextra 零警告
sudo make install    # 只装 TouchViz + TouchViz-ctl 到 /usr/local/bin
./install.sh         # 编译 + 安装 + 写登录自启动 + 立即启动（--no-autostart 跳过自启动）
./uninstall.sh       # 卸载：停进程、删自启动项、删程序

TouchViz-ctl status  # 看运行状态
```

没有触控屏也能试：

```bash
./tools/start_mtinject.sh      # 起一块 uinput 虚拟触控屏
./tools/mouse_touch_test.sh    # 终端 1：鼠标当手指
TouchViz-ctl start             # 终端 2：看触点圆 / 拖尾 / 长按环
```

位置不对？先量再修：

```bash
./tools/verify_offset.sh            # 一键量偏移，并区分「映射问题」还是「坐标问题」
./touchcal --points 9 --apply       # 全屏标定：点十字，自动算校正矩阵并验证
```

## 目录结构

```
TouchViz.c           主程序（单文件 C，约 1000 行）
TouchViz-ctl         启动/停止/状态控制脚本（安装在 /usr/local/bin）
install.sh           编译 + 安装 + 写登录自启动
uninstall.sh         卸载
tools/mtinject.c     测试用：uinput 虚拟触控屏（可指定量程、可两台同时起）
tools/touchprobe.c   测试用：便携诊断（量偏移、截图、比对、查光标、改映射）
tools/touchcal.c     测试用：全屏触摸标定（点十字 → 算校正矩阵）
tools/lib.sh         测试脚本的公共函数
tools/*.sh           测试脚本（见「测试工具」一节）
Makefile             make / sudo make install / make clean
```

编译产物：`TouchViz`、`mtinject`、`touchprobe`、`touchcal`（后三个供测试，`make` 一并编译，已列入 `.gitignore`）。

## 依赖

Debian / Ubuntu 包名：

| 用途 | 包 |
|:--|:--|
| 运行 | `libxi6` `libcairo2` `libxtst6` `libxfixes3` `libx11-6` |
| 编译 | `libxi-dev` `libcairo2-dev` `libxtst-dev` `libxfixes-dev` `libxext-dev` `libx11-dev` |
| 测试工具 | 同上，无需其它依赖：不用 Python / PIL / ImageMagick / `xinput` / `xdotool` / `qdbus` |
| 虚拟触控屏 | `/dev/uinput` 写权限，一次即可：`sudo setfacl -m u:$USER:rw /dev/uinput` |

脚本在需要时会尝试 `sudo -n`（非交互），从不喂密码；失败时给出手动命令。

## 用法

```bash
TouchViz-ctl start | stop | restart | status | toggle
```

环境变量（供 `TouchViz-ctl` 使用）：

```bash
LONG_PRESS=600       # 长按总时长(ms)，0 = 关闭长按
LP_ARM=200           # 稳定期(ms)：按下后多久才开始算进度
LP_MOVE=18           # 长按允许的手指漂移(px)，超出即作废
RIGHTCLICK_AT=press  # press（默认，到阈值当场发）| release（抬手后 150ms 发）
SCROLL=1             # 双指滑动转滚轮
TRAIL=0              # 关闭拖尾
OVERLAY=0            # 只做长按/隐藏指针，不显示触点
HIDE_CURSOR=0        # 触控时不隐藏指针
BIN=/path/to/TouchViz
```

直接运行：

```
TouchViz [--no-overlay] [--no-trail] [--no-rightclick] [--no-hide-cursor]
         [--scroll] [--long-press MS] [--lp-arm MS] [--lp-move PX]
         [--rightclick-at press|release] [--idle-ms MS]
```

登录自启动：`~/.config/autostart/TouchViz.desktop`，内容为 `Exec=/usr/local/bin/TouchViz-ctl start`
（`X-GNOME-Autostart-Delay=3` 等桌面就绪；`NoDisplay=true` 使其不出现在启动列表里）。
控制脚本在 `DISPLAY` 为空时（自启动环境常见）会扫描 `/tmp/.X11-unix/X*` 自行寻找可用显示。

## 长按时序

```
按下               稳定期结束                        阈值                     抬手
 │─── 稳定期 ───────│──────── 进度环生长 ────────────│
 │  lp-arm(200ms)   │   (long-press - lp-arm)        │
 │  期间不显示进度   │   白色弧从 0 涨到 100%          │  抬手后圆圈淡出
 └─ 手指漂移 >18px、或出现第 2 根手指、或坐标无效 → 本次长按作废
```

- **按下不马上算进度**：前 `--lp-arm` 毫秒是稳定期，只显示触点圆。所以轻点不会闪出进度环，
  也能筛掉「按住后立刻拖走」的误操作。
- 稳定期结束才开始画进度环，并在 `--long-press` 时刻触发（从按下算起）。
  实测：阈值 600ms 时，从触控开始到发出右键为 602ms。
- 触发时机：

  | 模式 | 行为 | 适用 |
  |:--|:--|:--|
  | `press`（默认） | 到阈值当场发右键 | 反馈直接 |
  | `release` | 到阈值进入就绪态（环满 + 外圈亮环），抬手后 150ms 再发 | 桌面会忽略「触控进行中合成的右键」时 |

## 工作原理

- **事件源**：向根窗口订阅 `XI_RawTouchBegin/Update/End`（`XIAllMasterDevices`）。
  原始事件只读，不抓取、不影响事件正常投递给应用，因此对其它程序零副作用。
- **绘制**：一个 `override_redirect` 的 32 位 ARGB 全屏窗口，input shape 置空实现事件穿透。
  只重绘脏矩形：上一帧的矩形擦成全透明，再按本帧矩形绘制，单帧写入量是几十 KB 而非整屏 8~16MB。
- **坐标**（关键）：`valuators.values[]` **已经是套过设备变换矩阵（Coordinate Transformation Matrix）后的值**，
  `raw_values[]` 才是设备原值。按设备量程归一化 × (屏幕尺寸 − 1) 即与点击落点像素级一致；
  自己再乘一次变换矩阵会偏 2 倍（实测踩过）。X server 把量程映射到 `[0, W−1]` 而不是 `[0, W]`，
  早期用 `W` 时整体偏 1px，已修正。
  推论：**设备被系统映射到半屏或别的屏时，TouchViz 会如实画在真实落点上**，
  看起来像「触控偏移」，实际要修的是设备映射（见[触控偏移](#触控偏移先量再修)）。
- **设备无关**：X/Y 通道按 `valuator.label` 识别（`Abs MT Position X/Y`、`Abs X/Y`；含 `Position` 优先，
  避开 `Tilt X/Y`），量程取设备声明的 `min/max`，触点按 `(设备号, 触点号)` 归位。
  设备信息按设备缓存，同一条事件流走快路径。
- **隐藏指针**：`XFixesHideCursor(root)` 全局隐藏，**并**把指针所在（最深层）窗口的 cursor 临时设为
  1×1 透明光标。后者必需：它强制 X server 立刻重绘，否则手指不动时已画出的箭头不会消失。
  抬手恢复；`SIGTERM/SIGINT` 退出前也恢复（信号处理里只置标志，绝不在其中调用 Xlib）。
- **资源**：主循环 `poll(X 连接 fd)`，无定时任务时无限期阻塞；静止手指不重绘；重绘上限约 60fps
  （采样仍逐事件进行，拖尾几何不受影响）；只有淡出、拖尾、进度环会按帧唤醒。
- **健壮性**：分辨率/旋转变化重建窗口与画布；覆盖层被销毁（合成器重启等）自动重建；
  `BadWindow/BadDrawable/BadCursor` 静默处理（都是操作「刚好消失的窗口」）。

## 可移植性

换触控屏、换厂商、换分辨率/多屏/旋转都不需要改代码：

| 关注点 | 做法 | 实测 |
|:--|:--|:--|
| 设备识别 | 不认设备名/VID/PID，只要求来源设备有 `XITouchClass` | 两台不同设备同时使用互不干扰 |
| X/Y 通道 | 按 valuator 标签识别，无标签才退回 0/1 | 本机（libinput）为 v0 / v1 |
| 量程 | 读取设备声明的 min/max | 用 32767 等不同量程的虚拟设备测过，落点与指针一致 |
| 多屏 / 旋转 / 校准 | 直接使用已套过变换矩阵的 `valuators.values[]` | 半屏矩阵、半屏+平移矩阵下偏移 ≤1px |
| 多块触控屏 | 触点按 `(设备号, 触点号)` 归位 | 两指来自两块设备时各画各的 |

> **「触控偏移」多半不是 TouchViz 的问题。** X 里每个设备都有自己的 `Coordinate Transformation Matrix`，
> 它决定手指落在桌面的哪一块。多屏时系统（dde-daemon / KWin）常把触控屏自动映射到主屏，
> 于是手指在第 2 块屏上时落点会整体偏一个屏宽。TouchViz 如实跟随真实落点，
> 需要修的是映射——见下一节。

## 触控偏移：先量，再修

### 排查

```bash
make && ./tools/verify_offset.sh            # 自动：注入 3 个点量偏移（无触控屏也能跑）
./tools/verify_offset.sh --real --visual    # 真触控屏：按提示点 3 下，并截图交叉验证
./tools/verify_offset.sh --fix              # 映射只盖住部分桌面时，本会话内修成整屏
./touchprobe devices                        # 只看设备：量程、变换矩阵、映射矩形
```

判据有两条，脚本会分别给出结论：

1. **TouchViz 公式 vs 指针落点**（`touchprobe watch`）：差 >3px 才是 TouchViz 的坐标问题；
2. **设备映射矩形 vs 整个桌面**（`touchprobe devices`）：只覆盖一部分 ⇒ 手指与落点整体错位。

两种修法不要混：

| 现象 | 修法 |
|:--|:--|
| 整体偏一个屏宽（双屏 3840 宽时落点正好是一半，或差 1920） | 改**映射**：`./touchprobe map --full`（整个桌面）或 `map --rect 1920 0 1920 1080`（只映射右屏）；长期在「控制中心 → 显示 → 触摸屏映射」选择对应显示器 |
| 中心准、边缘偏二三十像素，或整块面板线性缩放（约 1.00x~1.05x） | 做**标定**：`./touchcal --points 9 --apply` |

### 标定

需要人配合点屏幕：

```bash
TouchViz-ctl stop                 # 先停掉，覆盖层会挡住十字目标
./touchcal                        # 5 个点（正中 + 四角），只测量、不改系统
./touchcal --points 9 --apply     # 3x3 更稳；测完写回设备，并自动第 2 轮验证
```

- 屏幕上依次出现十字，点每个十字的**正中心**；每点实时打印「目标 / 落点 / 偏差」，
  屏幕上用红线画出偏差方向。
- 全部点完输出：逐点偏差、平均/最大偏差、离散度、逐轴最小二乘拟合，
  以及能抵掉偏差的 `Coordinate Transformation Matrix`，
  并附上等价的 `xinput set-prop` 命令和可直接落地的 `/etc/X11/xorg.conf.d/99-touchcal.conf`
  （写入后重启仍生效）。
- 参数：`--points 9`（3x3）、`--device ID`、`--rounds N`、`--report 路径`（默认 `/tmp/touchcal-report.txt`）。
  按键：`Esc`/`q` 中止，`r` 重来，`空格` 跳过当前点。
- 没有触控屏也能跑流程：鼠标左键点目标即可（日志标 `[click]`，这种点量不出偏移，
  程序会明确说明并拒绝生成矩阵）。想让鼠标走真实触控链路，配合 `tools/mouse_touch_test.sh`
  并给 `touchcal` 加 `--no-click`。

## 测试工具

全部可移植：不写死显示器、分辨率、坐标或账号，不需要 root 密码，不依赖
Python / PIL / ImageMagick / `xinput` / `xdotool` / `qdbus`。
脚本可在任意目录运行；`DISPLAY` 未设或设错时，`touchprobe` 会扫描 `/tmp/.X11-unix/X*` 自行寻找。

### touchprobe

只依赖 `libX11` `libXi` `libXtst` `libXfixes`。

| 子命令 | 作用 |
|:--|:--|
| `geom` | 打印 DISPLAY 与桌面尺寸 |
| `devices` | 每台触控设备的量程、变换矩阵、映射矩形；映射不是整屏会直接告警 |
| `expect x y` | 注入（或手指按）x,y 后真实会落在哪（已过变换矩阵） |
| `watch [-n N]` | 实时量偏移：每次触控打印「TouchViz 位置 / 指针落点 / 差值」 |
| `latency` | 同一时钟下打印触控开始/结束与真实按键时刻（验长按时序） |
| `buttons` | 只打印真实按键时间戳（含 XTEST 合成的右键） |
| `cursor` | 当前光标是否被隐藏（`XFixesGetCursorImage`；根窗口截图看不到光标） |
| `shot [x y w h]` | 用 `XGetImage` 截根窗口为 PPM（内建，不依赖截图工具） |
| `diff a.ppm b.ppm` | 两图差异：计数 / 最大差 / 包围盒 / 质心 |
| `ring a.ppm x y pad` | 在 (x,y) 附近找触点圆，输出圆心与半径 |
| `move x y` / `click btn [ms]` | 移动指针（XTest）/ 合成真实按键 |
| `finger` | 鼠标当手指（左键 → 虚拟触控屏） |
| `map --full \| --rect X Y W H \| --show` | 读写设备的变换矩阵 |

### 脚本

| 脚本 | 用途 |
|:--|:--|
| `./touchcal` | 全屏触摸标定（见[标定](#标定)） |
| `verify_offset.sh` | 触控偏移一键排查（见[排查](#排查)）：`--real` / `--visual` / `--fix` / `--tol` |
| `start_mtinject.sh` / `start_mtinject2.sh` | 起 1~2 块 uinput 虚拟触控屏。`--range 32767` 指定量程；默认把虚拟设备映射到整个桌面，想复现「被系统按主屏打折」加 `--keep-map` |
| `mouse_touch_test.sh` | 鼠标当手指：左键 = 触点、拖动 = 拖尾、按住 = 进度环。未起注入器/TouchViz 会自动启动，Ctrl-C 结束；`--no-viz` 只做转发 |
| `cleanup_test.sh` | 杀掉虚拟触控屏、FIFO 与常开写端（用完请跑） |
| `verify_device.sh [标签] ["hold X Y 900"] [期望x 期望y]` | 单点位置精度（触点圆 vs 真实落点） |
| `verify_lp.sh [阈值ms] [稳定期ms]` | 长按时序（短按不出右键；到阈值准时出一次，误差 <15ms） |
| `verify_visual.sh` | 拖尾、隐藏指针对照、鼠标不触发、空闲全透明 |
| `verify_rewrite.sh` / `verify_round2.sh` | 整套回归（位置、时序、多指取消、release 模式、资源占用） |
| `measure.sh <进程名> <秒数> [标签]` | 资源测量（纯 `/proc`，不需要 Python/pgrep/top） |
| `lib.sh` | 公共函数（被上面脚本 source） |

给虚拟触控屏下命令（路径可用 `MTINJ_FIFO` 或 `--fifo` 覆盖，默认 `/tmp/mtinject.fifo`）：

```bash
echo "tap 500 750"               > /tmp/mtinject.fifo   # 点一下
echo "hold 500 750 2000"         > /tmp/mtinject.fifo   # 按住 2 秒
echo "drag 300 700 900 700 800"  > /tmp/mtinject.fifo   # 拖动
echo "down 800 400" > /tmp/mtinject.fifo
echo "move 900 500" > /tmp/mtinject.fifo
echo "up"           > /tmp/mtinject.fifo
```

注入坐标是**根窗口像素**。`start_mtinject.sh` 默认已把虚拟设备映射到整屏，所以注入点就是落点；
若设备映射不是整屏，落点会被变换矩阵缩放/平移（用 `touchprobe expect X Y` 可算出落点，
`verify_*` 脚本会自动处理）。

## 性能

3840×1080 双屏实测：

| 场景 | 优化前 | 现在 |
|:--|:--|:--|
| 空闲 | 0.17% CPU，66 唤醒/s | **0.00% CPU，0 唤醒/s** |
| 按住不动（圆静止） | 0.20% | **0.00%** |
| 拖动中 | 1.75% | **≈0.75%**（事件驱动 + 帧率上限） |
| 内存 | — | VmRSS ≈ 6MB（覆盖层缓冲在 X server 侧） |

自行测量：`tools/measure.sh TouchViz 5`。

## 已知限制

- 只对**被 X 认成触控设备**（有 `XITouchClass`）的硬件生效。若某厂商驱动把触摸屏只暴露成鼠标，
  日志会打印 `非触控(忽略)`，这超出本程序能解决的范围。
- 完全依赖 X11 的 XI2 原始事件，**Wayland 原生会话不适用**。
- 被 `SIGKILL` 强杀时，之前在某个窗口上设置的透明光标可能残留（正常 `SIGTERM` 会恢复）；
  遇到时 `TouchViz-ctl restart` 即可。
- 右键用 XTEST 合成，与真实鼠标右键走同一路径，不绕过任何权限或设置。
- `touchprobe map` 与 `touchcal --apply` 的改动只在**当前会话**有效；dde-daemon 等会在设备热插拔时
  重新设置映射。长期生效：映射走控制中心，标定走报告里给出的 xorg.conf.d 片段。
- `install.sh` 只安装 `TouchViz` 与 `TouchViz-ctl`；`touchprobe`、`touchcal`、`mtinject` 是测试工具，
  `make` 编好后留在仓库内使用。

## 许可证

GPL-3.0，全文见 [LICENSE](LICENSE)。`TouchViz.c`、`tools/*.c` 与各脚本均以此协议分发，
再分发时请保留许可证与来源声明。