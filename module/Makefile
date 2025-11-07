#
# Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
#
# This file is subject to the terms and conditions of the GNU General Public
# License v2. See the file COPYING in the main directory of this archive for
# more details.
#

# 操作系统检测
include /etc/os-release
## 如果操作系统是RHEL，则设置ELFLAG，定义 -DEL+版本号
ifneq (,$(findstring rhel,$(ID_LIKE)))
ELFLAG := -DEL$(VERSION_ID)
endif

## 如果操作系统是Raspberry Pi，则设置RPIFLAG，定义 -DRPI
Raspbian := $(shell grep -Eic 'raspb(erry|ian)' /proc/cpuinfo /etc/os-release 2>/dev/null )
ifeq (,$(findstring 0, $(Raspbian)))
RPIFLAG := -DRPI
endif



#   ┌─────────────────────────────────┐
#   │  DKMS_BUILD 是否定义？            │
#   └────────────┬────────────────────┘
#                │
#       ┌────────┴────────┐
#       │                 │
#      是                否
#       │                 │
#       │        ┌────────┴────────┐
#       │        │ KERNELRELEASE   │
#       │        │ 是否定义？        │
#       │        └────────┬────────┘
#       │                 │
#       │        ┌────────┴────────┐
#       │        │                 │
#       │        是                否
#       │        │                 │
#       │        │                 │
#       ▼        ▼                 ▼
#    DKMS     kbuild内部      用户直接运行
#    分支      分支            make 分支


# DKMS 构建模式
## 如果DKMS_BUILD不为空:
ifneq ($(DKMS_BUILD),)

# DKMS
# 设置KERN_DIR，定义 /lib/modules/$(KERNELRELEASE)/build
# 由于这是编译内核模块，所以会递归调用内核构建系统编译模块，由 DKMS 系统提供 KERNELRELEASE 变量，表示当前内核版本。
KERN_DIR := /lib/modules/$(KERNELRELEASE)/build

# 编译选项，包含头文件路径和系统标志
ccflags-y := -Iinclude/uapi/drm -Iinclude/drm $(ELFLAG) $(RPIFLAG)

# 组成 evdi.ko 的文件列表：
#      evdi_platform_drv.o 是主模块文件
#      evdi_platform_dev.o 是平台设备文件
#      evdi_sysfs.o 是系统文件
#      evdi_modeset.o 是模式设置文件
#      evdi_connector.o 是连接器文件
#      evdi_encoder.o 是编码器文件
#      evdi_drm_drv.o 是 DRM 驱动文件
#      evdi_fb.o 是帧缓冲文件
#      evdi_gem.o 是全局内存文件
#      evdi_painter.o 是绘图文件
#      evdi_params.o 是参数文件
#      evdi_cursor.o 是光标文件
#      evdi_debug.o 是调试文件
#      evdi_i2c.o 是 I2C 文件
evdi-y := evdi_platform_drv.o evdi_platform_dev.o evdi_sysfs.o evdi_modeset.o evdi_connector.o evdi_encoder.o evdi_drm_drv.o evdi_fb.o evdi_gem.o evdi_painter.o evdi_params.o evdi_cursor.o evdi_debug.o evdi_i2c.o
# 如果配置了 COMPAT 模块，则编译 evdi_ioc32.o 文件。 提供 32 位兼容性支持。
evdi-$(CONFIG_COMPAT) += evdi_ioc32.o
# 生成可加载模块 evdi.ko
obj-m := evdi.o

KBUILD_VERBOSE ?= 1

# 调用内核构建系统编译 evdi.ko 模块。
## KBUILD_VERBOSE=1 表示编译过程详细输出
## M=$(CURDIR) 表示当前目录，即 evdi 模块目录
## SUBDIRS=$(CURDIR) 表示当前目录，即 evdi 模块目录
## SRCROOT=$(CURDIR) 表示当前目录，即 evdi 模块目录
## CONFIG_MODULE_SIG= 表示不签名模块
## -C $(KERN_DIR) 表示进入内核构建系统目录，即 /lib/modules/$(KERNELRELEASE)/build
## modules 表示编译模块
## 最终变成：
##     make KBUILD_VERBOSE=1 M=$(CURDIR) SUBDIRS=$(CURDIR) SRCROOT=$(CURDIR) CONFIG_MODULE_SIG= -C $(KERN_DIR) modules
##     这就是一条指令，前面的 KBUILD_VERBOSE=1 是执行 make KBUILD_VERBOSE=1 M=$(CURDIR) SUBDIRS=$(CURDIR) SRCROOT=$(CURDIR) CONFIG_MODULE_SIG= -C $(KERN_DIR) modules 的环境变量。  
all:
	echo "111111111";
	$(MAKE) KBUILD_VERBOSE=$(KBUILD_VERBOSE) M=$(CURDIR) SUBDIRS=$(CURDIR) SRCROOT=$(CURDIR) CONFIG_MODULE_SIG= -C $(KERN_DIR) modules

clean:
	@echo $(KERN_DIR)
	$(MAKE) KBUILD_VERBOSE=$(KBUILD_VERBOSE) M=$(CURDIR) SUBDIRS=$(CURDIR) SRCROOT=$(CURDIR) -C $(KERN_DIR) clean

else

# Not DKMS
# 非 DKMS，手动编译内核。
ifneq ($(KERNELRELEASE),)

# inside kbuild
# Note: this can be removed once it is in kernel tree and Kconfig is properly used

# 编译选项，包含头文件路径和系统标志
ccflags-y := -isystem include/uapi/drm $(CFLAGS) $(ELFLAG) $(RPIFLAG)
# 组成 evdi.ko 的文件列表：
## 1. 内核构建系统看到 evdi-y
## 2. 自动查找对应的 .c 文件（如 evdi_platform_drv.c）
## 3. 编译每个 .c 生成 .o
## 4. 将所有 .o 链接成 evdi.ko
evdi-y := evdi_platform_drv.o evdi_platform_dev.o evdi_sysfs.o evdi_modeset.o evdi_connector.o evdi_encoder.o evdi_drm_drv.o evdi_fb.o evdi_gem.o evdi_painter.o evdi_params.o evdi_cursor.o evdi_debug.o evdi_i2c.o
# 如果配置了 COMPAT 模块，则编译 evdi_ioc32.o 文件。 提供 32 位兼容性支持。
evdi-$(CONFIG_COMPAT) += evdi_ioc32.o
# 配置 DRM_EVDI 模块，设置为 m，表示编译为模块，y 表示编译进内核。
CONFIG_DRM_EVDI ?= m
# 配置 DRM_EVDI 模块，设置为 m，表示编译为模块。
obj-$(CONFIG_DRM_EVDI) := evdi.o
# 编译 tests 模块。
obj-y += tests/

##### y 和 m 的区别 #####
# ---- y 表示编译进内核。
# # 编译时
# # 1. 编译所有源文件
# gcc -c evdi_platform_drv.c -o evdi_platform_drv.o
# # ... 编译其他文件
# 
# # 2. 链接进内核（不是生成 .ko）
# ld -r ... evdi_platform_drv.o ... -o built-in.o
# # built-in.o 会被链接到内核镜像中
# 
# # 3. 最终结果
# # 内核镜像 vmlinuz-6.8.0-85-generic 包含 evdi 的代码
# ---- m 表示编译为模块。
# # 编译时
# # 1. 编译所有源文件（和 obj-y 相同）
# gcc -c evdi_platform_drv.c -o evdi_platform_drv.o
# # ... 编译其他文件
# 
# # 2. 链接成模块文件（不是链接进内核）
# ld -r evdi_platform_drv.o ... -o evdi.o
# # 然后处理模块符号、版本信息等
# 
# # 3. 生成最终模块
# # 生成 evdi.ko 文件（独立文件）
# 
# # 4. 使用方式
# sudo insmod evdi.ko      # 加载模块
# sudo rmmod evdi          # 卸载模块
# sudo modprobe evdi       # 加载模块（推荐）

$(info "222222222")

else

$(info "333333333")
# kbuild against specified or current kernel
CP ?= cp
DKMS ?= dkms
RM ?= rm

MODVER=1.14.11

ifeq ($(KVER),)
	KVER := $(shell uname -r)
endif

ifneq ($(RUN_DEPMOD),)
	DEPMOD := /sbin/depmod -a
else
	DEPMOD := true
endif

ifeq ($(KDIR),)
	KDIR := /lib/modules/$(KVER)/build
endif

MOD_KERNEL_PATH := /kernel/drivers/gpu/drm/evdi

default: module


# 调用内核构建系统编译 evdi.ko 模块。
## KDIR 是内核构建系统目录，即 /lib/modules/$(KVER)/build
## -C $(KERN_DIR) 表示进入内核构建系统目录，即 /lib/modules/$(KERNELRELEASE)/build
## M=$$PWD 表示当前目录，即 evdi 模块目录
## 最终变成：
# 实际执行的命令
#    make -C /lib/modules/6.8.0-85-generic/build M=/home/qch/syc/CodeGithub/evdi/module M=$(pwd)
# 内核构建系统的工作：
# 1. cd /lib/modules/6.8.0-85-generic/build
# 2. 读取顶层 Makefile
# 3. 发现 M= 参数，知道要编译外部模块
# 4. 设置 KERNELRELEASE=6.8.0-85-generic
# 5. 读取 /home/qch/syc/CodeGithub/evdi/module/Makefile
# 6. 此时 KERNELRELEASE 已定义，进入 "inside kbuild" 分支
# 7. 使用 evdi-y 和 obj-m 定义编译模块
# 8. 生成 evdi.ko
module:
	echo "444444444"
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	$(RM) -rf *.o *.a *.ko .tmp* .*.*.cmd Module.symvers evdi.mod.c modules.order


# 实际执行：
# 第一步：
# make -C /lib/modules/6.8.0-85-generic/build \
#       M=/home/qch/syc/CodeGithub/evdi/module \
#       INSTALL_MOD_PATH= \
#       INSTALL_MOD_DIR=/kernel/drivers/gpu/drm/evdi \
#       modules_install

# 这会将 evdi.ko 复制到：
# /lib/modules/6.8.0-85-generic/kernel/drivers/gpu/drm/evdi/evdi.ko

# 第二步：
# /sbin/depmod -a
# 更新模块依赖数据库
install:
	$(MAKE) -C $(KDIR) M=$$PWD INSTALL_MOD_PATH=$(DESTDIR) INSTALL_MOD_DIR=$(MOD_KERNEL_PATH) modules_install
	$(DEPMOD)

uninstall:
	$(RM) -rf $(DESTDIR)/lib/modules/$(KVER)/$(MOD_KERNEL_PATH)
	$(DEPMOD)

install_dkms:
	$(DKMS) install .

uninstall_dkms:
	$(DKMS) remove evdi/$(MODVER) --all
	$(RM) -rf /usr/src/evdi-$(MODVER)

endif # ifneq ($(KERNELRELEASE),)

endif # ifneq ($(DKMS_BUILD),)
