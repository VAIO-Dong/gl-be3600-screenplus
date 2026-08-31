# GL-BE3600 hardware notes

## Confirmed interfaces

| Function | Interface | Observed value |
| --- | --- | --- |
| Framebuffer | `/dev/fb0` | `fb_st7789p3` |
| Native framebuffer | sysfs/ioctl | 76 x 284, 16-bpp RGB565 |
| Stride | sysfs/ioctl | 152 bytes |
| Panel transfer | synchronised write test | about 60 Hz, 50 MHz SPI, 4 KiB transfer buffer |
| Touch | `/dev/input/event0` | Hynitron CST816X |
| Backlight | `/sys/class/backlight/soc:backlight` | 0-11, observed 5 |
| WAN/LAN NICs | `eth0`, `eth1` | both 10/100/1000/2500 Mbps |
| CPU thermal zones | `/sys/class/thermal/thermal_zone*` | about 68-71 °C observed |
| Fan tachometer | `/sys/class/hwmon/*/fan1_input` | `pwmfan`, about 1269 RPM observed |
| NSS data plane | `qca_nss_dp` netdevice statistics | hardware-accelerated `eth0` counters |

In GL access point mode, the default route uses `br-lan`, while accelerated
forwarded traffic continues to advance the physical bridge-port counters. The
retained `network.wan.device` identifies the upstream member on the confirmed
firmware (`eth0`); verify its membership before using it rather than treating
the bridge's host counters as total traffic.

The LCD is mounted as a landscape touchscreen while the kernel framebuffer is
reported in portrait scan order. Visual hardware testing confirmed that the
normal logical landscape orientation is the 90-degree mapping:

`native_x = logical_y`, `native_y = 283 - logical_x`.

The 270-degree mapping is a valid flipped orientation and remains available as
a LuCI option. It is not only a development-test setting. Touch keeps the same
native-panel calibration for both orientations so LVGL can rotate the input and
display together.

## Hardware probe pattern

The test pattern has these logical landmarks:

- top-left: red;
- top-right: green;
- bottom-left: blue;
- bottom-right: yellow;
- centre: white arrow pointing right.

The probe saves the existing framebuffer before drawing and restores it after
the requested duration or when interrupted by SIGINT/SIGTERM.

## Bring-up record

- Static ARM64/musl probe executed successfully on the target.
- Framebuffer ioctl values match sysfs.
- One native RGB565 frame is 43,168 bytes (345,344 bits). At 50 MHz its ideal
  SPI wire time is 6.907 ms and its bandwidth-only ceiling is 144.78 FPS;
  commands, chunking, synchronisation and driver pacing lower that ceiling.
- Device-tree `fps` is 40 while the probe log prints 50. With this kernel's
  100 Hz timer both settings resolve to an approximately 20 ms deferred-I/O
  interval, so neither value alone proves the physical frame rate.
- Paced full-frame writes completed 100 distinct transfers in 1.98 seconds.
  SPI, BAM DMA and TE counters all advanced consistently, confirming that
  50 Hz is real and not merely a driver log value.
- A stricter test submitted the next frame only after the fifth SPI completion
  interrupt for the previous frame. Across 60 frames, completion averaged
  16.328 ms, with a 16.971 ms P95 and 18.378 ms maximum. This establishes an
  overlap-free measured ceiling of about 61 FPS; the 50 FPS probe message is
  conservative rather than the physical limit.
- The SPI controller already uses its QUP/BAM TX DMA path. A 50-frame test
  advanced the relevant BAM DMA interrupt by 550 and the SPI interrupt by
  250, so enabling DMA in ScreenPlus is neither necessary nor possible at the
  application layer.
- The standard `FBIO_WAITFORVSYNC` ioctl returns `ENOTTY`. TE is owned by the
  panel driver; applications must use coherent full-frame submission instead
  of attempting a separate userspace vsync wait.
- ScreenPlus requests LVGL refreshes every 16 ms and queues only the latest
  complete frame. Its display thread performs one full-frame `pwrite`, waits
  for all five SPI completion interrupts, then accepts the next queued frame.
  This prevents DMA overlap while approaching the measured 60 Hz ceiling.
  Touch is sampled independently every 10 ms.
- The vendor `gl_screen` binary also embeds LVGL's Linux fbdev driver and mmaps
  `/dev/fb0`; its first frame produces the same SPI/BAM DMA interrupt pattern.
  Its init script runs at `nice -20`. ScreenPlus matches that scheduling
  priority but uses its own completion-synchronised full-frame backend to avoid
  mmap page-fault stalls and framebuffer changes during DMA.
- The 90-degree test rendered all four corner colours and the right-pointing
  arrow in the correct physical positions.
- Touch ioctl reports `ABS_X=0..240` and `ABS_Y=0..240`; LVGL's evdev input
  driver scales those raw coordinates into the rotated display space.
