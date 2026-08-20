#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lvgl.h>
#include <src/drivers/display/fb/lv_linux_fbdev.h>
#include <src/drivers/evdev/lv_evdev.h>
#include <src/libs/qrcode/lv_qrcode.h>

#include "app_config.h"
#include "metrics.h"
#include "system_info.h"

#define DEFAULT_FRAMEBUFFER "/dev/fb0"
#define DEFAULT_INPUT "/dev/input/event0"

struct options {
	const char *framebuffer;
	const char *input;
	const char *config;
	int rotation;
	bool rotation_override;
	int duration_seconds;
	bool touch_enabled;
	bool metrics_once;
	bool system_info_once;
	bool config_once;
	int start_page;
};

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reload_requested = 0;
static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static lv_obj_t *dashboard_screen;
static lv_obj_t *clock_screen;
static lv_obj_t *status_screen;
static lv_obj_t *traffic_screen;
static lv_obj_t *network_screen;
static lv_obj_t *wifi_screen;
static lv_obj_t *wifi_qr_screen;
static lv_obj_t *wifi_qr_code;
static lv_obj_t *openclash_screen;
static lv_obj_t *screens[SCREENPLUS_PAGE_COUNT];
static lv_obj_t *screens_by_page[SCREENPLUS_PAGE_COUNT];
static unsigned int screen_count;
static unsigned int current_screen_index;
static lv_obj_t *cpu_value;
static lv_obj_t *cpu_detail;
static lv_obj_t *memory_value;
static lv_obj_t *memory_detail;
static lv_obj_t *fan_value;
static lv_obj_t *fan_detail;
static struct metrics_state metric_state;
static struct system_metrics latest_metrics;
static pthread_mutex_t metrics_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t metrics_worker_thread;
static bool metrics_worker_started;
static bool metrics_ready;
static uint64_t metrics_generation;
static uint64_t applied_metrics_generation;
static lv_obj_t *traffic_download_label;
static lv_obj_t *traffic_upload_label;
static lv_obj_t *traffic_chart;
static lv_chart_series_t *traffic_download_series;
static lv_chart_series_t *traffic_upload_series;
static double traffic_target_download;
static double traffic_target_upload;
static double traffic_last_download;
static double traffic_last_upload;
static unsigned int traffic_download_zero_samples;
static unsigned int traffic_upload_zero_samples;
static int32_t traffic_chart_peak = 64;
static lv_obj_t *wifi_band_rows[2];
static lv_obj_t *wifi_band_titles[2];
static lv_obj_t *wifi_ssid_labels[2];
static lv_obj_t *wifi_password_labels[2];
static lv_obj_t *network_lan_label;
static lv_obj_t *network_uplink_labels[4];
static lv_obj_t *network_wan_value;
static lv_obj_t *openclash_state_label;
static lv_obj_t *openclash_download_label;
static lv_obj_t *openclash_upload_label;
static lv_obj_t *openclash_connections_label;
static lv_obj_t *openclash_totals_label;
static struct system_info_state system_state;
static struct system_snapshot latest_snapshot;
static pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t system_worker_thread;
static bool system_worker_started;
static bool snapshot_ready;
static uint64_t snapshot_generation;
static uint64_t applied_snapshot_generation;
static bool password_revealed[2];
static bool password_long_press_handled;
static uint32_t password_reveal_deadline[2];
static unsigned int qr_wifi_band;
static struct screenplus_config app_config;
static lv_display_t *main_display;
static bool backlight_on = true;
static void *background_buffers[SCREENPLUS_PAGE_COUNT];
static int requested_start_page = -1;
static bool drag_tracking;
static bool drag_moved;
static bool page_animation_running;
static lv_point_t drag_start_point;
static int32_t drag_offset;
static int pending_page_delta;

static void on_signal(int signal_number)
{
	if (signal_number == SIGHUP)
		reload_requested = 1;
	else
		running = 0;
}

static uint32_t monotonic_milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U);
}

static void sleep_milliseconds(uint32_t milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000U,
		.tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
	};
	while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
		if (!running)
			break;
	}
}

static lv_color_t colour(unsigned int hex)
{
	return lv_color_hex(hex);
}

static const char *translated(const char *chinese, const char *english)
{
	return app_config.chinese ? chinese : english;
}

static const lv_font_t *small_ui_font(void)
{
	return app_config.chinese ? &lv_font_source_han_sans_sc_14_cjk :
		&lv_font_montserrat_14;
}

static const char *display_state_text(enum screenplus_state state)
{
	if (!app_config.chinese)
		return system_info_state_text(state);
	switch (state) {
	case SCREENPLUS_STATE_IDLE: return "待机";
	case SCREENPLUS_STATE_CONNECTING: return "等待";
	case SCREENPLUS_STATE_CONNECTED: return "可用";
	case SCREENPLUS_STATE_ACTIVE: return "使用中";
	case SCREENPLUS_STATE_ERROR: return "故障";
	default: return "不可用";
	}
}

static void add_page_background(lv_obj_t *screen, enum screenplus_page_id page);

static void style_screen(lv_obj_t *screen)
{
	lv_obj_set_size(screen, 284, 76);
	lv_obj_set_style_radius(screen, 0, 0);
	lv_obj_set_style_bg_color(screen, colour(app_config.background_colour), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(screen, 0, 0);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_page(lv_obj_t *parent, enum screenplus_page_id page)
{
	lv_obj_t *object = lv_obj_create(parent);
	style_screen(object);
	add_page_background(object, page);
	lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
	return object;
}

static lv_obj_t *create_divider(lv_obj_t *parent, int x, int y, int width, int height)
{
	lv_obj_t *line = lv_obj_create(parent);
	lv_obj_set_pos(line, x, y);
	lv_obj_set_size(line, width, height);
	lv_obj_set_style_radius(line, 0, 0);
	lv_obj_set_style_border_width(line, 0, 0);
	lv_obj_set_style_pad_all(line, 0, 0);
	lv_obj_set_style_bg_color(line, colour(app_config.border_colour), 0);
	lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
	lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	return line;
}

static void add_page_background(lv_obj_t *screen, enum screenplus_page_id page)
{
	char expected_name[32];
	if (app_config.global_background)
		strcpy(expected_name, "global.rgb565");
	else {
		snprintf(expected_name, sizeof(expected_name), "%s.rgb565",
			screenplus_page_name(page));
		if (strcmp(app_config.pages[page].background, expected_name) != 0)
			return;
	}
	char path[192];
	snprintf(path, sizeof(path), "/usr/share/screenplus/backgrounds/%s", expected_name);
	FILE *file = fopen(path, "rb");
	if (!file)
		return;
	const size_t expected_size = 284U * 76U * 2U;
	void *buffer = malloc(expected_size);
	if (!buffer) {
		fclose(file);
		return;
	}
	bool valid = fread(buffer, 1, expected_size, file) == expected_size && fgetc(file) == EOF;
	fclose(file);
	if (!valid) {
		free(buffer);
		return;
	}
	background_buffers[page] = buffer;
	lv_obj_t *canvas = lv_canvas_create(screen);
	lv_canvas_set_buffer(canvas, buffer, 284, 76, LV_COLOR_FORMAT_RGB565);
	lv_obj_set_pos(canvas, 0, 0);
	lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	if (app_config.overlay_opacity) {
		lv_obj_t *overlay = lv_obj_create(screen);
		lv_obj_set_pos(overlay, 0, 0);
		lv_obj_set_size(overlay, 284, 76);
		lv_obj_set_style_radius(overlay, 0, 0);
		lv_obj_set_style_border_width(overlay, 0, 0);
		lv_obj_set_style_pad_all(overlay, 0, 0);
		lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(overlay,
			(lv_opa_t)(255U * app_config.overlay_opacity / 100U), 0);
		lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	}
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int x, int y,
			      const lv_font_t *font, unsigned int text_colour)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_obj_set_pos(label, x, y);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, colour(text_colour), 0);
	return label;
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
	if (label && strcmp(lv_label_get_text(label), text) != 0)
		lv_label_set_text(label, text);
}

static void update_clock(lv_timer_t *timer)
{
	(void)timer;
	if (!clock_label || !date_label)
		return;
	time_t now = time(NULL);
	struct tm local;
	if (!localtime_r(&now, &local))
		return;
	char time_text[16];
	char date_text[64];
	strftime(time_text, sizeof(time_text),
		screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_HOME, "seconds") ?
		"%H:%M:%S" : "%H:%M", &local);
	bool show_date = screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_HOME, "date");
	bool show_weekday = screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_HOME, "weekday");
	bool show_timezone = screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_HOME, "timezone");
	if (app_config.chinese && show_weekday) {
		static const char *const weekdays[] = {
			"周日", "周一", "周二", "周三", "周四", "周五", "周六"
		};
		if (show_date)
			snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d %s",
				local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
				weekdays[local.tm_wday]);
		else
			snprintf(date_text, sizeof(date_text), "%s", weekdays[local.tm_wday]);
	}
	else if (show_date && show_weekday)
		strftime(date_text, sizeof(date_text), "%Y-%m-%d %a", &local);
	else if (show_date)
		strftime(date_text, sizeof(date_text), "%Y-%m-%d", &local);
	else if (show_weekday)
		strftime(date_text, sizeof(date_text), "%a", &local);
	else
		date_text[0] = '\0';
	if (show_timezone) {
		char zone[16];
		strftime(zone, sizeof(zone), "%Z", &local);
		if (zone[0]) {
			size_t used = strlen(date_text);
			snprintf(date_text + used, sizeof(date_text) - used,
				"%s%s", used ? " " : "", zone);
		}
	}
	set_label_text_if_changed(clock_label, time_text);
	set_label_text_if_changed(date_label, date_text);
}

static void apply_metrics(lv_timer_t *timer)
{
	(void)timer;
	if (drag_tracking || page_animation_running)
		return;
	struct system_metrics metrics;
	pthread_mutex_lock(&metrics_mutex);
	bool ready = metrics_ready;
	uint64_t generation = metrics_generation;
	if (ready)
		metrics = latest_metrics;
	pthread_mutex_unlock(&metrics_mutex);
	if (!ready || generation == applied_metrics_generation)
		return;
	applied_metrics_generation = generation;
	char main_text[24];
	char detail_text[24];

	if (cpu_value) {
		snprintf(main_text, sizeof(main_text), "%.0f%%", metrics.cpu_percent);
		set_label_text_if_changed(cpu_value, main_text);
	}
	if (cpu_detail) {
		snprintf(detail_text, sizeof(detail_text), "%.0fC", metrics.temperature_celsius);
		set_label_text_if_changed(cpu_detail, detail_text);
	}
	if (memory_value) {
		snprintf(main_text, sizeof(main_text), "%.0f%%", metrics.memory_percent);
		set_label_text_if_changed(memory_value, main_text);
	}
	if (memory_detail) {
		snprintf(detail_text, sizeof(detail_text), "%lluM",
			(unsigned long long)(metrics.memory_used_bytes / (1024U * 1024U)));
		set_label_text_if_changed(memory_detail, detail_text);
	}
	if (fan_value) {
		if (metrics.fan_rpm)
			snprintf(main_text, sizeof(main_text), "%u", metrics.fan_rpm);
		else
			strcpy(main_text, "OFF");
		set_label_text_if_changed(fan_value, main_text);
	}
	if (fan_detail)
		set_label_text_if_changed(fan_detail, metrics.fan_rpm ? "RPM" : "--");

	if (metrics.network_receive_bytes_per_second > 0.5) {
		traffic_last_download = metrics.network_receive_bytes_per_second;
		traffic_download_zero_samples = 0;
	} else if (++traffic_download_zero_samples >= 2) {
		traffic_last_download = 0.0;
	}
	if (metrics.network_transmit_bytes_per_second > 0.5) {
		traffic_last_upload = metrics.network_transmit_bytes_per_second;
		traffic_upload_zero_samples = 0;
	} else if (++traffic_upload_zero_samples >= 2) {
		traffic_last_upload = 0.0;
	}
	traffic_target_download = traffic_last_download;
	traffic_target_upload = traffic_last_upload;
	char rate[16];
	char rate_text[20];
	if (traffic_download_label) {
		metrics_format_rate(traffic_target_download, rate, sizeof(rate));
		snprintf(rate_text, sizeof(rate_text), "%s/s", rate);
		set_label_text_if_changed(traffic_download_label, rate_text);
	}
	if (traffic_upload_label) {
		metrics_format_rate(traffic_target_upload, rate, sizeof(rate));
		snprintf(rate_text, sizeof(rate_text), "%s/s", rate);
		set_label_text_if_changed(traffic_upload_label, rate_text);
	}
	if (traffic_chart) {
		double maximum = traffic_target_download > traffic_target_upload ?
			traffic_target_download : traffic_target_upload;
		int32_t point = (int32_t)(maximum / 1024.0);
		if (point > traffic_chart_peak)
			traffic_chart_peak = point + point / 5 + 1;
		else if (traffic_chart_peak > 64)
			traffic_chart_peak = (traffic_chart_peak * 98) / 100;
		if (traffic_chart_peak < 64)
			traffic_chart_peak = 64;
		lv_chart_set_range(traffic_chart, LV_CHART_AXIS_PRIMARY_Y, 0, traffic_chart_peak);
		lv_chart_set_next_value(traffic_chart, traffic_download_series,
			(int32_t)(traffic_target_download / 1024.0));
		lv_chart_set_next_value(traffic_chart, traffic_upload_series,
			(int32_t)(traffic_target_upload / 1024.0));
	}
}

static void *metrics_worker(void *unused)
{
	(void)unused;
	while (running) {
		struct system_metrics metrics;
		metrics_sample(&metric_state, &metrics);
		pthread_mutex_lock(&metrics_mutex);
		latest_metrics = metrics;
		metrics_ready = true;
		++metrics_generation;
		pthread_mutex_unlock(&metrics_mutex);
		for (unsigned int count = 0; count < 10 && running; ++count)
			sleep_milliseconds(100);
	}
	return NULL;
}

static void set_page_visible(lv_obj_t *page, bool visible)
{
	bool hidden = lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN);
	if (visible && hidden)
		lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
	else if (!visible && !hidden)
		lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

static void place_pages(int32_t offset)
{
	int transition_delta = offset < 0 ? 1 : offset > 0 ? -1 : pending_page_delta;
	int neighbour = -1;
	if (transition_delta) {
		int candidate = (int)current_screen_index + transition_delta;
		if (app_config.swipe_loop)
			neighbour = (candidate + (int)screen_count) % (int)screen_count;
		else if (candidate >= 0 && candidate < (int)screen_count)
			neighbour = candidate;
	}
	for (unsigned int index = 0; index < screen_count; ++index) {
		bool current = index == current_screen_index;
		bool adjacent = (int)index == neighbour;
		set_page_visible(screens[index], current || adjacent);
		if (!current && !adjacent)
			continue;
		int32_t x = current ? offset : offset + transition_delta * 284;
		if (lv_obj_get_x(screens[index]) != x)
			lv_obj_set_x(screens[index], x);
	}
}

static void page_animation_exec(void *object, int32_t value)
{
	(void)object;
	place_pages(value);
}

static void page_animation_complete(lv_anim_t *animation)
{
	(void)animation;
	if (pending_page_delta > 0)
		current_screen_index = (current_screen_index + 1U) % screen_count;
	else if (pending_page_delta < 0)
		current_screen_index = (current_screen_index + screen_count - 1U) % screen_count;
	pending_page_delta = 0;
	drag_offset = 0;
	drag_moved = false;
	page_animation_running = false;
	place_pages(0);
}

static void animate_page_settle(int32_t end, int page_delta)
{
	pending_page_delta = page_delta;
	page_animation_running = true;
	uint32_t distance = (uint32_t)abs(end - drag_offset);
	uint32_t duration = app_config.slide_animation ? 70U + distance / 3U : 1U;
	if (duration > 180U)
		duration = 180U;
	lv_anim_t animation;
	lv_anim_init(&animation);
	lv_anim_set_var(&animation, dashboard_screen);
	lv_anim_set_exec_cb(&animation, page_animation_exec);
	lv_anim_set_values(&animation, drag_offset, end);
	lv_anim_set_duration(&animation, duration);
	lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&animation, page_animation_complete);
	lv_anim_start(&animation);
}

static void page_drag_event(lv_event_t *event)
{
	lv_event_code_t code = lv_event_get_code(event);
	lv_indev_t *input = lv_indev_active();
	if (!input || screen_count < 2)
		return;
	if (code == LV_EVENT_PRESSED) {
		if (page_animation_running)
			return;
		lv_indev_get_point(input, &drag_start_point);
		drag_tracking = true;
		drag_moved = false;
		drag_offset = 0;
		return;
	}
	if (code == LV_EVENT_PRESSING && drag_tracking && !page_animation_running) {
		lv_point_t point;
		lv_indev_get_point(input, &point);
		int32_t horizontal = point.x - drag_start_point.x;
		int32_t vertical = point.y - drag_start_point.y;
		if (!drag_moved) {
			if (abs(horizontal) < 12 || abs(horizontal) <= abs(vertical))
				return;
			drag_moved = true;
		}
		if (!app_config.swipe_loop &&
		    ((current_screen_index == 0 && horizontal > 0) ||
		     (current_screen_index + 1U == screen_count && horizontal < 0)))
			horizontal /= 3;
		drag_offset = horizontal;
		place_pages(drag_offset);
		return;
	}
	if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && drag_tracking) {
		drag_tracking = false;
		if (!drag_moved) {
			drag_offset = 0;
			return;
		}
		if (drag_offset < -42 &&
		    (app_config.swipe_loop || current_screen_index + 1U < screen_count))
			animate_page_settle(-284, 1);
		else if (drag_offset > 42 &&
			 (app_config.swipe_loop || current_screen_index > 0))
			animate_page_settle(284, -1);
		else
			animate_page_settle(0, 0);
	}
}

static void auto_carousel(lv_timer_t *timer)
{
	(void)timer;
	if (!app_config.auto_carousel || screen_count < 2 || page_animation_running || drag_tracking)
		return;
	drag_offset = 0;
	animate_page_settle(-284, 1);
}

static void set_backlight(bool enabled)
{
	if (enabled == backlight_on)
		return;
	FILE *file = fopen("/sys/class/backlight/soc:backlight/brightness", "w");
	if (!file)
		return;
	fprintf(file, "%d\n", enabled ? app_config.brightness : 0);
	if (fclose(file) == 0)
		backlight_on = enabled;
}

static void manage_idle_state(lv_timer_t *timer)
{
	(void)timer;
	if (!main_display)
		return;
	bool should_be_on = app_config.always_on ||
		lv_display_get_inactive_time(main_display) < app_config.idle_timeout_seconds * 1000U;
	set_backlight(should_be_on);
	for (unsigned int band = 0; band < 2; ++band) {
		if (password_revealed[band] && password_reveal_deadline[band] &&
		    monotonic_milliseconds() >= password_reveal_deadline[band]) {
			password_revealed[band] = false;
			password_reveal_deadline[band] = 0;
			applied_snapshot_generation = 0;
		}
	}
}

static lv_obj_t *build_clock_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_HOME);

	lv_obj_t *accent = lv_obj_create(screen);
	lv_obj_set_pos(accent, 14, 8);
	lv_obj_set_size(accent, 4, 60);
	lv_obj_set_style_radius(accent, 0, 0);
	lv_obj_set_style_border_width(accent, 0, 0);
	lv_obj_set_style_bg_color(accent, colour(app_config.accent_colour), 0);
	lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

	clock_label = create_label(screen, "--:--", 28, 11, &lv_font_montserrat_28,
		app_config.primary_colour);
	lv_obj_set_width(clock_label, 246);
	lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_letter_space(clock_label, -1, 0);
	date_label = create_label(screen, "---- -- --", 28, 48, small_ui_font(),
		app_config.primary_colour);
	lv_obj_set_width(date_label, 246);
	lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_LEFT, 0);
	if (!screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_HOME, "time"))
		lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
}

static void create_metric(lv_obj_t *parent, const char *title, int x,
			  lv_obj_t **value_label, lv_obj_t **detail_label)
{
	create_label(parent, title, x + 8, 4, &lv_font_montserrat_14,
		app_config.accent_colour);
	*value_label = create_label(parent, "--", x + 8, 24, &lv_font_montserrat_18,
		app_config.primary_colour);
	*detail_label = create_label(parent, "--", x + 8, 50, &lv_font_montserrat_14,
		app_config.secondary_colour);
	lv_obj_set_width(*value_label, 78);
	lv_obj_set_width(*detail_label, 78);
	lv_label_set_long_mode(*value_label, LV_LABEL_LONG_DOT);
	lv_label_set_long_mode(*detail_label, LV_LABEL_LONG_DOT);
}

static lv_obj_t *build_status_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_STATUS);
	create_divider(screen, 94, 10, 2, 56);
	create_divider(screen, 189, 10, 2, 56);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_STATUS, "cpu"))
		create_metric(screen, "CPU", 0,
			&cpu_value, &cpu_detail);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_STATUS, "memory"))
		create_metric(screen, "MEM", 95,
			&memory_value, &memory_detail);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_STATUS, "fan"))
		create_metric(screen, "FAN", 190,
			&fan_value, &fan_detail);
	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
}

static lv_obj_t *build_traffic_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_TRAFFIC);
	create_divider(screen, 124, 8, 2, 60);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_TRAFFIC, "rates")) {
		create_label(screen, "UP", 8, 2, &lv_font_montserrat_14,
			app_config.accent_colour);
		traffic_upload_label = create_label(screen, "0B/s", 8, 19,
			&lv_font_montserrat_18, app_config.primary_colour);
		create_label(screen, "DOWN", 8, 39, &lv_font_montserrat_14,
			app_config.accent_colour);
		traffic_download_label = create_label(screen, "0B/s", 8, 55,
			&lv_font_montserrat_18, app_config.primary_colour);
		lv_obj_set_width(traffic_download_label, 110);
		lv_obj_set_width(traffic_upload_label, 110);
	}
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_TRAFFIC, "history")) {
		traffic_chart = lv_chart_create(screen);
		lv_obj_set_pos(traffic_chart, 130, 3);
		lv_obj_set_size(traffic_chart, 151, 70);
		lv_obj_set_style_bg_opa(traffic_chart, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(traffic_chart, 0, 0);
		lv_obj_set_style_pad_all(traffic_chart, 0, 0);
		lv_obj_set_style_line_width(traffic_chart, 2, LV_PART_ITEMS);
		lv_obj_set_style_size(traffic_chart, 0, 0, LV_PART_INDICATOR);
		lv_chart_set_type(traffic_chart, LV_CHART_TYPE_LINE);
		lv_chart_set_point_count(traffic_chart, 30);
		lv_chart_set_div_line_count(traffic_chart, 0, 0);
		lv_chart_set_update_mode(traffic_chart, LV_CHART_UPDATE_MODE_SHIFT);
		lv_chart_set_range(traffic_chart, LV_CHART_AXIS_PRIMARY_Y, 0, traffic_chart_peak);
		traffic_download_series = lv_chart_add_series(traffic_chart,
			colour(app_config.accent_colour), LV_CHART_AXIS_PRIMARY_Y);
		traffic_upload_series = lv_chart_add_series(traffic_chart,
			colour(app_config.secondary_colour), LV_CHART_AXIS_PRIMARY_Y);
		lv_chart_set_all_values(traffic_chart, traffic_download_series, 0);
		lv_chart_set_all_values(traffic_chart, traffic_upload_series, 0);
		lv_obj_clear_flag(traffic_chart, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	}
	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
}

static unsigned int state_colour(enum screenplus_state state)
{
	switch (state) {
	case SCREENPLUS_STATE_ACTIVE: return app_config.accent_colour;
	case SCREENPLUS_STATE_CONNECTED: return app_config.primary_colour;
	case SCREENPLUS_STATE_CONNECTING: return app_config.warning_colour;
	case SCREENPLUS_STATE_ERROR: return app_config.error_colour;
	case SCREENPLUS_STATE_IDLE: return app_config.secondary_colour;
	default: return app_config.border_colour;
	}
}

static unsigned int network_state_colour(enum screenplus_state state)
{
	switch (state) {
	case SCREENPLUS_STATE_ACTIVE: return app_config.accent_colour;
	case SCREENPLUS_STATE_IDLE: return app_config.standby_colour;
	case SCREENPLUS_STATE_CONNECTING:
	case SCREENPLUS_STATE_CONNECTED:
	case SCREENPLUS_STATE_ERROR: return app_config.warning_colour;
	default: return app_config.absent_colour;
	}
}

static lv_obj_t *build_network_screen(lv_obj_t *parent)
{
	static const char *const fields[] = {
		"ethernet", "repeater", "tethering", "cellular"
	};
	static const char *const titles[] = { "ETH", "REPEATER", "USB", "CELLULAR" };
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_NETWORK);
	create_divider(screen, 8, 25, 268, 2);
	create_divider(screen, 8, 50, 268, 2);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_NETWORK, "wan_detail")) {
		create_label(screen, "WAN", 8, 4, &lv_font_montserrat_14,
			app_config.accent_colour);
		network_wan_value = create_label(screen, "--", 55, 4,
			&lv_font_montserrat_14, app_config.primary_colour);
		lv_obj_set_width(network_wan_value, 221);
		lv_label_set_long_mode(network_wan_value, LV_LABEL_LONG_DOT);
	}
	unsigned int visible = 0;
	for (unsigned int index = 0; index < 4; ++index)
		visible += screenplus_page_has_field(&app_config,
			SCREENPLUS_PAGE_NETWORK, fields[index]) ? 1U : 0U;
	unsigned int slot = 0;
	for (unsigned int index = 0; index < 4; ++index) {
		if (!screenplus_page_has_field(&app_config,
		    SCREENPLUS_PAGE_NETWORK, fields[index]))
			continue;
		static const int four_column_edges[] = { 8, 52, 145, 196, 276 };
		int x = visible == 4 ? four_column_edges[slot] :
			8 + (int)(268U * slot / visible);
		int next = visible == 4 ? four_column_edges[slot + 1U] :
			8 + (int)(268U * (slot + 1U) / visible);
		network_uplink_labels[index] = create_label(screen, titles[index], x, 29,
			&lv_font_montserrat_14, app_config.secondary_colour);
		lv_obj_set_width(network_uplink_labels[index], next - x);
		lv_label_set_long_mode(network_uplink_labels[index], LV_LABEL_LONG_DOT);
		++slot;
	}
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_NETWORK, "lan")) {
		create_label(screen, "LAN", 8, 55, &lv_font_montserrat_14,
			app_config.accent_colour);
		network_lan_label = create_label(screen, "--", 55, 55,
			&lv_font_montserrat_14, app_config.primary_colour);
		lv_obj_set_width(network_lan_label, 221);
		lv_label_set_long_mode(network_lan_label, LV_LABEL_LONG_DOT);
	}
	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
}

static void update_wifi_band_display(unsigned int band, const struct wifi_info *wifi)
{
	if (band >= 2 || !wifi_band_rows[band] || !wifi)
		return;
	char text[160];
	if (!wifi->enabled) {
		set_label_text_if_changed(wifi_ssid_labels[band], "OFF");
		set_label_text_if_changed(wifi_password_labels[band], "");
		lv_obj_set_y(wifi_ssid_labels[band], 10);
		lv_obj_set_style_text_color(wifi_ssid_labels[band],
			colour(app_config.secondary_colour), 0);
		return;
	}
	lv_obj_set_y(wifi_ssid_labels[band], 2);
	set_label_text_if_changed(wifi_ssid_labels[band], wifi->ssid[0] ? wifi->ssid : "--");
	lv_obj_set_style_text_color(wifi_ssid_labels[band],
		colour(app_config.primary_colour), 0);
	if (app_config.password_mode == SCREENPLUS_PASSWORD_HIDDEN)
		strcpy(text, "KEY ********");
	else if (!wifi->password[0])
		strcpy(text, "KEY OPEN");
	else if (app_config.password_mode == SCREENPLUS_PASSWORD_VISIBLE ||
		 password_revealed[band])
		snprintf(text, sizeof(text), "KEY %s", wifi->password);
	else if (app_config.password_mode == SCREENPLUS_PASSWORD_QR)
		strcpy(text, "KEY QR CODE");
	else
		strcpy(text, "KEY TAP TO SHOW");
	set_label_text_if_changed(wifi_password_labels[band], text);
}

static void password_event(lv_event_t *event)
{
	lv_event_code_t code = lv_event_get_code(event);
	unsigned int band = (unsigned int)(uintptr_t)lv_event_get_user_data(event);
	if (band >= 2)
		return;
	if (lv_event_get_current_target_obj(event) == wifi_band_rows[band] &&
	    lv_event_get_target_obj(event) != wifi_band_rows[band])
		return;
	if (code == LV_EVENT_LONG_PRESSED &&
	    app_config.password_mode != SCREENPLUS_PASSWORD_HIDDEN) {
		password_long_press_handled = true;
		struct system_snapshot snapshot;
		pthread_mutex_lock(&snapshot_mutex);
		bool ready = snapshot_ready;
		if (ready)
			snapshot = latest_snapshot;
		pthread_mutex_unlock(&snapshot_mutex);
		if (!ready) {
			password_long_press_handled = false;
			return;
		}
		const struct wifi_info *wifi = band == 0 ? &snapshot.wifi_2g : &snapshot.wifi_5g;
		if (!wifi->enabled) {
			password_long_press_handled = false;
			return;
		}
		char escaped_ssid[SCREENPLUS_TEXT_MEDIUM * 2];
		char escaped_password[SCREENPLUS_TEXT_MEDIUM * 2];
		size_t ssid_used = 0;
		size_t password_used = 0;
		for (const char *source = wifi->ssid; *source && ssid_used + 2 < sizeof(escaped_ssid); ++source) {
			if (strchr("\\;,:\"", *source))
				escaped_ssid[ssid_used++] = '\\';
			escaped_ssid[ssid_used++] = *source;
		}
		escaped_ssid[ssid_used] = '\0';
		for (const char *source = wifi->password;
		     *source && password_used + 2 < sizeof(escaped_password); ++source) {
			if (strchr("\\;,:\"", *source))
				escaped_password[password_used++] = '\\';
			escaped_password[password_used++] = *source;
		}
		escaped_password[password_used] = '\0';
		char payload[448];
		if (wifi->password[0])
			snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;",
				escaped_ssid, escaped_password);
		else
			snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", escaped_ssid);
		lv_qrcode_set_data(wifi_qr_code, payload);
		qr_wifi_band = band;
		lv_screen_load(wifi_qr_screen);
		return;
	}
	if (code == LV_EVENT_RELEASED && password_long_press_handled) {
		password_long_press_handled = false;
		return;
	}
	bool tap = !drag_moved || abs(drag_offset) < 28;
	if (code == LV_EVENT_RELEASED && app_config.password_mode == SCREENPLUS_PASSWORD_QR) {
		if (tap)
			lv_obj_send_event(wifi_password_labels[band], LV_EVENT_LONG_PRESSED, NULL);
		password_long_press_handled = false;
		return;
	}
	if (code == LV_EVENT_RELEASED && tap &&
	    app_config.password_mode == SCREENPLUS_PASSWORD_TAP) {
		password_revealed[band] = !password_revealed[band];
		password_reveal_deadline[band] = password_revealed[band] ?
			monotonic_milliseconds() + 15000U : 0;
		struct system_snapshot snapshot;
		pthread_mutex_lock(&snapshot_mutex);
		bool ready = snapshot_ready;
		if (ready)
			snapshot = latest_snapshot;
		pthread_mutex_unlock(&snapshot_mutex);
		if (ready)
			update_wifi_band_display(band,
				band == 0 ? &snapshot.wifi_2g : &snapshot.wifi_5g);
		applied_snapshot_generation = 0;
	}
}

static void close_wifi_qr(lv_event_t *event)
{
	if (lv_event_get_code(event) != LV_EVENT_CLICKED &&
	    lv_event_get_code(event) != LV_EVENT_GESTURE)
		return;
	(void)qr_wifi_band;
	password_long_press_handled = false;
	lv_screen_load(dashboard_screen);
	place_pages(0);
}

static lv_obj_t *build_wifi_qr_screen(void)
{
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(screen, 0, 0);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
	wifi_qr_code = lv_qrcode_create(screen);
	lv_qrcode_set_size(wifi_qr_code, 72);
	lv_qrcode_set_dark_color(wifi_qr_code, lv_color_black());
	lv_qrcode_set_light_color(wifi_qr_code, lv_color_white());
	lv_qrcode_set_quiet_zone(wifi_qr_code, true);
	lv_qrcode_set_data(wifi_qr_code, "SCREENPLUS");
	lv_obj_center(wifi_qr_code);
	lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(screen, close_wifi_qr, LV_EVENT_ALL, NULL);
	return screen;
}

static void create_wifi_band_row(lv_obj_t *parent, unsigned int band, int y,
				 const char *title)
{
	lv_obj_t *row = lv_obj_create(parent);
	wifi_band_rows[band] = row;
	lv_obj_set_pos(row, 0, y);
	lv_obj_set_size(row, 284, 37);
	lv_obj_set_style_radius(row, 0, 0);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE |
		LV_OBJ_FLAG_GESTURE_BUBBLE);
	wifi_band_titles[band] = create_label(row, title, 8, 10,
		&lv_font_montserrat_14, app_config.accent_colour);
	wifi_ssid_labels[band] = create_label(row, "--", 55, 2,
		&lv_font_montserrat_14, app_config.primary_colour);
	wifi_password_labels[band] = create_label(row, "KEY ********", 55, 20,
		&lv_font_montserrat_14, app_config.primary_colour);
	lv_obj_set_width(wifi_band_titles[band], 42);
	lv_obj_set_width(wifi_ssid_labels[band], 221);
	lv_obj_set_width(wifi_password_labels[band], 221);
	lv_label_set_long_mode(wifi_ssid_labels[band], LV_LABEL_LONG_DOT);
	lv_label_set_long_mode(wifi_password_labels[band], LV_LABEL_LONG_DOT);
	lv_obj_clear_flag(wifi_band_titles[band], LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(wifi_ssid_labels[band], LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(wifi_password_labels[band], LV_OBJ_FLAG_CLICKABLE |
		LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(wifi_password_labels[band], password_event,
		LV_EVENT_ALL, (void *)(uintptr_t)band);
	lv_obj_add_event_cb(row, password_event, LV_EVENT_ALL, (void *)(uintptr_t)band);
}

static lv_obj_t *build_wifi_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_WIFI);
	create_divider(screen, 8, 37, 268, 2);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_WIFI, "wifi_2g"))
		create_wifi_band_row(screen, 0, 0, "2.4G");
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_WIFI, "wifi_5g"))
		create_wifi_band_row(screen, 1, 39, "5G");
	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
}

static lv_obj_t *build_openclash_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_OPENCLASH);
	create_label(screen, "OPENCLASH", 8, 3, &lv_font_montserrat_14,
		app_config.accent_colour);
	openclash_state_label = create_label(screen, "N/A", 116, 3,
		small_ui_font(), app_config.primary_colour);
	create_divider(screen, 8, 24, 268, 2);
	openclash_download_label = create_label(screen, "DOWN --", 8, 29,
		&lv_font_montserrat_14, app_config.primary_colour);
	openclash_upload_label = create_label(screen, "UP --", 146, 29,
		&lv_font_montserrat_14, app_config.primary_colour);
	openclash_connections_label = create_label(screen, "CONN --", 8, 51,
		&lv_font_montserrat_14, app_config.secondary_colour);
	openclash_totals_label = create_label(screen, "DOWN --  UP --", 84, 51,
		&lv_font_montserrat_14, app_config.secondary_colour);
	lv_obj_set_width(openclash_download_label, 128);
	lv_obj_set_width(openclash_upload_label, 130);
	lv_obj_set_width(openclash_totals_label, 196);
	lv_label_set_long_mode(openclash_download_label, LV_LABEL_LONG_DOT);
	lv_label_set_long_mode(openclash_upload_label, LV_LABEL_LONG_DOT);
	lv_label_set_long_mode(openclash_totals_label, LV_LABEL_LONG_DOT);
	if (!screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_OPENCLASH, "rates")) {
		lv_obj_add_flag(openclash_download_label, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(openclash_upload_label, LV_OBJ_FLAG_HIDDEN);
	}
	if (!screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_OPENCLASH, "connections"))
		lv_obj_add_flag(openclash_connections_label, LV_OBJ_FLAG_HIDDEN);
	if (!screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_OPENCLASH, "totals"))
		lv_obj_add_flag(openclash_totals_label, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
}

static void format_total_bytes(uint64_t bytes, char *buffer, size_t size)
{
	if (bytes >= 1024ULL * 1024ULL * 1024ULL)
		snprintf(buffer, size, "%.1fG", (double)bytes / (1024.0 * 1024.0 * 1024.0));
	else if (bytes >= 1024ULL * 1024ULL)
		snprintf(buffer, size, "%.0fM", (double)bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024ULL)
		snprintf(buffer, size, "%.0fK", (double)bytes / 1024.0);
	else
		snprintf(buffer, size, "%lluB", (unsigned long long)bytes);
}

static void apply_system_snapshot(lv_timer_t *timer)
{
	(void)timer;
	if (drag_tracking || page_animation_running)
		return;
	struct system_snapshot snapshot;
	pthread_mutex_lock(&snapshot_mutex);
	bool ready = snapshot_ready;
	uint64_t generation = snapshot_generation;
	if (ready)
		snapshot = latest_snapshot;
	pthread_mutex_unlock(&snapshot_mutex);
	if (!ready || generation == applied_snapshot_generation)
		return;
	applied_snapshot_generation = generation;

	char text[160];
	if (network_lan_label)
		set_label_text_if_changed(network_lan_label,
			snapshot.lan_ipv4[0] ? snapshot.lan_ipv4 : "NO IP");

	const struct uplink_info *uplinks[] = {
		&snapshot.ethernet, &snapshot.repeater,
		&snapshot.tethering, &snapshot.cellular
	};
	static const char *const titles[] = { "ETH", "REPEATER", "USB", "CELLULAR" };
	const struct uplink_info *active = NULL;
	const char *active_title = NULL;
	for (unsigned int index = 0; index < 4; ++index) {
		const struct uplink_info *uplink = uplinks[index];
		if (network_uplink_labels[index]) {
			set_label_text_if_changed(network_uplink_labels[index], titles[index]);
			lv_obj_set_style_text_color(network_uplink_labels[index],
				colour(network_state_colour(uplink->state)), 0);
		}
		if (!active ||
		    (uplink->state == SCREENPLUS_STATE_ACTIVE &&
		     active->state != SCREENPLUS_STATE_ACTIVE) ||
		    (uplink->state == SCREENPLUS_STATE_CONNECTED &&
		     active->state != SCREENPLUS_STATE_ACTIVE &&
		     active->state != SCREENPLUS_STATE_CONNECTED) ||
		    (uplink->state == SCREENPLUS_STATE_CONNECTING &&
		     active->state != SCREENPLUS_STATE_ACTIVE &&
		     active->state != SCREENPLUS_STATE_CONNECTED &&
		     active->state != SCREENPLUS_STATE_CONNECTING)) {
			active = uplink;
			active_title = titles[index];
		}
	}
	if (network_wan_value) {
		if (active && (active->state == SCREENPLUS_STATE_ACTIVE ||
		    active->state == SCREENPLUS_STATE_CONNECTED))
			snprintf(text, sizeof(text), "%s  %s", active_title,
				active->ipv4[0] ? active->ipv4 : "UP");
		else if (active && active->state == SCREENPLUS_STATE_CONNECTING)
			snprintf(text, sizeof(text), "%s  WAIT", active_title);
		else
			strcpy(text, "OFF  NO UPLINK");
		set_label_text_if_changed(network_wan_value, text);
	}

	const struct wifi_info *wifi_bands[2] = { &snapshot.wifi_2g, &snapshot.wifi_5g };
	for (unsigned int band = 0; band < 2; ++band)
		update_wifi_band_display(band, wifi_bands[band]);

	if (openclash_state_label) {
		unsigned int openclash_hex = state_colour(snapshot.openclash.state);
		set_label_text_if_changed(openclash_state_label, display_state_text(snapshot.openclash.state));
		lv_obj_set_style_text_color(openclash_state_label, colour(openclash_hex), 0);
		char download[12];
		char upload[12];
		char download_total[12];
		char upload_total[12];
		if (snapshot.openclash.metrics_available) {
			metrics_format_rate(snapshot.openclash.download_bytes_per_second,
				download, sizeof(download));
			metrics_format_rate(snapshot.openclash.upload_bytes_per_second,
				upload, sizeof(upload));
			format_total_bytes(snapshot.openclash.download_total_bytes,
				download_total, sizeof(download_total));
			format_total_bytes(snapshot.openclash.upload_total_bytes,
				upload_total, sizeof(upload_total));
			snprintf(text, sizeof(text), "DOWN %s/s", download);
			set_label_text_if_changed(openclash_download_label, text);
			snprintf(text, sizeof(text), "UP %s/s", upload);
			set_label_text_if_changed(openclash_upload_label, text);
			snprintf(text, sizeof(text), "CONN %u", snapshot.openclash.connection_count);
			set_label_text_if_changed(openclash_connections_label, text);
			snprintf(text, sizeof(text), "DOWN %s  UP %s", download_total, upload_total);
			set_label_text_if_changed(openclash_totals_label, text);
		} else {
			set_label_text_if_changed(openclash_download_label, "DOWN --");
			set_label_text_if_changed(openclash_upload_label, "UP --");
			set_label_text_if_changed(openclash_connections_label, "CONN --");
			set_label_text_if_changed(openclash_totals_label, "DOWN --  UP --");
		}
	}
}

static void *system_worker(void *unused)
{
	(void)unused;
	while (running) {
		struct system_snapshot snapshot;
		system_info_sample(&system_state, &snapshot);
		pthread_mutex_lock(&snapshot_mutex);
		latest_snapshot = snapshot;
		snapshot_ready = true;
		++snapshot_generation;
		pthread_mutex_unlock(&snapshot_mutex);
		for (unsigned int count = 0; count < 20 && running; ++count)
			sleep_milliseconds(100);
	}
	return NULL;
}

static void build_ui(void)
{
	dashboard_screen = lv_obj_create(NULL);
	style_screen(dashboard_screen);
	clock_screen = build_clock_screen(dashboard_screen);
	status_screen = build_status_screen(dashboard_screen);
	traffic_screen = build_traffic_screen(dashboard_screen);
	network_screen = build_network_screen(dashboard_screen);
	wifi_screen = build_wifi_screen(dashboard_screen);
	wifi_qr_screen = build_wifi_qr_screen();
	openclash_screen = build_openclash_screen(dashboard_screen);
	screens_by_page[SCREENPLUS_PAGE_HOME] = clock_screen;
	screens_by_page[SCREENPLUS_PAGE_STATUS] = status_screen;
	screens_by_page[SCREENPLUS_PAGE_TRAFFIC] = traffic_screen;
	screens_by_page[SCREENPLUS_PAGE_NETWORK] = network_screen;
	screens_by_page[SCREENPLUS_PAGE_WIFI] = wifi_screen;
	screens_by_page[SCREENPLUS_PAGE_OPENCLASH] = openclash_screen;
	for (int page = 0; page < SCREENPLUS_PAGE_COUNT; ++page)
		lv_obj_add_flag(screens_by_page[page], LV_OBJ_FLAG_HIDDEN);
	bool added[SCREENPLUS_PAGE_COUNT] = { false };
	screen_count = 0;
	for (;;) {
		int selected = -1;
		for (int page = 0; page < SCREENPLUS_PAGE_COUNT; ++page) {
			if (added[page] || !app_config.pages[page].enabled)
				continue;
			if (selected < 0 || app_config.pages[page].order < app_config.pages[selected].order)
				selected = page;
		}
		if (selected < 0)
			break;
		added[selected] = true;
		lv_obj_clear_flag(screens_by_page[selected], LV_OBJ_FLAG_HIDDEN);
		screens[screen_count++] = screens_by_page[selected];
	}
	if (!screen_count) {
		lv_obj_clear_flag(clock_screen, LV_OBJ_FLAG_HIDDEN);
		screens[screen_count++] = clock_screen;
	}
	current_screen_index = 0;
	if (requested_start_page >= 0) {
		for (unsigned int index = 0; index < screen_count; ++index) {
			if (screens[index] == screens_by_page[requested_start_page]) {
				current_screen_index = index;
				break;
			}
		}
	}
	place_pages(0);
	lv_screen_load(dashboard_screen);
	lv_timer_create(update_clock, 250, NULL);
	lv_timer_create(apply_metrics, 100, NULL);
	lv_timer_create(apply_system_snapshot, 500, NULL);
	lv_timer_create(manage_idle_state, 250, NULL);
	if (app_config.auto_carousel)
		lv_timer_create(auto_carousel, app_config.carousel_interval_seconds * 1000U, NULL);
	update_clock(NULL);
}

static int parse_integer(const char *text, int minimum, int maximum, const char *name)
{
	char *end = NULL;
	errno = 0;
	long value = strtol(text, &end, 10);
	if (errno || end == text || !end || *end != '\0' || value < minimum || value > maximum) {
		fprintf(stderr, "screenplus: invalid %s: %s\n", name, text);
		exit(2);
	}
	return (int)value;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [--config path] [--rotation 90|270] [--start-page name]\n"
		"          [--duration seconds] [--no-touch]\n"
		"          [--metrics-once] [--system-info-once] [--config-once]\n"
		"          [--framebuffer path] [--input path]\n",
		program);
}

static struct options parse_options(int argc, char **argv)
{
	struct options options = {
		.framebuffer = DEFAULT_FRAMEBUFFER,
		.input = DEFAULT_INPUT,
		.config = "/etc/config/screenplus",
		.rotation = 90,
		.duration_seconds = 0,
		.touch_enabled = true,
		.metrics_once = false,
		.system_info_once = false,
		.start_page = -1,
	};
	for (int index = 1; index < argc; ++index) {
		if (strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
			options.config = argv[++index];
		} else if (strcmp(argv[index], "--rotation") == 0 && index + 1 < argc) {
			options.rotation = parse_integer(argv[++index], 90, 270, "rotation");
			options.rotation_override = true;
			if (options.rotation != 90 && options.rotation != 270) {
				fputs("screenplus: rotation must be 90 or 270\n", stderr);
				exit(2);
			}
		} else if (strcmp(argv[index], "--duration") == 0 && index + 1 < argc) {
			options.duration_seconds = parse_integer(argv[++index], 1, 86400, "duration");
		} else if (strcmp(argv[index], "--start-page") == 0 && index + 1 < argc) {
			const char *name = argv[++index];
			for (int page = 0; page < SCREENPLUS_PAGE_COUNT; ++page) {
				if (strcmp(name, screenplus_page_name((enum screenplus_page_id)page)) == 0)
					options.start_page = page;
			}
			if (options.start_page < 0) {
				fprintf(stderr, "screenplus: unknown start page: %s\n", name);
				exit(2);
			}
		} else if (strcmp(argv[index], "--framebuffer") == 0 && index + 1 < argc) {
			options.framebuffer = argv[++index];
		} else if (strcmp(argv[index], "--input") == 0 && index + 1 < argc) {
			options.input = argv[++index];
		} else if (strcmp(argv[index], "--no-touch") == 0) {
			options.touch_enabled = false;
		} else if (strcmp(argv[index], "--metrics-once") == 0) {
			options.metrics_once = true;
		} else if (strcmp(argv[index], "--system-info-once") == 0) {
			options.system_info_once = true;
		} else if (strcmp(argv[index], "--config-once") == 0) {
			options.config_once = true;
		} else if (strcmp(argv[index], "--help") == 0) {
			usage(argv[0]);
			exit(0);
		} else {
			usage(argv[0]);
			exit(2);
		}
	}
	return options;
}

int main(int argc, char **argv)
{
	struct options options = parse_options(argc, argv);
	if (screenplus_config_load(&app_config, options.config) != 0)
		fprintf(stderr, "screenplus: cannot read configuration %s: %s\n",
			options.config, strerror(errno));
	if (screenplus_timezone_load(&app_config, "/etc/config/system") == 0 &&
	    app_config.timezone_rule[0]) {
		setenv("TZ", app_config.timezone_rule, 1);
		tzset();
	}
	if (options.rotation_override)
		app_config.rotation = options.rotation;
	else
		options.rotation = app_config.rotation;
	requested_start_page = options.start_page;
	struct sigaction action = { .sa_handler = on_signal };
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	if (options.config_once) {
		printf("{\"enabled\":%s,\"language\":\"%s\",\"brightness\":%d,"
		       "\"rotation\":%d,\"always_on\":%s,\"swipe_loop\":%s,"
		       "\"auto_carousel\":%s,\"carousel_interval\":%u,"
		       "\"timezone_rule\":\"%s\",\"timezone_name\":\"%s\",\"pages\":{",
		       app_config.enabled ? "true" : "false", app_config.chinese ? "zh_cn" : "en",
		       app_config.brightness, app_config.rotation,
		       app_config.always_on ? "true" : "false",
		       app_config.swipe_loop ? "true" : "false",
		       app_config.auto_carousel ? "true" : "false",
		       app_config.carousel_interval_seconds,
		       app_config.timezone_rule, app_config.timezone_name);
		for (int page = 0; page < SCREENPLUS_PAGE_COUNT; ++page) {
			printf("%s\"%s\":{\"enabled\":%s,\"order\":%d,\"field_count\":%zu}",
			       page ? "," : "", screenplus_page_name((enum screenplus_page_id)page),
			       app_config.pages[page].enabled ? "true" : "false",
			       app_config.pages[page].order, app_config.pages[page].field_count);
		}
		puts("}}");
		return 0;
	}
	if (options.metrics_once) {
		struct metrics_state state;
		struct system_metrics metrics;
		metrics_state_initialize(&state);
		metrics_sample(&state, &metrics);
		sleep_milliseconds(1000);
		int result = metrics_sample(&state, &metrics);
		printf("{\"cpu_percent\":%.2f,\"temperature_celsius\":%.2f,"
		       "\"fan_rpm\":%u,"
		       "\"memory_percent\":%.2f,\"memory_used_bytes\":%llu,"
		       "\"memory_total_bytes\":%llu,\"storage_percent\":%.2f,"
		       "\"storage_used_bytes\":%llu,\"storage_total_bytes\":%llu,"
		       "\"disk_read_bytes_per_second\":%.2f,"
		       "\"disk_write_bytes_per_second\":%.2f,"
		       "\"network_interface\":\"%s\","
		       "\"network_hardware_accelerated\":%s,"
		       "\"network_acceleration\":\"%s\","
		       "\"network_receive_bytes_per_second\":%.2f,"
		       "\"network_transmit_bytes_per_second\":%.2f,"
		       "\"uptime_seconds\":%llu}\n",
		       metrics.cpu_percent, metrics.temperature_celsius, metrics.fan_rpm,
		       metrics.memory_percent, (unsigned long long)metrics.memory_used_bytes,
		       (unsigned long long)metrics.memory_total_bytes, metrics.storage_percent,
		       (unsigned long long)metrics.storage_used_bytes,
		       (unsigned long long)metrics.storage_total_bytes,
		       metrics.disk_read_bytes_per_second, metrics.disk_write_bytes_per_second,
		       metrics.network_interface,
		       metrics.network_hardware_accelerated ? "true" : "false",
		       metrics_acceleration_text(metrics.network_acceleration),
		       metrics.network_receive_bytes_per_second,
		       metrics.network_transmit_bytes_per_second,
		       (unsigned long long)metrics.uptime_seconds);
		return result == 0 ? 0 : 1;
	}
	if (options.system_info_once) {
		struct system_info_state state;
		struct system_snapshot snapshot;
		system_info_state_initialize(&state);
		system_info_sample(&state, &snapshot);
		sleep_milliseconds(1000);
		system_info_sample(&state, &snapshot);
		printf("{\"ethernet\":\"%s\",\"repeater\":\"%s\","
		       "\"tethering\":\"%s\",\"cellular\":\"%s\","
		       "\"ethernet_ipv4\":\"%s\",\"ethernet_gateway\":\"%s\","
		       "\"ethernet_dns\":\"%s\",\"repeater_ipv4\":\"%s\","
		       "\"tethering_ipv4\":\"%s\",\"cellular_ipv4\":\"%s\","
		       "\"wifi_2g_enabled\":%s,\"wifi_5g_enabled\":%s,"
		       "\"wifi_mlo_enabled\":%s,\"wifi_2g_channel\":\"%s\","
		       "\"wifi_5g_channel\":\"%s\",\"lan_ipv4\":\"%s\","
		       "\"port0_role\":\"%s\",\"port0_carrier\":%s,"
		       "\"port0_speed\":%u,\"port0_ipv4\":\"%s\",\"port1_role\":\"%s\","
		       "\"port1_carrier\":%s,\"port1_speed\":%u,"
		       "\"port1_ipv4\":\"%s\","
		       "\"openclash\":\"%s\",\"openclash_mode\":\"%s\","
		       "\"openclash_metrics\":%s,"
		       "\"openclash_download_bps\":%.2f,\"openclash_upload_bps\":%.2f,"
		       "\"openclash_connections\":%u,"
		       "\"openclash_download_total\":%llu,"
		       "\"openclash_upload_total\":%llu}\n",
		       system_info_state_text(snapshot.ethernet.state),
		       system_info_state_text(snapshot.repeater.state),
		       system_info_state_text(snapshot.tethering.state),
		       system_info_state_text(snapshot.cellular.state),
		       snapshot.ethernet.ipv4, snapshot.ethernet.gateway, snapshot.ethernet.dns,
		       snapshot.repeater.ipv4, snapshot.tethering.ipv4, snapshot.cellular.ipv4,
		       snapshot.wifi_2g.enabled ? "true" : "false",
		       snapshot.wifi_5g.enabled ? "true" : "false",
		       snapshot.wifi_mlo.enabled ? "true" : "false",
		       snapshot.wifi_2g.channel, snapshot.wifi_5g.channel, snapshot.lan_ipv4,
		       snapshot.ports[0].role, snapshot.ports[0].carrier ? "true" : "false",
		       snapshot.ports[0].speed_mbps, snapshot.ports[0].ipv4, snapshot.ports[1].role,
		       snapshot.ports[1].carrier ? "true" : "false",
		       snapshot.ports[1].speed_mbps, snapshot.ports[1].ipv4,
		       system_info_state_text(snapshot.openclash.state), snapshot.openclash.mode,
		       snapshot.openclash.metrics_available ? "true" : "false",
		       snapshot.openclash.download_bytes_per_second,
		       snapshot.openclash.upload_bytes_per_second,
		       snapshot.openclash.connection_count,
		       (unsigned long long)snapshot.openclash.download_total_bytes,
		       (unsigned long long)snapshot.openclash.upload_total_bytes);
		return 0;
	}

	lv_init();
	lv_tick_set_cb(monotonic_milliseconds);
	lv_display_t *display = lv_linux_fbdev_create();
	if (!display || lv_linux_fbdev_set_file(display, options.framebuffer) != LV_RESULT_OK) {
		fprintf(stderr, "screenplus: failed to initialize framebuffer %s\n", options.framebuffer);
		lv_deinit();
		return 1;
	}
	lv_display_set_rotation(display,
		options.rotation == 90 ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_270);
	main_display = display;

	if (options.touch_enabled) {
		lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, options.input);
		if (!touch) {
			fprintf(stderr, "screenplus: failed to initialize touch input %s\n", options.input);
			lv_deinit();
			return 1;
		}
		lv_indev_set_display(touch, display);
	}

	backlight_on = false;
	set_backlight(true);
	build_ui();
	metrics_state_initialize(&metric_state);
	if (pthread_create(&metrics_worker_thread, NULL, metrics_worker, NULL) == 0)
		metrics_worker_started = true;
	else
		fputs("screenplus: failed to start metrics worker\n", stderr);
	system_info_state_initialize(&system_state);
	if (pthread_create(&system_worker_thread, NULL, system_worker, NULL) == 0)
		system_worker_started = true;
	else
		fputs("screenplus: failed to start system information worker\n", stderr);
	fprintf(stderr, "screenplus: running at %" LV_PRId32 "x%" LV_PRId32 ", rotation=%d\n",
		lv_display_get_horizontal_resolution(display),
		lv_display_get_vertical_resolution(display), options.rotation);

	uint32_t started = monotonic_milliseconds();
	while (running) {
		if (reload_requested) {
			fputs("screenplus: configuration reload requested\n", stderr);
			break;
		}
		if (options.duration_seconds > 0 &&
		    monotonic_milliseconds() - started >= (uint32_t)options.duration_seconds * 1000U)
			break;
		uint32_t wait = lv_timer_handler();
		if (wait == LV_NO_TIMER_READY || wait > 20U)
			wait = 20U;
		if (wait < 1U)
			wait = 1U;
		sleep_milliseconds(wait);
	}

	running = 0;
	if (metrics_worker_started)
		pthread_join(metrics_worker_thread, NULL);
	if (system_worker_started)
		pthread_join(system_worker_thread, NULL);
	lv_deinit();
	for (int page = 0; page < SCREENPLUS_PAGE_COUNT; ++page) {
		free(background_buffers[page]);
		background_buffers[page] = NULL;
	}
	if (reload_requested) {
		execv(argv[0], argv);
		fprintf(stderr, "screenplus: reload exec failed: %s\n", strerror(errno));
	}
	fputs("screenplus: stopped\n", stderr);
	return 0;
}
