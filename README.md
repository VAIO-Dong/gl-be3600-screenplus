# ScreenPlus for GL-BE3600

ScreenPlus is an open replacement for GL.iNet's official `gl_screen` service
on the GL-BE3600 (Slate 7). It drives the 284 × 76 LCD and touchscreen
directly, and exposes its pages, visible fields, theme, backgrounds and display
behaviour through LuCI.

## Current pages

- Home: centred time, date and weekday.
- System: CPU utilisation, memory and overlay storage.
- Network: both physical Ethernet ports plus Wi-Fi state.
- Wi-Fi: SSID and password; a disabled radio is shown as off.
- OpenClash: live upload/download rates, active connections and cumulative
  upload/download traffic.

The interface defaults to a high-contrast dark theme. Primary text, secondary
text, accent, background, card surface, inactive-border, warning and error
colours are all independently configurable. Each page can also use a browser-cropped custom
background.

## Target

- GL.iNet GL-BE3600 / Slate 7
- GL.iNet OpenWrt 23.05-SNAPSHOT
- `aarch64_cortex-a53_neon-vfpv4`

Normal orientation is 90 degrees. The 270-degree flipped orientation remains a
user-facing option. Instant page switching is the default for the smoothest
response; a short slide animation is optional.

## Packages

The project produces two packages:

- `screenplus_<version>-1_aarch64_cortex-a53_neon-vfpv4.ipk`
- `luci-app-screenplus_<version>-1_all.ipk`

Install the daemon first, followed by the LuCI application. Installing
ScreenPlus stops and disables `gl_screen` but does not remove it, so uninstall
can restore the official service. Existing prototype configurations are
migrated once to schema v2 while preserving page enable/order settings and
uploaded backgrounds.

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
