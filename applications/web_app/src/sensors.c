/*
 * On-board sensors. For now only the ESP32-C6 die temperature; external
 * sensors (BME688 etc.) go here later.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include "app.h"

static const struct device *const temp_dev = DEVICE_DT_GET(DT_ALIAS(die_temp0));

int sensors_die_temp(int32_t *milli_c)
{
	struct sensor_value val;
	int ret;

	if (!device_is_ready(temp_dev)) {
		return -ENODEV;
	}

	ret = sensor_sample_fetch(temp_dev);
	if (ret == 0) {
		ret = sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &val);
	}
	if (ret == 0) {
		*milli_c = (int32_t)sensor_value_to_milli(&val);
	}
	return ret;
}
