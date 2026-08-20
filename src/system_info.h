#ifndef SCREENPLUS_SYSTEM_INFO_H
#define SCREENPLUS_SYSTEM_INFO_H

#include <stdbool.h>
#include <stdint.h>

#define SCREENPLUS_TEXT_SHORT 32
#define SCREENPLUS_TEXT_MEDIUM 96

enum screenplus_state {
	SCREENPLUS_STATE_UNAVAILABLE,
	SCREENPLUS_STATE_IDLE,
	SCREENPLUS_STATE_CONNECTING,
	SCREENPLUS_STATE_CONNECTED,
	SCREENPLUS_STATE_ACTIVE,
	SCREENPLUS_STATE_ERROR,
};

struct uplink_info {
	enum screenplus_state state;
	char logical_interface[SCREENPLUS_TEXT_SHORT];
	char device[SCREENPLUS_TEXT_SHORT];
	char ipv4[SCREENPLUS_TEXT_SHORT];
	char gateway[SCREENPLUS_TEXT_SHORT];
	char dns[SCREENPLUS_TEXT_SHORT];
	char detail[SCREENPLUS_TEXT_MEDIUM];
};

struct wifi_info {
	bool configured;
	bool enabled;
	char ssid[SCREENPLUS_TEXT_MEDIUM];
	char password[SCREENPLUS_TEXT_MEDIUM];
	char encryption[SCREENPLUS_TEXT_SHORT];
	char channel[SCREENPLUS_TEXT_SHORT];
};

struct ethernet_port_info {
	bool present;
	bool carrier;
	unsigned int speed_mbps;
	char device[SCREENPLUS_TEXT_SHORT];
	char role[SCREENPLUS_TEXT_SHORT];
	char logical_interface[SCREENPLUS_TEXT_SHORT];
	char duplex[SCREENPLUS_TEXT_SHORT];
	char mac[SCREENPLUS_TEXT_SHORT];
	char ipv4[SCREENPLUS_TEXT_SHORT];
	char gateway[SCREENPLUS_TEXT_SHORT];
	char dns[SCREENPLUS_TEXT_SHORT];
	double receive_bytes_per_second;
	double transmit_bytes_per_second;
};

struct openclash_info {
	enum screenplus_state state;
	char mode[SCREENPLUS_TEXT_SHORT];
	bool metrics_available;
	double download_bytes_per_second;
	double upload_bytes_per_second;
	uint64_t download_total_bytes;
	uint64_t upload_total_bytes;
	unsigned int connection_count;
};

struct system_snapshot {
	struct uplink_info ethernet;
	struct uplink_info repeater;
	struct uplink_info tethering;
	struct uplink_info cellular;
	struct wifi_info wifi_2g;
	struct wifi_info wifi_5g;
	struct wifi_info wifi_mlo;
	struct ethernet_port_info ports[2];
	struct openclash_info openclash;
	char hostname[SCREENPLUS_TEXT_MEDIUM];
	char model[SCREENPLUS_TEXT_MEDIUM];
	char firmware[SCREENPLUS_TEXT_MEDIUM];
	char lan_ipv4[SCREENPLUS_TEXT_SHORT];
};

struct system_info_state {
	uint64_t sampled_milliseconds;
	uint64_t port_receive_bytes[2];
	uint64_t port_transmit_bytes[2];
	uint64_t openclash_download_bytes;
	uint64_t openclash_upload_bytes;
};

void system_info_state_initialize(struct system_info_state *state);
int system_info_sample(struct system_info_state *state, struct system_snapshot *snapshot);
const char *system_info_state_text(enum screenplus_state state);

#endif
