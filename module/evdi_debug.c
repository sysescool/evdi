// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2015 - 2019 DisplayLink (UK) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/sched.h>
#include <linux/proc_fs.h>

#include "evdi_debug.h"

/**
 * 把当前任务（线程）和所属进程的信息写入 buf。
 * @param buf 字符缓冲区
 * @param size 字符缓冲区大小
 */
void evdi_log_process(char *buf, size_t size)
{
	// 内核宏，指向当前执行的 task_struct 结构体。获取任务的 PID。
	int task_pid = (int)task_pid_nr(current);
	char task_comm[TASK_COMM_LEN] = { 0 };

	// 内核宏，获取任务（线程）的名称。
	get_task_comm(task_comm, current);

	// 如果任务（线程）属于一个进程，则获取进程的名称。
	if (current->group_leader) {
		char process_comm[TASK_COMM_LEN] = { 0 };

		get_task_comm(process_comm, current->group_leader);
		// 格式化字符串，将任务（线程）和进程的信息写入 buf。
		snprintf(buf, size, "Task %d (%s) of process %d (%s)",
			  task_pid,
			  task_comm,
			  (int)task_pid_nr(current->group_leader),
			  process_comm);
	} else {
		// 格式化字符串，将任务（线程）的信息写入 buf。
		snprintf(buf, size, "Task %d (%s)",
			  task_pid,
			  task_comm);
	}
}
