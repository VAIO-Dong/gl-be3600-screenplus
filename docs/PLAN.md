# ScreenPlus implementation plan

## Product scope

ScreenPlus replaces `gl_screen` on the GL-BE3600 and owns `/dev/fb0`,
`/dev/input/event0` and the display backlight through one procd-supervised native
process. The official package remains installed for rollback.

The screen model has six independently enabled and ordered pages:

1. Home: time, optional seconds, date, weekday and timezone.
2. System: CPU utilisation/temperature, memory utilisation/used space and fan
   RPM.
3. Traffic: fixed-position icon-led rate fields plus a 30-point/30-second
   history graph; acceleration status remains available in diagnostics.
4. Network: bridge-side LAN address, Ethernet/Wi-Fi repeater/USB
   tethering/cellular WAN states and the active WAN address. Ethernet state is
   carrier-aware: no cable inherits secondary text, link without uplink is
   yellow, and the healthy active uplink is green.
5. Wi-Fi: separate 2.4/5 GHz SSID and password rows, with hidden, tap, visible
   and QR policies.
6. OpenClash: a three-row grid with a top-right asynchronous service switch,
   equal-width state, compact merged rate/total, connection, CPU and memory
   cells.

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
- OpenClash: init/UCI state, `/proc` CPU/RSS and the loopback Clash
  `/connections` API. Its dashboard secret is neither logged nor passed on a
  command line.

The collector normalises Ethernet, Wi-Fi repeater, USB tethering and cellular
status for both the compact network page and diagnostics.

### LuCI and UCI

- General: service, language, brightness, orientation, backlight timeout,
  navigation, transition and password policy.
- Pages: one central order section followed by per-page visibility and field
  selection.
- Appearance: theme, primary, secondary, background and divider palette plus
  available-but-disabled/offline/fault state colours. Healthy states inherit
  theme and missing connections inherit secondary.
  Global/per-page background uploads apply immediately, with previews shown
  only for installed images.
- Diagnostics: service/hardware state, live metrics, connectivity/OpenClash
  snapshot and bounded logs.
- Idempotent schema-v10 migration preserves compatible settings and renames
  legacy page/background assets.
- Native GL.iNet Toggle discovery through `/etc/gl-switch.d/screenplus.sh`,
  with ScreenPlus-aware ON/OFF labels, a firmware-checked frontend patch and
  deterministic hand-off to/from the retained official screen service.

## Visual rules

- Default text colours are white or near-white; muted low-contrast gray is not
  used for operational data.
- Dividers use a deep neutral gray and are at least 2 physical pixels wide.
- Compact pages use a 14 px baseline for labels, values and details. Only a few
  high-priority readings, such as system metrics and live traffic rates, use
  18 px. Dense network data uses full-width rows rather than narrow cards so
  complete IP addresses remain legible.
- Theme colour marks headings, enabled controls, download graphs and healthy
  states. Primary colour carries high-priority content; secondary colour carries
  supporting content, upload graphs and missing states. Divider colour is
  limited to separators and inactive control tracks.
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
