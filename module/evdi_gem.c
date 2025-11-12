// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/sched.h>
#include <linux/version.h>
#if KERNEL_VERSION(5, 18, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
#elif KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE
#include <linux/dma-buf-map.h>
#endif
#if KERNEL_VERSION(5, 16, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
#include <drm/drm_prime.h>
#include <drm/drm_file.h>
#elif KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE
#else
#include <drm/drmP.h>
#endif
#include "evdi_drm_drv.h"
#include "evdi_params.h"
#include <linux/shmem_fs.h>
#include <linux/dma-buf.h>
#include <drm/drm_cache.h>
#include <linux/vmalloc.h>


#if KERNEL_VERSION(6, 13, 0) <= LINUX_VERSION_CODE || defined(EL10)
MODULE_IMPORT_NS("DMA_BUF");
#elif KERNEL_VERSION(5, 16, 0) <= LINUX_VERSION_CODE || defined(EL9)
MODULE_IMPORT_NS(DMA_BUF);
#endif

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
static int evdi_prime_pin(struct drm_gem_object *obj);
static void evdi_prime_unpin(struct drm_gem_object *obj);

static const struct vm_operations_struct evdi_gem_vm_ops = {
	.fault = evdi_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/**
 * GEM 对象回调函数表
 * @param free 释放 GEM 对象
 * @param pin 固定页面
 * @param unpin 释放页面
 * @param vm_ops 虚拟内存操作
 * @param export 导出 GEM 对象
 * @param get_sg_table 获取散列表
 */
static struct drm_gem_object_funcs gem_obj_funcs = {
	.free = evdi_gem_free_object,
	.pin = evdi_prime_pin,
	.unpin = evdi_prime_unpin,
	.vm_ops = &evdi_gem_vm_ops,
	.export = drm_gem_prime_export,
	.get_sg_table = evdi_prime_get_sg_table,
};
#endif

static bool evdi_was_called_by_mutter(void)
{
	char task_comm[TASK_COMM_LEN] = { 0 };

	get_task_comm(task_comm, current);

	return strcmp(task_comm, "gnome-shell") == 0;
}

static bool evdi_drm_gem_object_use_import_attach(struct drm_gem_object *obj)
{
	if (!obj || !obj->import_attach || !obj->import_attach->dmabuf->owner)
		return false;

	return strcmp(obj->import_attach->dmabuf->owner->name, "amdgpu") != 0;
}

uint32_t evdi_gem_object_handle_lookup(struct drm_file *filp,
				       struct drm_gem_object *obj)
{
	uint32_t it_handle = 0;
	struct drm_gem_object *it_obj = NULL;

	spin_lock(&filp->table_lock);
	idr_for_each_entry(&filp->object_idr, it_obj, it_handle) {
		if (it_obj == obj)
			break;
	}
	spin_unlock(&filp->table_lock);

	if (!it_obj)
		it_handle = 0;

	return it_handle;
}

/**
 * 分配 GEM 对象
 * @param dev 设备指针
 * @param size 对象大小
 * @return GEM 对象指针
 * @note 在 evdi_gem_create 函数中调用。
 */
struct evdi_gem_object *evdi_gem_alloc_object(struct drm_device *dev,
					      size_t size)
{
	struct evdi_gem_object *obj;

	// 这里只分配 evdi_gem_object 结构，不分配实际的物理内存页面。
	// 页面在需要时才分配（延迟分配）。

	// 1. 分配 evdi_gem_object 结构
	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (obj == NULL)
		return NULL;

	// 2. 初始化 DRM GEM 对象（设置大小等）
	if (drm_gem_object_init(dev, &obj->base, size) != 0) {
		kfree(obj);
		return NULL;
	}


#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
	// 3. 设置回调函数（新版本内核）
	obj->base.funcs = &gem_obj_funcs;
#endif

	// 4. 设置是否允许软件光标更新
	obj->allow_sw_cursor_rect_updates = false;

	// 5. 初始化互斥锁
	mutex_init(&obj->pages_lock);

	return obj;
}

/**
 * 创建 GEM 对象
 * @param file 文件指针
 * @param dev 设备指针
 * @param size 对象大小
 * @param handle_p 对象句柄
 * @return 0 成功，其他错误码失败
 * @note 在 evdi_gem_create 函数中调用。
 */
static int
evdi_gem_create(struct drm_file *file,
		struct drm_device *dev, uint64_t size, uint32_t *handle_p)
{
	struct evdi_gem_object *obj;
	int ret;
	u32 handle;

	size = roundup(size, PAGE_SIZE);

	obj = evdi_gem_alloc_object(dev, size);
	if (obj == NULL)
		return -ENOMEM;

	obj->allow_sw_cursor_rect_updates = evdi_was_called_by_mutter();
	ret = drm_gem_handle_create(file, &obj->base, &handle);
	if (ret) {
		drm_gem_object_release(&obj->base);
		kfree(obj);
		return ret;
	}
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE || defined(EL8)
	drm_gem_object_put(&obj->base);
#else
	drm_gem_object_put_unlocked(&obj->base);
#endif
	*handle_p = handle;
	return 0;
}

/**
 * 计算行跨度
 * @param width 宽度
 * @param cpp 每像素位数
 * @return 行跨度
 * @note - **pitch = 行跨度**，每行字节数
 *       - 为了硬件兼容性，需要对齐到特定值
 *       - 常见对齐值：1, 2, 3, 4 字节
 */
static int evdi_align_pitch(int width, int cpp)
{
	int aligned = width;
	int pitch_mask = 0;

	// 1. 根据每像素位数选择对齐掩码
	switch (cpp) {
	case 1:
		pitch_mask = 255;
		break;
	case 2:
		pitch_mask = 127;
		break;
	case 3:
	case 4:
		pitch_mask = 63;
		break;
	}

	aligned += pitch_mask;
	aligned &= ~pitch_mask;
	return aligned * cpp;
}

/**
 * 创建 Dumb 缓冲区
 * @param file 文件指针
 * @param dev 设备指针
 * @param args 创建参数
 * @return 0 成功，其他错误码失败
 * @note - **Dumb = 简单**，最简单的缓冲区创建接口
 *       - 只需要宽度、高度、每像素位数（bpp）
 *       - 自动计算 pitch 和大小
 */
int evdi_dumb_create(struct drm_file *file,
		     struct drm_device *dev, struct drm_mode_create_dumb *args)
{
	// 1. 计算 pitch（行跨度）
	args->pitch = evdi_align_pitch(args->width, DIV_ROUND_UP(args->bpp, 8));

	// 2. 计算总大小
	args->size = args->pitch * args->height;

	// 3. 创建 GEM 对象
	return evdi_gem_create(file, dev, args->size, &args->handle);
}

/**
 * 内存映射 - 用户空间访问
 * @param filp 文件指针
 * @param vma 虚拟内存区域指针
 * @return 0 成功，其他错误码失败
 * @note 在 evdi_drm_gem_mmap 函数中调用。
 */
int evdi_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;

	// 1. 调用 DRM 框架的 mmap
	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

/* Some VMA modifier function patches present in 6.3 were reverted in EL8 kernels */
#if KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE || defined(EL9)
	// 2. 设置 VMA 标志
	vm_flags_mod(vma, VM_MIXEDMAP, VM_PFNMAP);
#else
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_MIXEDMAP;
#endif

	return ret;
}

#if KERNEL_VERSION(4, 17, 0) <= LINUX_VERSION_CODE
/**
 * 页面错误处理 - 用户空间访问
 * @param vmf 虚拟内存区域指针
 * @return vm_fault_t 成功，其他错误码失败
 * @note - **页面错误 = Page Fault**
 *       - 用户程序访问映射的内存时，如果页面不在，触发页面错误；
 *       - 内核处理页面错误，内核调用 evdi_gem_fault()，从 obj->pages 获取物理页面；
 *       - 将物理页面插入用户空间，用户程序可以访问。
 */
vm_fault_t evdi_gem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#else
int evdi_gem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#endif
	struct evdi_gem_object *obj = to_evdi_bo(vma->vm_private_data);
	struct page *page;
	pgoff_t page_offset;
	loff_t num_pages = obj->base.size >> PAGE_SHIFT;
	int ret = 0;

	// 1. 计算页面偏移
	page_offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT;

	// 2. 检查边界
	if (!obj->pages || page_offset >= (unsigned long)num_pages)
		return VM_FAULT_SIGBUS;

	// 3. 获取对应的物理页面
	page = obj->pages[page_offset];

	// 4. 插入页面到用户空间
	ret = vm_insert_page(vma, vmf->address, page);

	// 5. 返回结果
	switch (ret) {
	case -EAGAIN:
	case 0:
	case -ERESTARTSYS:
	case -EBUSY:
		return VM_FAULT_NOPAGE;
	case -ENOMEM:
		return VM_FAULT_OOM;
	default:
		return VM_FAULT_SIGBUS;
	}
	return VM_FAULT_SIGBUS;
}

/**
 * 获取物理页面
 * @param obj GEM 对象指针
 * @param gfpmask 页面分配标志
 * @return 0 成功，其他错误码失败
 * @note 只在需要时才分配页面。
 */
static int evdi_gem_get_pages(struct evdi_gem_object *obj,
			      __always_unused gfp_t gfpmask)
{
	struct page **pages;

	// 1. 如果已经有页面，直接返回
	if (obj->pages)
		return 0;

	// 2. 从 DRM 框架获取页面
	pages = drm_gem_get_pages(&obj->base);

	// 3. 如果获取页面失败，返回错误码
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	// 4. 设置页面
	obj->pages = pages;

#if defined(CONFIG_X86)
	// 5. 如果架构是 X86，刷新 CPU 缓存，确保数据一致性
	drm_clflush_pages(obj->pages, DIV_ROUND_UP(obj->base.size, PAGE_SIZE));
#endif

	return 0;
}

/**
 * 释放物理页面
 * @param obj GEM 对象指针
 * @note 在 evdi_unpin_pages 函数中调用。
 */
static void evdi_gem_put_pages(struct evdi_gem_object *obj)
{
	// 1. 如果是导入的对象，只释放页面数组（页面属于其他驱动）
	if (obj->base.import_attach) {
		kvfree(obj->pages);
		obj->pages = NULL;
		return;
	}

	// 2. 普通对象：释放页面
	drm_gem_put_pages(&obj->base, obj->pages, false, false);
	obj->pages = NULL;
}

/**
 * 固定页面
 * @param obj GEM 对象指针
 * @return 0 成功，其他错误码失败
 * @note - 图形缓冲区需要常驻内存，如果被换出，访问会很慢。
 *       - DMA 操作需要物理地址，页面必须在内存中，所以需要固定页面。
 */
static int evdi_pin_pages(struct evdi_gem_object *obj)
{
	int ret = 0;

	mutex_lock(&obj->pages_lock);
	// 1. 引用计数：第一次固定时才获取页面
	if (obj->pages_pin_count++ == 0) {
		// 2. 获取页面
		ret = evdi_gem_get_pages(obj, GFP_KERNEL);
		if (ret)
			// 3. 失败时回退
			obj->pages_pin_count--;
	}
	mutex_unlock(&obj->pages_lock);
	// 4. 返回结果
	return ret;
}

static void evdi_unpin_pages(struct evdi_gem_object *obj)
{
	mutex_lock(&obj->pages_lock);
	if (--obj->pages_pin_count == 0)
		evdi_gem_put_pages(obj);
	mutex_unlock(&obj->pages_lock);
}

/**
 * 虚拟地址映射
 * @param obj GEM 对象指针
 * @return 0 成功，其他错误码失败
 * @note - **vmap = 虚拟映射**，将物理页面映射到内核虚拟地址空间
 *       - 映射后，内核可以通过 `obj->vmapping` 直接访问数据
 *       - 需要先固定页面（pin），确保页面在内存中
 */
int evdi_gem_vmap(struct evdi_gem_object *obj)
{
	int page_count = DIV_ROUND_UP(obj->base.size, PAGE_SIZE);
	int ret;

	// 情况1. 如果是导入的对象（来自其他驱动），使用 DMA-BUF 的映射
	if (evdi_drm_gem_object_use_import_attach(&obj->base)) {
#if KERNEL_VERSION(5, 18, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
		struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
#elif KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE
		struct dma_buf_map map = DMA_BUF_MAP_INIT_VADDR(NULL);
#endif

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
# if KERNEL_VERSION(6, 2, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
		ret = dma_buf_vmap_unlocked(obj->base.import_attach->dmabuf, &map);
# else
		ret = dma_buf_vmap(obj->base.import_attach->dmabuf, &map);
# endif
		if (ret)
			return -ENOMEM;
		// 4. 设置虚拟地址
		obj->vmapping = map.vaddr;
		// 5. 设置是否是 IOMEM 映射
		obj->vmap_is_iomem = map.is_iomem;
#else
		obj->vmapping = dma_buf_vmap(obj->base.import_attach->dmabuf);
		if (!obj->vmapping)
			return -ENOMEM;
#endif
		return 0;
	}

	// 情况2. 普通对象：固定页面并映射到虚拟地址
	// 1. 固定页面，确保页面在内存中
	ret = evdi_pin_pages(obj);
	if (ret)
		return ret;

	// 2. 将物理页面映射到虚拟地址
	obj->vmapping = vmap(obj->pages, page_count, 0, PAGE_KERNEL);
	if (!obj->vmapping)
		return -ENOMEM;

	// 3. 返回成功，现在内核可以通过 `obj->vmapping` 直接访问数据，obj->vmapping 即是虚拟地址。
	return 0;
}

/**
 * 取消虚拟地址映射: 先取消映射，再取消固定页面，确保页面可以被交换出去
 * @param obj GEM 对象指针
 * @note 在 evdi_gem_vunmap 函数中调用。
 */
void evdi_gem_vunmap(struct evdi_gem_object *obj)
{
	// 情况1. 如果是导入的对象（来自其他驱动），使用 DMA-BUF 的取消映射
	if (evdi_drm_gem_object_use_import_attach(&obj->base)) {
#if KERNEL_VERSION(5, 18, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
		struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);

		if (obj->vmap_is_iomem)
			iosys_map_set_vaddr_iomem(&map, obj->vmapping);
		else
			iosys_map_set_vaddr(&map, obj->vmapping);

# if KERNEL_VERSION(6, 2, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
		dma_buf_vunmap_unlocked(obj->base.import_attach->dmabuf, &map);
# else
		dma_buf_vunmap(obj->base.import_attach->dmabuf, &map);
# endif

#elif KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE
		struct dma_buf_map map;

		if (obj->vmap_is_iomem)
			dma_buf_map_set_vaddr_iomem(&map, obj->vmapping);
		else
			dma_buf_map_set_vaddr(&map, obj->vmapping);

		dma_buf_vunmap(obj->base.import_attach->dmabuf, &map);
#else
		dma_buf_vunmap(obj->base.import_attach->dmabuf, obj->vmapping);
#endif
		obj->vmapping = NULL;
		return;
	}

	// 情况2. 普通对象：取消映射
	if (obj->vmapping) {
		// 1. 取消映射
		vunmap(obj->vmapping);
		obj->vmapping = NULL;
	}

	// 3. 取消固定页面，确保页面可以被交换出去
	evdi_unpin_pages(obj);
}

/**
 * 释放 GEM 对象
 * @param gem_obj GEM 对象指针
 * @note 在 evdi_gem_free_object 函数中调用。
 */
void evdi_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct evdi_gem_object *obj = to_evdi_bo(gem_obj);

	// 1. 如果已虚拟地址映射，先取消映射
	if (obj->vmapping)
		evdi_gem_vunmap(obj);

	// 2. 如果是导入的对象，清理 Prime 相关
	// 导入的对象是指从其他驱动（如其他显卡驱动）导入的 GEM 对象。
	// 这些对象需要特殊处理，因为它们不是由当前驱动分配的。
	if (gem_obj->import_attach)
		drm_prime_gem_destroy(gem_obj, obj->sg);

	// 3. 释放物理页面
	if (obj->pages)
		evdi_gem_put_pages(obj);

	// 4. 释放 mmap 偏移
	if (gem_obj->dev->vma_offset_manager)
		drm_gem_free_mmap_offset(gem_obj);

	// 5. 销毁互斥锁
	mutex_destroy(&obj->pages_lock);

	// 6. 释放 DRM 对象
	drm_gem_object_release(&obj->base);
	kfree(obj);
}

/**
 * the dumb interface doesn't work with the GEM straight MMAP
 * interface, it expects to do MMAP on the drm fd, like normal
 * 该简化的接口无法直接使用 GEM 的 MMAP 接口，它需要像往常一样对 DRM 文件描述符执行 MMAP 操作。
 * @param file 文件指针
 * @param dev 设备指针
 * @param handle 对象句柄
 * @param offset 偏移地址
 * @return 0 成功，其他错误码失败
 */
int evdi_gem_mmap(struct drm_file *file,
		  struct drm_device *dev, uint32_t handle, uint64_t *offset)
{
	struct evdi_gem_object *gobj;
	struct drm_gem_object *obj;
	int ret = 0;

	// 1. 查找 GEM 对象（通过 handle）
	obj = drm_gem_object_lookup(file, handle);
	if (obj == NULL) {
		return -ENOENT;
	}
	gobj = to_evdi_bo(obj);

	// 2. 固定页面，确保页面在内存中
	ret = evdi_pin_pages(gobj);
	if (ret)
		goto out;

	/* Don't allow imported objects to be mapped */
	// 3. 不允许导入的对象被映射
	if (obj->import_attach) {
		EVDI_WARN("Don't allow imported objects to be mapped: owner: %s\n",  obj->import_attach->dmabuf->owner->name);
		ret = -EINVAL;
		goto out;
	}

	// 4. 创建 mmap 偏移
	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto out;

	// 5. 返回偏移地址
	*offset = drm_vma_node_offset_addr(&gobj->base.vma_node);

 out:
	drm_gem_object_put(&gobj->base);
	return ret;
}

/**
 * 导入散列表
 * @param dev 设备指针
 * @param attach 附件指针
 * @param sg 散列表指针
 * @return 0 成功，其他错误码失败
 * @note - **Prime = 跨驱动缓冲区共享**，将其他驱动（如其他显卡驱动）的 GEM 对象导入到当前驱动。
 *       - 这些对象需要特殊处理，因为它们不是由当前驱动分配的，使用 DMA-BUF 机制导入。
 */
struct drm_gem_object *
evdi_prime_import_sg_table(struct drm_device *dev,
			   struct dma_buf_attachment *attach,
			   struct sg_table *sg)
{
	struct evdi_gem_object *obj;
	int npages;
	bool called_by_mutter;

	called_by_mutter = evdi_was_called_by_mutter();

	// 1. 分配 GEM 对象
	obj = evdi_gem_alloc_object(dev, attach->dmabuf->size);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	// 2. 分配页面数组
	npages = DIV_ROUND_UP(attach->dmabuf->size, PAGE_SIZE);
	DRM_DEBUG_PRIME("Importing %d pages\n", npages);
	obj->pages = kvmalloc_array(npages, sizeof(struct page *), GFP_KERNEL);
	if (!obj->pages) {
		evdi_gem_free_object(&obj->base);
		return ERR_PTR(-ENOMEM);
	}

#if KERNEL_VERSION(5, 12, 0) <= LINUX_VERSION_CODE || defined(EL8)
	// 3. 将散列表转换为页面数组
	drm_prime_sg_to_page_array(sg, obj->pages, npages);
#else
	drm_prime_sg_to_page_addr_arrays(sg, obj->pages, NULL, npages);
#endif

	// 4. 保存散列表
	obj->sg = sg;
	// 5. 设置是否允许软件光标更新
	obj->allow_sw_cursor_rect_updates = called_by_mutter;
	// 6. 返回 GEM 对象
	return &obj->base;
}

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
static int evdi_prime_pin(struct drm_gem_object *obj)
{
	struct evdi_gem_object *bo = to_evdi_bo(obj);

	return evdi_pin_pages(bo);
}

static void evdi_prime_unpin(struct drm_gem_object *obj)
{
	struct evdi_gem_object *bo = to_evdi_bo(obj);

	evdi_unpin_pages(bo);
}
#endif

/**
 * 导出散列表
 * @param obj GEM 对象指针
 * @return 散列表指针
 * @note - 将 EVDI 的缓冲区导出给其他驱动
 *       - 将页面数组转换为散列表（DMA 需要）
 */
struct sg_table *evdi_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct evdi_gem_object *bo = to_evdi_bo(obj);

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE || defined(EL8)
	// 1. 将页面数组转换为散列表
	return drm_prime_pages_to_sg(obj->dev, bo->pages, bo->base.size >> PAGE_SHIFT);
#else
	return drm_prime_pages_to_sg(bo->pages, bo->base.size >> PAGE_SHIFT);
#endif
}

