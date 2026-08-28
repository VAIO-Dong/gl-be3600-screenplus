# ScreenPlus for GL-BE3600

ScreenPlus 是为 GL.iNet GL-BE3600（Slate 7）做的一套开源屏幕服务。直接接管设备自带的 284 × 76 彩色触摸屏，用更紧凑、更直观的方式展示路由器真正值得随手看一眼的信息：实时速率、连接数、设备状态、Wi-Fi、WAN/LAN 和 OpenClash。速率显示与硬件加速不冲突，同时还支持屏幕翻转，壁挂安装时也能正常操作。

<p align="center">
  <img src="docs/images/home.png" width="568" alt="ScreenPlus 首页">
</p>

## 页面一览

目前有六个页面，默认顺序是：首页、速率、系统状态、Wi-Fi、网络连接、OpenClash。页面可单独关闭，也可以在 LuCI 中调整顺序和显示字段。

### 首页

<p align="center">
  <img src="docs/images/home.png" width="568" alt="首页时间和日期">
</p>

只保留时间、日期和星期，简单干净。可以选择显示秒、时区。

### 实时速率

<p align="center">
  <img src="docs/images/traffic.png" width="568" alt="实时网络速率和连接数">
</p>

固定位置显示上行、下行和实时连接数，右侧是最近 30 秒的流量趋势。

### 系统状态

<p align="center">
  <img src="docs/images/status.png" width="568" alt="CPU 内存温度和风扇状态">
</p>

显示 CPU 占用与温度、内存占用与已用空间，以及风扇转速。

### Wi-Fi

显示 2.4 GHz 和 5 GHz 的 SSID、开关状态与密码。密码支持隐藏、点击显示、始终显示和二维码模式。

### 网络连接

<p align="center">
  <img src="docs/images/network.png" width="568" alt="WAN LAN 和四种联网方式">
</p>

页面会同时展示四种 WAN 来源的状态：

- Ethernet
- Wi-Fi Repeater
- USB Tethering
- Cellular

与当前所使用的WAN侧连接方式与LAN的IP地址

### OpenClash

<p align="center">
  <img src="docs/images/openclash.png" width="568" alt="OpenClash 状态和实时信息">
</p>

显示 OpenClash 状态、实时上下行速率、累计流量、连接数、CPU 和内存占用。右上角可以直接开关 OpenClash。

## LuCI 配置

安装后进入：

`系统 → ScreenPlus`

可以配置：

- LuCI 管理页面提供完整简体中文界面
- ScreenPlus 开关、语言、亮度、息屏时间和屏幕方向
- 页面循环、自动轮播与滑动过渡
- 在屏幕常亮模式下按每天相同、工作日/周末或一周七天自定义时间表自动开关屏幕；指定日期可以全天关闭
- Wi-Fi 密码展示方式
- 六个页面的顺序、启用状态和显示字段
- 主题色、主色、次要色、背景色、分割线和状态颜色
- 全局背景，或者每一页独立的背景图

背景图在浏览器里居中裁切为 284 × 76，再转换成固定大小的 RGB565 文件，上传后立即应用。全局背景固定在页面内容下方，切页时只有内容移动；每页独立背景则和对应页面一起移动。

## 官方屏幕一键切换

安装时 ScreenPlus 会停止并禁用官方 `gl_screen`，但不会删除它。

项目会把 ScreenPlus 加入 GL.iNet 原生 Toggle 设置。将机身侧面的拨动开关分配给 ScreenPlus 后，就可以在官方屏幕和 ScreenPlus 之间一键切换。卸载 ScreenPlus 时会自动恢复并启动官方屏幕服务。

## 安装

### 适用环境

- GL.iNet GL-BE3600 / Slate 7
- GL.iNet OpenWrt 23.05-SNAPSHOT
- 架构：`aarch64_cortex-a53_neon-vfpv4`

前往 [Releases](https://github.com/VAIO-Dong/gl-be3600-screenplus/releases) 下载最新的单一 IPK，在luci的系统-软件包里面安装，或者是上传到路由器之后通过SSH指令安装：

```sh
opkg install screenplus_<version>-1_aarch64_cortex-a53_neon-vfpv4.ipk
```

> 当前项目只在 GL-BE3600 原厂固件上开发和做了一些实机验证。第三方固件没有测试过，建议安装前先保留好路由配置的备份，以防万一。

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

本地构建使用项目固定的 LVGL 9.5.0 和 Zig ARM64/musl 交叉编译器，两者均由 `scripts\bootstrap-toolchain.ps1` 自动下载（重复运行会跳过已装部分）。如果 GitHub / ziglang.org 无法直连，可给脚本追加 SOCKS5 代理参数：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-toolchain.ps1 --socks-proxy 127.0.0.1:1080
```

OpenWrt SDK 使用 [package/screenplus/Makefile](package/screenplus/Makefile)。

## 调试与贡献

仓库提供了可复用的设备工具：

- 触摸坐标监控、逻辑点击和滑动注入
- framebuffer 抓取与 RGB565 转 PNG
- 局部区域截图对比
- 字体覆盖检查和硬件测试程序

详细方法见 [设备测试文档](docs/DEVICE-TESTING.md) 和 [硬件记录](docs/HARDWARE.md)。如果你在其他固件版本、不同网络接入方式或不同 OpenClash 版本上测试，欢迎提交 Issue 或 PR。

## License

[MIT License](LICENSE)
