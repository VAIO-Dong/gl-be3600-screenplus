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

`/etc/hotplug.d/button/99-screenplus-reset` records the press timestamp in
`/tmp/screenplus-reset-button`; the UI samples that tmpfs marker every 50 ms.
On release the marker changes to `released <pressed> <released>` so the UI can
show the result immediately. ScreenPlus removes it after the confirmation page.

Test the display stages without calling `/etc/rc.button/reset` and without
performing a real reset:

~~~sh
BUTTON=reset ACTION=pressed /etc/hotplug.d/button/99-screenplus-reset
sleep 1
BUTTON=reset ACTION=released /etc/hotplug.d/button/99-screenplus-reset
~~~

Capture at 0–3, 3–8, 8–20, and over 20 seconds, matching the vendor reset
script exactly. Releasing before 3 seconds must immediately show Reset
cancelled, count down for 3 seconds, then return to the same dashboard page.
The package must never change the vendor thresholds. The restore-only
`/usr/libexec/screenplus-reset-threshold` exists solely to undo the 16-second
change made by older ScreenPlus builds when their marker is present.
