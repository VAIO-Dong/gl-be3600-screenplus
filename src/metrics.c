#define _POSIX_C_SOURCE 200809L

#include "metrics.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static int read_cpu(uint64_t *total, uint64_t *idle)
{
	FILE *file = fopen("/proc/stat", "r");
	if (!file)
		return -1;
	char line[512];
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return -1;
	}
	fclose(file);
	unsigned long long values[10] = {0};
	int fields = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
		&values[0], &values[1], &values[2], &values[3], &values[4],
		&values[5], &values[6], &values[7], &values[8], &values[9]);
	if (fields < 4)
		return -1;
	*total = 0;
	for (int index = 0; index < fields; ++index)
		*total += values[index];
	*idle = values[3] + (fields > 4 ? values[4] : 0);
	return 0;
}

static int read_memory(struct system_metrics *metrics)
{
	FILE *file = fopen("/proc/meminfo", "r");
	if (!file)
		return -1;
	uint64_t total_kib = 0;
	uint64_t available_kib = 0;
	uint64_t free_kib = 0;
	uint64_t buffers_kib = 0;
	uint64_t cached_kib = 0;
	char line[256];
	while (fgets(line, sizeof(line), file)) {
		unsigned long long value = 0;
		if (sscanf(line, "MemTotal: %llu kB", &value) == 1)
			total_kib = value;
		else if (sscanf(line, "MemAvailable: %llu kB", &value) == 1)
			available_kib = value;
		else if (sscanf(line, "MemFree: %llu kB", &value) == 1)
			free_kib = value;
		else if (sscanf(line, "Buffers: %llu kB", &value) == 1)
			buffers_kib = value;
		else if (sscanf(line, "Cached: %llu kB", &value) == 1)
			cached_kib = value;
	}
	fclose(file);
	if (!total_kib)
		return -1;
	if (!available_kib)
		available_kib = free_kib + buffers_kib + cached_kib;
	if (available_kib > total_kib)
		available_kib = total_kib;
	metrics->memory_total_bytes = total_kib * 1024U;
	metrics->memory_used_bytes = (total_kib - available_kib) * 1024U;
	metrics->memory_percent = 100.0 * (double)metrics->memory_used_bytes /
		(double)metrics->memory_total_bytes;
	return 0;
}

static int read_storage(struct system_metrics *metrics)
{
	struct statvfs status;
	if (statvfs("/", &status) != 0 || !status.f_blocks)
		return -1;
	uint64_t block_size = status.f_frsize ? status.f_frsize : status.f_bsize;
	metrics->storage_total_bytes = (uint64_t)status.f_blocks * block_size;
	uint64_t available = (uint64_t)status.f_bavail * block_size;
	if (available > metrics->storage_total_bytes)
		available = metrics->storage_total_bytes;
	metrics->storage_used_bytes = metrics->storage_total_bytes - available;
	metrics->storage_percent = 100.0 * (double)metrics->storage_used_bytes /
		(double)metrics->storage_total_bytes;
	return 0;
}

static bool is_counted_disk(const char *name)
{
	if (strncmp(name, "mtdblock", 8) == 0)
		return true;
	if (strncmp(name, "mmcblk", 6) == 0)
		return strchr(name + 6, 'p') == NULL;
	if (strncmp(name, "nvme", 4) == 0)
		return strrchr(name, 'p') == NULL;
	if (name[0] == 's' && name[1] == 'd' && name[2] >= 'a' && name[2] <= 'z' && !name[3])
		return true;
	if (name[0] == 'v' && name[1] == 'd' && name[2] >= 'a' && name[2] <= 'z' && !name[3])
		return true;
	return false;
}

static int read_disk_bytes(uint64_t *read_bytes, uint64_t *write_bytes)
{
	FILE *file = fopen("/proc/diskstats", "r");
	if (!file)
		return -1;
	*read_bytes = 0;
	*write_bytes = 0;
	char line[512];
	while (fgets(line, sizeof(line), file)) {
		unsigned int major = 0;
		unsigned int minor = 0;
		char name[64] = {0};
		unsigned long long reads = 0;
		unsigned long long reads_merged = 0;
		unsigned long long sectors_read = 0;
		unsigned long long read_ms = 0;
		unsigned long long writes = 0;
		unsigned long long writes_merged = 0;
		unsigned long long sectors_written = 0;
		int fields = sscanf(line,
			"%u %u %63s %llu %llu %llu %llu %llu %llu %llu",
			&major, &minor, name, &reads, &reads_merged, &sectors_read,
			&read_ms, &writes, &writes_merged, &sectors_written);
		(void)major;
		(void)minor;
		(void)reads;
		(void)reads_merged;
		(void)read_ms;
		(void)writes;
		(void)writes_merged;
		if (fields >= 10 && is_counted_disk(name)) {
			*read_bytes += sectors_read * 512U;
			*write_bytes += sectors_written * 512U;
		}
	}
	fclose(file);
	return 0;
}

static int find_default_interface(char *interface, unsigned int size)
{
	FILE *file = fopen("/proc/net/route", "r");
	if (!file)
		return -1;
	char line[512];
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return -1;
	}
	unsigned long best_metric = ~0UL;
	bool found = false;
	while (fgets(line, sizeof(line), file)) {
		char candidate[SCREENPLUS_INTERFACE_NAME_SIZE] = {0};
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
			found = true;
		}
	}
	fclose(file);
	return found ? 0 : -1;
}

static int read_counter_file(const char *path, uint64_t *value)
{
	FILE *file = fopen(path, "r");
	if (!file)
		return -1;
	unsigned long long parsed = 0;
	int result = fscanf(file, "%llu", &parsed);
	fclose(file);
	if (result != 1)
		return -1;
	*value = parsed;
	return 0;
}

static int read_network_bytes(char *interface, unsigned int size,
			      uint64_t *receive, uint64_t *transmit)
{
	if (find_default_interface(interface, size) != 0)
		strncpy(interface, "eth0", size - 1);
	interface[size - 1] = '\0';
	char path[256];
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", interface);
	if (read_counter_file(path, receive) != 0)
		return -1;
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", interface);
	return read_counter_file(path, transmit);
}

static double read_temperature(void)
{
	double maximum = 0.0;
	for (unsigned int index = 0; index < 64; ++index) {
		char path[128];
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%u/temp", index);
		uint64_t raw = 0;
		if (read_counter_file(path, &raw) != 0)
			continue;
		double value = raw > 1000U ? (double)raw / 1000.0 : (double)raw;
		if (value > maximum && value < 160.0)
			maximum = value;
	}
	return maximum;
}

static uint64_t read_uptime(void)
{
	FILE *file = fopen("/proc/uptime", "r");
	if (!file)
		return 0;
	double seconds = 0.0;
	int result = fscanf(file, "%lf", &seconds);
	fclose(file);
	return result == 1 && seconds > 0.0 ? (uint64_t)seconds : 0;
}

void metrics_state_initialize(struct metrics_state *state)
{
	memset(state, 0, sizeof(*state));
}

static double counter_rate(uint64_t current, uint64_t previous, double elapsed_seconds)
{
	if (!previous || current < previous || elapsed_seconds <= 0.0)
		return 0.0;
	return (double)(current - previous) / elapsed_seconds;
}

int metrics_sample(struct metrics_state *state, struct system_metrics *metrics)
{
	memset(metrics, 0, sizeof(*metrics));
	uint64_t now = monotonic_milliseconds();
	uint64_t cpu_total = 0;
	uint64_t cpu_idle = 0;
	uint64_t disk_read = 0;
	uint64_t disk_write = 0;
	uint64_t network_receive = 0;
	uint64_t network_transmit = 0;
	char interface[SCREENPLUS_INTERFACE_NAME_SIZE] = {0};
	int errors = 0;
	if (read_cpu(&cpu_total, &cpu_idle) != 0)
		errors++;
	if (read_memory(metrics) != 0)
		errors++;
	if (read_storage(metrics) != 0)
		errors++;
	if (read_disk_bytes(&disk_read, &disk_write) != 0)
		errors++;
	if (read_network_bytes(interface, sizeof(interface), &network_receive, &network_transmit) != 0)
		errors++;

	double elapsed = state->sampled_milliseconds && now > state->sampled_milliseconds ?
		(double)(now - state->sampled_milliseconds) / 1000.0 : 0.0;
	if (state->cpu_total && cpu_total >= state->cpu_total && cpu_idle >= state->cpu_idle) {
		uint64_t total_delta = cpu_total - state->cpu_total;
		uint64_t idle_delta = cpu_idle - state->cpu_idle;
		if (total_delta && idle_delta <= total_delta)
			metrics->cpu_percent = 100.0 * (double)(total_delta - idle_delta) / (double)total_delta;
	}
	metrics->disk_read_bytes_per_second = counter_rate(disk_read, state->disk_read_bytes, elapsed);
	metrics->disk_write_bytes_per_second = counter_rate(disk_write, state->disk_write_bytes, elapsed);
	if (strcmp(interface, state->network_interface) == 0) {
		metrics->network_receive_bytes_per_second =
			counter_rate(network_receive, state->network_receive_bytes, elapsed);
		metrics->network_transmit_bytes_per_second =
			counter_rate(network_transmit, state->network_transmit_bytes, elapsed);
	}
	metrics->temperature_celsius = read_temperature();
	metrics->uptime_seconds = read_uptime();
	strncpy(metrics->network_interface, interface, sizeof(metrics->network_interface) - 1);

	state->cpu_total = cpu_total;
	state->cpu_idle = cpu_idle;
	state->disk_read_bytes = disk_read;
	state->disk_write_bytes = disk_write;
	state->network_receive_bytes = network_receive;
	state->network_transmit_bytes = network_transmit;
	state->sampled_milliseconds = now;
	strncpy(state->network_interface, interface, sizeof(state->network_interface) - 1);
	return errors ? -1 : 0;
}

void metrics_format_rate(double bytes_per_second, char *buffer, unsigned int size)
{
	if (!buffer || !size)
		return;
	if (bytes_per_second < 1024.0)
		snprintf(buffer, size, "%.0fB", bytes_per_second);
	else if (bytes_per_second < 1024.0 * 1024.0)
		snprintf(buffer, size, "%.0fK", bytes_per_second / 1024.0);
	else if (bytes_per_second < 1024.0 * 1024.0 * 1024.0)
		snprintf(buffer, size, "%.1fM", bytes_per_second / (1024.0 * 1024.0));
	else
		snprintf(buffer, size, "%.1fG", bytes_per_second / (1024.0 * 1024.0 * 1024.0));
}
