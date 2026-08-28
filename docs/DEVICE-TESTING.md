# Device testing and development notes

This document records repeatable GL-BE3600 screen tests and hardware-specific
pitfalls. Keep it updated when a bug reveals a new assumption about the panel,
touch controller, service lifecycle or GL.iNet integration.

## Touch controller calibration

The Hynitron CST816X driver reports both absolute axes as 0..240. Those ioctl
ranges do not describe the GL-BE3600 panel orientation and must not be passed
through LVGL unchanged. ScreenPlus keeps the axes unswapped and applies
calibration values 0, 0, 75, 283 for the normal orientation. The flipped
orientation uses the same calibration. LVGL rotates input points together with
the display; reversing the calibration a second time makes flipped gestures
move opposite to the page animation.

Without this correction, the 76-pixel axis is compressed into the top portion
of the UI. Symptoms include both Wi-Fi rows selecting 2.4 GHz and touches below
the OpenClash switch being interpreted as switch touches.

The vendor swap/calibration rotates motion by 90 degrees: physical up/down
movement becomes left/right page movement. Do not copy that mapping into
ScreenPlus. Keeping the axes unswapped is the required counter-clockwise
correction and lets `page_drag_event()` use the normal point delta so pages
follow the finger.

Do not validate a touch fix by injecting coordinates derived from the same
uncalibrated mapping. That creates a self-consistent synthetic test while real
finger input remains wrong. Test with the official calibration and, when a new
panel revision is suspected, use the monitor command to record physical taps.

## Reusable touch tool

Build and upload the static AArch64 helper:

~~~powershell
$device = 'user@router-address'
$identityFile = 'path\to\private-key'
$knownHostsFile = 'path\to\known-hosts'
powershell -ExecutionPolicy Bypass -File scripts\build-touch-tool.ps1
scp -O -i $identityFile -o "UserKnownHostsFile=$knownHostsFile" `
    -o StrictHostKeyChecking=yes build\screenplus-touch "${device}:/tmp/"
~~~

Useful device commands:

~~~sh
/tmp/screenplus-touch info
/tmp/screenplus-touch monitor
/tmp/screenplus-touch tap-screen 100 55 90
/tmp/screenplus-touch tap-raw 48 75
/tmp/screenplus-touch swipe-screen 230 38 50 38 250 90
/tmp/screenplus-touch swipe-raw 15 140 60 140 250
~~~

Monitor prints raw coordinates and their logical 90/270-degree mappings.
Tap-screen accepts ScreenPlus coordinates and converts them with the same
swap/calibration rules used by the daemon.
Swipe-screen and swipe-raw attempt to emit a 16-step drag. On the current
factory-reset firmware, writing evdev input records can return success without
the CST816X driver looping those records back to other readers. Always confirm
the framebuffer actually moved; a zero tool exit status alone is not a valid
touch result. Use a physical swipe when validating user touch behaviour.
Use an empty part of a page for direction tests. Beginning on a clickable child
(for example the OpenClash switch) intentionally gives that control ownership
of the gesture.

For a repeatable daemon-level regression, stop the managed service and pass a
named FIFO as `--input`. Keep the FIFO open while ScreenPlus initializes, then
use `screenplus-touch ... /tmp/screenplus-test-input` to emit a timed drag. This
exercises LVGL's evdev calibration, display rotation and page-drag code without
depending on the CST816X driver to loop injected records back. The 270-degree
regression starts on Home and sends a logical left swipe from x=230 to x=50; it
must settle on Traffic, not wrap backwards to OpenClash. Always remove the FIFO
and restore the managed service after the capture.

## Framebuffer capture and private comparisons

Capture the current screen and convert RGB565 to PNG:

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts\capture-device-screen.ps1 `
    -Device $device -IdentityFile $identityFile -KnownHostsFile $knownHostsFile `
    -OutputPath build\device-screen.png
~~~

`Device` is required. `IdentityFile` and `KnownHostsFile` are optional when the
SSH agent and the user's normal known-hosts database are used. Relative file
paths are resolved from the repository root.

Compare logical regions without rendering sensitive Wi-Fi passwords:

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts\compare-framebuffer-regions.ps1 -BeforePath build\wifi-before.fb -AfterPath build\wifi-after.fb
~~~

Regions use name=x0:y0:x1:y1. For example, switch=240:3:275:21 compares
only the visible OpenClash switch instead of a row containing live metrics.
The scripts support the Windows PowerShell 5.1 runtime shipped with Windows.

For a Wi-Fi row test, tapping 2.4 GHz should change only top; tapping 5 GHz
should change only bottom. Do not attach or publish a password-revealed PNG.

## Frame pacing and swipe performance

Confirm the panel driver's real transfer limit before changing LVGL refresh
timing:

~~~sh
dmesg | grep 'frame buffer'
~~~

The confirmed GL-BE3600 driver line reports a 76 x 284 framebuffer, 42 KiB of
video memory, a 4 KiB transfer buffer, `fps=50`, and SPI at 50 MHz. Do not use
that log line or `msync(MS_SYNC)` latency as proof of physical refresh rate.
`msync` does not wait for this driver's deferred SPI transfer.

Build the hardware probe and stop the managed display service before paced
tests:

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts\build-hwtest.ps1
~~~

~~~sh
/tmp/screenplus-hwtest benchmark 60
/tmp/screenplus-hwtest paced 20 100 pwrite
/tmp/screenplus-hwtest synchronised 60
/tmp/screenplus-hwtest vsync 3
~~~

A frame contains 43,168 bytes. Its ideal 50 MHz SPI wire time is 6.907 ms, but
the operating limit must include driver scheduling and transfer completion.
Count the `78b5000.spi`, relevant `bam_dma`, and `TE_GPIO` interrupt deltas
around a paced test. On the confirmed firmware, 100 submissions at 20 ms
produced 100 panel updates; `FBIO_WAITFORVSYNC` is unsupported (`ENOTTY`). TE
only becomes active around transfers and must not be interpreted as a
free-running panel refresh counter in isolation.

The synchronised test waits for five SPI completion interrupts before writing
the next framebuffer image. The confirmed device averaged 16.328 ms per frame,
with a 16.971 ms P95 and 18.378 ms maximum, for an overlap-free measured ceiling
of about 61 FPS. Use this test rather than the probe log when changing pacing.

Page dragging begins after 6 logical horizontal pixels, reduced from the old
12-pixel threshold. Use the FIFO input method for repeatable 5-8 pixel boundary
tests, then confirm with a real finger that taps still activate controls without
accidentally starting a page drag.

Do not use an mmap framebuffer write path for the daemon. Under page pressure,
`fb_deferred_io_mkwrite` blocked the LVGL thread for 216-512 ms and made rows
appear progressively. In the controlled 20 ms test, mmap writes had a 16.6 ms
P95 and a 388 ms maximum, while a coherent full-frame `pwrite` had a 0.17 ms
P95 and a 0.26 ms maximum. ScreenPlus uses two full LVGL render buffers plus a
two-slot latest-frame queue. The display thread rotates and submits one complete
native frame, then waits for its fifth SPI interrupt before submitting another.
Keep the 16 ms LVGL request period so rendering can feed the measured roughly
60 Hz safe ceiling. Sample touch independently at 10 ms.

For an A/B test, use a `/tmp` standalone binary and follow the service stop and
restore rules below. During a real-finger swipe, sample the ScreenPlus process
with `top`. Low CPU together with intermittent `D` state indicates waiting in
the framebuffer/SPI path rather than a software renderer that needs more CPU.
Match the vendor service's `nice -20` priority, but do not treat priority as a
fix for framebuffer waits. Confirm lack of visible tearing plus both normal
and 270-degree orientations with physical swipes because synthetic input does
not validate touch direction or subjective frame pacing.

## Screen schedule

The weekly schedule controls only the backlight and is active only when both
`always_on` and `schedule_enabled` are set. With `always_on=0`, confirm that the
existing inactivity timeout remains authoritative even if stale schedule
options are present. With scheduling active, verify one interval containing
the current local minute leaves the configured brightness in sysfs, and one
interval excluding it writes zero. Restore the original UCI values and confirm
the managed service after testing.

Test a same-day interval, an interval crossing midnight, and a custom interval
carried over from the previous day. Equal on/off times intentionally mean all
day on only when that row is enabled. A disabled row means all day off. In
LuCI, confirm Every day shows one compact range, Weekdays and weekends shows
two ranges with independent enable switches, and Custom days shows seven
compact rows. Copy to all other days must update the six unsaved enable states
and time pairs before Apply is pressed. The schedule controls belong to the
same Display and controls section immediately below Keep the screen on; with
that flag disabled, no schedule heading or option may remain visible. Confirm
that each range uses native minute-precision time controls.

Regression-test the two common configurations directly: Monday through Friday
09:00-19:00 with the weekend row disabled, and every day 08:00-00:00. The first
must stay off all Saturday and Sunday; the second must turn off at midnight and
remain off until 08:00. Also test a disabled custom day and a previous day's
cross-midnight interval extending into it.

With LuCI set to Simplified Chinese, reload all four ScreenPlus views and check
their headings, option labels, descriptions, action buttons, notifications and
menu entries. The package supplies `screenplus.zh-cn.lmo`; validate at least one
ScreenPlus-specific sentence rather than relying on translations already
provided by `base.zh-cn.lmo`. When a view changes, continue to version all four
resource names together as described below.

## LVGL event ownership

- Bind each Wi-Fi row's band index through callback user data. Do not infer the
  band from the current event target or a display-coordinate split.
- Child labels are non-clickable so their row owns the press. Row events bubble
  to the page so swipe tracking can cancel long presses.
- A page drag resets the input device's long-press timer as soon as movement is
  accepted. This prevents a slow swipe from opening a QR code.
- The OpenClash switch does not bubble pointer or gesture events. Its clickable
  object is exactly the visible 36 x 19 switch at the top right.

## Device lifecycle and transport

- OpenWrt on the test device has no SFTP server; use scp -O for legacy SCP.
- After a factory reset or device replacement, verify the SSH host fingerprint
  again and refresh the selected known-hosts entry and authorized key.
- Stop the managed ScreenPlus service before running a standalone capture
  process, and always restore /etc/init.d/screenplus start after the test.
- Keep temporary captures under /tmp on the router to avoid flash writes.
- Verify the service with pgrep -af /usr/sbin/screenplus after package updates.

## LuCI cache invalidation

The GL-BE3600 LuCI build appends its own LuCI revision to view resources, not
the ScreenPlus package version. A browser can therefore retain an obsolete
ScreenPlus view across package upgrades and even across a router factory reset.
When a release changes a view, give all four ScreenPlus view files a new
versioned resource name and update `luci-app-screenplus.json`. Clearing only
`/tmp/luci-indexcache` does not invalidate a browser's existing static asset.

This firmware stores its dispatcher index as a hash-suffixed file such as
`/tmp/luci-indexcache.7fd20528.json`. Package lifecycle scripts must remove
`/tmp/luci-indexcache*`; deleting only the unsuffixed path leaves the old menu
action active even when the new view file was installed correctly.

Verify the requested resource in the browser network log. For v1.1.0 the Pages
view must be `screenplus/pages-v110.js`, never the unversioned `pages.js`.

## Responsive layout and configuration reload

Visible fields are layout inputs, not merely visibility flags. Test at least
one representative of each reduced layout: a single centred status metric,
rates without a chart, a full-width chart, one centred Wi-Fi band, a two-row
network page, and OpenClash with only its traffic or summary row. Dividers must
exist only between visible groups.

LuCI apply must signal the existing procd instance instead of stop/start.
Before the apply, record the current ScreenPlus PID and select a non-home page.
Change both that page's order and one visible field, then save and apply. The
PID must remain unchanged and the same page ID must remain visible at its new
order. If the current page itself is disabled, ScreenPlus should retain its
numeric position and display the adjacent page rather than jumping home.

The procd instance runs `/usr/libexec/screenplus-run`. In enabled mode it execs
the dashboard in place; in disabled mode the same PID waits while `gl_screen`
owns the display. A UCI reload must switch both directions without stop/start:
enabled changes from `1` to `0` should show the wrapper plus `gl_screen`, and a
change back to `1` should leave only `/usr/sbin/screenplus`. This stable entry
point is what lets normal configuration changes retain the current page.

## Reset-button overlay

`/usr/libexec/screenplus-reset-hook` adds an idempotent, reversible call at the
start of the vendor `/etc/rc.button/reset` handler. That early call runs
`/etc/hotplug.d/button/99-screenplus-reset`, which records the press timestamp
in `/tmp/screenplus-reset-button`; the UI samples that tmpfs marker every 50 ms.
On release the marker changes to `released <pressed> <released>` before the
vendor handler waits for its screen service, so ScreenPlus can show the result
immediately. ScreenPlus removes the marker after the three-second result page.
The hook does not change the vendor reset actions or their 3/8/20-second
thresholds, and uninstalling the package removes only the marked hook block.

Test the display stages without calling `/etc/rc.button/reset` and without
performing a real reset:

~~~sh
BUTTON=reset ACTION=pressed /etc/hotplug.d/button/99-screenplus-reset
sleep 1
BUTTON=reset ACTION=released /etc/hotplug.d/button/99-screenplus-reset
~~~

Capture at 0–3, 3–8, 8–20, and over 20 seconds, matching the vendor reset
script exactly. Releasing in any interval must immediately show the selected
result. A release before 3 seconds must show Reset cancelled, count down for
3 seconds, then return to the same dashboard page.
The package must never change the vendor thresholds. The restore-only
`/usr/libexec/screenplus-reset-threshold` exists solely to undo the 16-second
change made by older ScreenPlus builds when their marker is present.
