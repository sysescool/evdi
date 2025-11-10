# EVDI 内核模块学习指南

## 📚 第一部分：内核模块基础知识

### 1.1 什么是内核模块？

内核模块（Kernel Module）是一段可以动态加载到 Linux 内核中的代码。它：
- **不是独立的程序**：不能直接运行，必须加载到内核中
- **运行在内核空间**：拥有最高权限，可以直接访问硬件
- **可以动态加载/卸载**：使用 `insmod`/`rmmod` 命令
- **必须遵循内核编程规范**：不能使用标准 C 库（如 `printf`），必须使用内核 API

### 1.2 内核模块的基本结构

每个内核模块必须包含：
```c
// 模块初始化函数（加载时调用）
static int __init my_module_init(void)
{
    // 初始化代码
    return 0;  // 0 表示成功
}

// 模块清理函数（卸载时调用）
static void __exit my_module_exit(void)
{
    // 清理代码
}

// 注册这两个函数
module_init(my_module_init);
module_exit(my_module_exit);
```

---

## 📖 第二部分：阅读顺序（从简单到复杂）

### 阶段 1：理解构建系统（最简单）

#### 文件 1：`Makefile` ⭐ 入门必读

**为什么先看这个？**
- 它告诉你模块由哪些文件组成
- 了解编译流程
- 不需要深入理解内核 API

**关键点：**

```makefile
# 第 31 行：定义模块包含的所有 .o 文件
evdi-y := evdi_platform_drv.o evdi_platform_dev.o evdi_sysfs.o ...

# 第 33 行：告诉内核构建系统生成 evdi.ko
obj-m := evdi.o
```

**学习要点：**
- `evdi-y` 表示这些文件会被编译并链接到 `evdi.ko` 中
- `obj-m` 表示这是一个可加载模块（m = module）
- `obj-y` 表示编译进内核（y = yes）

---

### 阶段 2：理解调试和参数系统

#### 文件 2：`evdi_debug.h` ⭐ 简单实用

**为什么看这个？**
- 代码简单，容易理解
- 所有其他文件都会用到它
- 学习内核的日志系统

**关键代码解析：**

```c
// 第 21-25 行：定义了一个条件打印宏
#define EVDI_PRINTK(KERN_LEVEL, LEVEL, FORMAT_STR, ...)	do { \
	if (evdi_loglevel >= LEVEL) {\
		printk(KERN_LEVEL "evdi: " FORMAT_STR, ##__VA_ARGS__); \
	} \
} while (0)
```

**解释：**
- `printk`：内核版本的 `printf`，用于打印日志
- `KERN_LEVEL`：日志级别（如 `KERN_ERR`、`KERN_INFO`）
- `##__VA_ARGS__`：C 宏的可变参数
- `do { ... } while (0)`：确保宏在任何上下文中都能正确使用

**使用示例：**
```c
EVDI_INFO("设备 %d 已创建\n", device_id);
// 展开后相当于：
if (evdi_loglevel >= 4) {
    printk(KERN_DEFAULT "evdi: [I] 设备 %d 已创建\n", device_id);
}
```

#### 文件 3：`evdi_params.h` 和 `evdi_params.c`

**作用：** 定义模块参数，允许用户在加载模块时传入参数

**学习要点：**
- `module_param()`：定义模块参数
- 用户可以通过 `modprobe evdi evdi_loglevel=5` 来设置参数

---

### 阶段 3：理解模块的入口和出口

#### 文件 4：`evdi_platform_drv.c` ⭐⭐⭐ 核心入口

**这是模块的"主函数"，从这里开始理解整个模块！**

**关键代码解析：**

```c
// 第 213-241 行：模块初始化函数
static int __init evdi_init(void)
{
    int ret;
    
    // 1. 初始化日志
    EVDI_INFO("Initialising logging on level %u\n", evdi_loglevel);
    
    // 2. 初始化全局上下文结构
    memset(&g_ctx, 0, sizeof(g_ctx));
    
    // 3. 注册根设备（在 /sys 下创建目录）
    g_ctx.root_dev = root_device_register(DRIVER_NAME);
    
    // 4. 初始化互斥锁（保护共享数据）
    mutex_init(&g_ctx.lock);
    
    // 5. 初始化 sysfs 接口（用户空间可以通过 /sys 操作）
    evdi_sysfs_init(g_ctx.root_dev);
    
    // 6. 注册平台驱动（这是关键！）
    ret = platform_driver_register(&evdi_platform_driver);
    
    return 0;
}
```

**第 203-211 行：平台驱动结构体**
```c
static struct platform_driver evdi_platform_driver = {
    .probe = evdi_platform_device_probe,    // 当设备匹配时调用
    .remove = evdi_platform_device_remove,   // 当设备移除时调用
    .driver = {
        .name = DRIVER_NAME,                 // 驱动名称 "evdi"
        .owner = THIS_MODULE,
    }
};
```

**理解平台驱动：**
- Linux 内核使用"平台设备"模型来管理虚拟设备
- `probe` 函数：当系统发现匹配的设备时自动调用
- `remove` 函数：当设备被移除时调用

**第 243-258 行：模块退出函数**
```c
static void __exit evdi_exit(void)
{
    // 1. 移除所有设备
    evdi_platform_remove_all_devices(g_ctx.root_dev);
    
    // 2. 注销平台驱动
    platform_driver_unregister(&evdi_platform_driver);
    
    // 3. 清理 sysfs
    evdi_sysfs_exit(g_ctx.root_dev);
    
    // 4. 注销根设备
    root_device_unregister(g_ctx.root_dev);
}
```

**第 260-261 行：注册入口和出口函数**
```c
module_init(evdi_init);   // 告诉内核：加载时调用 evdi_init
module_exit(evdi_exit);   // 告诉内核：卸载时调用 evdi_exit
```

**全局上下文结构（第 31-39 行）：**
```c
static struct evdi_platform_drv_context {
    struct device *root_dev;              // 根设备指针
    unsigned int dev_count;               // 当前设备数量
    struct platform_device *devices[16];  // 最多支持 16 个设备
    struct mutex lock;                    // 保护共享数据的锁
} g_ctx;
```

**学习要点：**
- `static`：限制作用域，只在当前文件可见
- `mutex`：互斥锁，防止多线程/多进程同时访问共享数据
- `g_ctx`：全局上下文，存储模块的全局状态

---

### 阶段 4：理解设备管理

#### 文件 5：`evdi_platform_dev.c`

**作用：** 管理单个平台设备的创建和销毁

**关键概念：**
- `platform_device`：代表一个虚拟显示设备
- `platform_device_info`：创建设备时需要的参数

**学习要点：**
- 设备如何创建和销毁
- 设备如何与父设备（如 USB 设备）关联

---

### 阶段 5：理解用户空间接口

#### 文件 6：`evdi_sysfs.c` ⭐⭐ 用户可见的接口

**作用：** 提供 sysfs 接口，让用户空间程序可以通过文件系统操作模块

**关键概念：**
- **sysfs**：Linux 内核提供的虚拟文件系统，通常在 `/sys` 目录下
- **设备属性**：通过读写文件来操作设备

**示例：**
```bash
# 用户可以通过这些文件操作模块：
echo 1 > /sys/class/drm/evdi/version    # 查看版本
cat /sys/class/drm/evdi/count            # 查看设备数量
echo 1 > /sys/class/drm/evdi/add_device  # 添加设备
```

**关键函数模式：**
```c
// 读取属性（用户 cat 文件时调用）
static ssize_t version_show(struct device *dev,
                           struct device_attribute *attr,
                           char *buf)
{
    return snprintf(buf, PAGE_SIZE, "%u.%u.%u\n", ...);
}

// 写入属性（用户 echo > 文件时调用）
static ssize_t add_device_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t count)
{
    // 解析 buf，执行添加设备的操作
    return count;
}

// 定义属性
static DEVICE_ATTR_RW(add_device);  // 可读写
static DEVICE_ATTR_RO(version);     // 只读
```

---

### 阶段 6：理解 DRM 驱动（较复杂）

#### 文件 6：`evdi_drm_drv.c` 和 `evdi_drm_drv.h` ⭐⭐⭐⭐ 核心驱动

**作用：** 实现 Linux DRM（Direct Rendering Manager）接口

**为什么需要 DRM？**
- DRM 是 Linux 图形系统的标准接口
- 通过 DRM，虚拟显示器可以被系统识别为真正的显示器
- 支持 `xrandr`、Wayland 等图形系统

**关键结构：**

```c
// DRM 驱动结构体
static struct drm_driver driver = {
    .driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
    .open = evdi_driver_open,           // 打开设备文件时调用
    .postclose = evdi_driver_postclose, // 关闭设备文件时调用
    .ioctls = evdi_painter_ioctls,      // 定义 ioctl 接口
    // ... 更多回调函数
};
```

**ioctl 接口（第 39-50 行）：**
```c
struct drm_ioctl_desc evdi_painter_ioctls[] = {
    DRM_IOCTL_DEF_DRV(EVDI_CONNECT, evdi_painter_connect_ioctl, ...),
    DRM_IOCTL_DEF_DRV(EVDI_REQUEST_UPDATE, evdi_painter_request_update_ioctl, ...),
    // ...
};
```

**ioctl 是什么？**
- `ioctl` = "I/O Control"，用于设备控制
- 用户空间程序通过 `ioctl()` 系统调用与内核通信
- 例如：`ioctl(fd, EVDI_CONNECT, &connect_data)`

---

### 阶段 7：理解显示组件

#### 文件 8：`evdi_connector.c` ⭐⭐⭐

**作用：** 实现 DRM 连接器，模拟物理显示器

**关键函数：**

```c
// 获取显示模式（系统查询显示器支持的分辨率时调用）
static int evdi_get_modes(struct drm_connector *connector)
{
    // 1. 从 painter 获取 EDID（显示器识别信息）
    struct edid *edid = evdi_painter_get_edid_copy(evdi);
    
    // 2. 将 EDID 设置到连接器
    drm_connector_update_edid_property(connector, edid);
    
    // 3. 从 EDID 解析出支持的分辨率
    ret = drm_add_edid_modes(connector, edid);
    
    return ret;
}
```

**EDID 是什么？**
- Extended Display Identification Data
- 包含显示器的分辨率、刷新率等信息
- 物理显示器通过 I2C 总线发送 EDID
- 虚拟显示器由用户空间程序提供 EDID

---

### 阶段 7：理解 GEM 内存管理（重要基础）

#### 文件 7：`evdi_gem.c` ⭐⭐⭐⭐ DRM 内存管理基础

**作用：** 实现 GEM（Graphics Execution Manager）对象管理，这是 DRM 驱动中内存管理的核心

**为什么需要 GEM？**
- DRM 驱动需要管理图形缓冲区（framebuffer）
- GEM 提供了统一的内存管理接口
- 支持用户空间和内核空间共享内存

**关键数据结构：**

```c
// 第 63-74 行：GEM 对象结构
struct evdi_gem_object {
    struct drm_gem_object base;    // DRM 基类
    struct page **pages;           // 物理页面数组
    unsigned int pages_pin_count;  // 页面引用计数
    struct mutex pages_lock;       // 保护页面的锁
    void *vmapping;                // 虚拟地址映射
    struct sg_table *sg;           // 散列表（用于 DMA）
    bool allow_sw_cursor_rect_updates;  // 是否允许软件光标更新
};
```

**关键函数：**

1. **分配 GEM 对象（第 94-118 行）：**
```c
struct evdi_gem_object *evdi_gem_alloc_object(struct drm_device *dev, size_t size)
{
    // 1. 分配 evdi_gem_object 结构
    obj = kzalloc(sizeof(*obj), GFP_KERNEL);
    
    // 2. 初始化 DRM GEM 对象
    drm_gem_object_init(dev, &obj->base, size);
    
    // 3. 初始化互斥锁
    mutex_init(&obj->pages_lock);
    
    return obj;
}
```

2. **虚拟地址映射（第 293-331 行）：**
```c
int evdi_gem_vmap(struct evdi_gem_object *obj)
{
    // 将物理页面映射到虚拟地址空间
    // 这样内核可以直接访问缓冲区内容
    obj->vmapping = vmap(obj->pages, page_count, 0, PAGE_KERNEL);
    return 0;
}
```

3. **页面管理（第 237-291 行）：**
```c
// 获取物理页面
static int evdi_gem_get_pages(struct evdi_gem_object *obj, gfp_t gfpmask)
{
    pages = drm_gem_get_pages(&obj->base);
    obj->pages = pages;
    return 0;
}

// 固定页面（防止被换出）
static int evdi_pin_pages(struct evdi_gem_object *obj)
{
    mutex_lock(&obj->pages_lock);
    if (obj->pages_pin_count++ == 0) {
        ret = evdi_gem_get_pages(obj, GFP_KERNEL);
    }
    mutex_unlock(&obj->pages_lock);
    return ret;
}
```

**学习要点：**
- `drm_gem_object`：DRM 框架提供的基类
- `pages`：指向物理页面的指针数组
- `vmapping`：虚拟地址，用于内核直接访问
- `pin/unpin`：固定/释放页面，防止被换出到交换空间

---

### 阶段 8：理解帧缓冲区管理

#### 文件 8：`evdi_fb.c` ⭐⭐⭐⭐ 显示缓冲区

**作用：** 管理 DRM 帧缓冲区（framebuffer），这是实际存储像素数据的地方

**关键数据结构：**

```c
// 第 78-82 行：帧缓冲区结构
struct evdi_framebuffer {
    struct drm_framebuffer base;  // DRM 基类
    struct evdi_gem_object *obj;  // 关联的 GEM 对象（存储像素数据）
    bool active;                   // 是否处于活动状态
};
```

**关键函数：**

1. **创建用户空间帧缓冲区（第 609-663 行）：**
```c
struct drm_framebuffer *evdi_fb_user_fb_create(...)
{
    // 1. 查找 GEM 对象（通过 handle）
    obj = drm_gem_object_lookup(file, mode_cmd->handles[0]);
    
    // 2. 创建 evdi_framebuffer
    efb = kzalloc(sizeof(*efb), GFP_KERNEL);
    
    // 3. 初始化帧缓冲区
    evdi_framebuffer_init(dev, efb, mode_cmd, to_evdi_bo(obj));
    
    return &efb->base;
}
```

2. **处理脏矩形（第 88-106 行）：**
```c
static int evdi_handle_damage(struct evdi_framebuffer *fb,
                                int x, int y, int width, int height)
{
    // 1. 创建脏矩形
    const struct drm_clip_rect dirty_rect = { x, y, x + width, y + height };
    
    // 2. 设置扫描输出缓冲区
    evdi_painter_set_scanout_buffer(evdi->painter, fb);
    
    // 3. 标记为脏（需要更新）
    evdi_painter_mark_dirty(evdi, &rect);
    
    return 0;
}
```

3. **帧缓冲区操作（第 142-173 行）：**
```c
// 填充矩形（系统调用 fillrect 时）
static void evdi_fb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
    sys_fillrect(info, rect);  // 执行实际的填充操作
    evdi_handle_damage(&efbdev->efb, rect->dx, rect->dy, 
                       rect->width, rect->height);  // 标记为脏
}

// 复制区域（系统调用 copyarea 时）
static void evdi_fb_copyarea(struct fb_info *info, const struct fb_copyarea *region)
{
    sys_copyarea(info, region);
    evdi_handle_damage(&efbdev->efb, region->dx, region->dy,
                       region->width, region->height);
}
```

**学习要点：**
- 帧缓冲区是实际存储像素数据的地方
- 当系统绘制内容时，会调用 `evdi_handle_damage` 标记需要更新的区域
- 脏矩形（dirty rect）用于优化，只更新变化的部分

---

### 阶段 9：理解显示组件（补充）

#### 文件 9：`evdi_encoder.c` ⭐ 简单的编码器

**作用：** 实现 DRM 编码器（Encoder），将数字信号转换为显示信号

**关键代码：**

```c
// 第 49-74 行：初始化编码器
struct drm_encoder *evdi_encoder_init(struct drm_device *dev)
{
    encoder = kzalloc(sizeof(struct drm_encoder), GFP_KERNEL);
    
    // 注册编码器（类型为 TMDS，用于 DVI/HDMI）
    drm_encoder_init(dev, encoder, &evdi_enc_funcs,
                     DRM_MODE_ENCODER_TMDS, "%s", dev_name(dev->dev));
    
    encoder->possible_crtcs = 1;  // 可以连接到 1 个 CRTC
    return encoder;
}
```

**为什么这么简单？**
- EVDI 是虚拟显示器，不需要真正的硬件编码
- 编码器函数都是空的（dummy）
- 只是为了满足 DRM 框架的要求

**学习要点：**
- 编码器是 DRM 显示管道的一部分：CRTC → Encoder → Connector
- 对于虚拟显示器，编码器只是占位符

---

### 阶段 10：理解模式设置

#### 文件 10：`evdi_modeset.c` ⭐⭐⭐⭐ 分辨率切换核心

**作用：** 实现 DRM 模式设置（modeset），处理分辨率切换、显示模式配置等

**关键函数：**

1. **初始化模式配置（第 510-536 行）：**
```c
void evdi_modeset_init(struct drm_device *dev)
{
    // 1. 初始化模式配置
    drm_mode_config_init(dev);
    
    // 2. 设置最小/最大分辨率
    dev->mode_config.min_width = 64;
    dev->mode_config.min_height = 64;
    dev->mode_config.max_width = 7680;   // 8K
    dev->mode_config.max_height = 4320;
    
    // 3. 初始化 CRTC（显示控制器）
    evdi_crtc_init(dev);
    
    // 4. 初始化编码器
    encoder = evdi_encoder_init(dev);
    
    // 5. 初始化连接器
    evdi_connector_init(dev, encoder);
    
    // 6. 重置模式配置
    drm_mode_config_reset(dev);
}
```

2. **原子更新（第 220-307 行）：**
```c
static void evdi_plane_atomic_update(struct drm_plane *plane, ...)
{
    // 当显示内容更新时调用
    struct evdi_framebuffer *efb = to_evdi_fb(state->fb);
    
    // 1. 设置扫描输出缓冲区
    evdi_painter_set_scanout_buffer(painter, efb);
    
    // 2. 标记脏矩形（哪些区域需要更新）
    drm_atomic_helper_damage_iter_init(&iter, old_state, state);
    while (drm_atomic_helper_damage_iter_next(&iter, &rect)) {
        evdi_painter_mark_dirty(evdi, &clip_rect);
    }
}
```

3. **CRTC 刷新（第 66-95 行）：**
```c
static void evdi_crtc_atomic_flush(struct drm_crtc *crtc, ...)
{
    // 当模式改变时
    if (notify_mode_changed)
        evdi_painter_mode_changed_notify(evdi, &crtc_state->adjusted_mode);
    
    // 当显示开关时
    if (notify_dpms)
        evdi_painter_dpms_notify(evdi->painter,
            crtc_state->active ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF);
    
    // 设置垂直空白事件
    evdi_painter_set_vblank(evdi->painter, crtc, crtc_state->event);
    
    // 发送更新就绪事件
    evdi_painter_send_update_ready_if_needed(evdi->painter);
}
```

**学习要点：**
- **CRTC**：显示控制器，管理显示时序
- **Plane**：显示平面，可以是主平面或光标平面
- **原子更新**：DRM 使用原子模式设置，所有更改一起提交
- **脏矩形**：只更新变化的部分，提高效率

---

### 阶段 11：理解光标管理

#### 文件 11：`evdi_cursor.h` 和 `evdi_cursor.c` ⭐⭐⭐ 鼠标光标

**作用：** 管理鼠标光标的显示、位置和合成

**关键数据结构：**

```c
// 第 38-50 行：光标结构
struct evdi_cursor {
    bool enabled;              // 是否启用
    int32_t x, y;             // 光标位置
    uint32_t width, height;   // 光标大小
    int32_t hot_x, hot_y;     // 热点位置（点击点）
    uint32_t pixel_format;    // 像素格式（如 ARGB8888）
    uint32_t stride;          // 行跨度（字节）
    struct evdi_gem_object *obj;  // 光标图像数据
    struct mutex lock;        // 保护锁
};
```

**关键函数：**

1. **设置光标（第 118-145 行）：**
```c
void evdi_cursor_set(struct evdi_cursor *cursor,
                     struct evdi_gem_object *obj,
                     uint32_t width, uint32_t height,
                     int32_t hot_x, int32_t hot_y,
                     uint32_t pixel_format, uint32_t stride)
{
    evdi_cursor_lock(cursor);
    
    // 1. 映射 GEM 对象到虚拟地址
    if (obj && !obj->vmapping)
        evdi_gem_vmap(obj);
    
    // 2. 保存光标参数
    cursor->width = width;
    cursor->height = height;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;
    cursor->pixel_format = pixel_format;
    cursor->stride = stride;
    
    evdi_cursor_set_gem(cursor, obj);
    evdi_cursor_unlock(cursor);
}
```

2. **光标合成（第 187-260 行）：**
```c
int evdi_cursor_compose_and_copy(struct evdi_cursor *cursor,
                                  struct evdi_framebuffer *efb,
                                  char __user *buffer,
                                  int buf_byte_stride)
{
    // 将光标图像合成到帧缓冲区中
    // 遍历光标覆盖的每个像素
    for (y = -h_cursor_h; y < h_cursor_h; ++y) {
        for (x = -h_cursor_w; x < h_cursor_w; ++x) {
            // 1. 获取光标像素值
            curs_val = cursor_buffer[cursor_pix];
            
            // 2. 获取帧缓冲区像素值
            fb_value = *(fbsrc + ...);
            
            // 3. Alpha 混合
            composed_value = blend_alpha(fb_value, curs_val);
            
            // 4. 复制到用户空间缓冲区
            copy_to_user(buffer + cmd_offset, &composed_value, 4);
        }
    }
}
```

**Alpha 混合（第 155-175 行）：**
```c
// 将光标图像与背景图像混合
static inline uint32_t blend_alpha(const uint32_t pixel_val32,
                                    uint32_t blend_val32)
{
    uint32_t alpha = (blend_val32 >> 24);  // 提取 Alpha 通道
    
    // 对每个颜色分量进行混合
    return blend_component(pixel_val32 & 0xff, blend_val32 & 0xff, alpha) |
           blend_component(...) << 8 |
           blend_component(...) << 16;
}
```

**学习要点：**
- 光标是一个独立的图像，需要合成到帧缓冲区中
- Alpha 混合用于实现透明效果
- 热点（hot point）是光标的点击位置

---

### 阶段 12：理解通信机制

#### 文件 12：`evdi_painter.c` ⭐⭐⭐⭐⭐ 最复杂但最重要

**作用：** 内核和用户空间之间的通信桥梁

**关键数据结构：**

```c
struct evdi_painter {
    bool is_connected;                    // 用户空间是否已连接
    struct edid *edid;                    // 存储的 EDID 数据
    struct drm_clip_rect dirty_rects[16]; // 脏矩形区域（哪些区域需要更新）
    struct evdi_framebuffer *scanout_fb;  // 当前扫描输出的帧缓冲区
    struct drm_file *drm_filp;            // 关联的 DRM 文件
    // ...
};
```

**工作流程：**

1. **用户空间连接：**
   ```c
   // 用户程序调用 ioctl(EVDI_CONNECT, ...)
   int evdi_painter_connect_ioctl(...)
   {
       // 1. 保存 EDID 数据
       painter->edid = kmalloc(edid_length, GFP_KERNEL);
       memcpy(painter->edid, user_edid, edid_length);
       
       // 2. 标记为已连接
       painter->is_connected = true;
   }
   ```

2. **系统渲染到虚拟显示器：**
   ```c
   // 当系统需要更新显示内容时
   void evdi_painter_mark_dirty(...)
   {
       // 记录哪些区域需要更新
       painter->dirty_rects[painter->num_dirts++] = rect;
   }
   ```

3. **用户空间请求更新：**
   ```c
   // 用户程序调用 ioctl(EVDI_REQUEST_UPDATE, ...)
   int evdi_painter_request_update_ioctl(...)
   {
       // 发送事件通知用户空间有更新
       evdi_painter_send_update_ready_if_needed(painter);
   }
   ```

4. **用户空间抓取像素：**
   ```c
   // 用户程序调用 ioctl(EVDI_GRABPIX, ...)
   int evdi_painter_grabpix_ioctl(...)
   {
       // 将帧缓冲区数据复制到用户空间
       copy_to_user(user_buffer, kernel_buffer, size);
   }
   ```

---

### 阶段 13：理解辅助功能

#### 文件 13：`evdi_drm.h` ⭐⭐ 用户空间 API 定义

**作用：** 定义用户空间和内核空间之间的接口（ioctl 命令和数据结构）

**关键内容：**

```c
// 第 20-27 行：事件定义
#define DRM_EVDI_EVENT_UPDATE_READY  0x80000000
#define DRM_EVDI_EVENT_DPMS          0x80000001
#define DRM_EVDI_EVENT_MODE_CHANGED  0x80000002
#define DRM_EVDI_EVENT_CRTC_STATE    0x80000003
#define DRM_EVDI_EVENT_CURSOR_SET   0x80000004
#define DRM_EVDI_EVENT_CURSOR_MOVE   0x80000005
#define DRM_EVDI_EVENT_DDCCI_DATA    0x80000006

// 第 52-59 行：连接数据结构
struct drm_evdi_connect {
    int32_t connected;
    int32_t dev_index;
    const unsigned char * __user edid;  // 用户空间 EDID 数据
    uint32_t edid_length;
    uint32_t pixel_area_limit;
    uint32_t pixel_per_second_limit;
};

// 第 70-78 行：抓取像素数据结构
struct drm_evdi_grabpix {
    enum drm_evdi_grabpix_mode mode;
    int32_t buf_width, buf_height;
    int32_t buf_byte_stride;
    unsigned char __user *buffer;  // 用户空间缓冲区
    int32_t num_rects;
    struct drm_clip_rect __user *rects;
};

// 第 121-125 行：ioctl 命令定义
#define DRM_EVDI_CONNECT          0x00
#define DRM_EVDI_REQUEST_UPDATE  0x01
#define DRM_EVDI_GRABPIX         0x02
#define DRM_EVDI_DDCCI_RESPONSE  0x03
#define DRM_EVDI_ENABLE_CURSOR_EVENTS 0x04
```

**学习要点：**
- 这是用户空间库（`library/`）和内核模块之间的"合同"
- `__user` 标记表示这是用户空间指针，需要使用 `copy_from_user`/`copy_to_user`
- 所有 ioctl 命令都在这里定义

---

#### 文件 14：`evdi_i2c.h` 和 `evdi_i2c.c` ⭐⭐ I2C 总线模拟

**作用：** 模拟 I2C 总线，用于 EDID 和 DDC/CI 通信

**为什么需要 I2C？**
- 物理显示器通过 I2C 总线发送 EDID
- 某些应用程序（如 `ddcutil`）通过 I2C 读取显示器信息
- EVDI 需要模拟这个接口以保持兼容性

**关键代码：**

```c
// 第 14-27 行：I2C 主控制器访问函数
static int dli2c_access_master(struct i2c_adapter *adapter,
                               struct i2c_msg *msgs, int num)
{
    struct evdi_device *evdi = adapter->algo_data;
    struct evdi_painter *painter = evdi->painter;
    
    // 将 I2C 消息转发给 painter
    for (i = 0; i < num; i++) {
        if (evdi_painter_i2c_data_notify(painter, &msgs[i]))
            result++;
    }
    return result;
}

// 第 34-37 行：I2C 算法结构
static struct i2c_algorithm dli2c_algorithm = {
    .master_xfer = dli2c_access_master,  // 主控制器传输函数
    .functionality = dli2c_func,          // 返回支持的功能
};
```

**学习要点：**
- I2C 适配器是 Linux I2C 子系统的接口
- 当应用程序通过 I2C 读取 EDID 时，消息会被转发到 `evdi_painter`
- 这是虚拟硬件模拟的一个例子

---

#### 文件 15：`evdi_ioc32.c` ⭐ 32 位兼容性

**作用：** 提供 32 位应用程序在 64 位内核上的兼容性支持

**为什么需要？**
- 32 位应用程序使用 32 位指针，64 位内核使用 64 位指针
- 需要转换数据结构中的指针类型

**关键代码：**

```c
// 第 36-43 行：32 位版本的连接结构
struct drm_evdi_connect32 {
    int32_t connected;
    int32_t dev_index;
    uint32_t edid_ptr32;        // 32 位指针（而不是 64 位）
    uint32_t edid_length;
    uint32_t pixel_area_limit;
    uint32_t pixel_per_second_limit;
};

// 第 55-73 行：兼容性转换函数
static int compat_evdi_connect(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct drm_evdi_connect32 req32;
    struct drm_evdi_connect krequest;
    
    // 1. 从用户空间复制 32 位结构
    copy_from_user(&req32, (void __user *)arg, sizeof(req32));
    
    // 2. 转换 32 位指针为 64 位指针
    krequest.edid = compat_ptr(req32.edid_ptr32);
    
    // 3. 调用实际的 ioctl 处理函数
    return drm_ioctl_kernel(file, evdi_painter_connect_ioctl, &krequest, 0);
}
```

**学习要点：**
- `compat_ptr()` 将 32 位指针转换为 64 位指针
- 这是 Linux 内核兼容性层的一部分
- 确保 32 位应用程序可以在 64 位系统上运行

---

## 🎯 第三部分：完整学习路径（更新）

### 学习步骤（按阶段顺序）

1. **第一周：基础概念**
   - [x] Stage 1: Makefile - 理解构建流程
   - [x] Stage 2: evdi_debug & evdi_params - 日志和参数系统
   - [x] Stage 3: evdi_platform_drv - 模块入口和出口
   - [x] Stage 4: evdi_platform_dev - 设备管理
   - [x] Stage 5: evdi_sysfs - 用户空间接口

2. **第二周：DRM 基础**
   - [ ] Stage 6: evdi_drm_drv & evdi_drm.h - DRM 驱动注册
   - [ ] Stage 7: evdi_gem.c - GEM 内存管理（**重要基础**）
   - [ ] Stage 8: evdi_fb.c - 帧缓冲区管理

3. **第三周：显示组件**
   - [ ] Stage 9: evdi_encoder.c & evdi_connector.c - 显示组件
   - [ ] Stage 10: evdi_modeset.c - 模式设置和分辨率切换

4. **第四周：高级功能**
   - [ ] Stage 11: evdi_cursor.h/c - 光标管理
   - [ ] Stage 12: evdi_painter.c - 内核-用户空间通信（**最复杂**）
   - [ ] Stage 13: evdi_i2c, evdi_ioc32 - 辅助功能

### 学习优先级建议

**必须理解（核心）：**
1. ✅ evdi_platform_drv.c - 模块入口
2. ✅ evdi_sysfs.c - 用户接口
3. ⭐ evdi_gem.c - 内存管理基础
4. ⭐ evdi_painter.c - 通信机制
5. ⭐ evdi_modeset.c - 显示核心

**应该理解（重要）：**
1. evdi_drm_drv.c - DRM 驱动框架
2. evdi_fb.c - 帧缓冲区
3. evdi_connector.c - 显示器模拟
4. evdi_cursor.c - 光标处理

**了解即可（辅助）：**
1. evdi_encoder.c - 编码器（很简单）
2. evdi_i2c.c - I2C 模拟
3. evdi_ioc32.c - 兼容性支持
4. evdi_drm.h - API 定义

---

## 🎯 第三部分：实际学习建议

### 调试技巧

1. **查看内核日志：**
   ```bash
   dmesg | grep evdi
   # 或
   journalctl -k | grep evdi
   ```

2. **设置日志级别：**
   ```bash
   # 加载模块时设置
   modprobe evdi evdi_loglevel=6  # 6 = VERBOSE，最详细
   ```

3. **查看 sysfs 接口：**
   ```bash
   ls -la /sys/class/drm/evdi*/
   cat /sys/class/drm/evdi*/version
   ```

4. **使用 strace 跟踪系统调用：**
   ```bash
   strace -e ioctl your_program
   ```

### 推荐资源

1. **Linux 内核模块编程：**
   - 《Linux Device Drivers》第 3 版（免费在线版）
   - Kernel.org 官方文档

2. **DRM 子系统：**
   - `Documentation/gpu/drm-kms.rst`（内核源码中）
   - DRM 驱动示例代码

3. **平台设备模型：**
   - `Documentation/driver-api/platform.rst`

---

## 🔍 第四部分：代码阅读检查清单

阅读每个文件时，问自己：

- [ ] 这个文件的**主要作用**是什么？
- [ ] 它**依赖**哪些其他文件？
- [ ] 它**被哪些**其他文件使用？
- [ ] 关键的**数据结构**是什么？
- [ ] 关键的**函数**实现了什么功能？
- [ ] 这个文件在**整个系统**中的位置？

---

## 💡 常见问题

**Q: 为什么代码这么复杂？**
A: 内核模块需要与内核的多个子系统交互（平台设备、DRM、sysfs 等），每个子系统都有自己的接口和规范。

**Q: 如何知道某个函数什么时候被调用？**
A: 查找函数名在代码库中的使用位置。例如，`evdi_init` 通过 `module_init()` 注册，在模块加载时自动调用。

**Q: 如何调试内核模块？**
A: 使用 `printk`（通过 EVDI_DEBUG 宏）打印日志，使用 `dmesg` 查看。对于复杂问题，可以使用 `kgdb` 或 `kprobe`。

**Q: 为什么使用 `mutex`？**
A: 内核模块可能被多个进程/线程同时访问，需要使用锁来保护共享数据，防止竞态条件。

---

## 🚀 下一步

完成基础学习后，可以：
1. 尝试修改代码，添加新功能
2. 阅读用户空间库代码，理解完整的通信流程
3. 研究其他 DRM 驱动（如 `drm_fbdev`）的实现
4. 贡献代码或修复 bug

---

## 📋 完整文件清单

### 按学习阶段组织

| 阶段 | 文件 | 复杂度 | 状态 |
|------|------|--------|------|
| **Stage 1** | `Makefile` | ⭐ | ✅ 已完成 |
| **Stage 2** | `evdi_debug.h/c`, `evdi_params.h/c` | ⭐ | ✅ 已完成 |
| **Stage 3** | `evdi_platform_drv.h/c` | ⭐⭐⭐ | ✅ 已完成 |
| **Stage 4** | `evdi_platform_dev.h/c` | ⭐⭐ | ✅ 已完成 |
| **Stage 5** | `evdi_sysfs.h/c` | ⭐⭐ | ✅ 已完成 |
| **Stage 6** | `evdi_drm_drv.h/c`, `evdi_drm.h` | ⭐⭐⭐⭐ | ⬜ 待学习 |
| **Stage 7** | `evdi_gem.c` | ⭐⭐⭐⭐ | ⬜ 待学习 |
| **Stage 8** | `evdi_fb.c` | ⭐⭐⭐⭐ | ⬜ 待学习 |
| **Stage 9** | `evdi_encoder.c`, `evdi_connector.c` | ⭐⭐⭐ | ⬜ 待学习 |
| **Stage 10** | `evdi_modeset.c` | ⭐⭐⭐⭐ | ⬜ 待学习 |
| **Stage 11** | `evdi_cursor.h/c` | ⭐⭐⭐ | ⬜ 待学习 |
| **Stage 12** | `evdi_painter.c` | ⭐⭐⭐⭐⭐ | ⬜ 待学习 |
| **Stage 13** | `evdi_i2c.h/c`, `evdi_ioc32.c` | ⭐⭐ | ⬜ 待学习 |

### 按文件类型组织

**核心驱动文件：**
- `evdi_platform_drv.c` - 模块入口（Stage 3）
- `evdi_drm_drv.c` - DRM 驱动注册（Stage 6）
- `evdi_painter.c` - 通信机制（Stage 12）

**内存和缓冲区管理：**
- `evdi_gem.c` - GEM 内存管理（Stage 7）
- `evdi_fb.c` - 帧缓冲区（Stage 8）

**显示组件：**
- `evdi_connector.c` - 连接器/显示器（Stage 9）
- `evdi_encoder.c` - 编码器（Stage 9）
- `evdi_modeset.c` - 模式设置（Stage 10）
- `evdi_cursor.c` - 光标管理（Stage 11）

**设备管理：**
- `evdi_platform_dev.c` - 平台设备（Stage 4）
- `evdi_sysfs.c` - sysfs 接口（Stage 5）

**辅助功能：**
- `evdi_i2c.c` - I2C 模拟（Stage 13）
- `evdi_ioc32.c` - 32位兼容（Stage 13）
- `evdi_drm.h` - API 定义（Stage 6）

**基础工具：**
- `evdi_debug.h/c` - 日志系统（Stage 2）
- `evdi_params.h/c` - 模块参数（Stage 2）

---

祝你学习愉快！🎉

