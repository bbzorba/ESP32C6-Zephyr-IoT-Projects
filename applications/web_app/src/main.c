/*
 * Home Hub - ESP32-C6 sensor node / server.
 *
 * Connects to the home Wi-Fi, serves a dashboard over HTTP and keeps a list
 * of the devices on the LAN (see lan_scan.c). Runs 24/7.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/net/http/server.h>

#include "app.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/*
 * The onboard LED on ESP32-C6-DevKitC is a WS2812B RGB LED on GPIO8.
 * It is driven via I2S, not plain GPIO. Use the led_strip API.
 */
#define STRIP_NODE       DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_NUM_PIXELS];

/* Low brightness: the WS2812 is very bright at full intensity */
static const struct led_rgb status_colors[] = {
	[STATUS_LED_NO_CONFIG]  = { .r = 0x10, .g = 0x00, .b = 0x00 },
	[STATUS_LED_CONNECTING] = { .r = 0x00, .g = 0x00, .b = 0x10 },
	[STATUS_LED_ONLINE]     = { .r = 0x00, .g = 0x08, .b = 0x00 },
	[STATUS_LED_SCANNING]   = { .r = 0x08, .g = 0x00, .b = 0x08 },
};

void status_led_set(enum status_led state)
{
	if (!device_is_ready(strip)) {
		return;
	}
	pixels[0] = status_colors[state];
	led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
}

int main(void)
{
	LOG_INF("Home Hub starting");

	http_server_start();
	wifi_start();

	return 0;
}
