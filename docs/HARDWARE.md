# GL-BE3600 hardware notes

## Confirmed interfaces

| Function | Interface | Observed value |
| --- | --- | --- |
| Framebuffer | `/dev/fb0` | `fb_st7789p3` |
| Native framebuffer | sysfs/ioctl | 76 x 284, 16-bpp RGB565 |
| Stride | sysfs/ioctl | 152 bytes |
| Touch | `/dev/input/event0` | Hynitron CST816X |
| Backlight | `/sys/class/backlight/soc:backlight` | 0-11, observed 5 |
| WAN/LAN NICs | `eth0`, `eth1` | both 10/100/1000/2500 Mbps |

The LCD is mounted as a landscape touchscreen while the kernel framebuffer is
reported in portrait scan order. Visual hardware testing confirmed that the
normal logical landscape orientation is the 90-degree mapping:

`native_x = logical_y`, `native_y = 283 - logical_x`.

The 270-degree mapping is a valid flipped orientation and remains available as
a LuCI option. It is not only a development-test setting.

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
- The 90-degree test rendered all four corner colours and the right-pointing
  arrow in the correct physical positions.
- Touch ioctl reports `ABS_X=0..240` and `ABS_Y=0..240`; LVGL's evdev input
  driver scales those raw coordinates into the rotated display space.
