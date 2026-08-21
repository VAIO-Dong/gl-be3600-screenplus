#define _POSIX_C_SOURCE 200809L

#include "app_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void copy_text(char *destination, size_t size, const char *source)
{
	if (!size)
		return;
	snprintf(destination, size, "%s", source ? source : "");
}

static void add_field(struct screenplus_page_config *page, const char *field)
{
	if (!field || !*field || page->field_count >= SCREENPLUS_MAX_FIELDS)
		return;
	for (size_t index = 0; index < page->field_count; ++index) {
		if (strcmp(page->fields[index], field) == 0)
			return;
	}
	copy_text(page->fields[page->field_count++], SCREENPLUS_FIELD_LENGTH, field);
}

static void set_default_page(struct screenplus_page_config *page, int order,
			     const char *const *fields, size_t field_count)
{
	page->enabled = true;
	page->order = order;
	for (size_t index = 0; index < field_count; ++index)
		add_field(page, fields[index]);
}

void screenplus_config_defaults(struct screenplus_config *config)
{
	static const char *const home_fields[] = { "time", "date", "weekday" };
	static const char *const status_fields[] = { "cpu", "memory", "fan" };
	static const char *const traffic_fields[] = { "rates", "connections", "history" };
	static const char *const network_fields[] = {
		"lan", "ethernet", "repeater", "tethering", "cellular", "wan_detail"
	};
	static const char *const wifi_fields[] = { "wifi_2g", "wifi_5g" };
	static const char *const openclash_fields[] = {
		"rates", "connections", "totals", "resources"
	};

	memset(config, 0, sizeof(*config));
	config->enabled = true;
	config->chinese = true;
	config->brightness = 5;
	config->rotation = 90;
	config->idle_timeout_seconds = 180;
	config->swipe_loop = true;
	config->slide_animation = true;
	config->carousel_interval_seconds = 10;
	config->password_mode = SCREENPLUS_PASSWORD_TAP;
	config->primary_colour = 0xffffff;
	config->secondary_colour = 0xdcecff;
	config->accent_colour = 0x37f59a;
	config->background_colour = 0x030912;
	config->border_colour = 0x3b424a;
	config->warning_colour = 0xffdc55;
	config->error_colour = 0xff5c70;
	config->standby_colour = 0x4b9fff;
	config->overlay_opacity = 35;
	set_default_page(&config->pages[SCREENPLUS_PAGE_HOME], 10,
		home_fields, sizeof(home_fields) / sizeof(home_fields[0]));
	set_default_page(&config->pages[SCREENPLUS_PAGE_TRAFFIC], 20,
		traffic_fields, sizeof(traffic_fields) / sizeof(traffic_fields[0]));
	set_default_page(&config->pages[SCREENPLUS_PAGE_STATUS], 30,
		status_fields, sizeof(status_fields) / sizeof(status_fields[0]));
	set_default_page(&config->pages[SCREENPLUS_PAGE_WIFI], 40,
		wifi_fields, sizeof(wifi_fields) / sizeof(wifi_fields[0]));
	set_default_page(&config->pages[SCREENPLUS_PAGE_NETWORK], 50,
		network_fields, sizeof(network_fields) / sizeof(network_fields[0]));
	set_default_page(&config->pages[SCREENPLUS_PAGE_OPENCLASH], 60,
		openclash_fields, sizeof(openclash_fields) / sizeof(openclash_fields[0]));
}

const char *screenplus_page_name(enum screenplus_page_id page)
{
	static const char *const names[] = {
		"home", "status", "traffic", "network", "wifi", "openclash"
	};
	return page < SCREENPLUS_PAGE_COUNT ? names[page] : "unknown";
}

static int page_from_name(const char *name)
{
	for (int page = 0; page < SCREENPLUS_PAGE_COUNT; ++page) {
		if (strcmp(screenplus_page_name((enum screenplus_page_id)page), name) == 0)
			return page;
	}
	return -1;
}

static char *skip_space(char *text)
{
	while (*text && isspace((unsigned char)*text))
		++text;
	return text;
}

static bool next_token(char **cursor, char *output, size_t size)
{
	char *input = skip_space(*cursor);
	if (!*input || *input == '#')
		return false;
	char quote = 0;
	if (*input == '\'' || *input == '"')
		quote = *input++;
	size_t used = 0;
	while (*input) {
		if (quote ? *input == quote : isspace((unsigned char)*input) || *input == '#')
			break;
		char value = *input++;
		if (value == '\\' && *input)
			value = *input++;
		if (used + 1 < size)
			output[used++] = value;
	}
	output[used] = '\0';
	if (quote && *input == quote)
		++input;
	*cursor = input;
	return true;
}

static bool parse_boolean(const char *value)
{
	return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
		strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0;
}

static unsigned int parse_unsigned(const char *value, unsigned int fallback,
				   unsigned int minimum, unsigned int maximum)
{
	char *end = NULL;
	errno = 0;
	unsigned long parsed = strtoul(value, &end, 10);
	if (errno || end == value || *end || parsed < minimum || parsed > maximum)
		return fallback;
	return (unsigned int)parsed;
}

static uint32_t parse_colour(const char *value, uint32_t fallback)
{
	if (*value == '#')
		++value;
	if (strlen(value) != 6)
		return fallback;
	char *end = NULL;
	errno = 0;
	unsigned long parsed = strtoul(value, &end, 16);
	if (errno || *end || parsed > 0xffffffUL)
		return fallback;
	return (uint32_t)parsed;
}

static void apply_main_option(struct screenplus_config *config,
			      const char *key, const char *value)
{
	if (strcmp(key, "enabled") == 0)
		config->enabled = parse_boolean(value);
	else if (strcmp(key, "language") == 0)
		config->chinese = strcmp(value, "en") != 0;
	else if (strcmp(key, "brightness") == 0)
		config->brightness = (int)parse_unsigned(value, 5, 1, 11);
	else if (strcmp(key, "rotation") == 0) {
		unsigned int rotation = parse_unsigned(value, 90, 90, 270);
		config->rotation = rotation == 270 ? 270 : 90;
	} else if (strcmp(key, "always_on") == 0)
		config->always_on = parse_boolean(value);
	else if (strcmp(key, "idle_timeout") == 0)
		config->idle_timeout_seconds = parse_unsigned(value, 180, 10, 86400);
	else if (strcmp(key, "swipe_loop") == 0)
		config->swipe_loop = parse_boolean(value);
	else if (strcmp(key, "auto_carousel") == 0)
		config->auto_carousel = parse_boolean(value);
	else if (strcmp(key, "carousel_interval") == 0)
		config->carousel_interval_seconds = parse_unsigned(value, 10, 3, 300);
	else if (strcmp(key, "password_mode") == 0) {
		if (strcmp(value, "hidden") == 0)
			config->password_mode = SCREENPLUS_PASSWORD_HIDDEN;
		else if (strcmp(value, "visible") == 0)
			config->password_mode = SCREENPLUS_PASSWORD_VISIBLE;
		else if (strcmp(value, "qr") == 0)
			config->password_mode = SCREENPLUS_PASSWORD_QR;
		else
			config->password_mode = SCREENPLUS_PASSWORD_TAP;
	} else if (strcmp(key, "page_transition") == 0)
		config->slide_animation = strcmp(value, "slide") == 0;
}

static void apply_appearance_option(struct screenplus_config *config,
				    const char *key, const char *value)
{
	if (strcmp(key, "foreground") == 0 || strcmp(key, "primary") == 0)
		config->primary_colour = parse_colour(value, config->primary_colour);
	else if (strcmp(key, "secondary") == 0)
		config->secondary_colour = parse_colour(value, config->secondary_colour);
	else if (strcmp(key, "accent") == 0)
		config->accent_colour = parse_colour(value, config->accent_colour);
	else if (strcmp(key, "background") == 0)
		config->background_colour = parse_colour(value, config->background_colour);
	else if (strcmp(key, "border") == 0)
		config->border_colour = parse_colour(value, config->border_colour);
	else if (strcmp(key, "warning") == 0)
		config->warning_colour = parse_colour(value, config->warning_colour);
	else if (strcmp(key, "error") == 0)
		config->error_colour = parse_colour(value, config->error_colour);
	else if (strcmp(key, "standby") == 0)
		config->standby_colour = parse_colour(value, config->standby_colour);
	else if (strcmp(key, "overlay_opacity") == 0)
		config->overlay_opacity = parse_unsigned(value, 35, 0, 100);
	else if (strcmp(key, "background_mode") == 0)
		config->global_background = strcmp(value, "global") == 0;
}

int screenplus_config_load(struct screenplus_config *config, const char *path)
{
	screenplus_config_defaults(config);
	FILE *file = fopen(path, "r");
	if (!file)
		return errno == ENOENT ? 0 : -1;

	char section_type[32] = "";
	char section_name[32] = "";
	bool page_fields_seen[SCREENPLUS_PAGE_COUNT] = { false };
	char *line = NULL;
	size_t capacity = 0;
	while (getline(&line, &capacity, file) >= 0) {
		char *cursor = line;
		char directive[32];
		char key[64];
		char value[160];
		if (!next_token(&cursor, directive, sizeof(directive)))
			continue;
		if (strcmp(directive, "config") == 0) {
			if (!next_token(&cursor, section_type, sizeof(section_type)))
				section_type[0] = '\0';
			if (!next_token(&cursor, section_name, sizeof(section_name)))
				section_name[0] = '\0';
			continue;
		}
		if (!next_token(&cursor, key, sizeof(key)) ||
		    !next_token(&cursor, value, sizeof(value)))
			continue;
		if (strcmp(section_type, "screenplus") == 0 && strcmp(directive, "option") == 0)
			apply_main_option(config, key, value);
		else if (strcmp(section_type, "appearance") == 0 && strcmp(directive, "option") == 0)
			apply_appearance_option(config, key, value);
		else if (strcmp(section_type, "page_order") == 0 && strcmp(directive, "option") == 0) {
			int page_index = page_from_name(key);
			if (page_index >= 0)
				config->pages[page_index].order = (int)parse_unsigned(value,
					(unsigned int)config->pages[page_index].order, 0, 999);
		}
		else if (strcmp(section_type, "page") == 0) {
			int page_index = page_from_name(section_name);
			if (page_index < 0)
				continue;
			struct screenplus_page_config *page = &config->pages[page_index];
			if (strcmp(directive, "list") == 0 && strcmp(key, "field") == 0) {
				if (!page_fields_seen[page_index]) {
					page->field_count = 0;
					page_fields_seen[page_index] = true;
				}
				add_field(page, value);
			} else if (strcmp(directive, "option") == 0) {
				if (strcmp(key, "enabled") == 0)
					page->enabled = parse_boolean(value);
				else if (strcmp(key, "order") == 0)
					page->order = (int)parse_unsigned(value, (unsigned int)page->order, 0, 999);
				else if (strcmp(key, "background") == 0)
					copy_text(page->background, sizeof(page->background), value);
			}
		}
	}
	free(line);
	fclose(file);
	return 0;
}

int screenplus_timezone_load(struct screenplus_config *config, const char *path)
{
	FILE *file = fopen(path, "r");
	if (!file)
		return errno == ENOENT ? 0 : -1;
	char section_type[32] = "";
	char *line = NULL;
	size_t capacity = 0;
	while (getline(&line, &capacity, file) >= 0) {
		char *cursor = line;
		char directive[32];
		char key[64];
		char value[160];
		if (!next_token(&cursor, directive, sizeof(directive)))
			continue;
		if (strcmp(directive, "config") == 0) {
			if (!next_token(&cursor, section_type, sizeof(section_type)))
				section_type[0] = '\0';
			continue;
		}
		if (strcmp(section_type, "system") != 0 || strcmp(directive, "option") != 0 ||
		    !next_token(&cursor, key, sizeof(key)) ||
		    !next_token(&cursor, value, sizeof(value)))
			continue;
		if (strcmp(key, "timezone") == 0)
			copy_text(config->timezone_rule, sizeof(config->timezone_rule), value);
		else if (strcmp(key, "zonename") == 0)
			copy_text(config->timezone_name, sizeof(config->timezone_name), value);
	}
	free(line);
	fclose(file);
	return 0;
}

bool screenplus_page_has_field(const struct screenplus_config *config,
			       enum screenplus_page_id page, const char *field)
{
	if (page >= SCREENPLUS_PAGE_COUNT)
		return false;
	const struct screenplus_page_config *page_config = &config->pages[page];
	for (size_t index = 0; index < page_config->field_count; ++index) {
		if (strcmp(page_config->fields[index], field) == 0)
			return true;
	}
	return false;
}
