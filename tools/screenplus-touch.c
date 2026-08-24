#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/input/event0"

static int clamp_value(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int calibrate(int value, int input_minimum, int input_maximum,
                     int output_minimum, int output_maximum)
{
    if (input_minimum != input_maximum)
        value = (value - input_minimum) * (output_maximum - output_minimum) /
            (input_maximum - input_minimum) + output_minimum;
    return clamp_value(value, output_minimum, output_maximum);
}

static void raw_to_logical(int raw_x, int raw_y, int rotation,
                           int *logical_x, int *logical_y)
{
    /*
     * evdev first maps onto the native 76 x 284 framebuffer and LVGL then
     * rotates that point into the 284 x 76 logical display. Both display
     * orientations use the same native calibration; LVGL supplies the
     * orientation-specific point rotation.
     */
    if (rotation == 270) {
        *logical_x = calibrate(raw_y, 0, 283, 0, 283);
        *logical_y = calibrate(raw_x, 0, 75, 75, 0);
    }
    else {
        *logical_x = calibrate(raw_y, 0, 283, 283, 0);
        *logical_y = calibrate(raw_x, 0, 75, 0, 75);
    }
}

static void logical_to_raw(int logical_x, int logical_y, int rotation,
                           int *raw_x, int *raw_y)
{
    logical_x = clamp_value(logical_x, 0, 283);
    logical_y = clamp_value(logical_y, 0, 75);
    if (rotation == 270) {
        *raw_x = 75 - logical_y;
        *raw_y = logical_x;
    }
    else {
        *raw_x = logical_y;
        *raw_y = 283 - logical_x;
    }
}

static int emit_event(int fd, unsigned short type, unsigned short code, int value)
{
    struct input_event event = {0};
    gettimeofday(&event.time, NULL);
    event.type = type;
    event.code = code;
    event.value = value;
    return write(fd, &event, sizeof(event)) == (ssize_t)sizeof(event) ? 0 : -1;
}

static int show_info(const char *device)
{
    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", device, strerror(errno));
        return 1;
    }
    struct input_absinfo x = {0}, y = {0};
    int result = 0;
    if (ioctl(fd, EVIOCGABS(ABS_X), &x) || ioctl(fd, EVIOCGABS(ABS_Y), &y)) {
        fprintf(stderr, "EVIOCGABS: %s\n", strerror(errno));
        result = 1;
    }
    else {
        printf("device=%s\nreported_x=%d..%d resolution=%d\n"
               "reported_y=%d..%d resolution=%d\n",
               device, x.minimum, x.maximum, x.resolution,
               y.minimum, y.maximum, y.resolution);
        puts("screenplus_swap_axes=false");
        puts("screenplus_calibration_90=0,0,75,283");
        puts("screenplus_calibration_270=0,0,75,283");
    }
    close(fd);
    return result;
}

static int tap_raw(const char *device, int raw_x, int raw_y)
{
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", device, strerror(errno));
        return 1;
    }
    int result = 0;
    if (emit_event(fd, EV_ABS, ABS_X, raw_x) ||
        emit_event(fd, EV_ABS, ABS_Y, raw_y) ||
        emit_event(fd, EV_KEY, BTN_TOUCH, 1) ||
        emit_event(fd, EV_SYN, SYN_REPORT, 0)) {
        fprintf(stderr, "press: %s\n", strerror(errno));
        result = 1;
    }
    usleep(80000);
    if (!result && (emit_event(fd, EV_KEY, BTN_TOUCH, 0) ||
                    emit_event(fd, EV_SYN, SYN_REPORT, 0))) {
        fprintf(stderr, "release: %s\n", strerror(errno));
        result = 1;
    }
    close(fd);
    return result;
}

static int swipe_raw(const char *device, int raw_x0, int raw_y0,
                     int raw_x1, int raw_y1, int duration_ms)
{
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", device, strerror(errno));
        return 1;
    }
    const int steps = 16;
    duration_ms = clamp_value(duration_ms, 80, 3000);
    int result = 0;
    if (emit_event(fd, EV_ABS, ABS_X, raw_x0) ||
        emit_event(fd, EV_ABS, ABS_Y, raw_y0) ||
        emit_event(fd, EV_KEY, BTN_TOUCH, 1) ||
        emit_event(fd, EV_SYN, SYN_REPORT, 0)) {
        fprintf(stderr, "press: %s\n", strerror(errno));
        result = 1;
    }
    for (int step = 1; !result && step <= steps; ++step) {
        int raw_x = raw_x0 + (raw_x1 - raw_x0) * step / steps;
        int raw_y = raw_y0 + (raw_y1 - raw_y0) * step / steps;
        usleep((useconds_t)duration_ms * 1000U / steps);
        if (emit_event(fd, EV_ABS, ABS_X, raw_x) ||
            emit_event(fd, EV_ABS, ABS_Y, raw_y) ||
            emit_event(fd, EV_SYN, SYN_REPORT, 0)) {
            fprintf(stderr, "move: %s\n", strerror(errno));
            result = 1;
        }
    }
    if (!result && (emit_event(fd, EV_KEY, BTN_TOUCH, 0) ||
                    emit_event(fd, EV_SYN, SYN_REPORT, 0))) {
        fprintf(stderr, "release: %s\n", strerror(errno));
        result = 1;
    }
    close(fd);
    return result;
}

static int monitor_input(const char *device)
{
    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", device, strerror(errno));
        return 1;
    }
    int raw_x = -1, raw_y = -1, touching = 0;
    puts("raw_x raw_y touch logical90_x logical90_y logical270_x logical270_y");
    fflush(stdout);
    for (;;) {
        struct input_event event;
        ssize_t bytes = read(fd, &event, sizeof(event));
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes != (ssize_t)sizeof(event)) {
            fprintf(stderr, "read: %s\n", bytes < 0 ? strerror(errno) : "short event");
            close(fd);
            return 1;
        }
        if (event.type == EV_ABS &&
            (event.code == ABS_X || event.code == ABS_MT_POSITION_X))
            raw_x = event.value;
        else if (event.type == EV_ABS &&
                 (event.code == ABS_Y || event.code == ABS_MT_POSITION_Y))
            raw_y = event.value;
        else if (event.type == EV_KEY && event.code == BTN_TOUCH)
            touching = event.value;
        else if (event.type == EV_SYN && event.code == SYN_REPORT &&
                 raw_x >= 0 && raw_y >= 0) {
            int x90, y90, x270, y270;
            raw_to_logical(raw_x, raw_y, 90, &x90, &y90);
            raw_to_logical(raw_x, raw_y, 270, &x270, &y270);
            printf("%d %d %d %d %d %d %d\n",
                   raw_x, raw_y, touching, x90, y90, x270, y270);
            fflush(stdout);
        }
    }
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage:\n  %s info [device]\n  %s monitor [device]\n"
        "  %s tap-raw RAW_X RAW_Y [device]\n"
        "  %s tap-screen X Y 90|270 [device]\n"
        "  %s swipe-raw X0 Y0 X1 Y1 DURATION_MS [device]\n"
        "  %s swipe-screen X0 Y0 X1 Y1 DURATION_MS 90|270 [device]\n",
        program, program, program, program, program, program);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }
    if (strcmp(argv[1], "info") == 0)
        return show_info(argc > 2 ? argv[2] : DEFAULT_DEVICE);
    if (strcmp(argv[1], "monitor") == 0)
        return monitor_input(argc > 2 ? argv[2] : DEFAULT_DEVICE);
    if (strcmp(argv[1], "tap-raw") == 0 && argc >= 4)
        return tap_raw(argc > 4 ? argv[4] : DEFAULT_DEVICE,
                       atoi(argv[2]), atoi(argv[3]));
    if (strcmp(argv[1], "tap-screen") == 0 && argc >= 5) {
        int rotation = atoi(argv[4]);
        if (rotation != 90 && rotation != 270) { usage(argv[0]); return 2; }
        int raw_x, raw_y;
        logical_to_raw(atoi(argv[2]), atoi(argv[3]), rotation, &raw_x, &raw_y);
        printf("raw_x=%d raw_y=%d\n", raw_x, raw_y);
        return tap_raw(argc > 5 ? argv[5] : DEFAULT_DEVICE, raw_x, raw_y);
    }
    if (strcmp(argv[1], "swipe-raw") == 0 && argc >= 7)
        return swipe_raw(argc > 7 ? argv[7] : DEFAULT_DEVICE,
                         atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                         atoi(argv[5]), atoi(argv[6]));
    if (strcmp(argv[1], "swipe-screen") == 0 && argc >= 8) {
        int rotation = atoi(argv[7]);
        if (rotation != 90 && rotation != 270) { usage(argv[0]); return 2; }
        int raw_x0, raw_y0, raw_x1, raw_y1;
        logical_to_raw(atoi(argv[2]), atoi(argv[3]), rotation, &raw_x0, &raw_y0);
        logical_to_raw(atoi(argv[4]), atoi(argv[5]), rotation, &raw_x1, &raw_y1);
        printf("raw_start=%d,%d raw_end=%d,%d\n",
               raw_x0, raw_y0, raw_x1, raw_y1);
        return swipe_raw(argc > 8 ? argv[8] : DEFAULT_DEVICE,
                         raw_x0, raw_y0, raw_x1, raw_y1, atoi(argv[6]));
    }
    usage(argv[0]);
    return 2;
}
