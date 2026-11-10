# TouchViz / mtinject 构建
#
# 依赖(Debian/Ubuntu 包名):
#   运行: libxi6 libcairo2 libxtst6 libxfixes3 libx11-6
#   编译: libxi-dev libcairo2-dev libxtst-dev libxfixes-dev libxext-dev libx11-dev
#   测试工具 mtinject 还需: libx11-dev (读屏幕尺寸)
#   测试工具 touchprobe 还需: libxi-dev libxtst-dev libxfixes-dev（与主程序同一套）
#
# 用法: make            # 编译
#       sudo make install   # 装到 /usr/local/bin
#       make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS   = -lX11 -lXi -lXext -lXtst -lXfixes -lcairo -lm

all: TouchViz mtinject touchprobe touchcal

TouchViz: TouchViz.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

mtinject: tools/mtinject.c
	$(CC) $(CFLAGS) -o $@ $< -lX11

# 便携验证工具：只依赖 libX11 + libXi + libm（截图/比对/偏移测量都在里面）
touchprobe: tools/touchprobe.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lXi -lXtst -lXfixes -lm

# 触摸标定（全屏，需要人点屏幕）
touchcal: tools/touchcal.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lXi -lm

install: TouchViz TouchViz-ctl
	install -m755 TouchViz /usr/local/bin/TouchViz
	install -m755 TouchViz-ctl /usr/local/bin/TouchViz-ctl

clean:
	rm -f TouchViz mtinject touchprobe touchcal TouchViz.new

.PHONY: all install clean
