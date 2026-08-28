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
#include <src/drivers/evdev/lv_evdev.h>
#include <src/libs/qrcode/lv_qrcode.h>

#include "app_config.h"
#include "metrics.h"
#include "safe_exec.h"
#include "screenplus_fbdev.h"
#include "system_info.h"

LV_FONT_DECLARE(screenplus_ui_14);
LV_FONT_DECLARE(screenplus_reset_16);

#define DEFAULT_FRAMEBUFFER "/dev/fb0"
#define DEFAULT_INPUT "/dev/input/event0"
#define RESET_BUTTON_MARKER "/tmp/screenplus-reset-button"
#define RESET_NETWORK_MS 3000U
#define RESET_FACTORY_MS 8000U
#define RESET_CANCEL_MS 20000U
#define RESET_CONFIRM_MS 3000U
/* Grace period past RESET_CANCEL_MS before a still-pressed marker is treated
 * as stale. Leaves the normal cancel-and-release flow untouched. */
#define RESET_RECOVERY_MS (RESET_CANCEL_MS + 5000U)
#define RESET_STAGE_COUNT 3U
#define TOUCH_READ_PERIOD_MS 10U
#define PAGE_DRAG_START_PX 6

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
static lv_obj_t *reset_screen;
static lv_obj_t *reset_stage_pages[RESET_STAGE_COUNT];
static lv_obj_t *reset_stage_main_labels[RESET_STAGE_COUNT];
static lv_obj_t *reset_stage_detail_labels[RESET_STAGE_COUNT];
static lv_obj_t *reset_stage_progress[RESET_STAGE_COUNT];
static bool reset_overlay_active;
static unsigned int reset_visible_stage = RESET_STAGE_COUNT;
static unsigned int reset_last_remaining = UINT32_MAX;
static bool reset_cancelled;
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
struct transfer_measurement {
	lv_obj_t *icon;
	lv_obj_t *rate_number;
	lv_obj_t *rate_unit;
	lv_obj_t *separator;
	lv_obj_t *total_number;
	lv_obj_t *total_unit;
};
static struct transfer_measurement traffic_upload_measurement;
static struct transfer_measurement traffic_download_measurement;
static lv_obj_t *traffic_connections_value;
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
static lv_obj_t *network_wan_title;
static lv_obj_t *network_wan_value;
static lv_obj_t *network_lan_title;
static lv_obj_t *openclash_state_label;
static lv_obj_t *openclash_toggle;
static struct transfer_measurement openclash_download_measurement;
static struct transfer_measurement openclash_upload_measurement;
static lv_obj_t *openclash_connections_value;
static lv_obj_t *openclash_cpu_value;
static lv_obj_t *openclash_memory_value;
static lv_obj_t *openclash_memory_unit;
static struct system_info_state system_state;
static struct system_snapshot latest_snapshot;
static pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t system_worker_thread;
static bool system_worker_started;
static bool snapshot_ready;
static uint64_t snapshot_generation;
static uint64_t applied_snapshot_generation;
static bool password_revealed[2];
static bool wifi_touch_active;
static unsigned int wifi_touch_band;
static bool password_long_press_handled;
static uint32_t password_reveal_deadline[2];
static unsigned int qr_wifi_band;
static struct screenplus_config app_config;
static lv_display_t *main_display;
static bool backlight_on = true;
static void *background_buffers[SCREENPLUS_PAGE_COUNT];
static int requested_start_page = -1;
static int requested_start_index = -1;
static bool drag_tracking;
static bool drag_moved;
static bool page_animation_running;
static lv_point_t drag_start_point;
static int32_t drag_offset;
static int pending_page_delta;
/* OpenClash toggle handshake between the UI thread and the system worker.
 * The command/state fields below are guarded by openclash_toggle_mutex;
 * openclash_toggle_syncing is only accessed from the LVGL/UI thread.
 * openclash_toggle_request_id is a generation counter bumped on every newly
 * accepted request: the worker remembers the id it started with and only
 * publishes its outcome while that id still identifies the pending request,
 * so a command that outlives the 30s UI timeout cannot clobber the state of
 * a newer request. */
static pthread_mutex_t openclash_toggle_mutex = PTHREAD_MUTEX_INITIALIZER;
static int openclash_toggle_request;
static int openclash_toggle_busy;
static int openclash_toggle_target;
static int openclash_toggle_failed;
static int openclash_toggle_command_done;
static uint32_t openclash_toggle_deadline;
static unsigned int openclash_toggle_request_id;
static bool openclash_toggle_syncing;

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

static double monotonic_seconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
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
	return app_config.chinese ? &screenplus_ui_14 :
		&lv_font_montserrat_14;
}

static const lv_font_t *reset_ui_font(void)
{
	return app_config.chinese ? &screenplus_reset_16 :
		&lv_font_montserrat_16;
}

/* Non-home pages use one shared hierarchy: label, primary value, detail. */
static const lv_font_t *ui_label_font(void)
{
	return &lv_font_montserrat_14;
}

static const lv_font_t *ui_value_font(void)
{
	return &lv_font_montserrat_18;
}

static const lv_font_t *ui_detail_font(void)
{
	return &lv_font_montserrat_14;
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
static void set_label_text_if_changed(lv_obj_t *label, const char *text);
static void set_page_visible(lv_obj_t *page, bool visible);

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

static int responsive_row_y(unsigned int index, unsigned int count)
{
	if (!count)
		return 29;
	return (int)(76U * (2U * index + 1U) / (2U * count)) - 9;
}

static void create_responsive_row_dividers(lv_obj_t *parent, unsigned int count)
{
	for (unsigned int index = 1; index < count; ++index) {
		int y = (int)(76U * index / count) - 1;
		create_divider(parent, 8, y, 268, 2);
	}
}

static void set_fixed_text(lv_obj_t *label, int width, lv_text_align_t alignment)
{
	lv_obj_set_width(label, width);
	lv_obj_set_height(label, 18);
	lv_obj_set_style_text_align(label, alignment, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
}

static struct transfer_measurement create_transfer_measurement(
	lv_obj_t *parent, int x, int y, const char *icon, unsigned int icon_colour)
{
	struct transfer_measurement measurement = {0};
	measurement.icon = create_label(parent, icon, x, y, ui_label_font(), icon_colour);
	set_fixed_text(measurement.icon, 12, LV_TEXT_ALIGN_CENTER);
	measurement.rate_number = create_label(parent, "0", x + 16, y,
		ui_label_font(), app_config.primary_colour);
	measurement.rate_unit = create_label(parent, "K/s", x + 43, y,
		ui_label_font(), app_config.secondary_colour);
	measurement.separator = lv_obj_create(parent);
	lv_obj_set_pos(measurement.separator, x + 70, y + 7);
	lv_obj_set_size(measurement.separator, 3, 3);
	lv_obj_set_style_radius(measurement.separator, 2, 0);
	lv_obj_set_style_border_width(measurement.separator, 0, 0);
	lv_obj_set_style_pad_all(measurement.separator, 0, 0);
	lv_obj_set_style_bg_color(measurement.separator,
		colour(app_config.secondary_colour), 0);
	lv_obj_set_style_bg_opa(measurement.separator, LV_OPA_COVER, 0);
	lv_obj_clear_flag(measurement.separator,
		LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	measurement.total_number = create_label(parent, "0", x + 76, y,
		ui_label_font(), app_config.primary_colour);
	measurement.total_unit = create_label(parent, "KB", x + 104, y,
		ui_label_font(), app_config.secondary_colour);
	set_fixed_text(measurement.rate_number, 27, LV_TEXT_ALIGN_RIGHT);
	set_fixed_text(measurement.rate_unit, 24, LV_TEXT_ALIGN_LEFT);
	set_fixed_text(measurement.total_number, 27, LV_TEXT_ALIGN_RIGHT);
	set_fixed_text(measurement.total_unit, 24, LV_TEXT_ALIGN_LEFT);
	return measurement;
}

static void format_fixed_quantity(double bytes, bool rate,
				  char *number, size_t number_size,
				  char *unit, size_t unit_size)
{
	static const char prefixes[] = { 'K', 'M', 'G' };
	double value = bytes > 0.0 ? bytes / 1024.0 : 0.0;
	unsigned int prefix = 0;
	while (value > 999.0 && prefix < 2) {
		value /= 1024.0;
		++prefix;
	}
	unsigned int rounded = (unsigned int)(value + 0.5);
	if (rounded > 999 && prefix < 2) {
		rounded = (unsigned int)(value / 1024.0 + 0.5);
		++prefix;
	}
	if (rounded > 999)
		rounded = 999;
	snprintf(number, number_size, "%u", rounded);
	snprintf(unit, unit_size, rate ? "%c/s" : "%cB", prefixes[prefix]);
}

static void update_transfer_measurement(struct transfer_measurement *measurement,
					double bytes_per_second, uint64_t total_bytes)
{
	if (!measurement || !measurement->rate_number)
		return;
	char number[8];
	char unit[8];
	format_fixed_quantity(bytes_per_second, true, number, sizeof(number), unit, sizeof(unit));
	set_label_text_if_changed(measurement->rate_number, number);
	set_label_text_if_changed(measurement->rate_unit, unit);
	format_fixed_quantity((double)total_bytes, false, number, sizeof(number), unit, sizeof(unit));
	set_label_text_if_changed(measurement->total_number, number);
	set_label_text_if_changed(measurement->total_unit, unit);
}

static void set_transfer_part_visible(struct transfer_measurement *measurement,
				      bool rates, bool totals)
{
	lv_obj_t *rate_objects[] = { measurement->rate_number, measurement->rate_unit };
	lv_obj_t *total_objects[] = {
		measurement->separator, measurement->total_number, measurement->total_unit
	};
	for (unsigned int index = 0; index < 2; ++index)
		set_page_visible(rate_objects[index], rates);
	for (unsigned int index = 0; index < 3; ++index)
		set_page_visible(total_objects[index], totals);
	set_page_visible(measurement->icon, rates || totals);
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
	update_transfer_measurement(&traffic_download_measurement,
		traffic_target_download, metrics.network_receive_total_bytes);
	update_transfer_measurement(&traffic_upload_measurement,
		traffic_target_upload, metrics.network_transmit_total_bytes);
	if (traffic_connections_value) {
		snprintf(main_text, sizeof(main_text), "%u",
			metrics.network_connection_count);
		set_label_text_if_changed(traffic_connections_value, main_text);
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
	if (!input)
		return;
	/*
	 * Gesture tracking runs for every page count. The Wi-Fi page relies on it
	 * for movement detection and for suppressing the QR long press once a
	 * swipe has started, and the release paths must always clear
	 * drag_tracking: apply_metrics() and apply_system_snapshot() pause while
	 * that flag is set, so a stuck flag would freeze all data updates. Only
	 * the page translation and settle animation require two or more pages.
	 */
	bool can_translate = screen_count >= 2;
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
			if (abs(horizontal) < PAGE_DRAG_START_PX ||
			    abs(horizontal) <= abs(vertical))
				return;
			drag_moved = true;
		}
		/* A page drag owns the gesture; never let it age into a long press. */
		lv_indev_reset_long_press(input);
		if (!can_translate)
			return;
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
		if (!drag_moved || !can_translate) {
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
		reset_overlay_active ||
		lv_display_get_inactive_time(main_display) < app_config.idle_timeout_seconds * 1000U;
	set_backlight(should_be_on);
	for (unsigned int band = 0; band < 2; ++band) {
		if (password_revealed[band] && password_reveal_deadline[band] &&
		    (int32_t)(monotonic_milliseconds() - password_reveal_deadline[band]) >= 0) {
			password_revealed[band] = false;
			password_reveal_deadline[band] = 0;
			applied_snapshot_generation = 0;
		}
	}
}

static lv_obj_t *build_clock_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_HOME);
	bool show_time = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_HOME, "time");
	bool show_detail = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_HOME, "date") ||
		screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_HOME, "weekday") ||
		screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_HOME, "timezone");

	lv_obj_t *accent = lv_obj_create(screen);
	lv_obj_set_pos(accent, 14, show_time && show_detail ? 8 : 18);
	lv_obj_set_size(accent, 4, show_time && show_detail ? 60 : 40);
	lv_obj_set_style_radius(accent, 0, 0);
	lv_obj_set_style_border_width(accent, 0, 0);
	lv_obj_set_style_bg_color(accent, colour(app_config.accent_colour), 0);
	lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

	clock_label = create_label(screen, "--:--", 28,
		show_detail ? 11 : 22, &lv_font_montserrat_28,
		app_config.primary_colour);
	lv_obj_set_width(clock_label, 246);
	lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_letter_space(clock_label, -1, 0);
	date_label = create_label(screen, "---- -- --", 28,
		show_time ? 48 : 29, small_ui_font(),
		app_config.primary_colour);
	lv_obj_set_width(date_label, 246);
	lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_LEFT, 0);
	if (!show_time)
		lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
	if (!show_detail)
		lv_obj_add_flag(date_label, LV_OBJ_FLAG_HIDDEN);
	if (!show_time && !show_detail)
		lv_obj_add_flag(accent, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
}

static void create_metric(lv_obj_t *parent, const char *title, int x,
			  lv_obj_t **value_label, lv_obj_t **detail_label)
{
	create_label(parent, title, x + 8, 4, ui_label_font(),
		app_config.accent_colour);
	*value_label = create_label(parent, "--", x + 8, 24, ui_value_font(),
		app_config.primary_colour);
	*detail_label = create_label(parent, "--", x + 8, 50, ui_detail_font(),
		app_config.secondary_colour);
	lv_obj_set_width(*value_label, 78);
	lv_obj_set_width(*detail_label, 78);
	lv_label_set_long_mode(*value_label, LV_LABEL_LONG_DOT);
	lv_label_set_long_mode(*detail_label, LV_LABEL_LONG_DOT);
}

static lv_obj_t *build_status_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_STATUS);
	bool visible[] = {
		screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_STATUS, "cpu"),
		screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_STATUS, "memory"),
		screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_STATUS, "fan")
	};
	unsigned int count = visible[0] + visible[1] + visible[2];
	int total_width = count ? (int)(count * 95U - 1U) : 0;
	int start = (284 - total_width) / 2;
	unsigned int slot = 0;
	for (unsigned int index = 0; index < 3; ++index) {
		if (!visible[index])
			continue;
		int x = start + (int)(slot * 95U);
		if (slot)
			create_divider(screen, x - 1, 10, 2, 56);
		if (index == 0)
			create_metric(screen, "CPU", x, &cpu_value, &cpu_detail);
		else if (index == 1)
			create_metric(screen, "MEM", x, &memory_value, &memory_detail);
		else
			create_metric(screen, "FAN", x, &fan_value, &fan_detail);
		++slot;
	}
	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
}

static lv_obj_t *build_traffic_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_TRAFFIC);
	bool show_rates = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_TRAFFIC, "rates");
	bool show_connections = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_TRAFFIC, "connections");
	bool show_history = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_TRAFFIC, "history");
	unsigned int row_count = (show_rates ? 2U : 0U) +
		(show_connections ? 1U : 0U);
	int item_x = show_history ? 34 : 108;
	unsigned int row = 0;
	if (row_count && show_history)
		create_divider(screen, 134, 8, 2, 60);
	if (show_rates) {
		traffic_upload_measurement = create_transfer_measurement(screen, item_x,
			responsive_row_y(row++, row_count),
			LV_SYMBOL_UPLOAD, app_config.secondary_colour);
		traffic_download_measurement = create_transfer_measurement(screen, item_x,
			responsive_row_y(row++, row_count),
			LV_SYMBOL_DOWNLOAD, app_config.accent_colour);
		set_transfer_part_visible(&traffic_upload_measurement, true, false);
		set_transfer_part_visible(&traffic_download_measurement, true, false);
	}
	if (show_connections) {
		int y = responsive_row_y(row, row_count);
		lv_obj_t *icon = create_label(screen, LV_SYMBOL_SHUFFLE, item_x, y,
			ui_label_font(), app_config.secondary_colour);
		set_fixed_text(icon, 12, LV_TEXT_ALIGN_CENTER);
		traffic_connections_value = create_label(screen, "0", item_x + 16, y,
			ui_label_font(), app_config.primary_colour);
		/*
		 * Centre the count in the same 51 px field occupied by the rate number
		 * and unit above. This keeps the icon column exact, balances short
		 * counts visually and still leaves enough fixed width for long counts.
		 */
		set_fixed_text(traffic_connections_value, 51, LV_TEXT_ALIGN_CENTER);
	}
	if (show_history) {
		traffic_chart = lv_chart_create(screen);
		lv_obj_set_pos(traffic_chart, row_count ? 142 : 8, 5);
		lv_obj_set_size(traffic_chart, row_count ? 134 : 268, 66);
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
	default: return app_config.secondary_colour;
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
	default: return app_config.secondary_colour;
	}
}

static int network_text_width(const char *text)
{
	lv_point_t size;
	lv_text_get_size(&size, text, ui_label_font(), 0, 0,
		LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return size.x + 2;
}

static void layout_network_detail_row(lv_obj_t *title, lv_obj_t *value)
{
	if (!title || !value)
		return;
	int title_width = network_text_width(lv_label_get_text(title));
	int value_width = network_text_width(lv_label_get_text(value));
	if (title_width + 12 + value_width > 268)
		value_width = 268 - title_width - 12;
	int group_width = title_width + 12 + value_width;
	int title_x = (284 - group_width) / 2;
	int value_x = title_x + title_width + 12;
	lv_obj_set_x(title, title_x);
	lv_obj_set_width(title, title_width);
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_x(value, value_x);
	lv_obj_set_width(value, value_width);
}

static void layout_network_detail_rows(void)
{
	layout_network_detail_row(network_wan_title, network_wan_value);
	layout_network_detail_row(network_lan_title, network_lan_label);
}

static lv_obj_t *build_network_screen(lv_obj_t *parent)
{
	static const char *const fields[] = {
		"ethernet", "repeater", "tethering", "cellular"
	};
	static const char *const titles[] = { "ETH", "REPEATER", "USB", "CELLULAR" };
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_NETWORK);
	unsigned int visible_indices[4] = {0};
	int uplink_widths[4] = {0};
	unsigned int visible = 0;
	for (unsigned int index = 0; index < 4; ++index) {
		if (!screenplus_page_has_field(&app_config,
		    SCREENPLUS_PAGE_NETWORK, fields[index]))
			continue;
		lv_point_t size;
		lv_text_get_size(&size, titles[index], ui_label_font(), 0, 0,
			LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		visible_indices[visible] = index;
		uplink_widths[visible] = size.x + 2;
		++visible;
	}
	bool show_wan = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_NETWORK, "wan_detail");
	bool show_lan = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_NETWORK, "lan");
	int detail_title_width = 0;
	if (show_wan) {
		lv_point_t size;
		lv_text_get_size(&size, "WAN", ui_label_font(), 0, 0,
			LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		detail_title_width = size.x + 2;
	}
	if (show_lan) {
		lv_point_t size;
		lv_text_get_size(&size, "LAN", ui_label_font(), 0, 0,
			LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x + 2 > detail_title_width)
			detail_title_width = size.x + 2;
	}
	if (visible && uplink_widths[0] < detail_title_width)
		uplink_widths[0] = detail_title_width;
	int uplink_text_width = 0;
	for (unsigned int slot = 0; slot < visible; ++slot)
		uplink_text_width += uplink_widths[slot];
	int natural_group_width = uplink_text_width +
		(visible > 1U ? 12 * (int)(visible - 1U) : 0);
	int content_width = natural_group_width > 236 ? natural_group_width : 236;
	if (content_width > 276)
		content_width = 276;
	int content_x = (284 - content_width) / 2;
	unsigned int row_count = (visible ? 1U : 0U) + show_wan + show_lan;
	unsigned int row = 0;
	for (unsigned int index = 1; index < row_count; ++index) {
		int y = (int)(76U * index / row_count) - 1;
		create_divider(screen, content_x, y, content_width, 2);
	}
	int uplink_y = visible ? responsive_row_y(row++, row_count) : 0;
	int group_width = natural_group_width;
	int x = content_x + (content_width - group_width) / 2;
	int gap_space = visible > 1U ? 12 : 0;
	int gap_remainder = 0;
	if (visible == 4U) {
		x = content_x - 2;
		gap_space = (content_width - uplink_text_width) / 3;
		gap_remainder = (content_width - uplink_text_width) % 3;
	}
	for (unsigned int slot = 0; slot < visible; ++slot) {
		unsigned int index = visible_indices[slot];
		int width = uplink_widths[slot];
		network_uplink_labels[index] = create_label(screen, titles[index], x, uplink_y,
			ui_label_font(), app_config.secondary_colour);
		lv_obj_set_width(network_uplink_labels[index], width);
		lv_obj_set_style_text_align(network_uplink_labels[index],
			LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(network_uplink_labels[index], LV_LABEL_LONG_DOT);
		x += width;
		if (slot + 1U < visible)
			x += gap_space + ((int)slot < gap_remainder ? 1 : 0);
	}
	if (show_wan) {
		int y = responsive_row_y(row++, row_count);
		network_wan_title = create_label(screen, "WAN", content_x, y, ui_label_font(),
			app_config.accent_colour);
		lv_obj_set_width(network_wan_title, detail_title_width);
		lv_obj_set_style_text_align(network_wan_title, LV_TEXT_ALIGN_CENTER, 0);
		int value_x = content_x + detail_title_width + 12;
		network_wan_value = create_label(screen, "--", value_x, y,
			ui_label_font(), app_config.primary_colour);
		lv_obj_set_width(network_wan_value,
			content_x + content_width - value_x);
		lv_label_set_long_mode(network_wan_value, LV_LABEL_LONG_DOT);
	}
	if (show_lan) {
		int y = responsive_row_y(row, row_count);
		network_lan_title = create_label(screen, "LAN", content_x, y, ui_label_font(),
			app_config.accent_colour);
		lv_obj_set_width(network_lan_title, detail_title_width);
		lv_obj_set_style_text_align(network_lan_title, LV_TEXT_ALIGN_CENTER, 0);
		int value_x = content_x + detail_title_width + 12;
		network_lan_label = create_label(screen, "--", value_x, y,
			ui_label_font(), app_config.primary_colour);
		lv_obj_set_width(network_lan_label,
			content_x + content_width - value_x);
		lv_label_set_long_mode(network_lan_label, LV_LABEL_LONG_DOT);
	}
	layout_network_detail_rows();
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

static bool show_wifi_qr(unsigned int band)
{
	if (band >= 2 || app_config.password_mode == SCREENPLUS_PASSWORD_HIDDEN)
		return false;
	struct system_snapshot snapshot;
	pthread_mutex_lock(&snapshot_mutex);
	bool ready = snapshot_ready;
	if (ready)
		snapshot = latest_snapshot;
	pthread_mutex_unlock(&snapshot_mutex);
	if (!ready)
		return false;
	const struct wifi_info *wifi = band == 0 ? &snapshot.wifi_2g : &snapshot.wifi_5g;
	if (!wifi->enabled)
		return false;
	char escaped_ssid[SCREENPLUS_TEXT_MEDIUM * 2];
	char escaped_password[SCREENPLUS_TEXT_MEDIUM * 2];
	size_t ssid_used = 0;
	size_t password_used = 0;
	for (const char *source = wifi->ssid;
	     *source && ssid_used + 2 < sizeof(escaped_ssid); ++source) {
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
	return true;
}

static void toggle_wifi_password(unsigned int band)
{
	if (band >= 2 || app_config.password_mode != SCREENPLUS_PASSWORD_TAP)
		return;
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

static void wifi_touch_event(lv_event_t *event)
{
	lv_event_code_t code = lv_event_get_code(event);
	if (code == LV_EVENT_PRESSED) {
		wifi_touch_active = false;
		password_long_press_handled = false;
		unsigned int band = (unsigned int)(uintptr_t)lv_event_get_user_data(event);
		if (band >= 2 || !wifi_band_rows[band])
			return;
		wifi_touch_band = band;
		wifi_touch_active = true;
		return;
	}
	if (code == LV_EVENT_LONG_PRESSED) {
		if (wifi_touch_active && drag_tracking && !drag_moved &&
		    !page_animation_running) {
			password_long_press_handled = show_wifi_qr(wifi_touch_band);
			wifi_touch_active = !password_long_press_handled;
		}
		return;
	}
	if (code == LV_EVENT_PRESS_LOST) {
		wifi_touch_active = false;
		password_long_press_handled = false;
		return;
	}
	if (code != LV_EVENT_RELEASED)
		return;
	bool activate = wifi_touch_active && !password_long_press_handled &&
		!drag_moved && !page_animation_running;
	wifi_touch_active = false;
	password_long_press_handled = false;
	if (activate)
		toggle_wifi_password(wifi_touch_band);
}

static void close_wifi_qr(lv_event_t *event)
{
	if (lv_event_get_code(event) != LV_EVENT_CLICKED &&
	    lv_event_get_code(event) != LV_EVENT_GESTURE)
		return;
	(void)qr_wifi_band;
	wifi_touch_active = false;
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

static lv_obj_t *create_reset_progress(lv_obj_t *parent, unsigned int fill_colour)
{
	lv_obj_t *track = lv_obj_create(parent);
	lv_obj_set_pos(track, 8, 70);
	lv_obj_set_size(track, 268, 4);
	lv_obj_set_style_radius(track, 0, 0);
	lv_obj_set_style_border_width(track, 0, 0);
	lv_obj_set_style_pad_all(track, 0, 0);
	lv_obj_set_style_bg_color(track, colour(app_config.border_colour), 0);
	lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
	lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *fill = lv_obj_create(track);
	lv_obj_set_pos(fill, 0, 0);
	lv_obj_set_size(fill, 1, 4);
	lv_obj_set_style_radius(fill, 0, 0);
	lv_obj_set_style_border_width(fill, 0, 0);
	lv_obj_set_style_pad_all(fill, 0, 0);
	lv_obj_set_style_bg_color(fill, colour(fill_colour), 0);
	lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
	lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	return fill;
}

static lv_obj_t *create_reset_stage_page(lv_obj_t *parent, unsigned int stage)
{
	static const char *const english_stage_names[] = {
		"NETWORK", "FACTORY", "CANCEL"
	};
	static const char *const chinese_stage_names[] = {
		"网络", "出厂", "取消"
	};
	unsigned int stage_colours[] = {
		app_config.accent_colour, app_config.warning_colour, app_config.error_colour
	};
	lv_obj_t *page = lv_obj_create(parent);
	style_screen(page);
	create_label(page, "RESET", 8, 3, reset_ui_font(), app_config.accent_colour);
	lv_obj_t *stage_name = create_label(page,
		app_config.chinese ? chinese_stage_names[stage] : english_stage_names[stage],
		146, 3, reset_ui_font(), stage_colours[stage]);
	lv_obj_set_width(stage_name, 130);
	lv_obj_set_style_text_align(stage_name, LV_TEXT_ALIGN_RIGHT, 0);
	create_divider(page, 8, 25, 268, 2);
	reset_stage_main_labels[stage] = create_label(page, "", 8, 28,
		reset_ui_font(), app_config.primary_colour);
	reset_stage_detail_labels[stage] = create_label(page, "", 8, 49,
		reset_ui_font(), app_config.primary_colour);
	lv_obj_set_width(reset_stage_main_labels[stage], 268);
	lv_obj_set_width(reset_stage_detail_labels[stage], 268);
	lv_label_set_long_mode(reset_stage_main_labels[stage], LV_LABEL_LONG_CLIP);
	lv_label_set_long_mode(reset_stage_detail_labels[stage], LV_LABEL_LONG_CLIP);
	reset_stage_progress[stage] = create_reset_progress(page, stage_colours[stage]);
	lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
	return page;
}

static lv_obj_t *build_reset_screen(void)
{
	lv_obj_t *screen = lv_obj_create(NULL);
	style_screen(screen);
	for (unsigned int stage = 0; stage < RESET_STAGE_COUNT; ++stage)
		reset_stage_pages[stage] = create_reset_stage_page(screen, stage);
	return screen;
}

enum reset_marker_state {
	RESET_MARKER_NONE,
	RESET_MARKER_PRESSED,
	RESET_MARKER_RELEASED
};

static uint32_t reset_elapsed_ms(double started, double ended)
{
	double elapsed = (ended - started) * 1000.0;
	if (elapsed < 0.0)
		elapsed = 0.0;
	if (elapsed > (double)UINT32_MAX)
		elapsed = (double)UINT32_MAX;
	return (uint32_t)elapsed;
}

static enum reset_marker_state read_reset_button_state(uint32_t *held_ms,
							uint32_t *released_ms)
{
	FILE *file = fopen(RESET_BUTTON_MARKER, "r");
	if (!file)
		return RESET_MARKER_NONE;
	char line[96] = {0};
	bool valid = fgets(line, sizeof(line), file) != NULL;
	fclose(file);
	if (!valid)
		return RESET_MARKER_NONE;
	double started = 0.0;
	double released = 0.0;
	if (sscanf(line, "released %lf %lf", &started, &released) == 2 &&
	    started > 0.0 && released >= started) {
		*held_ms = reset_elapsed_ms(started, released);
		*released_ms = reset_elapsed_ms(released, monotonic_seconds());
		return RESET_MARKER_RELEASED;
	}
	if (sscanf(line, "%lf", &started) == 1 && started > 0.0) {
		*held_ms = reset_elapsed_ms(started, monotonic_seconds());
		*released_ms = 0U;
		return RESET_MARKER_PRESSED;
	}
	return RESET_MARKER_NONE;
}

static void show_reset_stage(unsigned int stage, uint32_t elapsed_ms)
{
	uint32_t start = stage == 0 ? 0U : stage == 1 ? RESET_NETWORK_MS : RESET_FACTORY_MS;
	uint32_t end = stage == 0 ? RESET_NETWORK_MS :
		stage == 1 ? RESET_FACTORY_MS : RESET_CANCEL_MS;
	bool cancelled = elapsed_ms >= RESET_CANCEL_MS;
	unsigned int remaining = elapsed_ms < end ?
		(unsigned int)((end - elapsed_ms + 999U) / 1000U) : 0U;
	if (stage != reset_visible_stage) {
		for (unsigned int index = 0; index < RESET_STAGE_COUNT; ++index)
			set_page_visible(reset_stage_pages[index], index == stage);
		reset_visible_stage = stage;
		reset_last_remaining = UINT32_MAX;
	}
	if (remaining != reset_last_remaining || cancelled != reset_cancelled) {
		char detail[96];
		if (stage == 0) {
			set_label_text_if_changed(reset_stage_main_labels[stage],
				translated("继续按住 Reset", "KEEP HOLDING RESET"));
			snprintf(detail, sizeof(detail),
				translated("%u 秒后可重置网络", "NETWORK RESET IN %us"), remaining);
		} else if (stage == 1) {
			set_label_text_if_changed(reset_stage_main_labels[stage],
				translated("松开以重置网络", "RELEASE: RESET NETWORK"));
			snprintf(detail, sizeof(detail),
				translated("继续按住 %u 秒恢复出厂设置", "HOLD %us MORE: FACTORY RESET"),
				remaining);
		} else if (!cancelled) {
			set_label_text_if_changed(reset_stage_main_labels[stage],
				translated("松开以恢复出厂设置", "RELEASE: FACTORY RESET"));
			snprintf(detail, sizeof(detail),
				translated("继续按住 %u 秒取消操作", "HOLD %us MORE: CANCEL"), remaining);
		} else {
			set_label_text_if_changed(reset_stage_main_labels[stage],
				translated("操作已取消", "RESET CANCELLED"));
			snprintf(detail, sizeof(detail), "%s",
				translated("请松开 Reset 键", "RELEASE THE RESET BUTTON"));
		}
		set_label_text_if_changed(reset_stage_detail_labels[stage], detail);
		reset_last_remaining = remaining;
		reset_cancelled = cancelled;
	}
	uint32_t duration = end - start;
	uint32_t progress = elapsed_ms > start ? elapsed_ms - start : 0U;
	if (progress > duration)
		progress = duration;
	int32_t width = duration ? (int32_t)(268U * progress / duration) : 268;
	if (width < 1)
		width = 1;
	lv_obj_set_width(reset_stage_progress[stage], width);
}

static void show_reset_release_confirmation(uint32_t held_ms, uint32_t elapsed_ms)
{
	bool cancelled = held_ms < RESET_NETWORK_MS || held_ms >= RESET_CANCEL_MS;
	unsigned int stage = cancelled ? 2U :
		held_ms < RESET_FACTORY_MS ? 0U : 1U;
	if (stage != reset_visible_stage) {
		for (unsigned int index = 0; index < RESET_STAGE_COUNT; ++index)
			set_page_visible(reset_stage_pages[index], index == stage);
		reset_visible_stage = stage;
		reset_last_remaining = UINT32_MAX;
	}
	unsigned int remaining = elapsed_ms < RESET_CONFIRM_MS ?
		(unsigned int)((RESET_CONFIRM_MS - elapsed_ms + 999U) / 1000U) : 0U;
	if (remaining != reset_last_remaining || !reset_cancelled) {
		char detail[96];
		if (cancelled) {
			set_label_text_if_changed(reset_stage_main_labels[stage],
				translated("操作已取消", "RESET CANCELLED"));
			snprintf(detail, sizeof(detail),
				translated("%u 秒后返回", "RETURNING IN %us"), remaining);
		} else if (stage == 0U) {
			set_label_text_if_changed(reset_stage_main_labels[stage],
				translated("正在重置网络", "RESETTING NETWORK"));
			snprintf(detail, sizeof(detail), "%s",
				translated("请稍候", "PLEASE WAIT"));
		} else {
			set_label_text_if_changed(reset_stage_main_labels[stage],
				translated("正在恢复出厂设置", "FACTORY RESETTING"));
			snprintf(detail, sizeof(detail), "%s",
				translated("设备即将重启", "PLEASE WAIT"));
		}
		set_label_text_if_changed(reset_stage_detail_labels[stage], detail);
		reset_last_remaining = remaining;
		reset_cancelled = true;
	}
	uint32_t progress = elapsed_ms > RESET_CONFIRM_MS ? RESET_CONFIRM_MS : elapsed_ms;
	int32_t width = (int32_t)(268U * progress / RESET_CONFIRM_MS);
	if (width < 1)
		width = 1;
	lv_obj_set_width(reset_stage_progress[stage], width);
}

static void open_reset_overlay(void)
{
	if (reset_overlay_active)
		return;
	reset_overlay_active = true;
	lv_anim_delete(dashboard_screen, NULL);
	page_animation_running = false;
	drag_tracking = false;
	drag_moved = false;
	drag_offset = 0;
	pending_page_delta = 0;
	place_pages(0);
	lv_screen_load(reset_screen);
}

static void close_reset_overlay(void)
{
	if (!reset_overlay_active)
		return;
	reset_overlay_active = false;
	reset_visible_stage = RESET_STAGE_COUNT;
	reset_last_remaining = UINT32_MAX;
	reset_cancelled = false;
	lv_screen_load(dashboard_screen);
	place_pages(0);
	if (main_display)
		lv_display_trigger_activity(main_display);
}

static void update_reset_button(lv_timer_t *timer)
{
	(void)timer;
	uint32_t held_ms = 0;
	uint32_t released_ms = 0;
	enum reset_marker_state state = read_reset_button_state(&held_ms, &released_ms);
	if (state == RESET_MARKER_NONE) {
		close_reset_overlay();
		return;
	}
	if (state == RESET_MARKER_RELEASED) {
		if (released_ms < RESET_CONFIRM_MS) {
			open_reset_overlay();
			if (main_display)
				lv_display_trigger_activity(main_display);
			set_backlight(true);
			show_reset_release_confirmation(held_ms, released_ms);
			return;
		}
		unlink(RESET_BUTTON_MARKER);
		close_reset_overlay();
		return;
	}
	/*
	 * RESET_MARKER_PRESSED: the button is still held. The official cancel
	 * flow needs the marker to survive until the hotplug script rewrites it
	 * as "released", which then drives the cancelled stage and the 3 s
	 * release confirmation, so the marker must not be removed at
	 * RESET_CANCEL_MS itself. Only a genuinely lost release event warrants
	 * recovery: after RESET_RECOVERY_MS the marker is stale, so drop it and
	 * let the RESET_MARKER_NONE branch above close the overlay and release
	 * the forced backlight on the next tick.
	 */
	if (held_ms >= RESET_RECOVERY_MS) {
		unlink(RESET_BUTTON_MARKER);
		return;
	}
	open_reset_overlay();
	if (main_display)
		lv_display_trigger_activity(main_display);
	set_backlight(true);
	unsigned int stage = held_ms < RESET_NETWORK_MS ? 0U :
		held_ms < RESET_FACTORY_MS ? 1U : 2U;
	show_reset_stage(stage, held_ms);
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
		ui_label_font(), app_config.accent_colour);
	wifi_ssid_labels[band] = create_label(row, "--", 55, 2,
		ui_label_font(), app_config.primary_colour);
	wifi_password_labels[band] = create_label(row, "KEY ********", 55, 20,
		ui_detail_font(), app_config.primary_colour);
	lv_obj_set_width(wifi_band_titles[band], 42);
	lv_obj_set_width(wifi_ssid_labels[band], 221);
	lv_obj_set_width(wifi_password_labels[band], 221);
	lv_label_set_long_mode(wifi_ssid_labels[band], LV_LABEL_LONG_DOT);
	lv_label_set_long_mode(wifi_password_labels[band], LV_LABEL_LONG_DOT);
	lv_obj_clear_flag(wifi_band_titles[band], LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(wifi_ssid_labels[band], LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(wifi_password_labels[band], LV_OBJ_FLAG_CLICKABLE |
		LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(row, wifi_touch_event, LV_EVENT_ALL,
		(void *)(uintptr_t)band);
}

static lv_obj_t *build_wifi_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_WIFI);
	bool show_2g = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_WIFI, "wifi_2g");
	bool show_5g = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_WIFI, "wifi_5g");
	unsigned int count = show_2g + show_5g;
	if (count == 2)
		create_divider(screen, 8, 37, 268, 2);
	unsigned int row = 0;
	if (show_2g)
		create_wifi_band_row(screen, 0, count == 2 ? (int)(row++ * 39U) : 19, "2.4G");
	if (show_5g)
		create_wifi_band_row(screen, 1, count == 2 ? (int)(row * 39U) : 19, "5G");
	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
}

static void openclash_toggle_event(lv_event_t *event)
{
	if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
	    openclash_toggle_syncing)
		return;
	lv_obj_t *toggle = lv_event_get_target_obj(event);
	int target = lv_obj_has_state(toggle, LV_STATE_CHECKED) ? 1 : -1;
	bool accepted = false;
	pthread_mutex_lock(&openclash_toggle_mutex);
	if (!openclash_toggle_busy) {
		++openclash_toggle_request_id;
		openclash_toggle_target = target;
		openclash_toggle_request = target;
		openclash_toggle_failed = 0;
		openclash_toggle_command_done = 0;
		openclash_toggle_busy = 1;
		openclash_toggle_deadline = monotonic_milliseconds() + 30000U;
		accepted = true;
	}
	pthread_mutex_unlock(&openclash_toggle_mutex);
	if (!accepted)
		return;
	lv_obj_add_state(toggle, LV_STATE_DISABLED);
	if (openclash_state_label) {
		set_label_text_if_changed(openclash_state_label,
			target > 0 ?
			translated("启动中", "STARTING") :
			translated("停止中", "STOPPING"));
		lv_obj_set_style_text_color(openclash_state_label,
			colour(app_config.warning_colour), 0);
	}
}

static lv_obj_t *build_openclash_screen(lv_obj_t *parent)
{
	lv_obj_t *screen = create_page(parent, SCREENPLUS_PAGE_OPENCLASH);
	bool show_rates = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_OPENCLASH, "rates");
	bool show_totals = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_OPENCLASH, "totals");
	bool show_traffic = show_rates || show_totals;
	bool show_connections = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_OPENCLASH, "connections");
	bool show_resources = screenplus_page_has_field(
		&app_config, SCREENPLUS_PAGE_OPENCLASH, "resources");
	bool show_summary = show_connections || show_resources;
	lv_obj_t *title = create_label(screen, "OPENCLASH", 8, 4, ui_label_font(),
		app_config.accent_colour);
	openclash_state_label = create_label(screen, "N/A", 116, 4,
		small_ui_font(), app_config.primary_colour);
	lv_obj_set_width(title, 108);
	lv_obj_set_width(openclash_state_label, 114);
	openclash_toggle = lv_switch_create(screen);
	lv_obj_set_pos(openclash_toggle, 240, 3);
	lv_obj_set_size(openclash_toggle, 36, 19);
	lv_obj_set_style_bg_color(openclash_toggle,
		colour(app_config.border_colour), LV_PART_MAIN);
	lv_obj_set_style_bg_color(openclash_toggle,
		colour(app_config.accent_colour), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_opa(openclash_toggle, LV_OPA_COVER,
		LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(openclash_toggle,
		colour(app_config.primary_colour), LV_PART_KNOB);
	lv_obj_clear_flag(openclash_toggle, LV_OBJ_FLAG_EVENT_BUBBLE |
		LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(openclash_toggle, openclash_toggle_event,
		LV_EVENT_VALUE_CHANGED, NULL);
	if (show_traffic || show_summary)
		create_divider(screen, 8, 25, 268, 2);
	if (show_traffic && show_summary)
		create_divider(screen, 8, 50, 268, 2);
	int traffic_y = show_summary ? 29 : 43;
	if (show_traffic) {
		openclash_download_measurement = create_transfer_measurement(screen, 8, traffic_y,
			LV_SYMBOL_DOWNLOAD, app_config.accent_colour);
		openclash_upload_measurement = create_transfer_measurement(screen, 148, traffic_y,
			LV_SYMBOL_UPLOAD, app_config.secondary_colour);
		set_transfer_part_visible(&openclash_download_measurement, show_rates, show_totals);
		set_transfer_part_visible(&openclash_upload_measurement, show_rates, show_totals);
	}
	int summary_y = show_traffic ? 55 : 43;
	unsigned int summary_count = (show_connections ? 1U : 0U) +
		(show_resources ? 2U : 0U);
	int summary_x = (284 - (int)(summary_count * 90U)) / 2;
	if (show_connections) {
		create_label(screen, "CONN", summary_x, summary_y,
			ui_detail_font(), app_config.secondary_colour);
		openclash_connections_value = create_label(screen, "--", summary_x + 44, summary_y,
			ui_detail_font(), app_config.primary_colour);
		set_fixed_text(openclash_connections_value, 36, LV_TEXT_ALIGN_RIGHT);
		summary_x += 90;
	}
	if (show_resources) {
		create_label(screen, "CPU", summary_x, summary_y,
			ui_detail_font(), app_config.secondary_colour);
		openclash_cpu_value = create_label(screen, "--%", summary_x + 34, summary_y,
			ui_detail_font(), app_config.primary_colour);
		set_fixed_text(openclash_cpu_value, 47, LV_TEXT_ALIGN_RIGHT);
		summary_x += 90;
		create_label(screen, "MEM", summary_x, summary_y,
			ui_detail_font(), app_config.secondary_colour);
		openclash_memory_value = create_label(screen, "--", summary_x + 35, summary_y,
			ui_detail_font(), app_config.primary_colour);
		openclash_memory_unit = create_label(screen, "MB", summary_x + 64, summary_y,
			ui_detail_font(), app_config.secondary_colour);
		set_fixed_text(openclash_memory_value, 27, LV_TEXT_ALIGN_RIGHT);
		set_fixed_text(openclash_memory_unit, 22, LV_TEXT_ALIGN_LEFT);
	}
	lv_obj_add_event_cb(screen, page_drag_event, LV_EVENT_ALL, NULL);
	return screen;
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
	layout_network_detail_rows();

	const struct wifi_info *wifi_bands[2] = { &snapshot.wifi_2g, &snapshot.wifi_5g };
	for (unsigned int band = 0; band < 2; ++band)
		update_wifi_band_display(band, wifi_bands[band]);

	if (openclash_state_label) {
		pthread_mutex_lock(&openclash_toggle_mutex);
		bool toggle_pending = openclash_toggle_busy != 0;
		int pending_target = openclash_toggle_target;
		if (toggle_pending) {
			bool reached_target = pending_target > 0 ?
				snapshot.openclash.state == SCREENPLUS_STATE_ACTIVE :
				snapshot.openclash.state == SCREENPLUS_STATE_IDLE;
			bool timed_out = (int32_t)(monotonic_milliseconds() -
				openclash_toggle_deadline) >= 0;
			if ((reached_target && openclash_toggle_command_done) ||
			    openclash_toggle_failed || timed_out) {
				openclash_toggle_busy = 0;
				openclash_toggle_target = 0;
				openclash_toggle_failed = 0;
				openclash_toggle_command_done = 0;
				toggle_pending = false;
			}
		}
		pthread_mutex_unlock(&openclash_toggle_mutex);
		if (toggle_pending) {
			set_label_text_if_changed(openclash_state_label,
				pending_target > 0 ?
				translated("启动中", "STARTING") :
				translated("停止中", "STOPPING"));
			lv_obj_set_style_text_color(openclash_state_label,
				colour(app_config.warning_colour), 0);
		} else {
			unsigned int openclash_hex = state_colour(snapshot.openclash.state);
			set_label_text_if_changed(openclash_state_label,
				display_state_text(snapshot.openclash.state));
			lv_obj_set_style_text_color(openclash_state_label,
				colour(openclash_hex), 0);
		}
		if (openclash_toggle) {
			if (toggle_pending ||
			    snapshot.openclash.state == SCREENPLUS_STATE_UNAVAILABLE)
				lv_obj_add_state(openclash_toggle, LV_STATE_DISABLED);
			else
				lv_obj_remove_state(openclash_toggle, LV_STATE_DISABLED);
			if (!toggle_pending) {
				bool enabled = snapshot.openclash.state == SCREENPLUS_STATE_ACTIVE ||
					snapshot.openclash.state == SCREENPLUS_STATE_ERROR;
				openclash_toggle_syncing = true;
				if (enabled)
					lv_obj_add_state(openclash_toggle, LV_STATE_CHECKED);
				else
					lv_obj_remove_state(openclash_toggle, LV_STATE_CHECKED);
				openclash_toggle_syncing = false;
			}
		}
		if (snapshot.openclash.metrics_available) {
			update_transfer_measurement(&openclash_download_measurement,
				snapshot.openclash.download_bytes_per_second,
				snapshot.openclash.download_total_bytes);
			update_transfer_measurement(&openclash_upload_measurement,
				snapshot.openclash.upload_bytes_per_second,
				snapshot.openclash.upload_total_bytes);
		} else {
			update_transfer_measurement(&openclash_download_measurement, 0.0, 0);
			update_transfer_measurement(&openclash_upload_measurement, 0.0, 0);
		}
		if (openclash_connections_value) {
			snprintf(text, sizeof(text), "%u", snapshot.openclash.connection_count);
			set_label_text_if_changed(openclash_connections_value, text);
		}
		if (openclash_cpu_value) {
			unsigned int cpu = (unsigned int)(snapshot.openclash.cpu_percent + 0.5);
			if (cpu > 999)
				cpu = 999;
			snprintf(text, sizeof(text), "%u%%", cpu);
			set_label_text_if_changed(openclash_cpu_value, text);
		}
		if (openclash_memory_value) {
			char unit[8];
			format_fixed_quantity((double)snapshot.openclash.memory_bytes, false,
				text, sizeof(text), unit, sizeof(unit));
			set_label_text_if_changed(openclash_memory_value, text);
			set_label_text_if_changed(openclash_memory_unit, unit);
		}
	}
}

static void apply_openclash_toggle_request(void)
{
	pthread_mutex_lock(&openclash_toggle_mutex);
	int request = openclash_toggle_request;
	unsigned int request_id = openclash_toggle_request_id;
	openclash_toggle_request = 0;
	pthread_mutex_unlock(&openclash_toggle_mutex);
	if (!request)
		return;
	const char *const set_command[] = {
		"/sbin/uci", "-q", "set",
		request > 0 ? "openclash.config.enable=1" : "openclash.config.enable=0",
		NULL
	};
	const char *const commit_command[] = {
		"/sbin/uci", "-q", "commit", "openclash", NULL
	};
	const char *const service_command[] = {
		"/etc/init.d/openclash",
		request > 0 ? "restart" : "stop",
		NULL
	};
	int result = safe_exec_quiet(set_command);
	if (result == 0)
		result = safe_exec_quiet(commit_command);
	if (result == 0)
		result = safe_exec_quiet(service_command);
	pthread_mutex_lock(&openclash_toggle_mutex);
	/* Publish the outcome only if the pending request is still the one this
	 * command was started for. The UI timeout may have retired it (busy=0)
	 * and a newer request may already hold a fresh generation id; writing
	 * unconditionally would let the stale result end that request early or
	 * leave stray failed/command_done flags behind. */
	if (openclash_toggle_busy && request_id == openclash_toggle_request_id) {
		if (result != 0)
			openclash_toggle_failed = 1;
		openclash_toggle_command_done = 1;
	}
	pthread_mutex_unlock(&openclash_toggle_mutex);
}

static void *system_worker(void *unused)
{
	(void)unused;
	while (running) {
		apply_openclash_toggle_request();
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
	reset_screen = build_reset_screen();
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
	bool requested_page_found = false;
	if (requested_start_page >= 0) {
		for (unsigned int index = 0; index < screen_count; ++index) {
			if (screens[index] == screens_by_page[requested_start_page]) {
				current_screen_index = index;
				requested_page_found = true;
				break;
			}
		}
	}
	if (!requested_page_found && requested_start_index >= 0) {
		unsigned int index = (unsigned int)requested_start_index;
		current_screen_index = index < screen_count ? index : screen_count - 1U;
	}
	place_pages(0);
	lv_screen_load(dashboard_screen);
	lv_timer_create(update_clock, 250, NULL);
	lv_timer_create(apply_metrics, 100, NULL);
	lv_timer_create(apply_system_snapshot, 500, NULL);
	lv_timer_create(manage_idle_state, 250, NULL);
	lv_timer_create(update_reset_button, 50, NULL);
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
	if (!app_config.enabled && !options.config_once && !options.metrics_once &&
	    !options.system_info_once) {
		unsetenv("SCREENPLUS_RELOAD_PAGE");
		unsetenv("SCREENPLUS_RELOAD_INDEX");
		execl("/usr/libexec/screenplus-run", "screenplus-run", (char *)NULL);
		fprintf(stderr, "screenplus: disabled-mode handoff failed: %s\n",
			strerror(errno));
		return 1;
	}
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
	const char *reload_page = getenv("SCREENPLUS_RELOAD_PAGE");
	if (reload_page && reload_page[0]) {
		for (int page = 0; page < SCREENPLUS_PAGE_COUNT; ++page) {
			if (strcmp(reload_page,
			    screenplus_page_name((enum screenplus_page_id)page)) == 0) {
				requested_start_page = page;
				break;
			}
		}
	}
	const char *reload_index = getenv("SCREENPLUS_RELOAD_INDEX");
	if (reload_index && reload_index[0]) {
		char *end = NULL;
		long index = strtol(reload_index, &end, 10);
		if (end && *end == '\0' && index >= 0 && index < SCREENPLUS_PAGE_COUNT)
			requested_start_index = (int)index;
	}
	unsetenv("SCREENPLUS_RELOAD_PAGE");
	unsetenv("SCREENPLUS_RELOAD_INDEX");
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
		       "\"network_connection_count\":%u,"
		       "\"network_receive_total_bytes\":%llu,"
		       "\"network_transmit_total_bytes\":%llu,"
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
		       metrics.network_connection_count,
		       (unsigned long long)metrics.network_receive_total_bytes,
		       (unsigned long long)metrics.network_transmit_total_bytes,
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
		       "\"openclash_cpu_percent\":%.2f,"
		       "\"openclash_memory_bytes\":%llu,"
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
		       snapshot.openclash.cpu_percent,
		       (unsigned long long)snapshot.openclash.memory_bytes,
		       (unsigned long long)snapshot.openclash.download_total_bytes,
		       (unsigned long long)snapshot.openclash.upload_total_bytes);
		return 0;
	}

	lv_init();
	lv_tick_set_cb(monotonic_milliseconds);
	lv_display_t *display = screenplus_fbdev_create(options.framebuffer);
	if (!display) {
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
		/*
		 * The touch panel is mounted 90 degrees counter-clockwise relative to
		 * the old vendor mapping. Keep the axes unswapped so physical horizontal
		 * motion drives LVGL's horizontal page axis. Use the same native-panel
		 * calibration in both orientations: LVGL rotates input points together
		 * with the display, so reversing calibration here would cancel that
		 * rotation and make flipped gestures move opposite to the animation.
		 */
		lv_evdev_set_swap_axes(touch, false);
		lv_evdev_set_calibration(touch, 0, 0, 75, 283);
		lv_indev_set_display(touch, display);
		/* Keep touch sampling independent from the framebuffer cadence.
		 * The display consumes the newest position on each frame while a 10 ms
		 * input timer reduces finger-to-frame latency and coalesces intermediate
		 * points instead of trying to render each one. */
		lv_timer_t *touch_read_timer = lv_indev_get_read_timer(touch);
		if (touch_read_timer)
			lv_timer_set_period(touch_read_timer, TOUCH_READ_PERIOD_MS);
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

	if (reload_requested && current_screen_index < screen_count) {
		for (int page = 0; page < SCREENPLUS_PAGE_COUNT; ++page) {
			if (screens[current_screen_index] == screens_by_page[page]) {
				setenv("SCREENPLUS_RELOAD_PAGE",
					screenplus_page_name((enum screenplus_page_id)page), 1);
				break;
			}
		}
		char index[16];
		snprintf(index, sizeof(index), "%u", current_screen_index);
		setenv("SCREENPLUS_RELOAD_INDEX", index, 1);
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
