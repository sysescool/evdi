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

#include <linux/slab.h>
#ifdef CONFIG_FB
#include <linux/fb.h>
#endif /* CONFIG_FB */
#include <linux/dma-buf.h>
#include <linux/version.h>
#if KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
#include <drm/drmP.h>
#endif
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_atomic.h>
#if KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_damage_helper.h>
#endif
#include "evdi_drm_drv.h"


/**
 * 帧缓冲区设备结构体: 它的作用是——把内核的 DRM（Direct Rendering Manager） 框架和老式的 framebuffer（fbdev） 框架连接起来，
 * 以便让旧的用户空间程序（比如 Xorg 的 fbdev 驱动）也能在虚拟显示器上工作。
 * @param helper fb_helper 结构体
 * @param efb 帧缓冲区指针，是evdi 自己的framebuffer 对象。往虚拟显示器输出的图像数据最终都会落在这里。
 * @param fbdev_list 帧缓冲区设备列表，用于管理多个帧缓冲区设备。
 * @param fb_ops 帧缓冲区操作函数，用于操作帧缓冲区。有程序直接写 /dev/fb0，这些函数会被调用来更新图像。
 * @param fb_count 帧缓冲区计数
 * @note 在 evdi_fbdev_init 函数中初始化。
 */
struct evdi_fbdev {
	struct drm_fb_helper helper;
	struct evdi_framebuffer efb;
	struct list_head fbdev_list;
	const struct fb_ops *fb_ops;
	int fb_count;
};

struct drm_clip_rect evdi_framebuffer_sanitize_rect(
				const struct evdi_framebuffer *fb,
				const struct drm_clip_rect *dirty_rect)
{
	struct drm_clip_rect rect = *dirty_rect;

	if (rect.x1 > rect.x2) {
		unsigned short tmp = rect.x2;

		EVDI_WARN("Wrong clip rect: x1 > x2\n");
		rect.x2 = rect.x1;
		rect.x1 = tmp;
	}

	if (rect.y1 > rect.y2) {
		unsigned short tmp = rect.y2;

		EVDI_WARN("Wrong clip rect: y1 > y2\n");
		rect.y2 = rect.y1;
		rect.y1 = tmp;
	}


	if (rect.x1 > fb->base.width) {
		EVDI_DEBUG("Wrong clip rect: x1 > fb.width\n");
		rect.x1 = fb->base.width;
	}

	if (rect.y1 > fb->base.height) {
		EVDI_DEBUG("Wrong clip rect: y1 > fb.height\n");
		rect.y1 = fb->base.height;
	}

	if (rect.x2 > fb->base.width) {
		EVDI_DEBUG("Wrong clip rect: x2 > fb.width\n");
		rect.x2 = fb->base.width;
	}

	if (rect.y2 > fb->base.height) {
		EVDI_DEBUG("Wrong clip rect: y2 > fb.height\n");
		rect.y2 = fb->base.height;
	}

	return rect;
}

#ifdef CONFIG_FB
/**
 * 处理脏矩形
 * @param fb 帧缓冲区指针
 * @param x x 坐标
 * @param y y 坐标
 * @param width 宽度
 * @param height 高度
 * @return 0 成功，其他错误码失败
 * @note 在 evdi_fb_fillrect、evdi_fb_copyarea、evdi_fb_imageblit 函数中调用。
 */
static int evdi_handle_damage(struct evdi_framebuffer *fb,
		       int x, int y, int width, int height)
{
	// 1. 创建脏矩形
	const struct drm_clip_rect dirty_rect = { x, y, x + width, y + height };
	// 2. 对脏矩形进行边界检查
	const struct drm_clip_rect rect =
		evdi_framebuffer_sanitize_rect(fb, &dirty_rect);
	// 3. 获取设备指针
	struct drm_device *dev = fb->base.dev;
	struct evdi_device *evdi = dev->dev_private;

	EVDI_CHECKPT();

	if (!fb->active)
		return 0;
	// 4. 设置扫描输出缓冲区
	evdi_painter_set_scanout_buffer(evdi->painter, fb);
	// 5. 标记为脏（需要更新）
	evdi_painter_mark_dirty(evdi, &rect);

	return 0;
}

/**
 * 内存映射
 * @param info 帧缓冲区信息指针
 * @param vma 虚拟内存区域指针
 * @return 0 成功，其他错误码失败
 * @note 在 evdi_fb_mmap 函数中调用。
 */
static int evdi_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	unsigned long start = vma->vm_start;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long page, pos;

	if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
		return -EINVAL;

	if (offset > info->fix.smem_len ||
	    size > info->fix.smem_len - offset)
		return -EINVAL;

	pos = (unsigned long)info->fix.smem_start + offset;

	pr_notice("mmap() framebuffer addr:%lu size:%lu\n", pos, size);

	while (size > 0) {
		page = vmalloc_to_pfn((void *)pos);
		if (remap_pfn_range(vma, start, page, PAGE_SIZE, PAGE_SHARED))
			return -EAGAIN;

		start += PAGE_SIZE;
		pos += PAGE_SIZE;
		if (size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}

	return 0;
}

/**
 * 填充矩形
 * @param info 帧缓冲区信息指针
 * @param rect 矩形指针
 * @return 0 成功，其他错误码失败
 * @note 在 evdi_fb_fillrect 函数中调用。
 */
static void evdi_fb_fillrect(struct fb_info *info,
			     const struct fb_fillrect *rect)
{
	struct evdi_fbdev *efbdev = info->par;

	EVDI_CHECKPT();
	// 1. 执行实际的填充操作
	sys_fillrect(info, rect);
	// 2. 标记为脏（需要更新）
	evdi_handle_damage(&efbdev->efb, rect->dx, rect->dy, rect->width,
			   rect->height);
}

/**
 * 复制区域
 * @param info 帧缓冲区信息指针
 * @param region 区域指针
 * @return 0 成功，其他错误码失败
 * @note 在 evdi_fb_copyarea 函数中调用。
 */
static void evdi_fb_copyarea(struct fb_info *info,
			     const struct fb_copyarea *region)
{
	struct evdi_fbdev *efbdev = info->par;

	EVDI_CHECKPT();
	sys_copyarea(info, region);
	evdi_handle_damage(&efbdev->efb, region->dx, region->dy, region->width,
			   region->height);
}

/**
 * 图像位块传输
 * @param info 帧缓冲区信息指针
 * @param image 图像指针
 * @return 0 成功，其他错误码失败
 * @note 在 evdi_fb_imageblit 函数中调用。
 */
static void evdi_fb_imageblit(struct fb_info *info,
			      const struct fb_image *image)
{
	struct evdi_fbdev *efbdev = info->par;

	EVDI_CHECKPT();
	sys_imageblit(info, image);
	evdi_handle_damage(&efbdev->efb, image->dx, image->dy, image->width,
			   image->height);
}

/*
 * It's common for several clients to have framebuffer open simultaneously.
 * e.g. both fbcon and X. Makes things interesting.
 * Assumes caller is holding info->lock (for open and release at least)
 * 多个客户端同时打开帧缓冲区是很常见的。
 * 例如，fbcon 和 X 同时打开帧缓冲区。这会使情况变得复杂。
 * 假设调用者持有 info->lock 权限（至少在打开和释放帧缓冲区时如此）。
 */
static int evdi_fb_open(struct fb_info *info, int user)
{
	struct evdi_fbdev *efbdev = info->par;

	efbdev->fb_count++;
	pr_notice("open /dev/fb%d user=%d fb_info=%p count=%d\n",
		  info->node, user, info, efbdev->fb_count);

	return 0;
}

/*
 * Assumes caller is holding info->lock mutex (for open and release at least)
 * 假设调用者持有 info->lock 权限（至少在打开和释放帧缓冲区时如此）。
 */
static int evdi_fb_release(struct fb_info *info, int user)
{
	struct evdi_fbdev *efbdev = info->par;

	efbdev->fb_count--;

	pr_warn("released /dev/fb%d user=%d count=%d\n",
		info->node, user, efbdev->fb_count);

	return 0;
}
static const struct fb_ops evdifb_ops = {
	.owner = THIS_MODULE,
	.fb_check_var = drm_fb_helper_check_var,
	.fb_set_par = drm_fb_helper_set_par,
	.fb_fillrect = evdi_fb_fillrect,
	.fb_copyarea = evdi_fb_copyarea,
	.fb_imageblit = evdi_fb_imageblit,
	.fb_pan_display = drm_fb_helper_pan_display,
	.fb_blank = drm_fb_helper_blank,
	.fb_setcmap = drm_fb_helper_setcmap,
	.fb_debug_enter = drm_fb_helper_debug_enter,
	.fb_debug_leave = drm_fb_helper_debug_leave,
	.fb_mmap = evdi_fb_mmap,
	.fb_open = evdi_fb_open,
	.fb_release = evdi_fb_release,
};
#endif /* CONFIG_FB */

#if KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
/*
 * Function taken from
 * https://lore.kernel.org/dri-devel/20180905233901.2321-5-drawat@vmware.com/
 */
static int evdi_user_framebuffer_dirty(
		struct drm_framebuffer *fb,
		__maybe_unused struct drm_file *file_priv,
		__always_unused unsigned int flags,
		__always_unused unsigned int color,
		__always_unused struct drm_clip_rect *clips,
		__always_unused unsigned int num_clips)
{
	struct evdi_framebuffer *efb = to_evdi_fb(fb);
	struct drm_device *dev = efb->base.dev;
	struct evdi_device *evdi = dev->dev_private;

	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_plane *plane;
	int ret = 0;
	unsigned int i;

	EVDI_CHECKPT();

	drm_modeset_acquire_init(&ctx,
		/*
		 * When called from ioctl, we are interruptable,
		 * but not when called internally (ie. defio worker)
		 */
		file_priv ? DRM_MODESET_ACQUIRE_INTERRUPTIBLE :	0);

	state = drm_atomic_state_alloc(fb->dev);
	if (!state) {
		ret = -ENOMEM;
		goto out;
	}
	state->acquire_ctx = &ctx;

	for (i = 0; i < num_clips; ++i)
		evdi_painter_mark_dirty(evdi, &clips[i]);

retry:

	drm_for_each_plane(plane, fb->dev) {
		struct drm_plane_state *plane_state;

		if (plane->state->fb != fb)
			continue;

		/*
		 * Even if it says 'get state' this function will create and
		 * initialize state if it does not exists. We use this property
		 * to force create state.
		 */
		plane_state = drm_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state)) {
			ret = PTR_ERR(plane_state);
			goto out;
		}
	}

	ret = drm_atomic_commit(state);

out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}

	if (state)
		drm_atomic_state_put(state);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}
#endif

static int evdi_user_framebuffer_create_handle(struct drm_framebuffer *fb,
					       struct drm_file *file_priv,
					       unsigned int *handle)
{
	struct evdi_framebuffer *efb = to_evdi_fb(fb);

	return drm_gem_handle_create(file_priv, &efb->obj->base, handle);
}

static void evdi_user_framebuffer_destroy(struct drm_framebuffer *fb)
{
	struct evdi_framebuffer *efb = to_evdi_fb(fb);

	EVDI_CHECKPT();
	if (efb->obj)
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE || defined(EL8)
		drm_gem_object_put(&efb->obj->base);
#else
		drm_gem_object_put_unlocked(&efb->obj->base);
#endif
	drm_framebuffer_cleanup(fb);
	kfree(efb);
}

static const struct drm_framebuffer_funcs evdifb_funcs = {
	.create_handle = evdi_user_framebuffer_create_handle,
	.destroy = evdi_user_framebuffer_destroy,
#if KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE || defined(EL8)
	.dirty = drm_atomic_helper_dirtyfb,
#else
	.dirty = evdi_user_framebuffer_dirty,
#endif
};

/**
 * 初始化帧缓冲区
 * @param dev 设备指针
 * @param efb 帧缓冲区指针
 * @param info 格式信息指针
 * @param mode_cmd 模式命令指针
 * @param obj GEM 对象指针
 * @return 0 成功，其他错误码失败
 * @note 在 evdi_fbdev_init 函数中调用。
 */
static int
evdi_framebuffer_init(struct drm_device *dev,
		      struct evdi_framebuffer *efb,
#if KERNEL_VERSION(6, 17, 0) <= LINUX_VERSION_CODE
		      const struct drm_format_info *info,
#endif
		      const struct drm_mode_fb_cmd2 *mode_cmd,
		      struct evdi_gem_object *obj)
{
	efb->obj = obj;
#if KERNEL_VERSION(6, 17, 0) <= LINUX_VERSION_CODE
	if (info == NULL)
		info = drm_get_format_info(dev, mode_cmd->pixel_format,
					   mode_cmd->modifier[0]);
#endif
	drm_helper_mode_fill_fb_struct(dev, &efb->base,
#if KERNEL_VERSION(6, 17, 0) <= LINUX_VERSION_CODE
				       info,
#endif
				       mode_cmd);
	return drm_framebuffer_init(dev, &efb->base, &evdifb_funcs);
}

#ifdef CONFIG_FB
/**
 * 创建 framebuffer（显存缓冲区）并注册到 DRM 框架，使内核中的 fbdev 接口可以使用它。
 * @param helper 帧缓冲区帮助器指针
 * @param sizes 表面大小指针
 * @return 0 成功，其他错误码失败
 * @note 在 evdi_fbdev_init 函数中调用。
 *       - 分配显存页（GEM 对象），建立内核映射（vmap），注册 framebuffer 到 DRM 与 fbdev 层，
 *       - 从而让系统“以为”有个真实的显存设备存在。
 */
int evdifb_create(struct drm_fb_helper *helper,
			 struct drm_fb_helper_surface_size *sizes)
{
	struct evdi_fbdev *efbdev = (struct evdi_fbdev *)helper;
	struct drm_device *dev = efbdev->helper.dev;
	struct fb_info *info;
	struct device *device = dev->dev;
	struct drm_framebuffer *fb;
	struct drm_mode_fb_cmd2 mode_cmd;
	struct evdi_gem_object *obj;
	uint32_t size;
	int ret = 0;

	// 1. 检查像素格式，only support 32bpp
	if (sizes->surface_bpp == 24) {
		sizes->surface_bpp = 32;
	} else if (sizes->surface_bpp != 32) {
		EVDI_ERROR("Not supported pixel format (bpp=%d)\n",
			   sizes->surface_bpp);
		return -EINVAL;
	}

	// 2. 计算 framebuffer 相关参数
	mode_cmd.width = sizes->surface_width;
	mode_cmd.height = sizes->surface_height;
	mode_cmd.pitches[0] = mode_cmd.width * ((sizes->surface_bpp + 7) / 8);

	mode_cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
							  sizes->surface_depth);

	// 3. 计算 framebuffer 大小，并对齐到 PAGE_SIZE
	size = mode_cmd.pitches[0] * mode_cmd.height;
	size = ALIGN(size, PAGE_SIZE);

	// 4. 分配 GEM 对象（显存）
	obj = evdi_gem_alloc_object(dev, size);
	if (!obj)
		goto out;

	// 5. 映射 GEM 对象到虚拟地址空间
	ret = evdi_gem_vmap(obj);
	if (ret) {
		DRM_ERROR("failed to vmap fb\n");
		goto out_gfree;
	}

	// 6. 分配 fb_info 结构体
	info = framebuffer_alloc(0, device);
	if (!info) {
		ret = -ENOMEM;
		goto out_gfree;
	}
	info->par = efbdev;

	// 7. 初始化 DRM framebuffer 对象
	ret = evdi_framebuffer_init(dev, &efbdev->efb,
#if KERNEL_VERSION(6, 17, 0) <= LINUX_VERSION_CODE
				    NULL,
#endif
				    &mode_cmd, obj);
	if (ret)
		goto out_gfree;

	fb = &efbdev->efb.base;

	efbdev->helper.fb = fb;
#if KERNEL_VERSION(6, 2, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
	efbdev->helper.info = info;
#else
	efbdev->helper.fbdev = info;
#endif

	// 8. 设置 fb_info 结构体相关信息
	strscpy(info->fix.id, "evdidrmfb", sizeof(info->fix.id));

	info->screen_base = efbdev->efb.obj->vmapping;  // 内核虚拟地址；
	info->fix.smem_len = size;  // 显存缓冲区大小
	info->fix.smem_start = (unsigned long)efbdev->efb.obj->vmapping;  // 显存缓冲区起始地址（这里虚拟代替）

#if KERNEL_VERSION(6, 4, 0) <= LINUX_VERSION_CODE || defined(EL9)
#elif KERNEL_VERSION(4, 20, 0) <= LINUX_VERSION_CODE || defined(EL8)
	info->flags = FBINFO_DEFAULT;
#else
	info->flags = FBINFO_DEFAULT | FBINFO_CAN_FORCE_OUTPUT;
#endif

	efbdev->fb_ops = &evdifb_ops;
	info->fbops = efbdev->fb_ops;

#if KERNEL_VERSION(5, 2, 0) <= LINUX_VERSION_CODE || defined(EL8)
	// 9. 填充标志与回调函数，让 fbdev 层知道分辨率、像素深度等信息。
	drm_fb_helper_fill_info(info, &efbdev->helper, sizes);
#else
	drm_fb_helper_fill_fix(info, fb->pitches[0], fb->format->depth);
	drm_fb_helper_fill_var(info, &efbdev->helper, sizes->fb_width,
			       sizes->fb_height);
#endif

	// 10. 分配颜色映射表
	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret) {
		ret = -ENOMEM;
		goto out_gfree;
	}

	DRM_DEBUG_KMS("allocated %dx%d vmal %p\n",
		      fb->width, fb->height, efbdev->efb.obj->vmapping);

	return ret;
 out_gfree:
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE || defined(EL8)
	drm_gem_object_put(&efbdev->efb.obj->base);
#else
	drm_gem_object_put_unlocked(&efbdev->efb.obj->base);
#endif
 out:
	return ret;
}

#if KERNEL_VERSION(6, 15, 0) <= LINUX_VERSION_CODE
#else
static struct drm_fb_helper_funcs evdi_fb_helper_funcs = {
	.fb_probe = evdifb_create,
};
#endif

static void evdi_fbdev_destroy(__always_unused struct drm_device *dev,
			       struct evdi_fbdev *efbdev)
{
	struct fb_info *info;

#if KERNEL_VERSION(6, 2, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
	if (efbdev->helper.info) {
		info = efbdev->helper.info;
#else
	if (efbdev->helper.fbdev) {
		info = efbdev->helper.fbdev;
#endif
		unregister_framebuffer(info);
		if (info->cmap.len)
			fb_dealloc_cmap(&info->cmap);

		framebuffer_release(info);
	}
	drm_fb_helper_fini(&efbdev->helper);
	if (efbdev->efb.obj) {
		drm_framebuffer_unregister_private(&efbdev->efb.base);
		drm_framebuffer_cleanup(&efbdev->efb.base);
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE || defined(EL8)
		drm_gem_object_put(&efbdev->efb.obj->base);
#else
		drm_gem_object_put_unlocked(&efbdev->efb.obj->base);
#endif
	}
}

/**
 * 初始化帧缓冲区设备。
 * @param dev 设备指针
 * @return 0 成功，其他错误码失败
 * @note EVDI 驱动启动时创建 /dev/fbX 设备的核心函数
 *       - 初始化 DRM 的 fb_helper、注册虚拟 framebuffer、让 Linux 控制台或 TTY 可以使用这块虚拟显示器。
 */
int evdi_fbdev_init(struct drm_device *dev)
{
	struct evdi_device *evdi;
	struct evdi_fbdev *efbdev;
	int ret;

	evdi = dev->dev_private;
	efbdev = kzalloc(sizeof(struct evdi_fbdev), GFP_KERNEL);
	if (!efbdev)
		return -ENOMEM;

	evdi->fbdev = efbdev;
#if KERNEL_VERSION(6, 15, 0) <= LINUX_VERSION_CODE
	drm_fb_helper_prepare(dev, &efbdev->helper, 32, NULL);
#elif KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
	// 1. 准备 fb_helper, evdi_fb_helper_funcs 是一组回调（里面包含 evdifb_create()）；
	drm_fb_helper_prepare(dev, &efbdev->helper, 32, &evdi_fb_helper_funcs);
#else
	drm_fb_helper_prepare(dev, &efbdev->helper, &evdi_fb_helper_funcs);
#endif

#if KERNEL_VERSION(5, 7, 0) <= LINUX_VERSION_CODE || defined(EL8)
	// 2. 初始化 fb_helper, 注册 framebuffer 到 DRM 与 fbdev 层，从而让系统“以为”有个真实的显存设备存在。
	ret = drm_fb_helper_init(dev, &efbdev->helper);
#else
	ret = drm_fb_helper_init(dev, &efbdev->helper, 1);
#endif
	if (ret) {
		kfree(efbdev);
		return ret;
	}

#if KERNEL_VERSION(5, 7, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	// 3. 添加所有连接器（显卡上的输出端口）到 fb_helper，让系统可以找到这块虚拟显示器。老内核需要。
	drm_fb_helper_single_add_all_connectors(&efbdev->helper);
#endif

#if KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
	// 4. 创建 framebuffer 并注册到 DRM 与 fbdev 层，从而让系统“以为”有个真实的显存设备存在。
	// 这一步最关键，它会：
	// 检测可用显示连接器；
	// 创建 framebuffer；
	// 调用前面注册的回调 → evdifb_create()；
	// 建立 /dev/fbX 设备；
	// 分配内存、映射 vmap()、准备 console 输出。
	ret = drm_fb_helper_initial_config(&efbdev->helper);
#else
	ret = drm_fb_helper_initial_config(&efbdev->helper, 32);
#endif

	if (ret) {
		drm_fb_helper_fini(&efbdev->helper);
		kfree(efbdev);
	}
	return ret;
}

void evdi_fbdev_cleanup(struct drm_device *dev)
{
	struct evdi_device *evdi = dev->dev_private;

	if (!evdi->fbdev)
		return;

	evdi_fbdev_destroy(dev, evdi->fbdev);
	kfree(evdi->fbdev);
	evdi->fbdev = NULL;
}

void evdi_fbdev_unplug(struct drm_device *dev)
{
	struct evdi_device *evdi = dev->dev_private;
	struct evdi_fbdev *efbdev;

	if (!evdi->fbdev)
		return;

	efbdev = evdi->fbdev;
#if KERNEL_VERSION(6, 2, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
	if (efbdev->helper.info) {
		struct fb_info *info;

		info = efbdev->helper.info;
#else
	if (efbdev->helper.fbdev) {
		struct fb_info *info;

		info = efbdev->helper.fbdev;
#endif
#if KERNEL_VERSION(5, 6, 0) <= LINUX_VERSION_CODE || defined(EL8)
		unregister_framebuffer(info);
#else
		unlink_framebuffer(info);
#endif
	}
}
#endif /* CONFIG_FB */

int evdi_fb_get_bpp(uint32_t format)
{
	const struct drm_format_info *info = drm_format_info(format);

	if (!info)
		return 0;
	return info->cpp[0] * 8;
}

/**
 * 创建用户空间帧缓冲区，用于让基于DRM/KMS的用户空间程序（比如 Xorg）能够直接写入虚拟显示器的显存。
 * @param dev 设备指针
 * @param file 文件指针
 * @param info 格式信息指针
 * @param mode_cmd 模式命令指针
 * @return 帧缓冲区指针，成功返回，失败返回错误码
 * @note 在 evdi_fb_user_fb_create 函数中调用。
 */
struct drm_framebuffer *evdi_fb_user_fb_create(
					struct drm_device *dev,
					struct drm_file *file,
#if KERNEL_VERSION(6, 17, 0) <= LINUX_VERSION_CODE
					const struct drm_format_info *info,
#endif
					const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_gem_object *obj;
	struct evdi_framebuffer *efb;
	int ret;
	uint32_t size;
	// 1. 获取每像素位数
	int bpp = evdi_fb_get_bpp(mode_cmd->pixel_format);

	if (bpp != 32) {
		EVDI_ERROR("Unsupported bpp (%d)\n", bpp);
		return ERR_PTR(-EINVAL);
	}

	// 2. 查找 GEM 对象
	obj = drm_gem_object_lookup(file, mode_cmd->handles[0]);
	if (obj == NULL)
		return ERR_PTR(-ENOENT);

	size = mode_cmd->offsets[0] + mode_cmd->pitches[0] * mode_cmd->height;
	size = ALIGN(size, PAGE_SIZE);

	if (size > obj->size) {
		DRM_ERROR("object size not sufficient for fb %d %zu %u %d %d\n",
			  size, obj->size, mode_cmd->offsets[0],
			  mode_cmd->pitches[0], mode_cmd->height);
		goto err_no_mem;
	}

	efb = kzalloc(sizeof(*efb), GFP_KERNEL);
	if (efb == NULL)
		goto err_no_mem;
	efb->base.obj[0] = obj;

	// 3. 初始化帧缓冲区
	ret = evdi_framebuffer_init(dev, efb,
#if KERNEL_VERSION(6, 17, 0) <= LINUX_VERSION_CODE
				    info,
#endif
				    mode_cmd, to_evdi_bo(obj));
	if (ret)
		goto err_inval;
	return &efb->base;

 err_no_mem:
	drm_gem_object_put(obj);
	return ERR_PTR(-ENOMEM);
 err_inval:
	kfree(efb);
	drm_gem_object_put(obj);
	return ERR_PTR(-EINVAL);
}
