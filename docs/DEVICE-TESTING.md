# Device testing and development notes

This document records repeatable GL-BE3600 screen tests and hardware-specific
pitfalls. Keep it updated when a bug reveals a new assumption about the panel,
touch controller, service lifecycle or GL.iNet integration.

## Touch controller calibration

The Hynitron CST816X driver reports both absolute axes as 0..240. Those ioctl
ranges do not describe the GL-BE3600 panel orientation and must not be passed
through LVGL unchanged. ScreenPlus keeps the axes unswapped and applies
calibration values 0, 0, 75, 283 for the normal orientation. The flipped
orientation reverses both axes with 75, 283, 0, 0.

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
powershell -ExecutionPolicy Bypass -File scripts\build-touch-tool.ps1
scp -O -i .dev\screenplus_dev_key build\screenplus-touch root@192.168.8.1:/tmp/
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
Swipe-screen and swipe-raw emit a 16-step drag, which makes page-direction and
mid-transition framebuffer tests repeatable instead of relying on one-off
`sendevent` sequences.
Use an empty part of a page for direction tests. Beginning on a clickable child
(for example the OpenClash switch) intentionally gives that control ownership
of the gesture.

## Framebuffer capture and private comparisons

Capture the current screen and convert RGB565 to PNG:

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts\capture-device-screen.ps1 -OutputPath build\device-screen.png
~~~

Compare logical regions without rendering sensitive Wi-Fi passwords:

~~~powershell
powershell -ExecutionPolicy Bypass -File scripts\compare-framebuffer-regions.ps1 -BeforePath build\wifi-before.fb -AfterPath build\wifi-after.fb
~~~

Regions use name=x0:y0:x1:y1. For example, switch=240:3:275:21 compares
only the visible OpenClash switch instead of a row containing live metrics.
The scripts support the Windows PowerShell 5.1 runtime shipped with Windows.

For a Wi-Fi row test, tapping 2.4 GHz should change only top; tapping 5 GHz
should change only bottom. Do not attach or publish a password-revealed PNG.

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
- The user plans to factory-reset the test router after this build. Its SSH host
  key and development authorization may change. On the next connection, verify
  the new fingerprint and refresh `.device_known_hosts`/the authorized key
  instead of trusting the currently recorded key.
- Stop the managed ScreenPlus service before running a standalone capture
  process, and always restore /etc/init.d/screenplus start after the test.
- Keep temporary captures under /tmp on the router to avoid flash writes.
- Verify the service with pgrep -af /usr/sbin/screenplus after package updates.
