# TouchViz / mtinject 构建
# 依赖(Debian/Ubuntu 包名):
#   运行：libxi6 libcairo2 libxtst6 libxfixes3 libx11-6
#   编译：libxi-dev libcairo2-dev libxtst-dev libxfixes-dev libxext-dev libx11-dev
#   测试工具 mtinject：libx11-dev
#   测试工具 touchprobe：libxi-dev libxtst-dev libxfixes-dev

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS   = -lX11 -lXi -lXext -lXtst -lXfixes -lcairo -lm

all: TouchViz mtinject touchprobe touchcal

TouchViz: TouchViz.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

mtinject: tools/mtinject.c
	$(CC) $(CFLAGS) -o $@ $< -lX11

touchprobe: tools/touchprobe.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lXi -lXtst -lXfixes -lm

touchcal: tools/touchcal.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lXi -lm

install: TouchViz TouchViz-ctl
	install -m755 TouchViz /usr/local/bin/TouchViz
	install -m755 TouchViz-ctl /usr/local/bin/TouchViz-ctl

clean:
	rm -f TouchViz mtinject touchprobe touchcal TouchViz.new

.PHONY: all install clean