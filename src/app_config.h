#ifndef SCREENPLUS_APP_CONFIG_H
#define SCREENPLUS_APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREENPLUS_MAX_FIELDS 16
#define SCREENPLUS_FIELD_LENGTH 24
#define SCREENPLUS_WEEKDAY_COUNT 7

struct screenplus_schedule_day {
	bool enabled;
	unsigned int on_minute;
	unsigned int off_minute;
};

enum screenplus_schedule_mode {
	SCREENPLUS_SCHEDULE_DAILY,
	SCREENPLUS_SCHEDULE_WORKWEEK,
	SCREENPLUS_SCHEDULE_WEEKLY,
};

enum screenplus_page_id {
	SCREENPLUS_PAGE_HOME,
	SCREENPLUS_PAGE_STATUS,
	SCREENPLUS_PAGE_TRAFFIC,
	SCREENPLUS_PAGE_NETWORK,
	SCREENPLUS_PAGE_WIFI,
	SCREENPLUS_PAGE_OPENCLASH,
	SCREENPLUS_PAGE_COUNT,
};

enum screenplus_password_mode {
	SCREENPLUS_PASSWORD_HIDDEN,
	SCREENPLUS_PASSWORD_TAP,
	SCREENPLUS_PASSWORD_VISIBLE,
	SCREENPLUS_PASSWORD_QR,
};

struct screenplus_page_config {
	bool enabled;
	int order;
	char background[128];
	char fields[SCREENPLUS_MAX_FIELDS][SCREENPLUS_FIELD_LENGTH];
	size_t field_count;
};

struct screenplus_config {
	bool enabled;
	bool chinese;
	int brightness;
	int rotation;
	bool always_on;
	unsigned int idle_timeout_seconds;
	bool schedule_enabled;
	enum screenplus_schedule_mode schedule_mode;
	struct screenplus_schedule_day daily_schedule;
	struct screenplus_schedule_day weekday_schedule;
	struct screenplus_schedule_day weekend_schedule;
	struct screenplus_schedule_day weekly_schedule[SCREENPLUS_WEEKDAY_COUNT];
	bool swipe_loop;
	bool auto_carousel;
	unsigned int carousel_interval_seconds;
	enum screenplus_password_mode password_mode;
	uint32_t primary_colour;
	uint32_t secondary_colour;
	uint32_t accent_colour;
	uint32_t background_colour;
	uint32_t border_colour;
	uint32_t warning_colour;
	uint32_t error_colour;
	uint32_t standby_colour;
	bool global_background;
	bool slide_animation;
	unsigned int overlay_opacity;
	char timezone_rule[64];
	char timezone_name[64];
	struct screenplus_page_config pages[SCREENPLUS_PAGE_COUNT];
};

void screenplus_config_defaults(struct screenplus_config *config);
int screenplus_config_load(struct screenplus_config *config, const char *path);
int screenplus_timezone_load(struct screenplus_config *config, const char *path);
bool screenplus_page_has_field(const struct screenplus_config *config,
			       enum screenplus_page_id page, const char *field);
bool screenplus_schedule_allows_backlight(const struct screenplus_config *config,
					  int weekday, unsigned int minute);
const char *screenplus_page_name(enum screenplus_page_id page);

#endif
