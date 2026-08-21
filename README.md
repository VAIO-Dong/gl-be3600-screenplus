# ScreenPlus for GL-BE3600

> 更适合中国宝宝体质的 BE3600 屏幕。

ScreenPlus 是为 GL.iNet GL-BE3600（Slate 7）做的一套开源屏幕服务。它直接接管设备自带的 284 × 76 彩色触摸屏，用更紧凑、更直观的方式展示路由器真正值得随手看一眼的信息：实时速率、连接数、设备状态、Wi-Fi、WAN/LAN 和 OpenClash。

它不是给官方屏幕“换个皮肤”，而是一套独立的原生服务；同时保留官方 `gl_screen`，随时可以切回去。

<p align="center">
  <img src="docs/images/home.png" width="568" alt="ScreenPlus 首页">
</p>

## 为什么做 ScreenPlus

路由器的屏幕不大，所以比起堆满信息，ScreenPlus 更在意三件事：

- 重要信息一眼就能看到，字体、间距和层级适合这块小屏幕。
- 实时网络速率不会为了统计而破坏 Qualcomm NSS/PPE 硬件加速。
- 页面不是写死的：顺序、内容、主题色和背景都可以在 LuCI 里调整。

ScreenPlus 优先读取 NSS 数据平面的网卡计数器；NSS 不可用时，再回退到标准内核 netdevice statistics。它不依赖关闭 flow offloading，也不需要把整机流量绕进软件转发路径。

## 页面一览

目前有六个页面，默认顺序是：首页、速率、系统状态、Wi-Fi、网络连接、OpenClash。页面可单独关闭，也可以在 LuCI 中调整顺序和显示字段。

### 首页

<p align="center">
  <img src="docs/images/home.png" width="568" alt="首页时间和日期">
</p>

只保留时间、日期和星期，简单干净。可以选择显示秒、时区，也可以翻转屏幕方向。

### 实时速率

<p align="center">
  <img src="docs/images/traffic.png" width="568" alt="实时网络速率和连接数">
</p>

固定位置显示上行、下行和实时连接数，右侧是最近 30 秒的流量趋势。数值变化不会挤动布局，也不会为了监控速率影响硬件加速。

### 系统状态

<p align="center">
  <img src="docs/images/status.png" width="568" alt="CPU 内存温度和风扇状态">
</p>

显示 CPU 占用与温度、内存占用与已用空间，以及风扇转速。

### Wi-Fi

显示 2.4 GHz 和 5 GHz 的 SSID、开关状态与密码。密码支持隐藏、点击显示、始终显示和二维码模式；慢速滑动不会误触发二维码。

> 为避免把真实 Wi-Fi 密码提交到仓库，README 不放这一页的设备截图。

### 网络连接

<p align="center">
  <img src="docs/images/network.png" width="568" alt="WAN LAN 和四种联网方式">
</p>

目前测试能正常显示以太网WAN, Wi-Fi Repeater, USB Tethering的速率显示。能显示实时的连接数和OpenClash信息，页面信息与主题色可配置，可以自定义每一页的背景，可以通过边上的拨动开关实现官方屏幕和ScreenPlus的一键切换。

页面会同时展示四种 WAN 来源的状态：

- Ethernet
- Wi-Fi Repeater
- USB Tethering
- Cellular

下方显示当前 WAN 类型、WAN IP 和 LAN IP。颜色含义保持统一：灰色是不存在或未连接，蓝色是存在但未启用，黄色是已启用但无法联网，主题色表示连接正常。

### OpenClash

<p align="center">
  <img src="docs/images/openclash.png" width="568" alt="OpenClash 状态和实时信息">
</p>

显示 OpenClash 状态、实时上下行速率、累计流量、连接数、CPU 和内存占用。右上角可以直接开关 OpenClash；启动或停止过程中会显示明确的“启动中 / 停止中”状态，并暂时锁定开关，避免重复操作。

## LuCI 配置

安装后进入：

`系统 → ScreenPlus`

可以配置：

- ScreenPlus 开关、语言、亮度、息屏时间和屏幕方向
- 页面循环、自动轮播与滑动过渡
- Wi-Fi 密码展示方式
- 六个页面的顺序、启用状态和显示字段
- 主题色、主色、次要色、背景色、分割线和状态颜色
- 全局背景，或者每一页独立的背景图

背景图在浏览器里居中裁切为 284 × 76，再转换成固定大小的 RGB565 文件。没有开放任意服务器路径，上传后立即应用；未设置时也不会显示坏掉的图片占位。

## 官方屏幕一键切换

安装时 ScreenPlus 会停止并禁用官方 `gl_screen`，但不会删除它。

项目会把 ScreenPlus 加入 GL.iNet 原生 Toggle 设置。将机身侧面的拨动开关分配给 ScreenPlus 后，就可以在官方屏幕和 ScreenPlus 之间一键切换。卸载 ScreenPlus 时会自动恢复并启动官方屏幕服务。

## 安装

### 适用环境

- GL.iNet GL-BE3600 / Slate 7
- GL.iNet OpenWrt 23.05-SNAPSHOT
- 架构：`aarch64_cortex-a53_neon-vfpv4`

前往 [Releases](https://github.com/VAIO-Dong/gl-be3600-screenplus/releases) 下载最新的单一 IPK，然后直接通过网页安装：

1. 登录 GL.iNet 官方管理页面，进入 **系统 → 高级设置**。
2. 点击进入 LuCI 高级管理页面。
3. 在 LuCI 中进入 **系统 → 软件包**。
4. 点击 **上传软件包…**，选择下载好的
   `screenplus_<version>-1_aarch64_cortex-a53_neon-vfpv4.ipk`。
5. 上传完成后确认安装，等待页面提示安装成功。
6. 刷新 LuCI，在 **系统 → ScreenPlus** 中调整页面和显示设置。

一个 IPK 已经包含屏幕服务、LuCI 页面、菜单和 RPC 权限，不需要 SSH，也不需要再单独安装 `luci-app-screenplus`。

> 项目目前主要在 GL-BE3600 原厂固件上开发和实机验证。升级或安装前，建议保留配置备份。

## 工作方式

ScreenPlus 是 C11 + LVGL 9.5.0 的原生程序，由 procd 管理：

- 直接输出到 `/dev/fb0`
- 直接读取 `/dev/input/event0`
- 通过 sysfs 控制背光
- CPU、内存来自 `/proc`
- 温度和风扇来自 thermal/hwmon sysfs
- 网络速率来自 NSS 数据面计数器或内核网卡计数器
- WAN/LAN、Wi-Fi 与页面配置来自 UCI
- OpenClash 信息来自本机进程和 loopback controller API

指标采集在后台线程中进行，界面线程只应用最新快照；滑动期间会暂停非必要的数据刷新，尽量让这块小屏幕保持跟手。

## 本地构建

Windows PowerShell：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-toolchain.ps1
powershell -ExecutionPolicy Bypass -File scripts\build-prototype.ps1
powershell -ExecutionPolicy Bypass -File scripts\build-dev-ipk.ps1
```

生成结果：

```text
dist/screenplus_<version>-1_aarch64_cortex-a53_neon-vfpv4.ipk
```

本地构建使用项目固定的 LVGL 9.5.0 和 Zig ARM64/musl 交叉编译器。OpenWrt SDK 使用 [package/screenplus/Makefile](package/screenplus/Makefile)。

## 调试与贡献

仓库提供了可复用的设备工具：

- 触摸坐标监控、逻辑点击和滑动注入
- framebuffer 抓取与 RGB565 转 PNG
- 局部区域截图对比
- 字体覆盖检查和硬件测试程序

详细方法见 [设备测试文档](docs/DEVICE-TESTING.md) 和 [硬件记录](docs/HARDWARE.md)。如果你在其他固件版本、不同网络接入方式或不同 OpenClash 版本上测试，欢迎提交 Issue 或 PR。

## 隐私与安全

- Wi-Fi 密码只在渲染对应页面时读取，不写入 ScreenPlus 配置或诊断信息。
- OpenClash secret 只保留在进程内存，并且只访问回环地址。
- 背景上传只接受固定页面名和固定尺寸的 RGB565 资源。
- 实时采样数据保留在内存，不持续写入闪存。

## License

[MIT License](LICENSE)
