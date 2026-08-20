# ScreenPlus implementation plan

## Product scope

ScreenPlus replaces `gl_screen` on the GL-BE3600 and owns `/dev/fb0`,
`/dev/input/event0` and the display backlight through one procd-supervised native
process. The official package remains installed for rollback.

The screen model has six independently enabled and ordered pages:

1. Home: time, optional seconds, date, weekday and timezone.
2. System: CPU utilisation/temperature, memory utilisation/used space and fan
   RPM.
3. Traffic: sampled live rates, NSS/SW/off status plus a 60-point/30-second
   history graph.
4. Network: `eth0`, `eth1` and combined 2.4/5 GHz radio state.
5. Wi-Fi: separate 2.4/5 GHz SSID and password rows, with hidden, tap, visible
   and QR policies.
6. OpenClash: state, current rates, connection count and cumulative traffic.

## Implemented architecture

### Native daemon

- C11 and LVGL 9.5.0, pinned by commit.
- 284 × 76 logical RGB565 design space rotated onto the native 76 × 284
  framebuffer.
- Two full-capacity partial-render buffers, a roughly 60 Hz refresh period and
  change-only label updates.
- Direct finger-following page translation that redraws only the current and
  adjacent page, with a 70-180 ms ease-out settle,
  optional page loop/carousel, idle backlight control and 90/270-degree
  orientation.
- Asynchronous bounded collection so OpenClash and UCI probes do not block the
  drawing loop.
- Safe UCI parser with defaults for missing or malformed settings.

### Data sources

- CPU/memory: `/proc`; temperature and fan RPM: thermal/hwmon sysfs.
- Aggregate WAN traffic: NSS data-plane netdevice counters when `qca_nss_dp`
  is active, with kernel netdevice counters as the portable fallback.
- Ethernet: UCI network role mapping plus carrier, negotiated speed and IPv4.
- Wi-Fi: active AP sections from UCI and interface state.
- OpenClash: init/UCI state and the loopback Clash `/connections` API. Its
  dashboard secret is neither logged nor passed on a command line.

The collector still normalises repeater, USB tethering and cellular status for
diagnostics, although the streamlined v2 display no longer dedicates screen
slots to those sources.

### LuCI and UCI

- General: service, language, brightness, orientation, backlight timeout,
  navigation, transition and password policy.
- Pages: enable/order and field selection for every page.
- Appearance: primary, secondary, accent, background, surface, border, warning
  and error colours; per-page background upload/remove.
- Diagnostics: service/hardware state, live metrics, connectivity/OpenClash
  snapshot and bounded logs.
- Idempotent schema-v3 migration preserves compatible settings and renames
  legacy page/background assets.
- Native GL.iNet Toggle discovery through `/etc/gl-switch.d/ScreenPlus.sh`,
  with deterministic hand-off to/from the retained official screen service.

## Visual rules

- Default text colours are white or near-white; muted low-contrast gray is not
  used for operational data.
- Borders and decorative rules are at least 2 physical pixels wide.
- The smallest active UI font is 14 px. Dense network data uses full-width rows
  rather than narrow cards so complete IP addresses remain legible.
- Custom backgrounds receive a configurable dark overlay.

## Packaging

- `screenplus`: target-specific native daemon, init script, UCI defaults and
  constrained helpers.
- `luci-app-screenplus`: architecture-independent LuCI views, menu and ACL.
- Feed-compatible OpenWrt Makefiles plus a local reproducible development IPK
  builder.
- Install disables `gl_screen`; uninstall restores it when configured.

## Verified gates

- Framebuffer geometry, RGB565 format, rotation and backlight verified on real
  GL-BE3600 hardware.
- Six-page rendering captured from the real framebuffer.
- System timezone, thermal sensor, fan tachometer and NSS data-plane counters
  verified on real hardware.
- WAN/LAN role, carrier, speed and IPv4 verified on both physical ports.
- Wi-Fi AP state/SSID/password source verified without diagnostic disclosure.
- OpenClash state, controller authentication, cumulative traffic and connection
  count verified against a running installation.
- Chinese glyph coverage checked at build time.

## Remaining release gates

- Verify LuCI save/reload and all colour/background controls in a browser.
- Complete subjective real-touch swipe/frame-pacing acceptance testing.
- Test Wi-Fi disabled, OpenClash stopped/absent and cable transition cases.
- Reboot test, package uninstall/rollback test and a long-running stability
  soak.
- Build in a matching OpenWrt SDK and publish signed release artifacts.
