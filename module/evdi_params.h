/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#ifndef EVDI_PARAMS_H
#define EVDI_PARAMS_H

/**
 * 日志等级。只有 >= evdi_loglevel 的日志才会打印。
 */
extern unsigned int evdi_loglevel;

/**
 * 初始设备数量。在 evdi_init 中添加初始设备。
 */
extern unsigned short int evdi_initial_device_count;

#endif /* EVDI_PARAMS_H */
