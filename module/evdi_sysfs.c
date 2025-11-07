// SPDX-License-Identifier: GPL-2.0-only
/*
 * evdi_sysfs.c
 *
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


#include <linux/device.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "evdi_sysfs.h"
#include "evdi_params.h"
#include "evdi_debug.h"
#include "evdi_platform_drv.h"

#define MAX_EVDI_USB_ADDR 10

/**
 * 显示版本号。在用户空间通过 cat /sys/devices/evdi/version 读取。
 * @param dev 设备指针
 * @param attr 设备属性指针
 * @param buf 缓冲区指针
 * @return 写入的字节数
 */
static ssize_t version_show(__always_unused struct device *dev,
			    __always_unused struct device_attribute *attr,
			    char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u.%u.%u\n", DRIVER_MAJOR,
			DRIVER_MINOR, DRIVER_PATCH);
}

/**
 * 显示设备数量。在用户空间通过 cat /sys/devices/evdi/count 读取。
 * @param dev 设备指针
 * @param attr 设备属性指针
 * @param buf 缓冲区指针
 * @return 写入的字节数
 */
static ssize_t count_show(__always_unused struct device *dev,
			  __always_unused struct device_attribute *attr,
			  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", evdi_platform_device_count(dev));
}

struct evdi_usb_addr {
	int addr[MAX_EVDI_USB_ADDR];
	int len;
	struct usb_device *usb;
};

#ifdef CONFIG_USB_SUPPORT

static int evdi_platform_device_attach(struct device *device,
		struct evdi_usb_addr *parent_addr);

/**
 * 该函数用于将 EVDI 虚拟显示设备关联到 USB 设备。
 * @param dev 设备指针
 * @param buf 设备路径
 * @param count 设备路径长度
 * @return 写入的字节数
 * 
 * 输入的字符串 buf 应符合: usb:<bus>-<port>[.<port>...]:<interface>
 * 比如：
 * 		usb:1-5:1.0
 *      usb:2-1.1:0
 *      usb:1-1.2.3:0
 * 可以执行 `ls /sys/bus/usb/devices/` 拼接 usb 获得
 * 
 * 示例：
 * echo "usb:1-1.1:0" > /sys/devices/evdi/add
 * 
 */
static ssize_t add_device_with_usb_path(struct device *dev,
			 const char *buf, size_t count)
{
	// 1. 内存分配和初始化
	char *usb_path = kstrdup(buf, GFP_KERNEL);
	char *temp_path = usb_path;
	char *bus_token;
	char *usb_token;
	char *usb_token_copy = NULL;
	char *token;
	char *bus;
	char *port;
	struct evdi_usb_addr usb_addr;

	if (!usb_path)
		return -ENOMEM;

	memset(&usb_addr, 0, sizeof(usb_addr));

	// 2. 解析 USB 设备路径
	// 从 buf 中查找 "usb:" 前缀字符串
	temp_path = strnstr(temp_path, "usb:", count);
	if (!temp_path)
		goto err_parse_usb_path;

	// 3. 去除前导和后导空格
	temp_path = strim(temp_path); // usb:1-5:1.0 -> usb:1-5:1.0

	// 4. 分割 USB 设备路径
	bus_token = strsep(&temp_path, ":"); // bus_token: "usb"
	if (!bus_token)
		goto err_parse_usb_path;

	usb_token = strsep(&temp_path, ":"); // usb_token: "1-5"
	if (!usb_token)
		goto err_parse_usb_path;

	/* Separate trailing ':*' from usb_token */
	strsep(&temp_path, ":"); 	// 丢弃 "1.0"（接口号）

	token = usb_token_copy = kstrdup(usb_token, GFP_KERNEL); // token: "1-5"
	bus = strsep(&token, "-"); // bus: "1", token: "5"
	if (!bus)
		goto err_parse_usb_path;
	if (kstrtouint(bus, 10, &usb_addr.addr[usb_addr.len++]))
		goto err_parse_usb_path;

	do {
		port = strsep(&token, "."); // port: "5"
		if (!port)
			goto err_parse_usb_path;
		if (kstrtouint(port, 10, &usb_addr.addr[usb_addr.len++])) // usb_addr.addr = {1, 5}
			goto err_parse_usb_path;
	} while (token && port && usb_addr.len < MAX_EVDI_USB_ADDR);

	// 5. 将 USB 设备关联到平台设备: 将 evdi 设备 dev 关联到该 bus 1, port 5 的 USB 设备
	if (evdi_platform_device_attach(dev, &usb_addr) != 0) {
		EVDI_ERROR("Unable to attach to: %s\n", buf);
		kfree(usb_path);
		kfree(usb_token_copy);
		return -EINVAL;
	}

	EVDI_INFO("Attaching to %s:%s\n", bus_token, usb_token);
	kfree(usb_path);
	kfree(usb_token_copy);
	return count;

err_parse_usb_path:
	EVDI_ERROR("Unable to parse usb path: %s", buf);
	kfree(usb_path);
	kfree(usb_token_copy);
	return -EINVAL;
}

/**
 * 查找 USB 设备。在 evdi_platform_device_attach 中调用。
 * @param usb USB 设备指针，是当前遍历的 USB 设备
 * @param data 数据指针，是我们需要查找的 USB 设备地址，是 usb_for_each_dev 的 data 参数
 * @return 0 继续遍历，1 匹配成功
 * 
 * 遍历 USB 设备，对每一个 usb 设备，调用 find_usb_device_at_path 函数
 * 如果 find_usb_device_at_path 返回 1，则说明找到了对应的 USB 设备
 */
static int find_usb_device_at_path(struct usb_device *usb, void *data)
{
	struct evdi_usb_addr *find_path = (struct evdi_usb_addr *)(data);
	struct usb_device *pdev = usb;
	int port = 0;
	int i;

	i = find_path->len - 1;
	while (pdev != NULL && i >= 0 && i < MAX_EVDI_USB_ADDR) {
		port = pdev->portnum;
		if (port == 0)
			port = pdev->bus->busnum;

		if (port != find_path->addr[i])
			return 0;

		if (pdev->parent == NULL && i == 0) {
			find_path->usb = usb;
			return 1;
		}
		pdev = pdev->parent;
		i--;
	}

	return 0;
}

/**
 * 将 USB 设备关联到平台设备。在 evdi_platform_device_probe 中调用。
 * @param device 设备指针
 * @param parent_addr 父设备地址指针
 * @return 0 成功，其他错误码
 */
static int evdi_platform_device_attach(struct device *device,
		struct evdi_usb_addr *parent_addr)
{
	struct device *parent = NULL;

	if (!parent_addr)
		return -EINVAL;

	// 遍历 USB 设备，对每一个 usb 设备，调用 find_usb_device_at_path 函数
	// 如果 find_usb_device_at_path 返回 1，则说明找到了对应的 USB 设备
	if (!usb_for_each_dev(parent_addr, find_usb_device_at_path) ||
	    !parent_addr->usb)
		return -EINVAL;

	// 6. 将 USB 设备关联到平台设备: 将 evdi 设备 dev 关联到该 bus 1, port 5 的 USB 设备
	parent = &parent_addr->usb->dev;
	// 7. 添加设备
	return evdi_platform_device_add(device, parent);
}

#else /* !CONFIG_USB_SUPPORT */

static ssize_t add_device_with_usb_path(struct device *dev,
			 const char *buf, size_t count)
{
	return -EINVAL;
}

#endif /* CONFIG_USB_SUPPORT */

/**
 * 添加设备。在用户空间通过 echo "usb:1-5:1.0" > /sys/devices/evdi/add 写入。
 * @param dev 设备指针
 * @param attr 设备属性指针
 * @param buf 设备路径
 * @param count 设备路径长度
 * @return 写入的字节数
 */
static ssize_t add_store(struct device *dev,
			 __always_unused struct device_attribute *attr,
			 const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	if (strnstr(buf, "usb:", count))
		return add_device_with_usb_path(dev, buf, count);

	if (kstrtouint(buf, 10, &val)) {
		EVDI_ERROR("Invalid device count \"%s\"\n", buf);
		return -EINVAL;
	}

	ret = evdi_platform_add_devices(dev, val);
	if (ret)
		return ret;

	return count;
}

/**
 * 移除所有设备。在用户空间通过 echo "任意值" > /sys/devices/evdi/remove_all 写入。
 * @param dev 设备指针
 * @param attr 设备属性指针
 * @param buf 设备路径
 * @param count 设备路径长度
 * @return 写入的字节数
 */
static ssize_t remove_all_store(struct device *dev,
				__always_unused struct device_attribute *attr,
				__always_unused const char *buf,
				size_t count)
{
	evdi_platform_remove_all_devices(dev);
	return count;
}

/**
 * 显示日志等级。在用户空间通过 cat /sys/devices/evdi/loglevel 读取。
 * @param dev 设备指针
 * @param attr 设备属性指针
 * @param buf 缓冲区指针
 * @return 写入的字节数
 */
static ssize_t loglevel_show(__always_unused struct device *dev,
			     __always_unused struct device_attribute *attr,
			     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", evdi_loglevel);
}

/**
 * 设置日志等级。在用户空间通过 echo "5" > /sys/devices/evdi/loglevel 写入。
 * @param dev 设备指针
 * @param attr 设备属性指针
 * @param buf 设备路径
 * @param count 设备路径长度
 * @return 写入的字节数
 */
static ssize_t loglevel_store(__always_unused struct device *dev,
			      __always_unused struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val)) {
		EVDI_ERROR("Unable to parse %u\n", val);
		return -EINVAL;
	}
	if (val > EVDI_LOGLEVEL_VERBOSE) {
		EVDI_ERROR("Invalid loglevel %u\n", val);
		return -EINVAL;
	}

	EVDI_INFO("Setting loglevel to %u\n", val);
	evdi_loglevel = val;
	return count;
}

static struct device_attribute evdi_device_attributes[] = {
	__ATTR_RO(count),
	__ATTR_RO(version),  // 0444: 所有人都只能读，不能写、不能执行
	__ATTR_RW(loglevel), // 0644: 文件所有者可以读写，组和其他用户只能读
	__ATTR_WO(add),      // 0200: 只有文件所有者可以写
	__ATTR_WO(remove_all)
};

/**
 * 初始化 sysfs 接口。在 evdi_init 中调用。
 * @param root 根设备指针
 */
void evdi_sysfs_init(struct device *root)
{
	unsigned int i;

	// 新建上面提到的 sysfs 文件： count、version、loglevel、add、remove_all
	// 到目录：/sys/devices/evdi
	if (!PTR_ERR_OR_ZERO(root))
		for (i = 0; i < ARRAY_SIZE(evdi_device_attributes); i++)
			device_create_file(root, &evdi_device_attributes[i]);
}

/**
 * 退出 sysfs 接口。在 evdi_exit 中调用。
 * @param root 根设备指针
 */
void evdi_sysfs_exit(struct device *root)
{
	unsigned int i;

	if (PTR_ERR_OR_ZERO(root)) {
		EVDI_ERROR("root device is null");
		return;
	}
	// 删除上面提到的 sysfs 文件： count、version、loglevel、add、remove_all
	// 从目录：/sys/devices/evdi
	for (i = 0; i < ARRAY_SIZE(evdi_device_attributes); i++)
		device_remove_file(root, &evdi_device_attributes[i]);
}

