#include "screenplus_fbdev.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <src/draw/sw/lv_draw_sw_utils.h>

#define SPI_INTERRUPTS_PER_FRAME 5U

struct screenplus_fbdev {
	int fd;
	struct fb_var_screeninfo variable;
	struct fb_fix_screeninfo fixed;
	uint8_t *draw_buffers[2];
	uint8_t *native_frames[2];
	uint8_t *worker_frame;
	uint8_t *pending_frame;
	size_t frame_bytes;
	int irq_fd;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	pthread_t worker;
	bool mutex_ready;
	bool condition_ready;
	bool worker_started;
	bool frame_pending;
	bool stopping;
};

static uint64_t monotonic_nanoseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int open_interrupt_counter(const char *name)
{
	FILE *file = fopen("/proc/interrupts", "r");
	if (!file)
		return -1;
	char line[512];
	int interrupt = -1;
	while (fgets(line, sizeof(line), file)) {
		if (!strstr(line, name))
			continue;
		char *end = NULL;
		long value = strtol(line, &end, 10);
		if (end && end != line && value >= 0 && value <= INT32_MAX)
			interrupt = (int)value;
		break;
	}
	fclose(file);
	if (interrupt < 0)
		return -1;
	char path[96];
	snprintf(path, sizeof(path), "/sys/kernel/irq/%d/per_cpu_count", interrupt);
	return open(path, O_RDONLY | O_CLOEXEC);
}

static uint64_t read_interrupt_counter(int fd)
{
	char buffer[128];
	ssize_t bytes = pread(fd, buffer, sizeof(buffer) - 1U, 0);
	if (bytes <= 0)
		return 0;
	buffer[bytes] = '\0';
	uint64_t total = 0;
	char *position = buffer;
	while (*position) {
		char *end = NULL;
		total += strtoull(position, &end, 10);
		if (!end || end == position)
			break;
		position = end;
		if (*position == ',')
			++position;
		else
			break;
	}
	return total;
}

static void wait_for_transfer(struct screenplus_fbdev *fbdev, uint64_t before)
{
	/* A full native frame consistently completes after five QUP SPI
	 * interrupts. Waiting for the final interrupt prevents the next pwrite
	 * from changing framebuffer memory while DMA is still reading it. */
	if (fbdev->irq_fd < 0) {
		struct timespec fallback = { .tv_sec = 0, .tv_nsec = 25000000L };
		nanosleep(&fallback, NULL);
		return;
	}
	struct timespec initial = { .tv_sec = 0, .tv_nsec = 12000000L };
	nanosleep(&initial, NULL);
	uint64_t deadline = monotonic_nanoseconds() + 40000000ULL;
	while (read_interrupt_counter(fbdev->irq_fd) - before < SPI_INTERRUPTS_PER_FRAME &&
	       monotonic_nanoseconds() < deadline) {
		struct timespec poll_pause = { .tv_sec = 0, .tv_nsec = 250000L };
		nanosleep(&poll_pause, NULL);
	}
}

static void *display_worker(void *context)
{
	struct screenplus_fbdev *fbdev = context;
	for (;;) {
		pthread_mutex_lock(&fbdev->mutex);
		while (!fbdev->frame_pending && !fbdev->stopping)
			pthread_cond_wait(&fbdev->condition, &fbdev->mutex);
		if (fbdev->stopping) {
			pthread_mutex_unlock(&fbdev->mutex);
			break;
		}
		uint8_t *frame = fbdev->worker_frame;
		fbdev->worker_frame = fbdev->pending_frame;
		fbdev->pending_frame = frame;
		frame = fbdev->worker_frame;
		fbdev->frame_pending = false;
		pthread_mutex_unlock(&fbdev->mutex);

		uint64_t before = fbdev->irq_fd >= 0 ?
			read_interrupt_counter(fbdev->irq_fd) : 0;
		ssize_t written;
		do {
			written = pwrite(fbdev->fd, frame, fbdev->frame_bytes, 0);
		} while (written < 0 && errno == EINTR);
		if (written != (ssize_t)fbdev->frame_bytes) {
			int error = written < 0 ? errno : EIO;
			fprintf(stderr, "screenplus: framebuffer write failed: %s\n",
				strerror(error));
		}
		else
			wait_for_transfer(fbdev, before);
	}
	return NULL;
}

static void release_fbdev(struct screenplus_fbdev *fbdev)
{
	if (!fbdev)
		return;
	if (fbdev->worker_started) {
		pthread_mutex_lock(&fbdev->mutex);
		fbdev->stopping = true;
		pthread_cond_signal(&fbdev->condition);
		pthread_mutex_unlock(&fbdev->mutex);
		pthread_join(fbdev->worker, NULL);
	}
	if (fbdev->condition_ready)
		pthread_cond_destroy(&fbdev->condition);
	if (fbdev->mutex_ready)
		pthread_mutex_destroy(&fbdev->mutex);
	if (fbdev->fd >= 0)
		close(fbdev->fd);
	if (fbdev->irq_fd >= 0)
		close(fbdev->irq_fd);
	lv_free(fbdev->native_frames[1]);
	lv_free(fbdev->native_frames[0]);
	lv_free(fbdev->draw_buffers[1]);
	lv_free(fbdev->draw_buffers[0]);
	lv_free(fbdev);
}

static void flush_display(lv_display_t *display, const lv_area_t *area,
			  uint8_t *pixels)
{
	(void)area;
	struct screenplus_fbdev *fbdev = lv_display_get_driver_data(display);
	lv_display_rotation_t rotation = lv_display_get_rotation(display);
	pthread_mutex_lock(&fbdev->mutex);
	uint8_t *native = fbdev->pending_frame;
	if (rotation != LV_DISPLAY_ROTATION_0) {
		int32_t source_width = lv_display_get_horizontal_resolution(display);
		int32_t source_height = lv_display_get_vertical_resolution(display);
		uint32_t source_stride = (uint32_t)source_width * sizeof(uint16_t);
		lv_draw_sw_rotate(pixels, native,
			source_width, source_height, source_stride,
			fbdev->fixed.line_length, rotation, LV_COLOR_FORMAT_RGB565);
	}
	else
		memcpy(native, pixels, fbdev->frame_bytes);
	fbdev->frame_pending = true;
	pthread_cond_signal(&fbdev->condition);
	pthread_mutex_unlock(&fbdev->mutex);
	lv_display_flush_ready(display);
}

static void delete_display(lv_event_t *event)
{
	if (lv_event_get_code(event) != LV_EVENT_DELETE)
		return;
	lv_display_t *display = lv_event_get_target(event);
	struct screenplus_fbdev *fbdev = lv_display_get_driver_data(display);
	if (!fbdev)
		return;
	lv_display_set_driver_data(display, NULL);
	release_fbdev(fbdev);
}

lv_display_t *screenplus_fbdev_create(const char *path)
{
	struct screenplus_fbdev *fbdev = lv_malloc_zeroed(sizeof(*fbdev));
	if (!fbdev)
		return NULL;
	fbdev->fd = -1;
	fbdev->irq_fd = -1;
	fbdev->fd = open(path, O_RDWR | O_CLOEXEC);
	if (fbdev->fd < 0 ||
	    ioctl(fbdev->fd, FBIOGET_FSCREENINFO, &fbdev->fixed) < 0 ||
	    ioctl(fbdev->fd, FBIOGET_VSCREENINFO, &fbdev->variable) < 0) {
		if (fbdev->fd >= 0)
			close(fbdev->fd);
		lv_free(fbdev);
		return NULL;
	}
	if (fbdev->variable.bits_per_pixel != 16 ||
	    fbdev->fixed.line_length != fbdev->variable.xres * sizeof(uint16_t)) {
		errno = ENOTSUP;
		close(fbdev->fd);
		lv_free(fbdev);
		return NULL;
	}
	fbdev->frame_bytes = (size_t)fbdev->fixed.line_length * fbdev->variable.yres;
	if (fbdev->fixed.smem_len < fbdev->frame_bytes) {
		errno = ENOSPC;
		close(fbdev->fd);
		lv_free(fbdev);
		return NULL;
	}
	for (unsigned int index = 0; index < 2; ++index)
		fbdev->draw_buffers[index] = lv_malloc(fbdev->frame_bytes);
	for (unsigned int index = 0; index < 2; ++index)
		fbdev->native_frames[index] = lv_malloc(fbdev->frame_bytes);
	if (!fbdev->draw_buffers[0] || !fbdev->draw_buffers[1] ||
	    !fbdev->native_frames[0] || !fbdev->native_frames[1]) {
		release_fbdev(fbdev);
		return NULL;
	}
	fbdev->irq_fd = open_interrupt_counter("78b5000.spi");
	fbdev->worker_frame = fbdev->native_frames[0];
	fbdev->pending_frame = fbdev->native_frames[1];
	int result = pthread_mutex_init(&fbdev->mutex, NULL);
	if (result != 0) {
		errno = result;
		release_fbdev(fbdev);
		return NULL;
	}
	fbdev->mutex_ready = true;
	result = pthread_cond_init(&fbdev->condition, NULL);
	if (result != 0) {
		errno = result;
		release_fbdev(fbdev);
		return NULL;
	}
	fbdev->condition_ready = true;
	result = pthread_create(&fbdev->worker, NULL, display_worker, fbdev);
	if (result != 0) {
		errno = result;
		release_fbdev(fbdev);
		return NULL;
	}
	fbdev->worker_started = true;

	lv_display_t *display = lv_display_create((int32_t)fbdev->variable.xres,
		(int32_t)fbdev->variable.yres);
	if (!display) {
		release_fbdev(fbdev);
		return NULL;
	}
	lv_display_set_driver_data(display, fbdev);
	lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(display, fbdev->draw_buffers[0], fbdev->draw_buffers[1],
		fbdev->frame_bytes, LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(display, flush_display);
	lv_display_add_event_cb(display, delete_display, LV_EVENT_DELETE, NULL);
	return display;
}
