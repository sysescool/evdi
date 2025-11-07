#
# Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
#

FLAGS=-Werror -Wextra -Wall -Wno-error=missing-field-initializers -Werror=sign-compare
FLAGS_C=$(FLAGS) -Wmissing-prototypes -Wstrict-prototypes -Werror=discarded-qualifiers
FLAGS_CXX=$(FLAGS)

.PHONY: module library pyevdi
all: module library pyevdi

# $(MAKE) → 调用子 Make
#     -C module: 进入module目录，执行make命令
#     $(MFLAGS) → 把当前 make 的参数传递给子 Make
# CFLAGS=... → 为子 Make 指定编译器选项
#     -isystem./include -isystem./include/uapi → 头文件搜索路径
#         即当前 Makefile 所在目录下的 ./include 和 ./include/uapi 目录
#     $(FLAGS_C) → 上面定义的通用 C 编译选项
#     $(CFLAGS) → 如果外部有设置 CFLAGS，会追加
# 最终变成：
#     CFLAGS="-isystem./include -isystem./include/uapi \
#             -Werror -Wextra -Wall -Wno-error=missing-field-initializers -Werror=sign-compare \ # $(FLAGS)
#             -Wmissing-prototypes -Wstrict-prototypes -Werror=discarded-qualifiers \  # $(FLAGS_C)
#             " \ # $(CFLAGS) 默认是空，除非你在外面定义了环境变量
#             $(MAKE) -C module $(MFLAGS)
#     这就是一条指令，前面的 CFLAGS 是执行 make -C module 的环境变量。
module:
	CFLAGS="-isystem./include -isystem./include/uapi $(FLAGS_C) $(CFLAGS)" $(MAKE) -C module $(MFLAGS)

library:
	CFLAGS="-I../module $(FLAGS_C) $(CFLAGS)" $(MAKE) -C library $(MFLAGS)

pyevdi:
	CXXFLAGS="-I../module -I../library $(FLAGS_CXX) $(CXXFLAGS)" $(MAKE) -C pyevdi $(MFLAGS)

module-rc:
	ci/build_against_kernel --repo-ci rc

all-with-rc-linux: module-rc library pyevdi

install:
	$(MAKE) -C module install
	$(MAKE) -C library install
	$(MAKE) -C pyevdi install

uninstall:
	$(MAKE) -C module uninstall
	$(MAKE) -C library uninstall
	$(MAKE) -C pyevdi uninstall

clean:
	$(MAKE) clean -C module $(MFLAGS)
	$(MAKE) clean -C library $(MFLAGS)
	$(MAKE) clean -C pyevdi $(MFLAGS)
