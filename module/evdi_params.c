// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>

#include "evdi_params.h"
#include "evdi_debug.h"

// 更改默认日志等级为 VERBOSE，打印所有的日志。
unsigned int evdi_loglevel __read_mostly = EVDI_LOGLEVEL_VERBOSE;
unsigned short int evdi_initial_device_count __read_mostly;

/**
 * 为了使用下面这些模块参数，可以
 *     `sudo modprobe evdi initial_loglevel=5 initial_device_count=4`
 * 权限说明：
 *     - 0400：只有 root 内核可以读取，不能写
 *     - 0644：root 可以读写，其他用户可读
 *     - 0664：root 可以读写，同组用户可读写
 *     - 0666：只有 root 可读写，其他用户无法访问
 * 
 * sysfs 文件的读写规则：
 *     模块参数在 /sys/module/<modulename>/parameters/<paramname> 下生成文件
 *   读取：
 *     - cat /sys/module/<modulename>/parameters/<paramname>
 *   写入：
 *     - echo 5 > /sys/module/<modulename>/parameters/<paramname>
 */

module_param_named(initial_loglevel, evdi_loglevel, int, 0400);
MODULE_PARM_DESC(initial_loglevel, "Initial log level");

module_param_named(initial_device_count,
		   evdi_initial_device_count, ushort, 0644);
MODULE_PARM_DESC(initial_device_count, "Initial DRM device count (default: 0)");

