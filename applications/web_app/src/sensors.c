/*
 * Sensor readings, taken together on each sampler tick and cached so HTTP
 * requests never wait on a sensor. Hub: ESP32-C6 die temperature.
 * Environment: BME688 (bme688.c).
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include "app.h"

static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));

static struct readings latest = { .env_state = -EAGAIN };
static K_MUTEX_DEFINE(lock);

/* Busy share of the CPU since the last call, from the kernel's thread
 * usage accounting (CONFIG_SCHED_THREAD_USAGE_ALL).
 */
static double cpu_load_pct(void)
{
	static uint64_t last_all, last_busy;
	k_thread_runtime_stats_t s;
	double pct = 0.0;

	if (k_thread_runtime_stats_all_get(&s) == 0) {
		uint64_t all = s.execution_cycles - last_all;
		uint64_t busy = s.total_cycles - last_busy;

		if (all > 0) {
			pct = 100.0 * (double)busy / (double)all;
		}
		last_all = s.execution_cycles;
		last_busy = s.total_cycles;
	}
	return pct;
}

void sensors_sample(void)
{
	struct readings r = { .at = k_uptime_seconds() };
	struct sensor_value val;

	r.cpu_load_pct = cpu_load_pct();

	r.chip_ok = device_is_ready(die_temp) && sensor_sample_fetch(die_temp) == 0 &&
		    sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &val) == 0;
	if (r.chip_ok) {
		r.chip_temp_c = sensor_value_to_double(&val);
	}

	r.env_state = bme688_read(&r);

	k_mutex_lock(&lock, K_FOREVER);
	latest = r;
	k_mutex_unlock(&lock);
}

void sensors_get(struct readings *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = latest;
	k_mutex_unlock(&lock);
}
