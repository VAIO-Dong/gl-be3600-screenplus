#ifndef SCREENPLUS_METRICS_H
#define SCREENPLUS_METRICS_H

#include <stdint.h>

#define SCREENPLUS_INTERFACE_NAME_SIZE 32

enum network_acceleration_mode {
	NETWORK_ACCELERATION_OFF = 0,
	NETWORK_ACCELERATION_SOFTWARE,
	NETWORK_ACCELERATION_NSS,
};

struct system_metrics {
	double cpu_percent;
	double temperature_celsius;
	unsigned int fan_rpm;
	double memory_percent;
	uint64_t memory_used_bytes;
	uint64_t memory_total_bytes;
	double storage_percent;
	uint64_t storage_used_bytes;
	uint64_t storage_total_bytes;
	double disk_read_bytes_per_second;
	double disk_write_bytes_per_second;
	double network_receive_bytes_per_second;
	double network_transmit_bytes_per_second;
	unsigned int network_connection_count;
	uint64_t network_receive_total_bytes;
	uint64_t network_transmit_total_bytes;
	int network_hardware_accelerated;
	enum network_acceleration_mode network_acceleration;
	uint64_t uptime_seconds;
	char network_interface[SCREENPLUS_INTERFACE_NAME_SIZE];
};

struct metrics_state {
	uint64_t cpu_total;
	uint64_t cpu_idle;
	uint64_t disk_read_bytes;
	uint64_t disk_write_bytes;
	uint64_t network_receive_bytes;
	uint64_t network_transmit_bytes;
	uint64_t sampled_milliseconds;
	char network_interface[SCREENPLUS_INTERFACE_NAME_SIZE];
};

void metrics_state_initialize(struct metrics_state *state);
int metrics_sample(struct metrics_state *state, struct system_metrics *metrics);
void metrics_format_rate(double bytes_per_second, char *buffer, unsigned int size);
const char *metrics_acceleration_text(enum network_acceleration_mode mode);

#endif
