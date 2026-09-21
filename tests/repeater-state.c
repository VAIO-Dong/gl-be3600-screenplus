/* Included after the production samplers by test-repeater-state.ps1. */
#include <assert.h>

static const char *fixture_running, *fixture_status, *fixture_up;
static const char *fixture_available, *fixture_pending, *fixture_autostart;
static bool fail_second_up;
static unsigned int up_reads;
static int fixture_carrier = 1;
static int fixture_second_carrier;
static const char *fixture_second_up, *fixture_modem;

static int ethernet_carrier(const struct uplink_info *uplink)
{
	return strcmp(uplink->logical_interface, "secondwan") == 0 ?
		fixture_second_carrier : fixture_carrier;
}

static int device_carrier(const char *device)
{
	(void)device;
	return fixture_carrier;
}

static int fixture_value(const char *value, char *buffer, unsigned int size)
{
	if (!value)
		return -1;
	snprintf(buffer, size, "%s", value);
	return 0;
}

static int run_line(const char *command, char *buffer, unsigned int size)
{
	if (strstr(command, "modem_*"))
		return fixture_value(fixture_modem, buffer, size);
	return fixture_value(strstr(command, "@.running") ? fixture_running :
		fixture_status, buffer, size);
}

static int ubus_interface_value(const char *name, const char *path,
				char *buffer, unsigned int size)
{
	const char *value = NULL;
	if (strcmp(path, "@.up") == 0) {
		++up_reads;
		value = fail_second_up && up_reads > 1 ? NULL : fixture_up;
		if (strcmp(name, "secondwan") == 0)
			value = fixture_second_up;
	} else if (strcmp(path, "@.available") == 0)
		value = fixture_available;
	else if (strcmp(path, "@.pending") == 0)
		value = fixture_pending;
	else if (strcmp(path, "@.autostart") == 0)
		value = fixture_autostart;
	else if (strcmp(path, "@.l3_device") == 0)
		value = strcmp(name, "secondwan") == 0 ? "test-second" : "test-sta";
	else if (strcmp(path, "@[\"ipv4-address\"][0].address") == 0)
		value = "192.0.2.2";
	return fixture_value(value, buffer, size);
}

static void expect(const char *name, const char *running, const char *status,
		   const char *up, const char *available, const char *pending,
		   const char *autostart, const char *active, bool valid,
		   enum screenplus_state expected)
{
	fixture_running = running;
	fixture_status = status;
	fixture_up = up;
	fixture_available = available;
	fixture_pending = pending;
	fixture_autostart = autostart;
	up_reads = 0;
	struct uplink_info result = {0};
	assert(sample_repeater(active, &result) == valid);
	if (valid)
		assert(result.state == expected);
	printf("PASS %s\n", name);
}

int main(void)
{
	expect("current firmware idle, no STA device", NULL, "idle", "false",
		"false", "false", "true", "eth0", true, SCREENPLUS_STATE_UNAVAILABLE);
	expect("legacy stopped", "false", NULL, "false", "false", "false",
		"true", "eth0", true, SCREENPLUS_STATE_UNAVAILABLE);
	expect("legacy running before interface exists", "true", NULL, NULL, NULL,
		NULL, NULL, "eth0", false, SCREENPLUS_STATE_UNAVAILABLE);
	expect("new firmware pending", NULL, "unrecognised", "false", "false",
		"true", "true", "eth0", true, SCREENPLUS_STATE_UNAVAILABLE);
	expect("available and enabled without IP", NULL, "unrecognised", "false",
		"true", "false", "true", "eth0", true, SCREENPLUS_STATE_CONNECTING);
	expect("available but not enabled", NULL, "idle", "false", "true",
		"false", "false", "eth0", true, SCREENPLUS_STATE_UNAVAILABLE);
	expect("new firmware active", NULL, "unrecognised", "true", "true",
		"false", "true", "test-sta", true, SCREENPLUS_STATE_ACTIVE);
	expect("legacy active", "true", NULL, "true", "true", "false", "true",
		"test-sta", true, SCREENPLUS_STATE_ACTIVE);
	expect("up without default route", NULL, "unrecognised", "true", "true",
		"false", "true", "eth0", true, SCREENPLUS_STATE_CONNECTED);
	expect("operational interface overrides stale daemon idle", "false", NULL,
		"true", "true", "false", "true", "test-sta", true, SCREENPLUS_STATE_ACTIVE);
	expect("daemon unavailable, netifd explicitly absent", NULL, NULL, "false",
		"false", "false", "true", "eth0", true, SCREENPLUS_STATE_UNAVAILABLE);
	expect("all reads failed", NULL, NULL, NULL, NULL, NULL, NULL,
		"eth0", false, SCREENPLUS_STATE_UNAVAILABLE);
	expect("idle alone does not prove unavailable", NULL, "idle", NULL, NULL,
		NULL, NULL, "eth0", false, SCREENPLUS_STATE_UNAVAILABLE);
	expect("unknown status with incomplete interface data", NULL, "future-state",
		"false", "false", NULL, NULL, "eth0", true, SCREENPLUS_STATE_UNAVAILABLE);
	expect("malformed legacy running is not false", "garbage", NULL, NULL,
		NULL, NULL, NULL, "eth0", false, SCREENPLUS_STATE_UNAVAILABLE);
	fail_second_up = true;
	expect("interface query fails during connected sample", NULL, NULL, "true",
		"true", "false", "true", "test-sta", false, SCREENPLUS_STATE_UNAVAILABLE);
	fail_second_up = false;
	fixture_up = "false";
	fixture_available = "false";
	struct uplink_info other = {0};
	sample_interface("tethering", "eth0", &other);
	assert(other.state == SCREENPLUS_STATE_UNAVAILABLE);
	puts("PASS missing generic interface is unavailable");
	struct system_info_state cache = {0};
	struct uplink_info cached = {0};
	fixture_up = "true";
	sample_repeater_cached(&cache, "test-sta", 1000, &cached);
	assert(cached.state == SCREENPLUS_STATE_ACTIVE);
	fixture_up = NULL;
	fixture_pending = NULL;
	sample_repeater_cached(&cache, "test-sta", 2000, &cached);
	assert(cached.state == SCREENPLUS_STATE_ACTIVE);
	assert(strcmp(cached.ipv4, "192.0.2.2") == 0);
	sample_repeater_cached(&cache, "test-sta", 16001, &cached);
	assert(cached.state == SCREENPLUS_STATE_UNAVAILABLE);
	assert(strcmp(cached.detail, "UNKNOWN") == 0);
	assert(cached.ipv4[0] == '\0');
	puts("PASS transient failure retains snapshot, prolonged failure expires");
	fixture_up = "true";
	sample_repeater_cached(&cache, "test-sta", 17000, &cached);
	fixture_up = "false";
	fixture_available = "false";
	fixture_pending = "false";
	sample_repeater_cached(&cache, "eth0", 18000, &cached);
	assert(cached.state == SCREENPLUS_STATE_UNAVAILABLE);
	assert(strcmp(cached.detail, "NO DEVICE") == 0);
	assert(cached.ipv4[0] == '\0');
	puts("PASS confirmed disconnect immediately replaces cached connection");

	static const char *const names[] = { "wan", "wwan", "tethering", "modem_1_4" };
	for (unsigned int i = 0; i < 4; ++i) {
		struct uplink_health_cache health = {0};
		struct uplink_info link = {0};
		strcpy(link.logical_interface, names[i]);
		strcpy(link.device, "test-device");
		link.state = SCREENPLUS_STATE_CONNECTED;
		apply_uplink_health(&health, &link, 1, false, 1000);
		assert(network_state_colour(link.state) == app_config.standby_colour);
		link.state = SCREENPLUS_STATE_ACTIVE;
		apply_uplink_health(&health, &link, 1, false, 2000);
		assert(network_state_colour(link.state) == app_config.accent_colour);
		for (unsigned int balance = 0; balance < 2; ++balance) {
			link.state = SCREENPLUS_STATE_ACTIVE;
			apply_uplink_health(&health, &link, 0, balance, 3000);
			assert(network_state_colour(link.state) == app_config.warning_colour);
			link.state = SCREENPLUS_STATE_CONNECTED;
			apply_uplink_health(&health, &link, 0, balance, 4000);
			assert(network_state_colour(link.state) == app_config.warning_colour);
			link.state = SCREENPLUS_STATE_UNAVAILABLE;
			apply_uplink_health(&health, &link, 1, balance, 5000);
			assert(network_state_colour(link.state) == app_config.secondary_colour);
		}
		link.state = SCREENPLUS_STATE_CONNECTED;
		apply_uplink_health(&health, &link, 1, true, 6000);
		assert(network_state_colour(link.state) == app_config.accent_colour);
		link.state = SCREENPLUS_STATE_CONNECTED;
		apply_uplink_health(&health, &link, -1, false, 7000);
		assert(network_state_colour(link.state) == app_config.standby_colour);
		link.state = SCREENPLUS_STATE_ACTIVE;
		apply_uplink_health(&health, &link, -1, false, 21001);
		assert(network_state_colour(link.state) == app_config.secondary_colour);
		assert(strcmp(link.detail, "UNKNOWN") == 0);
		printf("PASS %s failover/balance colours and health read failure\n", names[i]);
	}
	assert(network_state_colour(SCREENPLUS_STATE_IDLE) == app_config.secondary_colour);
	FILE *table = tmpfile();
	assert(table);
	fputs("wan:online\nwwan:offline\nmodem_1_4:online\n", table);
	rewind(table);
	assert(parse_kmwan_health(table, "wan") == 1);
	rewind(table);
	assert(parse_kmwan_health(table, "wwan") == 0);
	rewind(table);
	assert(parse_kmwan_health(table, "modem_1_4") == 1);
	rewind(table);
	assert(parse_kmwan_health(table, "tethering") == 0);
	fclose(table);
	assert(parse_kmwan_health(NULL, "wan") == -1);
	table = tmpfile();
	assert(table);
	fputs("wan:future-status\n", table);
	rewind(table);
	assert(parse_kmwan_health(table, "wan") == -1);
	fclose(table);
	puts("PASS real kmwan table parser: online, offline, absent, missing, unknown");
	fixture_up = "false";
	fixture_available = "true";
	fixture_autostart = "true";
	fixture_carrier = 0;
	memset(&other, 0, sizeof(other));
	sample_interface("tethering", "eth0", &other);
	assert(other.state == SCREENPLUS_STATE_UNAVAILABLE);
	fixture_carrier = 1;
	fixture_autostart = "false";
	sample_interface("tethering", "eth0", &other);
	assert(other.state == SCREENPLUS_STATE_UNAVAILABLE);
	puts("PASS existing device without carrier and disabled interface are neutral");
	memset(&other, 0, sizeof(other));
	sample_ethernet("test-sta", &other);
	assert(other.state == SCREENPLUS_STATE_UNAVAILABLE);
	fixture_autostart = "true";
	fixture_up = "true";
	fixture_carrier = 0;
	sample_ethernet("test-sta", &other);
	assert(other.state == SCREENPLUS_STATE_UNAVAILABLE);
	fixture_carrier = 1;
	fixture_second_carrier = 1;
	fixture_second_up = "true";
	sample_ethernet("test-second", &other);
	assert(other.state == SCREENPLUS_STATE_ACTIVE);
	assert(strcmp(other.logical_interface, "secondwan") == 0);
	puts("PASS Ethernet disabled, cable removed, and second WAN selected");
	memset(&other, 0, sizeof(other));
	sample_tethering("test-sta", &other);
	assert(other.state == SCREENPLUS_STATE_ACTIVE);
	assert(strcmp(other.logical_interface, "tethering") == 0);
	fixture_modem = "network.interface.modem_1_4";
	memset(&other, 0, sizeof(other));
	sample_cellular("eth0", &other);
	assert(other.state == SCREENPLUS_STATE_CONNECTED);
	assert(strcmp(other.logical_interface, "modem_1_4") == 0);
	fixture_modem = NULL;
	memset(&other, 0, sizeof(other));
	sample_cellular("eth0", &other);
	assert(other.state == SCREENPLUS_STATE_UNAVAILABLE);
	puts("PASS USB and Cellular samplers use the common link states");
	return 0;
}
