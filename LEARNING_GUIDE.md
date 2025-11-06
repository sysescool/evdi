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

#### 文件 7：`evdi_drm_drv.c` ⭐⭐⭐⭐ 核心驱动

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

### 阶段 8：理解通信机制

#### 文件 9：`evdi_painter.c` ⭐⭐⭐⭐⭐ 最复杂但最重要

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

## 🎯 第三部分：实际学习建议

### 学习步骤

1. **第一周：基础概念**
   - 阅读 Makefile，理解构建流程
   - 理解 `evdi_debug.h` 的日志系统
   - 阅读 `evdi_platform_drv.c` 的 `evdi_init()` 和 `evdi_exit()`

2. **第二周：设备管理**
   - 理解 `evdi_platform_dev.c` 的设备创建流程
   - 理解 `evdi_sysfs.c` 的用户空间接口
   - 尝试通过 sysfs 添加/删除设备

3. **第三周：DRM 基础**
   - 学习 Linux DRM 子系统的基本概念
   - 理解 `evdi_drm_drv.c` 的驱动注册
   - 理解 `evdi_connector.c` 的连接器实现

4. **第四周：通信机制**
   - 深入理解 `evdi_painter.c` 的通信流程
   - 理解 ioctl 接口的使用
   - 查看用户空间库（`library/`）如何使用这些接口

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

祝你学习愉快！🎉

