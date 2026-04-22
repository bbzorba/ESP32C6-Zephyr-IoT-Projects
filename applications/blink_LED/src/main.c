/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS   1000

/*
 * The onboard LED on ESP32-C6-DevKitC is a WS2812B RGB LED on GPIO8.
 * It is driven via I2S, not plain GPIO. Use the led_strip API.
 */
#define STRIP_NODE       DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_NUM_PIXELS];

/* White at low brightness — WS2812 is very bright at full intensity */
static const struct led_rgb on_color  = { .r = 0x10, .g = 0x10, .b = 0x10 };
static const struct led_rgb off_color = { .r = 0x00, .g = 0x00, .b = 0x00 };

int main(void)
{
	bool led_state = false;

	if (!device_is_ready(strip)) {
		printf("LED strip device not ready\n");
		return 0;
	}

	while (1) {
		led_state = !led_state;
		pixels[0] = led_state ? on_color : off_color;
		led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
		printf("LED state: %s\n", led_state ? "ON" : "OFF");
		k_msleep(SLEEP_TIME_MS);
	}
	return 0;
}
