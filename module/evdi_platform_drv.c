// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#ifdef CONFIG_USB_SUPPORT
#include <linux/usb.h>
#endif

#include "evdi_params.h"
#include "evdi_debug.h"
#include "evdi_platform_drv.h"
#include "evdi_platform_dev.h"
#include "evdi_sysfs.h"

/**
evdi_init
> g_ctx: init                  # 初始化全局上下文
> [root_device_register]       # 注册根设备
> [dev_set_drvdata]            # 设置驱动的上下文数据到根设备
> [platform_driver_register]   # 注册平台驱动，回调调用
> evdi_platform_add_devices    # 根据配置的初始设备数量，添加设备
  > evdi_platform_device_count # 获取当前设备数量
  > evdi_platform_device_add   # 添加设备
    > evdi_platform_drv_get_free_device    # 有父设备，就从上下文中获取空闲设备
      > {evdi_platform_device_is_free}
    > evdi_platform_drv_create_new_device  # 没有空闲设备，就创建新设备
      > evdi_platform_drv_get_free_idx     # 从上下中找出第一个空闲（devices[i] == NULL）的索引 i
      > {evdi_platform_dev_create}         # 创建新设备并保存到上下文
    > {evdi_platform_device_link}  # 将创建出的新设备和父设备建立关联
 */


MODULE_AUTHOR("DisplayLink (UK) Ltd.");
MODULE_DESCRIPTION("Extensible Virtual Display Interface");
MODULE_LICENSE("GPL");

#define EVDI_DEVICE_COUNT_MAX 16

/**
 * 全局上下文结构。
 * @param root_dev 根设备指针。在 evdi_init 中注册。
 * @param dev_count 当前设备数量
 * @param devices 设备数组。在 evdi_platform_device_add 中添加设备。
 * @param usb_notifier USB 设备通知器。在 evdi_init 中注册。
 * @param lock 互斥锁。在 evdi_platform_device_add 中锁定。
 */
static struct evdi_platform_drv_context {
	struct device *root_dev;
	unsigned int dev_count;
	struct platform_device *devices[EVDI_DEVICE_COUNT_MAX];
#ifdef CONFIG_USB_SUPPORT
	struct notifier_block usb_notifier;
#endif
	struct mutex lock;
} g_ctx;

// 定义互斥锁的宏，方便使用，用来保护 g_ctx 结构体
#define evdi_platform_drv_context_lock(ctx) \
		mutex_lock(&ctx->lock)

#define evdi_platform_drv_context_unlock(ctx) \
		mutex_unlock(&ctx->lock)

#ifdef CONFIG_USB_SUPPORT
/**
 * USB 设备通知器回调函数，当 USB 设备插入或移除时调用。
 * @param nb 通知器块指针（未使用）
 * @param action USB 设备动作 (只处理了移除设备)
 * @param data 数据
 * @return 0 成功，其他 失败
 */
static int evdi_platform_drv_usb(__always_unused struct notifier_block *nb,
		unsigned long action,
		void *data)
{
	struct usb_device *usb_dev = (struct usb_device *)(data); // 获取 USB 设备指针
	struct platform_device *pdev;
	int i = 0;

	if (!usb_dev)
		return 0;
	if (action != BUS_NOTIFY_DEL_DEVICE) // 只处理移除设备的情况
		return 0;

	for (i = 0; i < EVDI_DEVICE_COUNT_MAX; ++i) {
		pdev = g_ctx.devices[i];
		// 跳过空设备
		if (!pdev)
			continue;
		// 将设备和父设备解除关联
		evdi_platform_device_unlink_if_linked_with(pdev, &usb_dev->dev);
		// 如果设备和父设备关联
		if (pdev->dev.parent == &usb_dev->dev) {
			EVDI_INFO("Parent USB removed. Removing evdi.%d\n", i);
			// 销毁设备
			evdi_platform_dev_destroy(pdev);
			evdi_platform_drv_context_lock((&g_ctx));
			// 更新上下文中的设备数量和设备数组
			g_ctx.dev_count--;
			g_ctx.devices[i] = NULL;
			evdi_platform_drv_context_unlock((&g_ctx));
		}
	}
	return 0;
}
#endif

/**
 * 获取空闲设备索引，就是从遍历 ctx->devices 数组，找到第一个是 NULL 的索引。
 * @param ctx 全局上下文结构
 * @return 空闲设备索引，不存在返回 -ENOMEM
 */
static int evdi_platform_drv_get_free_idx(struct evdi_platform_drv_context *ctx)
{
	int i;

	for (i = 0; i < EVDI_DEVICE_COUNT_MAX; ++i) {
		if (ctx->devices[i] == NULL)
			return i;
	}
	return -ENOMEM;
}

/**
 * 获取空闲设备
 * @param ctx 全局上下文结构
 * @return 空闲设备指针，不存在返回 NULL
 */
static struct platform_device *evdi_platform_drv_get_free_device(struct evdi_platform_drv_context *ctx)
{
	int i;
	struct platform_device *pdev = NULL;

	for (i = 0; i < EVDI_DEVICE_COUNT_MAX; ++i) {
		pdev = ctx->devices[i];
		if (pdev && evdi_platform_device_is_free(pdev))
			return pdev;
	}
	return NULL;
}

/**
 * 创建新设备
 * @param ctx 全局上下文结构
 * @return 新设备指针，失败返回 ERR_PTR(-EINVAL)
 */
static struct platform_device *evdi_platform_drv_create_new_device(struct evdi_platform_drv_context *ctx)
{
	struct platform_device *pdev = NULL;
	struct platform_device_info pdevinfo = {
		.parent = NULL,
		.name = DRIVER_NAME,
		.id = evdi_platform_drv_get_free_idx(ctx),
		.res = NULL,
		.num_res = 0,
		.data = NULL,
		.size_data = 0,
		.dma_mask = DMA_BIT_MASK(32),
	};

	if (pdevinfo.id < 0 || ctx->dev_count >= EVDI_DEVICE_COUNT_MAX) {
		EVDI_ERROR("Evdi device add failed. Too many devices.\n");
		return ERR_PTR(-EINVAL);
	}

	pdev = evdi_platform_dev_create(&pdevinfo);
	ctx->devices[pdevinfo.id] = pdev;
	ctx->dev_count++;

	return pdev;
}

/**
 * 添加设备。
 * 父设备为 NULL：创建独立的 EVDI 虚拟显示设备（不关联物理设备）
 * 父设备不为 NULL：将 EVDI 设备与物理设备（如 USB）关联，实现：
 * * 在 sysfs 中建立可见关联
 * * 父设备移除时自动清理
 * * 优先重用空闲设备
 * @param device 设备指针
 * @param parent 父设备指针
 * @return 0 成功，其他 失败
 */
int evdi_platform_device_add(struct device *device, struct device *parent)
{
	// 1. 获取全局上下文结构
	struct evdi_platform_drv_context *ctx =
		(struct evdi_platform_drv_context *)dev_get_drvdata(device);
	struct platform_device *pdev = NULL;

	evdi_platform_drv_context_lock(ctx);
	// 2. 如果父设备存在，则获取空闲设备
	if (parent)
		pdev = evdi_platform_drv_get_free_device(ctx);

	if (IS_ERR_OR_NULL(pdev))
		// 3. 如果空闲设备不存在，则创建新设备
		pdev = evdi_platform_drv_create_new_device(ctx);
	evdi_platform_drv_context_unlock(ctx);

	if (IS_ERR_OR_NULL(pdev))
		return -EINVAL;

	// 4. 链接设备
	// 将 pdev 和 parent 设备在 sysfs 中建立可见关联：创建符号链接
	evdi_platform_device_link(pdev, parent);
	return 0;
}

/**
 * 添加设备
 * @param device 设备指针
 * @param val 设备数量
 * @return 0 成功，其他 失败
 */
int evdi_platform_add_devices(struct device *device, unsigned int val)
{
	// 1. 获取当前设备数量
	unsigned int dev_count = evdi_platform_device_count(device);

	// 2. 如果设备数量为 0，则返回
	if (val == 0) {
		EVDI_WARN("Adding 0 devices has no effect\n");
		return 0;
	}

	// 3. 如果设备数量超过最大数量，则返回
	if (val > EVDI_DEVICE_COUNT_MAX - dev_count) {
		EVDI_ERROR("Evdi device add failed. Too many devices.\n");
		return -EINVAL;
	}

	// 4. 增加设备数量
	EVDI_INFO("Increasing device count to %u\n", dev_count + val);

	// 5. 添加设备
	while (val-- && evdi_platform_device_add(device, NULL) == 0)
		;
	return 0;
}

/**
 * 移除所有设备
 * @param device 设备指针
 */
void evdi_platform_remove_all_devices(struct device *device)
{
	// 1. 获取全局上下文结构
	int i;
	struct evdi_platform_drv_context *ctx =
		(struct evdi_platform_drv_context *)dev_get_drvdata(device);

	evdi_platform_drv_context_lock(ctx);

	// 2. 遍历所有设备
	for (i = 0; i < EVDI_DEVICE_COUNT_MAX; ++i) {
		if (ctx->devices[i]) {
			EVDI_INFO("Removing evdi %d\n", i);
			// 3. 销毁设备
			evdi_platform_dev_destroy(ctx->devices[i]);
			g_ctx.dev_count--;
			g_ctx.devices[i] = NULL;
		}
	}

	// 4. 设置设备数量为 0
	ctx->dev_count = 0;
	evdi_platform_drv_context_unlock(ctx);
}

/**
 * 获取设备数量
 * @param device 设备指针
 * @return 设备数量
 */
unsigned int evdi_platform_device_count(struct device *device)
{
	unsigned int count = 0;
	struct evdi_platform_drv_context *ctx = NULL;

	// 1. 获取全局上下文结构
	ctx = (struct evdi_platform_drv_context *)dev_get_drvdata(device);

	// 2. 锁定互斥锁
	evdi_platform_drv_context_lock(ctx);

	// 3. 获取设备数量
	count = ctx->dev_count;

	// 4. 解锁互斥锁
	evdi_platform_drv_context_unlock(ctx);

	return count;

}

/**
 * 平台设备驱动结构体。这是关键！
 * @param probe 当设备发现匹配的设备时自动调用。
 * @param remove 当设备移除时调用。
 * @param driver 驱动结构体。
 * @param name 驱动名称。 // evdi
 * @param mod_name 模块名称。 // evdi
 * @param owner 模块所有者
 */
static struct platform_driver evdi_platform_driver = {
	.probe = evdi_platform_device_probe,
	.remove = evdi_platform_device_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .mod_name = KBUILD_MODNAME,
		   .owner = THIS_MODULE,
	}
};

// 模块入口函数
static int __init evdi_init(void)
{
	int ret;

	// 1. 初始化日志。
	EVDI_INFO("Initialising logging on level %u\n", evdi_loglevel);
	EVDI_INFO("Atomic driver: yes\n");

	// 2. 初始化全局上下文结构。
	memset(&g_ctx, 0, sizeof(g_ctx));

	// 3. 注册根设备（在 /sys/devices 下创建目录）
	g_ctx.root_dev = root_device_register(DRIVER_NAME);
#ifdef CONFIG_USB_SUPPORT  // https://www.kernelconfig.io/config_usb_support
	g_ctx.usb_notifier.notifier_call = evdi_platform_drv_usb;
#endif

	// 4. 初始化互斥锁（保护共享数据）
	mutex_init(&g_ctx.lock);
	dev_set_drvdata(g_ctx.root_dev, &g_ctx);

#ifdef CONFIG_USB_SUPPORT
	usb_register_notify(&g_ctx.usb_notifier);
#endif

	// 5. 初始化 sysfs 接口（用户空间可以通过 /sys 操作）
	evdi_sysfs_init(g_ctx.root_dev);

	// 6. 注册平台驱动（这是关键！）
	ret = platform_driver_register(&evdi_platform_driver);
	if (ret)
		return ret;

	// 7. 添加初始设备（如果配置了初始设备数量）
	if (evdi_initial_device_count)
		return evdi_platform_add_devices(
			g_ctx.root_dev, evdi_initial_device_count);

	return 0;
}

// 模块出口函数
static void __exit evdi_exit(void)
{
	EVDI_CHECKPT();
	// 1. 移除所有设备
	evdi_platform_remove_all_devices(g_ctx.root_dev);

	// 2. 注销平台驱动
	platform_driver_unregister(&evdi_platform_driver);

	if (!PTR_ERR_OR_ZERO(g_ctx.root_dev)) {
		// 3. 清理 sysfs 接口
		evdi_sysfs_exit(g_ctx.root_dev);

#ifdef CONFIG_USB_SUPPORT
		// 4. 注销 USB 设备通知器
		usb_unregister_notify(&g_ctx.usb_notifier);
#endif
		// 5. 清理根设备
		dev_set_drvdata(g_ctx.root_dev, NULL);
		// 6. 注销根设备
		root_device_unregister(g_ctx.root_dev);
	}
	EVDI_INFO("Exit %s driver\n", DRIVER_NAME);
}

// 这是模块的入口和出口函数，告诉内核：加载时调用 evdi_init，卸载时调用 evdi_exit
module_init(evdi_init);
module_exit(evdi_exit);
