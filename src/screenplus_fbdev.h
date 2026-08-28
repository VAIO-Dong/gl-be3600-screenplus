#ifndef SCREENPLUS_FBDEV_H
#define SCREENPLUS_FBDEV_H

#include <lvgl.h>

/* Create a full-frame RGB565 display backed by a Linux framebuffer. LVGL
 * rotates into a latest-frame queue; a display thread submits one coherent
 * pwrite and waits for SPI completion before presenting the next frame. */
lv_display_t *screenplus_fbdev_create(const char *path);

#endif
