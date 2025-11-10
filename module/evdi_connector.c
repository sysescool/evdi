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
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>
#include "evdi_drm_drv.h"

#if KERNEL_VERSION(5, 1, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_probe_helper.h>
#endif

/*
 * dummy connector to just get EDID,
 * all EVDI appear to have a DVI-D
 */

/**
 * 获取显示模式
 * @param connector 连接器指针
 * @return 显示模式数量
 * @note 在 evdi_connector_helper_funcs.get_modes 函数中调用。
 */
static int evdi_get_modes(struct drm_connector *connector)
{
	struct evdi_device *evdi = connector->dev->dev_private;
	struct edid *edid = NULL;
	int ret = 0;

	// 从 painter 获取 EDID（显示器识别信息）
	edid = (struct edid *)evdi_painter_get_edid_copy(evdi);

	if (!edid) {
		// 如果 EDID 为空，则更新连接器 EDID 属性为空
#if KERNEL_VERSION(4, 19, 0) <= LINUX_VERSION_CODE || defined(EL8)
		drm_connector_update_edid_property(connector, NULL);
#else
		drm_mode_connector_update_edid_property(connector, NULL);
#endif
		return 0;
	}

#if KERNEL_VERSION(4, 19, 0) <= LINUX_VERSION_CODE || defined(EL8)
	// 更新连接器 EDID 属性
	ret = drm_connector_update_edid_property(connector, edid);
#else
	ret = drm_mode_connector_update_edid_property(connector, edid);
#endif

	if (ret) {
		EVDI_ERROR("Failed to set edid property! error: %d\n", ret);
		goto err;
	}

	// 将 EDID 中的显示模式添加到连接器
	ret = drm_add_edid_modes(connector, edid);
	EVDI_INFO("(card%d) Edid property set\n", evdi->dev_index);
err:
	kfree(edid);
	// 返回显示模式数量
	return ret;
}

/**
 * 判断显示模式是否为给定分辨率中最低频率的
 * @param connector 连接器指针
 * @param mode 显示模式指针
 * @return 是否为最低频率的显示模式
 * @note 在 evdi_mode_valid 函数中调用。
 */
static bool is_lowest_frequency_mode_of_given_resolution(
	struct drm_connector *connector, const struct drm_display_mode *mode)
{
	struct drm_display_mode *modeptr;

	list_for_each_entry(modeptr, &(connector->modes), head) {
		if (modeptr->hdisplay == mode->hdisplay &&
			modeptr->vdisplay == mode->vdisplay &&
			drm_mode_vrefresh(modeptr) < drm_mode_vrefresh(mode)) {
			return false;
		}
	}
	return true;
}

/**
 * 判断显示模式是否有效
 * @param connector 连接器指针
 * @param mode 显示模式指针
 * @return 显示模式状态
 * @note 在 evdi_connector_helper_funcs.mode_valid 函数中调用。
 */
static enum drm_mode_status evdi_mode_valid(struct drm_connector *connector,
#if KERNEL_VERSION(6, 15, 0) <= LINUX_VERSION_CODE
					    const struct drm_display_mode *mode)
#else
					    struct drm_display_mode *mode)
#endif
{
	// 获取 evdi 设备指针
	struct evdi_device *evdi = connector->dev->dev_private;
	// 计算显示模式面积
	uint32_t area_limit = mode->hdisplay * mode->vdisplay;
	uint32_t mode_limit = area_limit * drm_mode_vrefresh(mode);

	// 如果每秒像素限制为 0，则认为显示模式有效
	if (evdi->pixel_per_second_limit == 0)
		return MODE_OK;

	// 如果显示模式面积大于像素区域限制，则认为显示模式无效
	if (area_limit > evdi->pixel_area_limit) {
		EVDI_WARN(
			"(card%d) Mode %dx%d@%d rejected. Reason: mode area too big\n",
			evdi->dev_index,
			mode->hdisplay,
			mode->vdisplay,
			drm_mode_vrefresh(mode));
		return MODE_BAD;
	}

	// 如果显示模式像素时钟小于每秒像素限制，则认为显示模式有效
	if (mode_limit <= evdi->pixel_per_second_limit)
		return MODE_OK;

	// 如果显示模式是给定分辨率中最低频率的，则认为显示模式有效
	if (is_lowest_frequency_mode_of_given_resolution(connector, mode)) {
		EVDI_WARN(
			"(card%d) Mode exceeds maximal frame rate for the device. Mode %dx%d@%d may have a limited output frame rate",
			evdi->dev_index,
			mode->hdisplay,
			mode->vdisplay,
			drm_mode_vrefresh(mode));
		return MODE_OK;
	}

	// 如果显示模式像素时钟大于每秒像素限制，则认为显示模式无效，打印警告日志
	EVDI_WARN(
		"(card%d) Mode %dx%d@%d rejected. Reason: mode pixel clock too high\n",
		evdi->dev_index,
		mode->hdisplay,
		mode->vdisplay,
		drm_mode_vrefresh(mode));

	return MODE_BAD;
}

/**
 * 判断显示设备是否插入
 * @param connector 连接器指针
 * @param force 是否强制检测
 * @return 连接状态
 * @note 在 evdi_connector_init 函数中调用。
 */
static enum drm_connector_status
evdi_detect(struct drm_connector *connector, __always_unused bool force)
{
	struct evdi_device *evdi = connector->dev->dev_private;

	EVDI_CHECKPT();
	// 如果 painter 连接成功，则认为显示设备插入
	if (evdi_painter_is_connected(evdi->painter)) {
		EVDI_INFO("(card%d) Connector state: connected\n",
			   evdi->dev_index);
		// 返回连接状态
		return connector_status_connected;
	}
	// 如果 painter 连接失败，则认为显示设备未插入
	EVDI_VERBOSE("(card%d) Connector state: disconnected\n",
		   evdi->dev_index);
	return connector_status_disconnected;
}

static void evdi_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(connector);
}

/**
 * 获取最佳编码器
 * @param connector 连接器指针
 * @return 最佳编码器指针
 * @note 在 evdi_connector_helper_funcs.best_encoder 函数中调用。
 * Framebuffer -> CRTC -> Encoder -> Connector -> 显示器
 * 一个 encoder 是 显示流水线中的一个硬件或逻辑模块，负责把 framebuffer（显存里的像素数据）转换成具体的信号，
 * 输出到 connector（HDMI、DP、DVI、虚拟显示器等）。
 * - Framebuffer：存放在显存或内存里的像素数据。
 * - CRTC（Cathode Ray Tube Controller）：显卡中的时序控制模块，负责扫描、刷新、时钟等。
 * - Encoder：显卡的一个模块，把 CRTC 输出的信号转换成某种物理接口格式（如 TMDS / HDMI / DP / 虚拟信号）。
 * - Connector：表示接口端口（HDMI1, DP1, VGA, EVDI0），可以是物理接口或虚拟接口。
 * - 显示器：真正显示图像的屏幕。
 */
static struct drm_encoder *evdi_best_encoder(struct drm_connector *connector)
{
#if KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE || defined(EL8)
	struct drm_encoder *encoder;

	// drm_connector_for_each_possible_encoder 会遍历 connector 的 possible_encoders 列表，找到第一个可用的 encoder。
	// 如果找到，则返回该 encoder。
	// 如果未找到，则返回 NULL。
	drm_connector_for_each_possible_encoder(connector, encoder) {
		// 返回最佳编码器
		return encoder;
	}

	return NULL;
#else
	return drm_encoder_find(connector->dev,
				NULL,
				connector->encoder_ids[0]);
#endif
}

static struct drm_connector_helper_funcs evdi_connector_helper_funcs = {
	// 在探测/枚举模式时调用，用来把驱动已知的显示模式（resolution/refresh）加入 DRM。
	// 获取显示模式（系统查询显示器支持的分辨率时调用）
	.get_modes = evdi_get_modes,
	// 用于验证一个 drm_display_mode 是否对该 connector/硬件有效。
	.mode_valid = evdi_mode_valid,
	// 当 DRM 需要将 connector 绑定到某个 encoder（显示流水线）时，选择并返回最合适的 drm_encoder 指针。
	.best_encoder = evdi_best_encoder,
};

static const struct drm_connector_funcs evdi_connector_funcs = {
	// DRM 驱动中判断显示设备是否插入的函数。
	.detect = evdi_detect,
	// 获取显示模式（系统查询显示器支持的分辨率时调用）
	.fill_modes = drm_helper_probe_single_connector_modes,
	// 销毁连接器
	.destroy = evdi_connector_destroy,
	// 重置连接器
	.reset = drm_atomic_helper_connector_reset,
	// 复制连接器状态
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	// 销毁连接器状态
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state
};

/**
 * 初始化连接器。
 * @param dev DRM 设备指针
 * @param encoder 编码器指针
 * @return 0 成功，其他 失败
 * @note Start。在 evdi_modeset_init 函数中调用。
 */
int evdi_connector_init(struct drm_device *dev, struct drm_encoder *encoder)
{
	struct drm_connector *connector;
	struct evdi_device *evdi = dev->dev_private;

	connector = kzalloc(sizeof(struct drm_connector), GFP_KERNEL);
	if (!connector)
		return -ENOMEM;

	/* TODO: Initialize connector with actual connector type */
	drm_connector_init(dev, connector, &evdi_connector_funcs,
			   DRM_MODE_CONNECTOR_DVII);
	drm_connector_helper_add(connector, &evdi_connector_helper_funcs);
	connector->polled = DRM_CONNECTOR_POLL_HPD;

	drm_connector_register(connector);

	evdi->conn = connector;

#if KERNEL_VERSION(4, 19, 0) <= LINUX_VERSION_CODE  || defined(EL8)
	drm_connector_attach_encoder(connector, encoder);
#else
	drm_mode_connector_attach_encoder(connector, encoder);
#endif
	return 0;
}
