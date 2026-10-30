/*
 * Home Hub - ESP32-C6 sensor node / server.
 *
 * Connects to the home Wi-Fi, serves a dashboard over HTTP and keeps a list
 * of the devices on the LAN. Runs 24/7.
 *
 * All periodic work hangs off one timer, so the CPU wakes once per tick:
 * every 10 s the hub and BME688 readings are taken together, and every
 * 60th tick (10 min) a LAN scan is started in the same wake-up.
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
	[STATUS_LED_CONNECTING] = { .r = 0x00, .g = 0x00, .b = 0x10 },
	[STATUS_LED_ONLINE]     = { .r = 0x00, .g = 0x08, .b = 0x00 },
	[STATUS_LED_SCANNING]   = { .r = 0x08, .g = 0x00, .b = 0x08 },
};

static K_TIMER_DEFINE(sample_timer, NULL, NULL);
static atomic_t next_scan_at;

void status_led_set(enum status_led state)
{
	if (!device_is_ready(strip)) {
		return;
	}
	pixels[0] = status_colors[state];
	led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
}

uint32_t sampler_next_scan_at(void)
{
	return atomic_get(&next_scan_at);
}

int main(void)
{
	uint32_t tick = 0;
	uint32_t next_scan_tick = 0;

	LOG_INF("Home Hub starting");

	http_server_start();
	wifi_start();

	k_timer_start(&sample_timer, K_NO_WAIT, K_SECONDS(SAMPLE_PERIOD_S));

	for (;; tick++) {
		k_timer_status_sync(&sample_timer);

		sensors_sample();

		/* First scan on the first tick after joining Wi-Fi, then every
		 * SCAN_EVERY_TICKS. Manual scans don't move this schedule.
		 */
		if (wifi_is_online() && tick >= next_scan_tick && lan_scan_request()) {
			next_scan_tick = tick + SCAN_EVERY_TICKS;
		}
		atomic_set(&next_scan_at, k_uptime_seconds() +
			   (MAX(next_scan_tick, tick) - tick) * SAMPLE_PERIOD_S);
	}

	return 0;
}
