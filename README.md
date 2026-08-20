# ScreenPlus for GL-BE3600

ScreenPlus is an open replacement for GL.iNet's official `gl_screen` service
on the GL-BE3600 (Slate 7). It drives the 284 × 76 LCD and touchscreen
directly, and exposes its pages, visible fields, theme, backgrounds and display
behaviour through LuCI.

## Current pages

- Home: left-aligned time, date and weekday beside a high-contrast accent rule.
- System: CPU utilisation/temperature, memory utilisation/used space and fan RPM.
- Traffic: upload above download, stable sampled rates and a 30-second graph.
- Network: bridge-side LAN address, all four WAN sources (Ethernet, Wi-Fi
  repeater, USB tethering and cellular), plus the active WAN address.
- Wi-Fi: separate 2.4 GHz and 5 GHz SSID/password rows; a disabled radio is
  shown as off.
- OpenClash: live upload/download rates, active connections and cumulative
  upload/download traffic.

The interface defaults to a high-contrast dark theme. Primary/secondary text,
accent/healthy, background, divider and missing/disabled/offline/fault colours
are independently configurable. Browser-cropped custom backgrounds apply
immediately and can be global or page-specific.

## Target

- GL.iNet GL-BE3600 / Slate 7
- GL.iNet OpenWrt 23.05-SNAPSHOT
- `aarch64_cortex-a53_neon-vfpv4`

Normal orientation is 90 degrees. The 270-degree flipped orientation remains a
user-facing option. Pages follow the finger during a swipe and use a short,
high-frame-rate settling animation after release.

## Packages

The project produces two packages:

- `screenplus_<version>-1_aarch64_cortex-a53_neon-vfpv4.ipk`
- `luci-app-screenplus_<version>-1_all.ipk`

Install the daemon first, followed by the LuCI application. Installing
ScreenPlus stops and disables `gl_screen` but does not remove it, so uninstall
can restore the official service. The package also registers `ScreenPlus` in
GL.iNet's native Toggle settings; the physical switch can move between
ScreenPlus and the official screen service. Existing prototype configurations
are migrated idempotently to schema v7 while preserving compatible page
enable/order settings and uploaded backgrounds.

On Qualcomm NSS builds, traffic is read from the default NSS data-plane
netdevice counters. This keeps ECM/PPE hardware acceleration enabled. Other
builds fall back to the same kernel netdevice statistics interface.

## Local development build

From PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap-toolchain.ps1
powershell -ExecutionPolicy Bypass -File scripts\build-prototype.ps1
powershell -ExecutionPolicy Bypass -File scripts\build-dev-ipk.ps1
```

The local prototype uses a pinned LVGL 9.5.0 revision and a project-local Zig
cross compiler. The feed package Makefiles under `package/` are provided for an
OpenWrt SDK build.

## Safety and privacy

- The Wi-Fi password is read from the active wireless UCI section only when it
  is rendered; it is not copied into ScreenPlus UCI or diagnostics.
- The OpenClash dashboard secret stays in process memory and is sent only to
  the loopback controller API.
- Background uploads accept only fixed page names and are converted to an
  exact 284 × 76 RGB565 asset.
- Runtime samples remain in RAM and are not written continuously to flash.

See [hardware notes](docs/HARDWARE.md) and the [implementation plan](docs/PLAN.md)
for verified device details and remaining release gates.
