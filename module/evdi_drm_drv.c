// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 *
 * Based on parts on udlfb.c:
 * Copyright (C) 2009 its respective authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/version.h>
#if KERNEL_VERSION(5, 16, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include <drm/drm_vblank.h>
#elif KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE
#else
#include <drm/drmP.h>
#endif
#if KERNEL_VERSION(5, 1, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_probe_helper.h>
#endif
#if KERNEL_VERSION(5, 8, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_managed.h>
#endif
#include <drm/drm_atomic_helper.h>
#include "evdi_drm_drv.h"
#include "evdi_platform_drv.h"
#include "evdi_cursor.h"
#include "evdi_debug.h"
#include "evdi_drm.h"

// 核心概念图解：
// 
// - 向 Linux 内核注册 EVDI 为 DRM 驱动
// - 提供用户空间与内核的通信接口（ioctl）
// - 管理虚拟显示器的生命周期
//
//         用户程序（evdi_lib）
//             ↓ ioctl 调用
//         /dev/dri/card0（设备文件）
//             ↓ 通过 file_operations
//         evdi_drm_drv.c（DRM 驱动）
//             ↓ 查找 ioctl 映射表
//         evdi_painter_ioctls[]
//             ↓ 调用对应函数
//         evdi_painter_connect_ioctl() 等
//

// evdi_drm_device_create
//   > [drm_dev_alloc]
//   > evdi_drm_device_init
//     > {evdi_painter_init}
//     > {evdi_cursor_init}
//     > {evdi_modeset_init}
//     > {evdi_fbdev_init}
//     > [drm_vblank_init]
//     > [drm_kms_helper_poll_init]
//     > [drmm_add_action_or_reset]
//   > drm_dev_register


#if KERNEL_VERSION(6, 8, 0) <= LINUX_VERSION_CODE || defined(EL9)
#define EVDI_DRM_UNLOCKED 0
#else
#define EVDI_DRM_UNLOCKED DRM_UNLOCKED
#endif

static struct drm_driver driver;

/**
 * EVDI 驱动支持的自定义 IOCTL（设备控制命令）接口
 * 这些 ioctl 是内核空间 EVDI 驱动与用户空间程序（evdi_lib）通信的接口。
 * 	- 用户态程序使用 ioctl(fd, command, arg) 调用
 *  - 由 drm_ioctl_desc 表映射到具体的内核函数执行
 * EVDI_DRM_UNLOCKED：可并发执行（不需要大锁）
 */
struct drm_ioctl_desc evdi_painter_ioctls[] = {
	// 建立显示连接
	DRM_IOCTL_DEF_DRV(EVDI_CONNECT, evdi_painter_connect_ioctl, EVDI_DRM_UNLOCKED),
	// 请求刷新 framebuffer
	DRM_IOCTL_DEF_DRV(EVDI_REQUEST_UPDATE, evdi_painter_request_update_ioctl, EVDI_DRM_UNLOCKED),
	// 获取像素内容
	DRM_IOCTL_DEF_DRV(EVDI_GRABPIX, evdi_painter_grabpix_ioctl, EVDI_DRM_UNLOCKED),
	// 处理 DDC/CI 通信
	DRM_IOCTL_DEF_DRV(EVDI_DDCCI_RESPONSE, evdi_painter_ddcci_response_ioctl, EVDI_DRM_UNLOCKED),
	// 启用光标事件
	DRM_IOCTL_DEF_DRV(EVDI_ENABLE_CURSOR_EVENTS, evdi_painter_enable_cursor_events_ioctl, EVDI_DRM_UNLOCKED),
};

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
static const struct vm_operations_struct evdi_gem_vm_ops = {
	.fault = evdi_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};
#endif

/**
 * 内核 VFS 层：文件操作函数表，用于处理用户空间访问 /dev/dri/cardX 的行为。
 * 是 用户 --> 内核 VFS --> 对应函数。
 */
static const struct file_operations evdi_driver_fops = {
	.owner = THIS_MODULE,
	// 打开 /dev/dri/cardX 时调用 .open
	.open = drm_open,
	// 内存映射时调用 .mmap
	.mmap = evdi_drm_gem_mmap,
	// 轮询事件时调用 .poll
	.poll = drm_poll,
	// 读取事件时调用 .read
	.read = drm_read,
	// ioctl 调用时调用 .unlocked_ioctl
	.unlocked_ioctl = drm_ioctl,
	// 关闭 /dev/dri/cardX 时调用 .release
	.release = drm_release,

#ifdef CONFIG_COMPAT
	// 32位兼容时调用 .compat_ioctl，用于处理32位应用程序的 ioctl 调用
	.compat_ioctl = evdi_compat_ioctl,
#endif

	// 不支持 seek 操作时调用 .llseek
	.llseek = noop_llseek,

#if defined(FOP_UNSIGNED_OFFSET)
	.fop_flags = FOP_UNSIGNED_OFFSET,
#endif
};

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
static int evdi_enable_vblank(__always_unused struct drm_device *dev,
			      __always_unused unsigned int pipe)
{
	return 1;
}

static void evdi_disable_vblank(__always_unused struct drm_device *dev,
				__always_unused unsigned int pipe)
{
}
#endif

/**
 * DRM 驱动结构体
 * 这个结构体是 Linux DRM 框架中定义的一个「驱动描述符」，每个显卡驱动都必须定义它。
 */
static struct drm_driver driver = {
#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE || defined(EL8)
	// 驱动支持的特性：
	// DRIVER_MODESET：支持 KMS（Kernel Mode Setting）显示模式管理
	// DRIVER_GEM：使用 GEM（Graphics Execution Manager）显存管理
	// DRIVER_ATOMIC：支持原子化模式设置（atomic modesetting）
	// DRIVER_PRIME：支持 Prime 缓冲区共享（用于进程间共享）
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
#else
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME
			 | DRIVER_ATOMIC,
#endif
	// 打开 /dev/dri/cardX 时调用 .open
	.open = evdi_driver_open,
	// 关闭 /dev/dri/cardX 时调用 .postclose
	.postclose = evdi_driver_postclose,

// 内核新版本 (≥6.15) 中，如果启用了 framebuffer 子系统，则在 probe 时自动创建一个 fb 设备。
#if KERNEL_VERSION(6, 15, 0) <= LINUX_VERSION_CODE
#ifdef CONFIG_FB
	// 在 probe 时自动创建一个 fb 设备。
	.fbdev_probe = evdifb_create,
#endif
#endif

	/* gem hooks */ // GEM 内存(显存)管理回调，处理 GPU buffer 的生命周期：
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#elif KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
	// 释放 GEM 对象时调用
	.gem_free_object_unlocked = evdi_gem_free_object,
#else
	.gem_free_object = evdi_gem_free_object,
#endif

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	// 虚拟内存映射操作（mmap）
	.gem_vm_ops = &evdi_gem_vm_ops,
#endif
	// 为 “dumb buffer” 提供支持（CPU 可访问的帧缓存）
	.dumb_create = evdi_dumb_create,
	// 映射 dumb buffer 时调用
	.dumb_map_offset = evdi_gem_mmap,
#if KERNEL_VERSION(5, 12, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	.dumb_destroy = drm_gem_dumb_destroy,
#endif

	// DRM IOCTL 分发层：EVDI 驱动支持的自定义 IOCTL（设备控制命令）接口
	// 用于处理用户空间与内核空间的通信
	// 定义自定义命令号与对应处理函数
	.ioctls = evdi_painter_ioctls,
	.num_ioctls = ARRAY_SIZE(evdi_painter_ioctls),

	// 底层系统调用层：文件操作函数表，用于处理用户空间访问 /dev/dri/cardX 的行为
	// 处理 open/read/mmap/ioctl 等系统调用
	.fops = &evdi_driver_fops,

	// PRIME buffer 共享（跨设备 DMA）: 这些用于在不同 GPU / DRM 驱动之间共享显存句柄，比如通过 DMA-BUF 机制。
	// 导入 Prime 缓冲区时调用
	.gem_prime_import = drm_gem_prime_import,
#if KERNEL_VERSION(6, 6, 0) <= LINUX_VERSION_CODE || defined(EL9)
#else
	// 将 Prime 缓冲区转换为文件描述符
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	// 将文件描述符转换为 Prime 缓冲区
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
#endif
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	.preclose = evdi_driver_preclose,
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_get_sg_table = evdi_prime_get_sg_table,
	// 虚拟显示设备的 “垂直同步中断” 开关。EVDI 虽然是虚拟显示，但仍然需要模拟 vblank，以驱动上层 compositor 的帧刷新。
	.enable_vblank = evdi_enable_vblank,
	.disable_vblank = evdi_disable_vblank,
#endif
	.gem_prime_import_sg_table = evdi_prime_import_sg_table,

	// 驱动元信息：驱动名称、描述、版本号等。
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
#if KERNEL_VERSION(6, 14, 0) <= LINUX_VERSION_CODE
#else
	.date = DRIVER_DATE,
#endif
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCH,
};

static void evdi_drm_device_release_cb(__always_unused struct drm_device *dev,
				       __always_unused void *ptr)
{
	struct evdi_device *evdi = dev->dev_private;

	evdi_cursor_free(evdi->cursor);
	evdi_painter_cleanup(evdi->painter);
	kfree(evdi);
	dev->dev_private = NULL;
	EVDI_INFO("Evdi drm_device removed.\n");

	EVDI_TEST_HOOK(evdi_testhook_drm_device_destroyed());
}

/**
 * 初始化 DRM 设备
 * @param dev DRM 设备指针
 * @return 0 成功，其他 失败
 * @note 在 evdi_drm_device_create 函数中调用。
 */
static int evdi_drm_device_init(struct drm_device *dev)
{
	struct evdi_device *evdi;
	int ret;

	EVDI_CHECKPT();

	// 1. 分配 evdi_device 结构
	evdi = kzalloc(sizeof(struct evdi_device), GFP_KERNEL);
	if (!evdi)
		return -ENOMEM;

	// 2. 初始化基本字段
	evdi->ddev = dev;
	evdi->dev_index = dev->primary->index;
	evdi->cursor_events_enabled = false;
	dev->dev_private = evdi; // 将 evdi 保存到 drm_device 中

	// 3. 初始化 painter（与用户空间通信）
	ret = evdi_painter_init(evdi);
	if (ret)
		goto err_free;

	// 4. 初始化光标
	ret =  evdi_cursor_init(&evdi->cursor);
	if (ret)
		goto err_free;

	// 5. 初始化模式设置（分辨率、连接器等）
	evdi_modeset_init(dev);

	// 6. 初始化帧缓冲设备（可选）
#ifdef CONFIG_FB
	ret = evdi_fbdev_init(dev);
		if (ret)
			goto err_init;
	#endif /* CONFIG_FB */

	// 7. 初始化垂直空白（vblank）
	ret = drm_vblank_init(dev, 1);
	if (ret)
		goto err_init;

	// 8. 初始化轮询（检测连接状态）
	drm_kms_helper_poll_init(dev);

#if KERNEL_VERSION(5, 8, 0) <= LINUX_VERSION_CODE || defined(EL8)
	// 9. 注册清理回调（设备销毁时自动调用）
	ret = drmm_add_action_or_reset(dev, evdi_drm_device_release_cb, NULL);
	if (ret)
		goto err_init;
#endif

	return 0;

err_init:
#ifdef CONFIG_FB
	evdi_fbdev_cleanup(dev);
#endif /* CONFIG_FB */
err_free:
	EVDI_ERROR("Failed to setup drm device %d\n", ret);
	evdi_cursor_free(evdi->cursor);
	kfree(evdi->painter);
	kfree(evdi);
	dev->dev_private = NULL;
	return ret;
}

int evdi_driver_open(struct drm_device *dev, __always_unused struct drm_file *file)
{
	char buf[100];

	evdi_log_process(buf, sizeof(buf));
	EVDI_INFO("(card%d) Opened by %s\n", dev->primary->index, buf);
	return 0;
}

static void evdi_driver_close(struct drm_device *drm_dev, struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;

	EVDI_CHECKPT();
	if (evdi)
		evdi_painter_close(evdi, file);
}

void evdi_driver_preclose(struct drm_device *drm_dev, struct drm_file *file)
{
	evdi_driver_close(drm_dev, file);
}

void evdi_driver_postclose(struct drm_device *dev, struct drm_file *file)
{
	char buf[100];

	evdi_log_process(buf, sizeof(buf));
	evdi_driver_close(dev, file);
	EVDI_INFO("(card%d) Closed by %s\n", dev->primary->index, buf);
}

/**
 * 创建 DRM 设备
 * @param parent 父设备指针
 * @return DRM 设备指针
 * @note 在平台设备被探测到时调用。即 evdi_platform_dev.c 中的 evdi_platform_device_probe 函数中调用。
 */
struct drm_device *evdi_drm_device_create(struct device *parent)
{
	struct drm_device *dev = NULL;
	int ret;

	// 1. 分配 DRM 设备结构体
	dev = drm_dev_alloc(&driver, parent);
	if (IS_ERR(dev))
		return dev;

	// 2. 初始化 DRM 设备
	ret = evdi_drm_device_init(dev);
	if (ret)
		goto err_free;

	// 3. 注册 DRM 设备
	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_free;

	return dev;

err_free:
	drm_dev_put(dev);
	return ERR_PTR(ret);
}

static void evdi_drm_device_deinit(struct drm_device *dev)
{
	drm_kms_helper_poll_fini(dev);
#ifdef CONFIG_FB
	evdi_fbdev_unplug(dev);
	evdi_fbdev_cleanup(dev);
#endif /* CONFIG_FB */
	evdi_modeset_cleanup(dev);
	drm_atomic_helper_shutdown(dev);
}

int evdi_drm_device_remove(struct drm_device *dev)
{
	drm_dev_unplug(dev);
	evdi_drm_device_deinit(dev);
#if KERNEL_VERSION(5, 8, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	evdi_drm_device_release_cb(dev, NULL);
#endif
	drm_dev_put(dev);
	return 0;
}

