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
static lv_obj_t *clock_screen;
static lv_obj_t *status_screen;
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
static lv_obj_t *disk_value;
static lv_obj_t *disk_detail;
static struct metrics_state metric_state;
static lv_obj_t *wifi_device_label;
static lv_obj_t *wifi_2g_label;
static lv_obj_t *wifi_password_label;
static lv_obj_t *wifi_password_card;
static lv_obj_t *network_wifi_card;
static lv_obj_t *network_wifi_title;
static lv_obj_t *network_wifi_state;
static lv_obj_t *network_wifi_detail;
static lv_obj_t *port_cards[2];
static lv_obj_t *port_titles[2];
static lv_obj_t *port_links[2];
static lv_obj_t *port_rates[2];
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
static bool password_revealed;
static bool password_long_press_handled;
static uint32_t password_reveal_deadline;
static struct screenplus_config app_config;
static lv_display_t *main_display;
static bool backlight_on = true;
static void *background_buffers[SCREENPLUS_PAGE_COUNT];
static int requested_start_page = -1;

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

static void style_screen(lv_obj_t *screen)
{
	lv_obj_set_style_bg_color(screen, colour(app_config.background_colour), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(screen, 0, 0);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

static void add_page_background(lv_obj_t *screen, enum screenplus_page_id page)
{
	char expected_name[32];
	snprintf(expected_name, sizeof(expected_name), "%s.rgb565", screenplus_page_name(page));
	if (strcmp(app_config.pages[page].background, expected_name) != 0)
		return;
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
	char date_text[32];
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
			snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d %s%s",
				local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
				weekdays[local.tm_wday], show_timezone ? " CST" : "");
		else
			snprintf(date_text, sizeof(date_text), "%s%s", weekdays[local.tm_wday],
				show_timezone ? " CST" : "");
	}
	else if (show_date && show_weekday)
		strftime(date_text, sizeof(date_text), show_timezone ? "%Y-%m-%d %a %Z" : "%Y-%m-%d %a", &local);
	else if (show_date)
		strftime(date_text, sizeof(date_text), show_timezone ? "%Y-%m-%d %Z" : "%Y-%m-%d", &local);
	else if (show_weekday)
		strftime(date_text, sizeof(date_text), show_timezone ? "%a %Z" : "%a", &local);
	else if (show_timezone)
		strftime(date_text, sizeof(date_text), "%Z", &local);
	else
		date_text[0] = '\0';
	set_label_text_if_changed(clock_label, time_text);
	set_label_text_if_changed(date_label, date_text);
}

static void update_metrics(lv_timer_t *timer)
{
	(void)timer;
	struct system_metrics metrics;
	metrics_sample(&metric_state, &metrics);
	char main_text[24];
	char detail_text[24];

	if (cpu_value) {
		snprintf(main_text, sizeof(main_text), "%.0f%%", metrics.cpu_percent);
		set_label_text_if_changed(cpu_value, main_text);
	}
	if (memory_value) {
		snprintf(main_text, sizeof(main_text), "%.0f%%", metrics.memory_percent);
		set_label_text_if_changed(memory_value, main_text);
	}
	if (memory_detail) {
		snprintf(detail_text, sizeof(detail_text), "%llu/%lluM",
			(unsigned long long)(metrics.memory_used_bytes / (1024U * 1024U)),
			(unsigned long long)(metrics.memory_total_bytes / (1024U * 1024U)));
		set_label_text_if_changed(memory_detail, detail_text);
	}

	if (disk_value) {
		snprintf(main_text, sizeof(main_text), "%.0f%%", metrics.storage_percent);
		set_label_text_if_changed(disk_value, main_text);
	}
	if (disk_detail) {
		snprintf(detail_text, sizeof(detail_text), "%llu/%lluM",
			(unsigned long long)(metrics.storage_used_bytes / (1024U * 1024U)),
			(unsigned long long)(metrics.storage_total_bytes / (1024U * 1024U)));
		set_label_text_if_changed(disk_detail, detail_text);
	}
}

static void gesture_event(lv_event_t *event)
{
	(void)event;
	lv_indev_t *input = lv_indev_active();
	if (!input || screen_count < 2)
		return;
	lv_dir_t direction = lv_indev_get_gesture_dir(input);
	if (direction == LV_DIR_LEFT) {
		if (current_screen_index + 1U >= screen_count && !app_config.swipe_loop)
			return;
		current_screen_index = (current_screen_index + 1U) % screen_count;
		if (app_config.slide_animation)
			lv_screen_load_anim(screens[current_screen_index], LV_SCREEN_LOAD_ANIM_MOVE_LEFT,
				160, 0, false);
		else
			lv_screen_load(screens[current_screen_index]);
	} else if (direction == LV_DIR_RIGHT) {
		if (current_screen_index == 0 && !app_config.swipe_loop)
			return;
		current_screen_index = (current_screen_index + screen_count - 1U) % screen_count;
		if (app_config.slide_animation)
			lv_screen_load_anim(screens[current_screen_index], LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
				160, 0, false);
		else
			lv_screen_load(screens[current_screen_index]);
	}
	lv_indev_wait_release(input);
}

static void auto_carousel(lv_timer_t *timer)
{
	(void)timer;
	if (!app_config.auto_carousel || screen_count < 2)
		return;
	current_screen_index = (current_screen_index + 1U) % screen_count;
	if (app_config.slide_animation)
		lv_screen_load_anim(screens[current_screen_index], LV_SCREEN_LOAD_ANIM_MOVE_LEFT,
			160, 0, false);
	else
		lv_screen_load(screens[current_screen_index]);
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
	if (password_revealed && password_reveal_deadline &&
	    monotonic_milliseconds() >= password_reveal_deadline) {
		password_revealed = false;
		password_reveal_deadline = 0;
	}
}

static lv_obj_t *build_clock_screen(void)
{
	lv_obj_t *screen = lv_obj_create(NULL);
	style_screen(screen);
	add_page_background(screen, SCREENPLUS_PAGE_HOME);

	lv_obj_t *accent = lv_obj_create(screen);
	lv_obj_set_pos(accent, 61, 8);
	lv_obj_set_size(accent, 4, 60);
	lv_obj_set_style_radius(accent, 0, 0);
	lv_obj_set_style_border_width(accent, 0, 0);
	lv_obj_set_style_bg_color(accent, colour(app_config.accent_colour), 0);
	lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

	clock_label = create_label(screen, "--:--", 72, 10, &lv_font_montserrat_28,
		app_config.primary_colour);
	lv_obj_set_width(clock_label, 196);
	lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_letter_space(clock_label, -1, 0);
	date_label = create_label(screen, "---- -- --", 72, 46, small_ui_font(),
		app_config.primary_colour);
	lv_obj_set_width(date_label, 196);
	lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
	if (!screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_HOME, "time"))
		lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(screen, gesture_event, LV_EVENT_GESTURE, NULL);
	return screen;
}

static void create_metric(lv_obj_t *parent, const char *title, int x,
			  lv_obj_t **value_label, lv_obj_t **detail_label)
{
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_pos(card, x, 8);
	lv_obj_set_size(card, 88, 60);
	lv_obj_set_style_radius(card, 5, 0);
	lv_obj_set_style_bg_color(card, colour(app_config.surface_colour), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(card, 2, 0);
	lv_obj_set_style_border_color(card, colour(app_config.border_colour), 0);
	lv_obj_set_style_pad_all(card, 0, 0);
	lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	create_label(card, title, 7, 3, small_ui_font(), app_config.accent_colour);
	*value_label = create_label(card, "--", 7, 21, &lv_font_montserrat_18,
		app_config.primary_colour);
	*detail_label = create_label(card, "--", 7, 43, &lv_font_montserrat_14,
		app_config.secondary_colour);
	lv_obj_set_width(*detail_label, 74);
	lv_label_set_long_mode(*detail_label, LV_LABEL_LONG_DOT);
}

static lv_obj_t *build_status_screen(void)
{
	lv_obj_t *screen = lv_obj_create(NULL);
	style_screen(screen);
	add_page_background(screen, SCREENPLUS_PAGE_STATUS);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_STATUS, "cpu")) {
		create_metric(screen, "CPU", 4,
			&cpu_value, &cpu_detail);
		lv_obj_set_y(cpu_value, 29);
		lv_obj_add_flag(cpu_detail, LV_OBJ_FLAG_HIDDEN);
	}
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_STATUS, "memory"))
		create_metric(screen, translated("内存", "MEM"), 98,
			&memory_value, &memory_detail);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_STATUS, "storage"))
		create_metric(screen, "DISK", 192,
			&disk_value, &disk_detail);
	lv_obj_add_event_cb(screen, gesture_event, LV_EVENT_GESTURE, NULL);
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

static lv_obj_t *create_network_card(lv_obj_t *parent, unsigned int slot,
				     const char *title, lv_obj_t **title_label,
				     lv_obj_t **value_label, lv_obj_t **detail_label)
{
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_pos(card, 5, 4 + (int)slot * 24);
	lv_obj_set_size(card, 274, 21);
	lv_obj_set_style_radius(card, 4, 0);
	lv_obj_set_style_bg_color(card, colour(app_config.surface_colour), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(card, 2, 0);
	lv_obj_set_style_border_color(card, colour(app_config.border_colour), 0);
	lv_obj_set_style_pad_all(card, 0, 0);
	lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	*title_label = create_label(card, title, 7, 1, &lv_font_montserrat_14,
		app_config.accent_colour);
	*value_label = create_label(card, "--", 94, 1, &lv_font_montserrat_14,
		app_config.primary_colour);
	*detail_label = create_label(card, "--", 158, 1, &lv_font_montserrat_14,
		app_config.secondary_colour);
	lv_obj_set_size(*title_label, 82, 16);
	lv_obj_set_size(*value_label, 60, 16);
	lv_obj_set_size(*detail_label, 109, 16);
	lv_label_set_long_mode(*title_label, LV_LABEL_LONG_DOT);
	lv_label_set_long_mode(*value_label, LV_LABEL_LONG_DOT);
	lv_label_set_long_mode(*detail_label, LV_LABEL_LONG_DOT);
	lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
	return card;
}

static lv_obj_t *build_network_screen(void)
{
	lv_obj_t *screen = lv_obj_create(NULL);
	style_screen(screen);
	add_page_background(screen, SCREENPLUS_PAGE_NETWORK);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_NETWORK, "port_1"))
		port_cards[0] = create_network_card(screen, 0, "ETH0", &port_titles[0],
			&port_links[0], &port_rates[0]);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_NETWORK, "port_2"))
		port_cards[1] = create_network_card(screen, 1, "ETH1", &port_titles[1],
			&port_links[1], &port_rates[1]);
	if (screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_NETWORK, "wifi"))
		network_wifi_card = create_network_card(screen, 2, "WIFI", &network_wifi_title,
			&network_wifi_state, &network_wifi_detail);
	lv_obj_add_event_cb(screen, gesture_event, LV_EVENT_GESTURE, NULL);
	return screen;
}

static const struct wifi_info *select_wifi(const struct system_snapshot *snapshot,
					   const char **band)
{
	if (snapshot->wifi_5g.enabled) {
		*band = "5G";
		return &snapshot->wifi_5g;
	}
	if (snapshot->wifi_2g.enabled) {
		*band = "2.4G";
		return &snapshot->wifi_2g;
	}
	if (snapshot->wifi_mlo.enabled) {
		*band = "MLO";
		return &snapshot->wifi_mlo;
	}
	if (snapshot->wifi_5g.configured) {
		*band = "5G";
		return &snapshot->wifi_5g;
	}
	if (snapshot->wifi_2g.configured) {
		*band = "2.4G";
		return &snapshot->wifi_2g;
	}
	*band = "MLO";
	return &snapshot->wifi_mlo;
}

static void password_event(lv_event_t *event)
{
	lv_event_code_t code = lv_event_get_code(event);
	if (code == LV_EVENT_LONG_PRESSED &&
	    app_config.password_mode != SCREENPLUS_PASSWORD_HIDDEN) {
		password_long_press_handled = true;
		struct system_snapshot snapshot;
		pthread_mutex_lock(&snapshot_mutex);
		bool ready = snapshot_ready;
		if (ready)
			snapshot = latest_snapshot;
		pthread_mutex_unlock(&snapshot_mutex);
		if (!ready)
			return;
		const char *band;
		const struct wifi_info *wifi = select_wifi(&snapshot, &band);
		(void)band;
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
		lv_screen_load(wifi_qr_screen);
		return;
	}
	if (code == LV_EVENT_CLICKED && password_long_press_handled) {
		password_long_press_handled = false;
		return;
	}
	if (code == LV_EVENT_CLICKED && app_config.password_mode == SCREENPLUS_PASSWORD_QR) {
		lv_obj_send_event(wifi_password_card, LV_EVENT_LONG_PRESSED, NULL);
		password_long_press_handled = false;
		return;
	}
	if (code == LV_EVENT_CLICKED && app_config.password_mode == SCREENPLUS_PASSWORD_TAP) {
		password_revealed = !password_revealed;
		password_reveal_deadline = password_revealed ?
			monotonic_milliseconds() + 15000U : 0;
	}
}

static void close_wifi_qr(lv_event_t *event)
{
	if (lv_event_get_code(event) != LV_EVENT_CLICKED &&
	    lv_event_get_code(event) != LV_EVENT_GESTURE)
		return;
	lv_screen_load(screens[current_screen_index]);
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

static lv_obj_t *build_wifi_screen(void)
{
	lv_obj_t *screen = lv_obj_create(NULL);
	style_screen(screen);
	add_page_background(screen, SCREENPLUS_PAGE_WIFI);
	wifi_password_card = lv_obj_create(screen);
	lv_obj_set_pos(wifi_password_card, 5, 7);
	lv_obj_set_size(wifi_password_card, 274, 62);
	lv_obj_set_style_radius(wifi_password_card, 5, 0);
	lv_obj_set_style_bg_color(wifi_password_card, colour(app_config.surface_colour), 0);
	lv_obj_set_style_bg_opa(wifi_password_card, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(wifi_password_card, 2, 0);
	lv_obj_set_style_border_color(wifi_password_card, colour(app_config.border_colour), 0);
	lv_obj_set_style_pad_all(wifi_password_card, 0, 0);
	lv_obj_clear_flag(wifi_password_card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(wifi_password_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
	wifi_device_label = create_label(wifi_password_card, "WIFI", 9, 3,
		&lv_font_montserrat_14, app_config.accent_colour);
	wifi_2g_label = create_label(wifi_password_card, "SSID  --", 9, 23,
		&lv_font_montserrat_14, app_config.primary_colour);
	wifi_password_label = create_label(wifi_password_card, "KEY   ********", 9, 43,
		&lv_font_montserrat_14, app_config.primary_colour);
	lv_obj_set_width(wifi_2g_label, 252);
	lv_obj_set_width(wifi_password_label, 252);
	lv_label_set_long_mode(wifi_2g_label, LV_LABEL_LONG_DOT);
	lv_label_set_long_mode(wifi_password_label, LV_LABEL_LONG_DOT);
	lv_obj_add_event_cb(wifi_password_card, password_event, LV_EVENT_ALL, NULL);
	if (!screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_WIFI, "ssid"))
		lv_obj_add_flag(wifi_2g_label, LV_OBJ_FLAG_HIDDEN);
	if (!screenplus_page_has_field(&app_config, SCREENPLUS_PAGE_WIFI, "password"))
		lv_obj_add_flag(wifi_password_label, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(screen, gesture_event, LV_EVENT_GESTURE, NULL);
	return screen;
}

static lv_obj_t *build_openclash_screen(void)
{
	lv_obj_t *screen = lv_obj_create(NULL);
	style_screen(screen);
	add_page_background(screen, SCREENPLUS_PAGE_OPENCLASH);
	create_label(screen, "OPENCLASH", 8, 3, &lv_font_montserrat_14,
		app_config.accent_colour);
	openclash_state_label = create_label(screen, "N/A", 116, 3,
		small_ui_font(), app_config.primary_colour);
	lv_obj_t *openclash_card = lv_obj_create(screen);
	lv_obj_set_pos(openclash_card, 8, 24);
	lv_obj_set_size(openclash_card, 268, 2);
	lv_obj_set_style_radius(openclash_card, 0, 0);
	lv_obj_set_style_border_width(openclash_card, 0, 0);
	lv_obj_set_style_bg_color(openclash_card, colour(app_config.accent_colour), 0);
	lv_obj_set_style_bg_opa(openclash_card, LV_OPA_COVER, 0);
	lv_obj_clear_flag(openclash_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	openclash_download_label = create_label(screen, "DOWN --", 8, 29,
		&lv_font_montserrat_14, app_config.primary_colour);
	openclash_upload_label = create_label(screen, "UP --", 146, 29,
		&lv_font_montserrat_14, app_config.primary_colour);
	openclash_connections_label = create_label(screen, "CONN --", 8, 51,
		&lv_font_montserrat_14, app_config.secondary_colour);
	openclash_totals_label = create_label(screen, "D --  U --", 96, 51,
		&lv_font_montserrat_14, app_config.secondary_colour);
	lv_obj_set_width(openclash_download_label, 128);
	lv_obj_set_width(openclash_upload_label, 130);
	lv_obj_set_width(openclash_totals_label, 180);
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
	lv_obj_add_event_cb(screen, gesture_event, LV_EVENT_GESTURE, NULL);
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
	for (unsigned int index = 0; index < 2; ++index) {
		if (!port_cards[index])
			continue;
		const struct ethernet_port_info *port = &snapshot.ports[index];
		unsigned int hex = port->carrier ? app_config.accent_colour :
			app_config.secondary_colour;
		lv_obj_set_style_border_color(port_cards[index], colour(hex), 0);
		snprintf(text, sizeof(text), "%s ETH%u", port->role, index);
		set_label_text_if_changed(port_titles[index], text);
		if (port->carrier && port->speed_mbps == 2500)
			strcpy(text, "UP 2.5G");
		else if (port->carrier && port->speed_mbps >= 1000)
			snprintf(text, sizeof(text), "UP %uG", port->speed_mbps / 1000U);
		else if (port->carrier)
			snprintf(text, sizeof(text), "UP %uM", port->speed_mbps);
		else
			strcpy(text, "DOWN");
		set_label_text_if_changed(port_links[index], text);
		lv_obj_set_style_text_color(port_links[index], colour(hex), 0);
		if (port->ipv4[0])
			snprintf(text, sizeof(text), "%s", port->ipv4);
		else
			strcpy(text, "NO IP");
		set_label_text_if_changed(port_rates[index], text);
	}

	const char *band = NULL;
	const struct wifi_info *primary_wifi = select_wifi(&snapshot, &band);
	if (network_wifi_card) {
		lv_obj_set_style_border_color(network_wifi_card,
			colour(primary_wifi->enabled ? app_config.accent_colour : app_config.border_colour), 0);
		snprintf(text, sizeof(text), "WIFI %s", band);
		set_label_text_if_changed(network_wifi_title, text);
		set_label_text_if_changed(network_wifi_state, primary_wifi->enabled ? "ON" : "OFF");
		lv_obj_set_style_text_color(network_wifi_state,
			colour(primary_wifi->enabled ? app_config.accent_colour : app_config.secondary_colour), 0);
		set_label_text_if_changed(network_wifi_detail,
			primary_wifi->enabled && primary_wifi->ssid[0] ? primary_wifi->ssid : "OFF");
	}

	if (wifi_device_label) {
		snprintf(text, sizeof(text), "WIFI %s  %s", band, primary_wifi->enabled ? "ON" : "OFF");
		set_label_text_if_changed(wifi_device_label, text);
		if (!primary_wifi->enabled) {
			set_label_text_if_changed(wifi_2g_label, "SSID  OFF");
			set_label_text_if_changed(wifi_password_label, "KEY   OFF");
		} else {
			snprintf(text, sizeof(text), "SSID  %s", primary_wifi->ssid[0] ? primary_wifi->ssid : "--");
			set_label_text_if_changed(wifi_2g_label, text);
			if (app_config.password_mode == SCREENPLUS_PASSWORD_HIDDEN)
				strcpy(text, "KEY   ********");
			else if (!primary_wifi->password[0])
				strcpy(text, "KEY   OPEN");
			else if (app_config.password_mode == SCREENPLUS_PASSWORD_VISIBLE || password_revealed)
				snprintf(text, sizeof(text), "KEY   %s", primary_wifi->password);
			else if (app_config.password_mode == SCREENPLUS_PASSWORD_QR)
				strcpy(text, "KEY   QR CODE");
			else
				strcpy(text, "KEY   TAP TO SHOW");
			set_label_text_if_changed(wifi_password_label, text);
		}
	}

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
			snprintf(text, sizeof(text), "D %s  U %s", download_total, upload_total);
			set_label_text_if_changed(openclash_totals_label, text);
		} else {
			set_label_text_if_changed(openclash_download_label, "DOWN --");
			set_label_text_if_changed(openclash_upload_label, "UP --");
			set_label_text_if_changed(openclash_connections_label, "CONN --");
			set_label_text_if_changed(openclash_totals_label, "D --  U --");
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
	clock_screen = build_clock_screen();
	status_screen = build_status_screen();
	network_screen = build_network_screen();
	wifi_screen = build_wifi_screen();
	wifi_qr_screen = build_wifi_qr_screen();
	openclash_screen = build_openclash_screen();
	screens_by_page[SCREENPLUS_PAGE_HOME] = clock_screen;
	screens_by_page[SCREENPLUS_PAGE_STATUS] = status_screen;
	screens_by_page[SCREENPLUS_PAGE_NETWORK] = network_screen;
	screens_by_page[SCREENPLUS_PAGE_WIFI] = wifi_screen;
	screens_by_page[SCREENPLUS_PAGE_OPENCLASH] = openclash_screen;
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
		screens[screen_count++] = screens_by_page[selected];
	}
	if (!screen_count)
		screens[screen_count++] = clock_screen;
	current_screen_index = 0;
	if (requested_start_page >= 0) {
		for (unsigned int index = 0; index < screen_count; ++index) {
			if (screens[index] == screens_by_page[requested_start_page]) {
				current_screen_index = index;
				break;
			}
		}
	}
	lv_screen_load(screens[current_screen_index]);
	lv_timer_create(update_clock, 250, NULL);
	metrics_state_initialize(&metric_state);
	lv_timer_create(update_metrics, 1000, NULL);
	lv_timer_create(apply_system_snapshot, 500, NULL);
	lv_timer_create(manage_idle_state, 250, NULL);
	if (app_config.auto_carousel)
		lv_timer_create(auto_carousel, app_config.carousel_interval_seconds * 1000U, NULL);
	update_clock(NULL);
	update_metrics(NULL);
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
		       "\"auto_carousel\":%s,\"carousel_interval\":%u,\"pages\":{",
		       app_config.enabled ? "true" : "false", app_config.chinese ? "zh_cn" : "en",
		       app_config.brightness, app_config.rotation,
		       app_config.always_on ? "true" : "false",
		       app_config.swipe_loop ? "true" : "false",
		       app_config.auto_carousel ? "true" : "false",
		       app_config.carousel_interval_seconds);
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
		       "\"memory_percent\":%.2f,\"memory_used_bytes\":%llu,"
		       "\"memory_total_bytes\":%llu,\"storage_percent\":%.2f,"
		       "\"storage_used_bytes\":%llu,\"storage_total_bytes\":%llu,"
		       "\"disk_read_bytes_per_second\":%.2f,"
		       "\"disk_write_bytes_per_second\":%.2f,"
		       "\"network_interface\":\"%s\","
		       "\"network_receive_bytes_per_second\":%.2f,"
		       "\"network_transmit_bytes_per_second\":%.2f,"
		       "\"uptime_seconds\":%llu}\n",
		       metrics.cpu_percent, metrics.temperature_celsius,
		       metrics.memory_percent, (unsigned long long)metrics.memory_used_bytes,
		       (unsigned long long)metrics.memory_total_bytes, metrics.storage_percent,
		       (unsigned long long)metrics.storage_used_bytes,
		       (unsigned long long)metrics.storage_total_bytes,
		       metrics.disk_read_bytes_per_second, metrics.disk_write_bytes_per_second,
		       metrics.network_interface, metrics.network_receive_bytes_per_second,
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
		       "\"ethernet_dns\":\"%s\","
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
