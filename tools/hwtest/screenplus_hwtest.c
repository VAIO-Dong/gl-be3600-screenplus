#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_FB "/dev/fb0"
#define DEFAULT_INPUT "/dev/input/event0"
#define BACKLIGHT_BRIGHTNESS "/sys/class/backlight/soc:backlight/brightness"
#define BACKLIGHT_MAX "/sys/class/backlight/soc:backlight/max_brightness"

static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
}

static long monotonic_milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

static int read_integer_file(const char *path, int *value)
{
	char buffer[32];
	char *end = NULL;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (length <= 0)
		return -1;
	buffer[length] = '\0';
	errno = 0;
	long parsed = strtol(buffer, &end, 10);
	if (errno || end == buffer)
		return -1;
	*value = (int)parsed;
	return 0;
}

static int write_integer_file(const char *path, int value)
{
	char buffer[32];
	int length = snprintf(buffer, sizeof(buffer), "%d\n", value);
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t written = write(fd, buffer, (size_t)length);
	int saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return written == length ? 0 : -1;
}

static void print_abs_info(int fd, unsigned int code, const char *name, int *first)
{
	struct input_absinfo info;
	if (ioctl(fd, EVIOCGABS(code), &info) != 0)
		return;
	printf("%s\"%s\":{\"min\":%d,\"max\":%d,\"fuzz\":%d,\"flat\":%d,\"resolution\":%d}",
	       *first ? "" : ",", name, info.minimum, info.maximum, info.fuzz,
	       info.flat, info.resolution);
	*first = 0;
}

static int print_input_info(const char *input_path)
{
	char name[256] = {0};
	int fd = open(input_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", input_path, strerror(errno));
		return -1;
	}
	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
		strcpy(name, "unknown");
	printf("\"input\":{\"path\":\"%s\",\"name\":\"%s\",\"axes\":{", input_path, name);
	int first = 1;
	print_abs_info(fd, ABS_X, "ABS_X", &first);
	print_abs_info(fd, ABS_Y, "ABS_Y", &first);
	print_abs_info(fd, ABS_MT_POSITION_X, "ABS_MT_POSITION_X", &first);
	print_abs_info(fd, ABS_MT_POSITION_Y, "ABS_MT_POSITION_Y", &first);
	printf("}}");
	close(fd);
	return 0;
}

static int command_info(const char *fb_path, const char *input_path)
{
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	int fd = open(fb_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", fb_path, strerror(errno));
		return 1;
	}
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fixed) != 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &variable) != 0) {
		fprintf(stderr, "Cannot query %s: %s\n", fb_path, strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	int brightness = -1;
	int maximum = -1;
	read_integer_file(BACKLIGHT_BRIGHTNESS, &brightness);
	read_integer_file(BACKLIGHT_MAX, &maximum);

	printf("{\"framebuffer\":{\"path\":\"%s\",\"id\":\"%.16s\",", fb_path, fixed.id);
	printf("\"xres\":%u,\"yres\":%u,\"xres_virtual\":%u,\"yres_virtual\":%u,",
	       variable.xres, variable.yres, variable.xres_virtual, variable.yres_virtual);
	printf("\"bits_per_pixel\":%u,\"line_length\":%u,\"memory_length\":%u,",
	       variable.bits_per_pixel, fixed.line_length, fixed.smem_len);
	printf("\"red\":{\"offset\":%u,\"length\":%u},", variable.red.offset, variable.red.length);
	printf("\"green\":{\"offset\":%u,\"length\":%u},", variable.green.offset, variable.green.length);
	printf("\"blue\":{\"offset\":%u,\"length\":%u}},", variable.blue.offset, variable.blue.length);
	printf("\"backlight\":{\"brightness\":%d,\"maximum\":%d},", brightness, maximum);
	print_input_info(input_path);
	printf("}\n");
	return 0;
}

struct framebuffer {
	int fd;
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	uint8_t *memory;
	uint8_t *saved;
	size_t length;
};

static int framebuffer_open(struct framebuffer *fb, const char *path)
{
	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
	fb->fd = open(path, O_RDWR | O_CLOEXEC);
	if (fb->fd < 0)
		return -1;
	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fixed) != 0 ||
	    ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->variable) != 0)
		return -1;
	if (fb->variable.bits_per_pixel != 16) {
		errno = ENOTSUP;
		return -1;
	}
	fb->length = fb->fixed.smem_len;
	if (fb->length == 0)
		fb->length = (size_t)fb->fixed.line_length * fb->variable.yres_virtual;
	fb->memory = mmap(NULL, fb->length, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->memory == MAP_FAILED) {
		fb->memory = NULL;
		return -1;
	}
	fb->saved = malloc(fb->length);
	if (!fb->saved)
		return -1;
	memcpy(fb->saved, fb->memory, fb->length);
	return 0;
}

static void framebuffer_close(struct framebuffer *fb, int restore)
{
	if (restore && fb->saved && fb->memory) {
		memcpy(fb->memory, fb->saved, fb->length);
		msync(fb->memory, fb->length, MS_SYNC);
	}
	free(fb->saved);
	if (fb->memory)
		munmap(fb->memory, fb->length);
	if (fb->fd >= 0)
		close(fb->fd);
}

static uint16_t rgb565(unsigned int red, unsigned int green, unsigned int blue)
{
	return (uint16_t)(((red & 0xf8U) << 8U) | ((green & 0xfcU) << 3U) | (blue >> 3U));
}

static void logical_dimensions(const struct framebuffer *fb, int rotation,
			       unsigned int *width, unsigned int *height)
{
	if (rotation == 90 || rotation == 270) {
		*width = fb->variable.yres;
		*height = fb->variable.xres;
	} else {
		*width = fb->variable.xres;
		*height = fb->variable.yres;
	}
}

static int map_pixel(const struct framebuffer *fb, int rotation, unsigned int x,
		     unsigned int y, unsigned int *native_x, unsigned int *native_y)
{
	unsigned int width;
	unsigned int height;
	logical_dimensions(fb, rotation, &width, &height);
	if (x >= width || y >= height)
		return -1;
	if (rotation == 90) {
		*native_x = y;
		*native_y = fb->variable.yres - 1U - x;
	} else if (rotation == 270) {
		*native_x = fb->variable.xres - 1U - y;
		*native_y = x;
	} else if (rotation == 180) {
		*native_x = fb->variable.xres - 1U - x;
		*native_y = fb->variable.yres - 1U - y;
	} else {
		*native_x = x;
		*native_y = y;
	}
	return 0;
}

static void put_pixel(struct framebuffer *fb, int rotation, unsigned int x,
		      unsigned int y, uint16_t colour)
{
	unsigned int native_x;
	unsigned int native_y;
	if (map_pixel(fb, rotation, x, y, &native_x, &native_y) != 0)
		return;
	size_t offset = (size_t)(native_y + fb->variable.yoffset) * fb->fixed.line_length +
			(size_t)(native_x + fb->variable.xoffset) * 2U;
	if (offset + sizeof(colour) <= fb->length)
		memcpy(fb->memory + offset, &colour, sizeof(colour));
}

static void fill_rectangle(struct framebuffer *fb, int rotation, unsigned int x,
			   unsigned int y, unsigned int width, unsigned int height,
			   uint16_t colour)
{
	for (unsigned int row = y; row < y + height; ++row)
		for (unsigned int column = x; column < x + width; ++column)
			put_pixel(fb, rotation, column, row, colour);
}

static void draw_pattern(struct framebuffer *fb, int rotation)
{
	unsigned int width;
	unsigned int height;
	logical_dimensions(fb, rotation, &width, &height);
	uint16_t background = rgb565(7, 15, 27);
	uint16_t border = rgb565(93, 109, 132);
	uint16_t red = rgb565(245, 55, 68);
	uint16_t green = rgb565(45, 211, 111);
	uint16_t blue = rgb565(52, 118, 246);
	uint16_t yellow = rgb565(255, 203, 59);
	uint16_t white = rgb565(250, 252, 255);

	fill_rectangle(fb, rotation, 0, 0, width, height, background);
	fill_rectangle(fb, rotation, 0, 0, width, 2, border);
	fill_rectangle(fb, rotation, 0, height - 2, width, 2, border);
	fill_rectangle(fb, rotation, 0, 0, 2, height, border);
	fill_rectangle(fb, rotation, width - 2, 0, 2, height, border);

	unsigned int marker_w = width > 120 ? 28 : width / 3;
	unsigned int marker_h = height > 60 ? 20 : height / 8;
	fill_rectangle(fb, rotation, 4, 4, marker_w, marker_h, red);
	fill_rectangle(fb, rotation, width - 4 - marker_w, 4, marker_w, marker_h, green);
	fill_rectangle(fb, rotation, 4, height - 4 - marker_h, marker_w, marker_h, blue);
	fill_rectangle(fb, rotation, width - 4 - marker_w, height - 4 - marker_h,
		       marker_w, marker_h, yellow);

	unsigned int middle_y = height / 2;
	unsigned int shaft_start = width / 3;
	unsigned int shaft_end = (width * 2) / 3;
	fill_rectangle(fb, rotation, shaft_start, middle_y - 3, shaft_end - shaft_start, 7, white);
	for (unsigned int step = 0; step < height / 4; ++step) {
		unsigned int x = shaft_end + step;
		if (x >= width - 3)
			break;
		for (int dy = -(int)step; dy <= (int)step; ++dy) {
			int target_y = (int)middle_y + dy;
			if (target_y >= 0 && (unsigned int)target_y < height)
				put_pixel(fb, rotation, x, (unsigned int)target_y, white);
		}
	}
	msync(fb->memory, fb->length, MS_SYNC);
}

static int command_pattern(const char *fb_path, int rotation, int seconds)
{
	struct framebuffer fb;
	if (framebuffer_open(&fb, fb_path) != 0) {
		fprintf(stderr, "Cannot prepare %s: %s\n", fb_path, strerror(errno));
		framebuffer_close(&fb, 0);
		return 1;
	}
	unsigned int width;
	unsigned int height;
	logical_dimensions(&fb, rotation, &width, &height);
	printf("Drawing rotation=%d logical=%ux%u for %d seconds; framebuffer will be restored.\n",
	       rotation, width, height, seconds);
	draw_pattern(&fb, rotation);
	long deadline = monotonic_milliseconds() + seconds * 1000L;
	while (!interrupted && monotonic_milliseconds() < deadline) {
		struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };
		nanosleep(&delay, NULL);
	}
	framebuffer_close(&fb, 1);
	puts("Framebuffer restored.");
	return 0;
}

static const char *event_type_name(unsigned int type)
{
	switch (type) {
	case EV_SYN: return "SYN";
	case EV_KEY: return "KEY";
	case EV_ABS: return "ABS";
	default: return "OTHER";
	}
}

static int command_touch(const char *input_path, int seconds)
{
	int fd = open(input_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", input_path, strerror(errno));
		return 1;
	}
	printf("Reading touch events from %s for %d seconds.\n", input_path, seconds);
	long deadline = monotonic_milliseconds() + seconds * 1000L;
	while (!interrupted && monotonic_milliseconds() < deadline) {
		struct pollfd descriptor = { .fd = fd, .events = POLLIN };
		int remaining = (int)(deadline - monotonic_milliseconds());
		if (remaining < 0)
			remaining = 0;
		if (remaining > 250)
			remaining = 250;
		int result = poll(&descriptor, 1, remaining);
		if (result < 0 && errno != EINTR) {
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		if (result <= 0 || !(descriptor.revents & POLLIN))
			continue;
		struct input_event events[32];
		ssize_t length = read(fd, events, sizeof(events));
		if (length < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (length < 0) {
			fprintf(stderr, "read failed: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		size_t count = (size_t)length / sizeof(events[0]);
		for (size_t index = 0; index < count; ++index) {
			if (events[index].type == EV_SYN && events[index].code == SYN_REPORT) {
				puts("{\"type\":\"SYN\",\"code\":0}");
				continue;
			}
			printf("{\"type\":\"%s\",\"type_code\":%u,\"code\":%u,\"value\":%d}\n",
			       event_type_name(events[index].type), events[index].type,
			       events[index].code, events[index].value);
		}
		fflush(stdout);
	}
	close(fd);
	return 0;
}

static int command_backlight(int value)
{
	int maximum;
	if (read_integer_file(BACKLIGHT_MAX, &maximum) != 0) {
		fprintf(stderr, "Cannot read %s: %s\n", BACKLIGHT_MAX, strerror(errno));
		return 1;
	}
	if (value < 0 || value > maximum) {
		fprintf(stderr, "Brightness must be between 0 and %d.\n", maximum);
		return 1;
	}
	if (write_integer_file(BACKLIGHT_BRIGHTNESS, value) != 0) {
		fprintf(stderr, "Cannot write %s: %s\n", BACKLIGHT_BRIGHTNESS, strerror(errno));
		return 1;
	}
	printf("Brightness set to %d/%d.\n", value, maximum);
	return 0;
}

static int parse_integer(const char *text, int minimum, int maximum, const char *label)
{
	char *end = NULL;
	errno = 0;
	long value = strtol(text, &end, 10);
	if (errno || !end || *end != '\0' || value < minimum || value > maximum) {
		fprintf(stderr, "Invalid %s: %s\n", label, text);
		exit(2);
	}
	return (int)value;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s info [framebuffer] [input]\n"
		"  %s pattern <0|90|180|270> [seconds] [framebuffer]\n"
		"  %s touch [seconds] [input]\n"
		"  %s backlight <0..max>\n",
		program, program, program, program);
}

int main(int argc, char **argv)
{
	struct sigaction action = { .sa_handler = handle_signal };
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "info") == 0) {
		const char *fb = argc > 2 ? argv[2] : DEFAULT_FB;
		const char *input = argc > 3 ? argv[3] : DEFAULT_INPUT;
		return command_info(fb, input);
	}
	if (strcmp(argv[1], "pattern") == 0) {
		if (argc < 3) {
			usage(argv[0]);
			return 2;
		}
		int rotation = parse_integer(argv[2], 0, 270, "rotation");
		if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
			fprintf(stderr, "Rotation must be 0, 90, 180 or 270.\n");
			return 2;
		}
		int seconds = argc > 3 ? parse_integer(argv[3], 1, 300, "duration") : 10;
		const char *fb = argc > 4 ? argv[4] : DEFAULT_FB;
		return command_pattern(fb, rotation, seconds);
	}
	if (strcmp(argv[1], "touch") == 0) {
		int seconds = argc > 2 ? parse_integer(argv[2], 1, 3600, "duration") : 30;
		const char *input = argc > 3 ? argv[3] : DEFAULT_INPUT;
		return command_touch(input, seconds);
	}
	if (strcmp(argv[1], "backlight") == 0) {
		if (argc < 3) {
			usage(argv[0]);
			return 2;
		}
		return command_backlight(parse_integer(argv[2], 0, 255, "brightness"));
	}
	usage(argv[0]);
	return 2;
}
