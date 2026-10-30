/*
 * BME688 environmental sensor on I2C0 (SDA GPIO6, SCL GPIO7).
 *
 * Zephyr's bosch,bme680 driver reads temperature, humidity and pressure
 * correctly on the BME688 (same chip ID and registers). Its gas resistance
 * is NOT valid on the BME688: the driver uses the BME680's run_gas bit, gas
 * registers (0x2A/0x2B) and formula, while the BME688 needs 0x20, 0x2C/0x2D
 * and a different formula. So gas is not read here.
 *
 * The overlay declares the sensor at both possible addresses (0x76 and
 * 0x77); whichever one answered at boot is used.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(bme688, LOG_LEVEL_INF);

#define BME_DEV(node) DEVICE_DT_GET(node),
static const struct device *const candidates[] = {
	DT_FOREACH_STATUS_OKAY(bosch_bme680, BME_DEV)
};

static const struct device *find_sensor(void)
{
	static const struct device *dev;
	static bool searched;

	if (!searched) {
		searched = true;
		for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
			if (device_is_ready(candidates[i])) {
				dev = candidates[i];
				LOG_INF("BME688 found: %s", dev->name);
				break;
			}
		}
		if (dev == NULL) {
			LOG_WRN("No BME688 on I2C0 (0x76/0x77). Check wiring, then reset");
		}
	}
	return dev;
}

int bme688_read(struct readings *r)
{
	const struct device *dev = find_sensor();
	struct sensor_value temp, hum, press;
	int ret;

	if (dev == NULL) {
		return -ENODEV;
	}

	ret = sensor_sample_fetch(dev);
	if (ret) {
		LOG_WRN("BME688 read failed (%d)", ret);
		return ret;
	}

	sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hum);
	sensor_channel_get(dev, SENSOR_CHAN_PRESS, &press);

	r->temp_c = sensor_value_to_double(&temp);
	r->humidity = sensor_value_to_double(&hum);
	r->pressure_hpa = sensor_value_to_double(&press) * 10.0; /* driver reports kPa */
	return 0;
}
