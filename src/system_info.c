#define _POSIX_C_SOURCE 200809L

#include "system_info.h"

#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void trim(char *text)
{
	if (!text)
		return;
	size_t length = strlen(text);
	while (length && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
			  text[length - 1] == ' ' || text[length - 1] == '\t'))
		text[--length] = '\0';
}

static int read_file_line(const char *path, char *buffer, unsigned int size)
{
	if (!buffer || size < 2)
		return -1;
	FILE *file = fopen(path, "r");
	if (!file)
		return -1;
	char *result = fgets(buffer, (int)size, file);
	fclose(file);
	if (!result)
		return -1;
	trim(buffer);
	return 0;
}

static int run_line(const char *command, char *buffer, unsigned int size)
{
	if (!command || !buffer || size < 2)
		return -1;
	buffer[0] = '\0';
	FILE *pipe = popen(command, "r");
	if (!pipe)
		return -1;
	char *result = fgets(buffer, (int)size, pipe);
	int status = pclose(pipe);
	if (!result || status != 0) {
		buffer[0] = '\0';
		return -1;
	}
	trim(buffer);
	return 0;
}

static int uci_get(const char *key, char *buffer, unsigned int size)
{
	if (!key || strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@[]-.") != strlen(key))
		return -1;
	char command[256];
	snprintf(command, sizeof(command), "/sbin/uci -q get %s 2>/dev/null", key);
	return run_line(command, buffer, size);
}

static int ubus_interface_value(const char *interface, const char *path,
				char *buffer, unsigned int size)
{
	if (!interface || !path ||
	    strspn(interface, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(interface) ||
	    strspn(path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@.[]\"-") != strlen(path))
		return -1;
	char command[512];
	snprintf(command, sizeof(command),
		"/bin/ubus call network.interface.%s status 2>/dev/null | "
		"/usr/bin/jsonfilter -e '%s' 2>/dev/null", interface, path);
	return run_line(command, buffer, size);
}

static bool path_exists(const char *path)
{
	return access(path, F_OK) == 0;
}

static int read_uint64(const char *path, uint64_t *value)
{
	char text[64];
	if (read_file_line(path, text, sizeof(text)) != 0)
		return -1;
	char *end = NULL;
	errno = 0;
	unsigned long long parsed = strtoull(text, &end, 10);
	if (errno || end == text)
		return -1;
	*value = parsed;
	return 0;
}

static void find_default_interface(char *interface, unsigned int size)
{
	interface[0] = '\0';
	FILE *file = fopen("/proc/net/route", "r");
	if (!file)
		return;
	char line[512];
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return;
	}
	unsigned long best_metric = ~0UL;
	while (fgets(line, sizeof(line), file)) {
		char candidate[SCREENPLUS_TEXT_SHORT] = {0};
		unsigned long destination = 0;
		unsigned long gateway = 0;
		unsigned int flags = 0;
		unsigned long metric = 0;
		int fields = sscanf(line, "%31s %lx %lx %x %*u %*u %lu",
			candidate, &destination, &gateway, &flags, &metric);
		(void)gateway;
		if (fields == 5 && destination == 0 && (flags & 0x1U) && metric < best_metric) {
			strncpy(interface, candidate, size - 1);
			interface[size - 1] = '\0';
			best_metric = metric;
		}
	}
	fclose(file);
}

static void sample_interface(const char *name, const char *active_interface,
			     struct uplink_info *uplink)
{
	char up[16] = {0};
	char available[16] = {0};
	char device[SCREENPLUS_TEXT_SHORT] = {0};
	strncpy(uplink->logical_interface, name, sizeof(uplink->logical_interface) - 1);
	if (ubus_interface_value(name, "@.up", up, sizeof(up)) != 0) {
		uplink->state = SCREENPLUS_STATE_UNAVAILABLE;
		return;
	}
	ubus_interface_value(name, "@.available", available, sizeof(available));
	if (ubus_interface_value(name, "@.l3_device", device, sizeof(device)) != 0)
		ubus_interface_value(name, "@.device", device, sizeof(device));
	strncpy(uplink->device, device, sizeof(uplink->device) - 1);
	ubus_interface_value(name, "@[\"ipv4-address\"][0].address",
		uplink->ipv4, sizeof(uplink->ipv4));
	ubus_interface_value(name, "@.route[0].nexthop",
		uplink->gateway, sizeof(uplink->gateway));
	ubus_interface_value(name, "@[\"dns-server\"][0]",
		uplink->dns, sizeof(uplink->dns));
	if (strcmp(up, "true") == 0) {
		uplink->state = device[0] && strcmp(device, active_interface) == 0 ?
			SCREENPLUS_STATE_ACTIVE : SCREENPLUS_STATE_CONNECTED;
		snprintf(uplink->detail, sizeof(uplink->detail), "%s",
			uplink->ipv4[0] ? uplink->ipv4 : (device[0] ? device : "UP"));
	} else if (strcmp(available, "true") == 0) {
		uplink->state = SCREENPLUS_STATE_CONNECTING;
		strcpy(uplink->detail, "WAIT");
	} else {
		uplink->state = SCREENPLUS_STATE_IDLE;
		strcpy(uplink->detail, "IDLE");
	}
}

static void sample_ethernet(const char *active_interface, struct uplink_info *uplink)
{
	sample_interface("wan", active_interface, uplink);
	if (uplink->state == SCREENPLUS_STATE_UNAVAILABLE)
		sample_interface("secondwan", active_interface, uplink);
}

static void sample_repeater(const char *active_interface, struct uplink_info *uplink)
{
	char running[16] = {0};
	if (run_line("/bin/ubus call repeater status 2>/dev/null | "
		     "/usr/bin/jsonfilter -e '@.running' 2>/dev/null",
		     running, sizeof(running)) == 0 && strcmp(running, "true") != 0) {
		uplink->state = SCREENPLUS_STATE_UNAVAILABLE;
		return;
	}

	sample_interface("wwan", active_interface, uplink);
	if (strcmp(running, "true") == 0 &&
	    (uplink->state == SCREENPLUS_STATE_UNAVAILABLE ||
	     uplink->state == SCREENPLUS_STATE_IDLE)) {
		uplink->state = SCREENPLUS_STATE_CONNECTING;
		strcpy(uplink->detail, "WAIT");
	}
}

static void sample_tethering(const char *active_interface, struct uplink_info *uplink)
{
	sample_interface("tethering", active_interface, uplink);
}

static void sample_cellular(const char *active_interface, struct uplink_info *uplink)
{
	char object[SCREENPLUS_TEXT_MEDIUM];
	if (run_line("/bin/ubus list 'network.interface.modem_*' 2>/dev/null | head -n 1",
		     object, sizeof(object)) == 0 && strncmp(object, "network.interface.", 18) == 0) {
		sample_interface(object + 18, active_interface, uplink);
	}
}

static void sample_wifi_band(const char *section, const char *device_section, struct wifi_info *wifi)
{
	char key[128];
	char disabled[16] = {0};
	char device_disabled[16] = {0};
	snprintf(key, sizeof(key), "wireless.%s.ssid", section);
	if (uci_get(key, wifi->ssid, sizeof(wifi->ssid)) != 0)
		return;
	wifi->configured = true;
	snprintf(key, sizeof(key), "wireless.%s.key", section);
	uci_get(key, wifi->password, sizeof(wifi->password));
	snprintf(key, sizeof(key), "wireless.%s.encryption", section);
	uci_get(key, wifi->encryption, sizeof(wifi->encryption));
	snprintf(key, sizeof(key), "wireless.%s.channel", device_section);
	uci_get(key, wifi->channel, sizeof(wifi->channel));
	snprintf(key, sizeof(key), "wireless.%s.disabled", section);
	uci_get(key, disabled, sizeof(disabled));
	snprintf(key, sizeof(key), "wireless.%s.disabled", device_section);
	uci_get(key, device_disabled, sizeof(device_disabled));
	wifi->enabled = strcmp(disabled, "1") != 0 && strcmp(device_disabled, "1") != 0;
}

static void sample_port(struct system_info_state *state, struct system_snapshot *snapshot,
			unsigned int index, const char *device, double elapsed)
{
	struct ethernet_port_info *port = &snapshot->ports[index];
	strncpy(port->device, device, sizeof(port->device) - 1);
	char path[160];
	char value[64];
	snprintf(path, sizeof(path), "/sys/class/net/%s", device);
	port->present = path_exists(path);
	if (!port->present)
		return;
	snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", device);
	port->carrier = read_file_line(path, value, sizeof(value)) == 0 && strcmp(value, "1") == 0;
	snprintf(path, sizeof(path), "/sys/class/net/%s/speed", device);
	if (read_file_line(path, value, sizeof(value)) == 0)
		port->speed_mbps = (unsigned int)strtoul(value, NULL, 10);
	snprintf(path, sizeof(path), "/sys/class/net/%s/duplex", device);
	read_file_line(path, port->duplex, sizeof(port->duplex));
	snprintf(path, sizeof(path), "/sys/class/net/%s/address", device);
	read_file_line(path, port->mac, sizeof(port->mac));

	char wan_device[SCREENPLUS_TEXT_SHORT] = {0};
	char secondwan_device[SCREENPLUS_TEXT_SHORT] = {0};
	uci_get("network.wan.device", wan_device, sizeof(wan_device));
	uci_get("network.secondwan.device", secondwan_device, sizeof(secondwan_device));
	if (strcmp(device, wan_device) == 0) {
		strcpy(port->role, "WAN");
		strcpy(port->logical_interface, "wan");
	} else if (strcmp(device, secondwan_device) == 0) {
		strcpy(port->role, "WAN2");
		strcpy(port->logical_interface, "secondwan");
	} else {
		char command[256];
		snprintf(command, sizeof(command),
			"/sbin/uci -q show network 2>/dev/null | grep -E \"\\.ports=.*'%s'\" | head -n 1",
			device);
		if (run_line(command, value, sizeof(value)) == 0) {
			strcpy(port->role, "LAN");
			strcpy(port->logical_interface, "lan");
		} else
			strcpy(port->role, "PORT");
	}
	if (port->logical_interface[0]) {
		ubus_interface_value(port->logical_interface, "@[\"ipv4-address\"][0].address",
			port->ipv4, sizeof(port->ipv4));
		ubus_interface_value(port->logical_interface, "@.route[0].nexthop",
			port->gateway, sizeof(port->gateway));
		ubus_interface_value(port->logical_interface, "@[\"dns-server\"][0]",
			port->dns, sizeof(port->dns));
	}

	uint64_t receive = 0;
	uint64_t transmit = 0;
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", device);
	read_uint64(path, &receive);
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", device);
	read_uint64(path, &transmit);
	if (elapsed > 0.0 && receive >= state->port_receive_bytes[index] &&
	    state->port_receive_bytes[index] > 0)
		port->receive_bytes_per_second =
			(double)(receive - state->port_receive_bytes[index]) / elapsed;
	if (elapsed > 0.0 && transmit >= state->port_transmit_bytes[index] &&
	    state->port_transmit_bytes[index] > 0)
		port->transmit_bytes_per_second =
			(double)(transmit - state->port_transmit_bytes[index]) / elapsed;
	state->port_receive_bytes[index] = receive;
	state->port_transmit_bytes[index] = transmit;
}

static int parse_json_uint64(const char *json, const char *key, uint64_t *value)
{
	const char *position = strstr(json, key);
	if (!position)
		return -1;
	position = strchr(position + strlen(key), ':');
	if (!position)
		return -1;
	while (*++position == ' ' || *position == '\t') { }
	char *end = NULL;
	errno = 0;
	unsigned long long parsed = strtoull(position, &end, 10);
	if (errno || end == position)
		return -1;
	*value = parsed;
	return 0;
}

static unsigned int count_connection_objects(const char *json)
{
	const char *position = strstr(json, "\"connections\"");
	if (!position || !(position = strchr(position, '[')))
		return 0;
	unsigned int count = 0;
	unsigned int array_depth = 1;
	unsigned int object_depth = 0;
	bool in_string = false;
	bool escaped = false;
	for (++position; *position && array_depth; ++position) {
		char current = *position;
		if (in_string) {
			if (escaped)
				escaped = false;
			else if (current == '\\')
				escaped = true;
			else if (current == '"')
				in_string = false;
			continue;
		}
		if (current == '"')
			in_string = true;
		else if (current == '[')
			++array_depth;
		else if (current == ']')
			--array_depth;
		else if (current == '{') {
			if (array_depth == 1 && object_depth == 0)
				++count;
			++object_depth;
		} else if (current == '}' && object_depth)
			--object_depth;
	}
	return count;
}

static int query_openclash_api(unsigned int port, const char *secret,
			       uint64_t *download, uint64_t *upload,
			       unsigned int *connections)
{
	if (!port || port > 65535 || strpbrk(secret, "\r\n"))
		return -1;
	int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd < 0)
		return -1;
	struct timeval timeout = { .tv_sec = 1, .tv_usec = 500000 };
	setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons((uint16_t)port),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		close(socket_fd);
		return -1;
	}
	char request[512];
	int request_length = snprintf(request, sizeof(request),
		"GET /connections HTTP/1.0\r\nHost: 127.0.0.1\r\n"
		"Authorization: Bearer %s\r\nConnection: close\r\n\r\n", secret);
	if (request_length <= 0 || (size_t)request_length >= sizeof(request)) {
		close(socket_fd);
		return -1;
	}
	size_t sent = 0;
	while (sent < (size_t)request_length) {
		ssize_t result = send(socket_fd, request + sent, (size_t)request_length - sent, 0);
		if (result <= 0) {
			close(socket_fd);
			return -1;
		}
		sent += (size_t)result;
	}
	const size_t response_capacity = 256U * 1024U;
	char *response = malloc(response_capacity);
	if (!response) {
		close(socket_fd);
		return -1;
	}
	size_t used = 0;
	while (used + 1 < response_capacity) {
		ssize_t result = recv(socket_fd, response + used, response_capacity - used - 1, 0);
		if (result == 0)
			break;
		if (result < 0) {
			if (errno == EINTR)
				continue;
			free(response);
			close(socket_fd);
			return -1;
		}
		used += (size_t)result;
	}
	close(socket_fd);
	response[used] = '\0';
	bool http_ok = strstr(response, " 200 ") != NULL;
	int result = http_ok && parse_json_uint64(response, "\"downloadTotal\"", download) == 0 &&
		parse_json_uint64(response, "\"uploadTotal\"", upload) == 0 ? 0 : -1;
	if (result == 0)
		*connections = count_connection_objects(response);
	free(response);
	return result;
}

static void sample_openclash(struct system_info_state *state,
			     struct openclash_info *openclash, double elapsed)
{
	if (!path_exists("/etc/init.d/openclash")) {
		openclash->state = SCREENPLUS_STATE_UNAVAILABLE;
		strcpy(openclash->mode, "NOT INSTALLED");
		return;
	}
	char enabled[16] = {0};
	uci_get("openclash.config.enable", enabled, sizeof(enabled));
	uci_get("openclash.config.operation_mode", openclash->mode, sizeof(openclash->mode));
	char status[32];
	if (run_line("/etc/init.d/openclash status 2>/dev/null", status, sizeof(status)) == 0 &&
	    strcmp(status, "running") == 0) {
		openclash->state = SCREENPLUS_STATE_ACTIVE;
		char port_text[16] = {0};
		char secret[SCREENPLUS_TEXT_MEDIUM] = {0};
		uci_get("openclash.config.cn_port", port_text, sizeof(port_text));
		uci_get("openclash.config.dashboard_password", secret, sizeof(secret));
		unsigned long port = strtoul(port_text, NULL, 10);
		if (query_openclash_api((unsigned int)port, secret,
			&openclash->download_total_bytes, &openclash->upload_total_bytes,
			&openclash->connection_count) == 0) {
			openclash->metrics_available = true;
			if (elapsed > 0.0 && state->openclash_download_bytes > 0 &&
			    openclash->download_total_bytes >= state->openclash_download_bytes)
				openclash->download_bytes_per_second =
					(double)(openclash->download_total_bytes -
					state->openclash_download_bytes) / elapsed;
			if (elapsed > 0.0 && state->openclash_upload_bytes > 0 &&
			    openclash->upload_total_bytes >= state->openclash_upload_bytes)
				openclash->upload_bytes_per_second =
					(double)(openclash->upload_total_bytes -
					state->openclash_upload_bytes) / elapsed;
			state->openclash_download_bytes = openclash->download_total_bytes;
			state->openclash_upload_bytes = openclash->upload_total_bytes;
		}
	} else if (strcmp(enabled, "1") == 0)
		openclash->state = SCREENPLUS_STATE_ERROR;
	else
		openclash->state = SCREENPLUS_STATE_IDLE;
}

static void sample_firmware(char *buffer, size_t size)
{
	FILE *file = fopen("/etc/openwrt_release", "r");
	if (!file)
		return;
	char line[256];
	while (fgets(line, sizeof(line), file)) {
		const char prefix[] = "DISTRIB_DESCRIPTION=";
		if (strncmp(line, prefix, sizeof(prefix) - 1) != 0)
			continue;
		char *value = line + sizeof(prefix) - 1;
		trim(value);
		size_t length = strlen(value);
		if (length >= 2 && ((*value == '\'' && value[length - 1] == '\'') ||
				    (*value == '"' && value[length - 1] == '"'))) {
			value[length - 1] = '\0';
			++value;
		}
		snprintf(buffer, size, "%s", value);
		break;
	}
	fclose(file);
}

void system_info_state_initialize(struct system_info_state *state)
{
	memset(state, 0, sizeof(*state));
}

int system_info_sample(struct system_info_state *state, struct system_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	uint64_t now = monotonic_milliseconds();
	double elapsed = state->sampled_milliseconds && now > state->sampled_milliseconds ?
		(double)(now - state->sampled_milliseconds) / 1000.0 : 0.0;
	char active_interface[SCREENPLUS_TEXT_SHORT];
	find_default_interface(active_interface, sizeof(active_interface));
	sample_ethernet(active_interface, &snapshot->ethernet);
	sample_repeater(active_interface, &snapshot->repeater);
	sample_tethering(active_interface, &snapshot->tethering);
	sample_cellular(active_interface, &snapshot->cellular);
	sample_wifi_band("wifi2g", "wifi0", &snapshot->wifi_2g);
	sample_wifi_band("wifi5g", "wifi1", &snapshot->wifi_5g);
	sample_wifi_band("wlanmld2g", "wifi0", &snapshot->wifi_mlo);
	sample_port(state, snapshot, 0, "eth0", elapsed);
	sample_port(state, snapshot, 1, "eth1", elapsed);
	sample_openclash(state, &snapshot->openclash, elapsed);
	read_file_line("/proc/sys/kernel/hostname", snapshot->hostname, sizeof(snapshot->hostname));
	read_file_line("/tmp/sysinfo/model", snapshot->model, sizeof(snapshot->model));
	sample_firmware(snapshot->firmware, sizeof(snapshot->firmware));
	ubus_interface_value("lan", "@[\"ipv4-address\"][0].address",
		snapshot->lan_ipv4, sizeof(snapshot->lan_ipv4));
	state->sampled_milliseconds = now;
	return 0;
}

const char *system_info_state_text(enum screenplus_state state)
{
	switch (state) {
	case SCREENPLUS_STATE_IDLE: return "IDLE";
	case SCREENPLUS_STATE_CONNECTING: return "WAIT";
	case SCREENPLUS_STATE_CONNECTED: return "READY";
	case SCREENPLUS_STATE_ACTIVE: return "ACTIVE";
	case SCREENPLUS_STATE_ERROR: return "ERROR";
	default: return "N/A";
	}
}
