/*
 * BME688 environmental sensor on I2C0 (SDA GPIO6, SCL GPIO7).
 *
 * Zephyr's bosch,bme680 driver also handles the BME688 (same chip ID and
 * register map for temperature, humidity, pressure and gas resistance).
 * The overlay declares the sensor at both possible addresses (0x76 and
 * 0x77); whichever one answered at boot is used.
 *
 * A thread samples every READ_PERIOD and caches the result, so HTTP
 * requests never wait on the I2C bus.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(bme688, LOG_LEVEL_INF);

#define READ_PERIOD K_SECONDS(10)

#define BME_DEV(node) DEVICE_DT_GET(node),
static const struct device *const candidates[] = {
	DT_FOREACH_STATUS_OKAY(bosch_bme680, BME_DEV)
};

static struct env_reading latest;
static int state = -EAGAIN; /* 0 = have reading, -ENODEV = no sensor */
static K_MUTEX_DEFINE(lock);

static int read_once(const struct device *dev, struct env_reading *r)
{
	struct sensor_value temp, hum, press, gas;
	int ret;

	ret = sensor_sample_fetch(dev);
	if (ret) {
		return ret;
	}

	sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hum);
	sensor_channel_get(dev, SENSOR_CHAN_PRESS, &press);
	sensor_channel_get(dev, SENSOR_CHAN_GAS_RES, &gas);

	r->temp_mc = (int32_t)sensor_value_to_milli(&temp);
	r->humidity_m = (uint32_t)sensor_value_to_milli(&hum);
	/* Driver reports kPa, so milli-kPa is Pa */
	r->press_pa = (uint32_t)sensor_value_to_milli(&press);
	r->gas_ohm = (uint32_t)gas.val1;
	r->at = (uint32_t)(k_uptime_get() / MSEC_PER_SEC);
	return 0;
}

static void bme688_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = NULL;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
		if (device_is_ready(candidates[i])) {
			dev = candidates[i];
			break;
		}
	}

	if (dev == NULL) {
		LOG_WRN("No BME688 found on I2C0 (0x76/0x77). Check wiring, then reset");
		k_mutex_lock(&lock, K_FOREVER);
		state = -ENODEV;
		k_mutex_unlock(&lock);
		return;
	}

	LOG_INF("BME688 found: %s", dev->name);

	while (true) {
		struct env_reading r;
		int ret = read_once(dev, &r);

		if (ret) {
			LOG_WRN("BME688 read failed (%d)", ret);
		} else {
			k_mutex_lock(&lock, K_FOREVER);
			latest = r;
			state = 0;
			k_mutex_unlock(&lock);
			LOG_DBG("T %d mC, RH %u m%%, P %u Pa, gas %u ohm", r.temp_mc,
				r.humidity_m, r.press_pa, r.gas_ohm);
		}

		k_sleep(READ_PERIOD);
	}
}

K_THREAD_DEFINE(bme688_tid, 2048, bme688_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

int bme688_latest(struct env_reading *out)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = state;
	if (ret == 0) {
		*out = latest;
	}
	k_mutex_unlock(&lock);
	return ret;
}
