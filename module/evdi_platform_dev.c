// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 DisplayLink (UK) Ltd.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "evdi_platform_dev.h"
#include <linux/version.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "evdi_platform_drv.h"
#include "evdi_debug.h"
#include "evdi_drm_drv.h"

/**
 * 设备数据结构
 * @param drm_dev DRM 设备指针
 * @param parent 父设备指针
 * @param symlinked 是否已建立关联
 */
struct evdi_platform_device_data {
	struct drm_device *drm_dev;
	struct device *parent;
	bool symlinked;
};

/**
 * 创建设备。
 * 所谓创建设备，就是注册一个平台设备 platform_device，并设置设备名称和驱动名称。
 * `platform_device`：代表一个虚拟显示设备
 * @param info 设备信息
 * @return 设备指针，失败返回 NULL
 */
struct platform_device *evdi_platform_dev_create(struct platform_device_info *info)
{
	struct platform_device *platform_dev = NULL;

	// 注册平台设备。在这一步，就会进行 设备名称 info->name 和 驱动名称 driver->name 的匹配。
	// 如果匹配成功，就会调用 evdi_platform_device_probe 函数。
	platform_dev = platform_device_register_full(info);
	// 设置 DMA 掩码为 64 位，如果失败则提示并保持 32 位
	// DMA 是一种硬件机制，允许设备直接访问内存，提高数据传输效率，无需 CPU 参与每次数据传输。
	if (dma_set_mask(&platform_dev->dev, DMA_BIT_MASK(64))) { 
		EVDI_WARN("Unable to change dma mask to 64 bit. ");
		EVDI_WARN("Sticking with 32 bit\n");
	}

	EVDI_INFO("Evdi platform_device create\n");

	return platform_dev;
}

/**
 * 销毁设备。在 evdi_exit 及 usb 设备移除时调用。
 * @param dev 设备指针
 */
void evdi_platform_dev_destroy(struct platform_device *dev)
{
	platform_device_unregister(dev);
	EVDI_INFO("Evdi platform_device destroy\n");
}

/**
 * 设备探测。在设备匹配时调用。
 *   当发现一个设备 pdev->name == 驱动名称 driver->name 时，即会调用此函数。
 *   所谓设备探测，就是驱动匹配到了设备，然后进行设备初始化。也就是创建 DRM 设备。
 * @param pdev 设备指针
 * @return 0 成功，其他 失败
 */
int evdi_platform_device_probe(struct platform_device *pdev)
{
	struct drm_device *dev;
	struct evdi_platform_device_data *data;

	EVDI_CHECKPT();
	// 分配设备数据结构
	data = kzalloc(sizeof(struct evdi_platform_device_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	// 设置虚拟 IOMMU 域，避免平台总线与 Intel IOMMU 的兼容问题
	#if IS_ENABLED(CONFIG_IOMMU_API) && defined(CONFIG_INTEL_IOMMU)
	/* Intel-IOMMU workaround: platform-bus unsupported, force ID-mapping */
	#define INTEL_IOMMU_DUMMY_DOMAIN                ((void *)-1)
	pdev->dev.archdata.iommu = INTEL_IOMMU_DUMMY_DOMAIN;
	#endif
#endif

	// 创建 DRM 设备 dev。将 DRM 设备关联到平台设备 pdev->dev。
	dev = evdi_drm_device_create(&pdev->dev);
	if (IS_ERR_OR_NULL(dev))
		goto err_free;

	// 保存 drm 设备指针到设备数据结构
	data->drm_dev = dev;
	data->symlinked = false;
	// 设置设备数据结构到平台设备
	platform_set_drvdata(pdev, data);
	// 返回 DRM 设备指针
	return PTR_ERR_OR_ZERO(dev);

err_free:
	kfree(data);
	return PTR_ERR_OR_ZERO(dev);
}

/* EL9 kernel removed the callback that was returning void  */
#if KERNEL_VERSION(6, 11, 0) <= LINUX_VERSION_CODE
void evdi_platform_device_remove(struct platform_device *pdev)
#else
int evdi_platform_device_remove(struct platform_device *pdev)
#endif
{
	struct evdi_platform_device_data *data = platform_get_drvdata(pdev);

	EVDI_CHECKPT();

	evdi_drm_device_remove(data->drm_dev);
	kfree(data);
#if KERNEL_VERSION(6, 11, 0) <= LINUX_VERSION_CODE
#else
	return 0;
#endif
}

/**
 * 判断设备是否空闲
 * @param pdev 设备指针
 * @return true 空闲，false 不空闲
 */
bool evdi_platform_device_is_free(struct platform_device *pdev)
{
	struct evdi_platform_device_data *data = platform_get_drvdata(pdev);
	struct evdi_device *evdi = data->drm_dev->dev_private;

	if (evdi && !evdi_painter_is_connected(evdi->painter) &&
	    !data->symlinked)
		return true;
	return false;
}

/**
 * 将设备和父设备建立关联
 * @param pdev 设备指针
 * @param parent 父设备指针
 */
void evdi_platform_device_link(struct platform_device *pdev,
				      struct device *parent)
{
	struct evdi_platform_device_data *data = NULL;
	int ret = 0;

	if (!parent || !pdev)
		return;

	data = platform_get_drvdata(pdev);
	if (!evdi_platform_device_is_free(pdev)) {
		EVDI_FATAL("Device is already attached can't symlink again\n");
		return;
	}

	// 在 sysfs 中建立可见关联：创建符号链接，在 parent 设备下创建一个以 evdi 设备命名的链接
	// 目标目录： &pdev->dev.kobj： 
	// 链接指向的源目录： &parent->kobj： parent 设备
	// 在 /sys/.../evdi_device/ 下创建一个名为 device 的符号链接，指向父设备的 sysfs 目录。
	ret = sysfs_create_link(&pdev->dev.kobj, &parent->kobj, "device");
	if (ret) {
		EVDI_FATAL("Failed to create sysfs link from evdi to parent device\n");
	} else {
		data->symlinked = true;
		data->parent = parent;
	}
}

/**
 * 解除设备和父设备的关联
 * @param pdev 设备指针
 * @param parent 父设备指针
 */
void evdi_platform_device_unlink_if_linked_with(struct platform_device *pdev,
				struct device *parent)
{
	struct evdi_platform_device_data *data = platform_get_drvdata(pdev);

	if (parent && data->parent == parent) {
		// 在 sysfs 中解除可见关联：删除符号链接
		sysfs_remove_link(&pdev->dev.kobj, "device");
		// 更新设备数据结构
		data->symlinked = false;
		data->parent = NULL;
		EVDI_INFO("Detached from parent device\n");
	}
}
