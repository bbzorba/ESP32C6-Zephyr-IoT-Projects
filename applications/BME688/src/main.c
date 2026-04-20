/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define LED3_NODE DT_ALIAS(led3)

/* The devicetree node identifier for the "sw0" alias (user button). */
#define SW0_NODE DT_ALIAS(sw0)

/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(LED0_NODE, gpios),
	GPIO_DT_SPEC_GET(LED1_NODE, gpios),
	GPIO_DT_SPEC_GET(LED2_NODE, gpios),
	GPIO_DT_SPEC_GET(LED3_NODE, gpios),
};

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);

static struct gpio_callback button_cb_data;
static K_SEM_DEFINE(button_sem, 0, 1);

#define BUTTON_IRQ_FLAGS_PRESS (GPIO_INT_EDGE_TO_ACTIVE)
#define BUTTON_IRQ_FLAGS_RELEASE (GPIO_INT_EDGE_TO_INACTIVE)

#define BUTTON_DEBOUNCE_MS 30

/*
 * Locked configuration:
 * - Internal pull-down to avoid floating
 * - Active-high (pressed reads 1)
 *
 * Note: Zephyr's stm32f4_disco board DTS also defines USER button (PA0) as
 * GPIO_ACTIVE_HIGH.
 */
#define BUTTON_FLAGS (GPIO_INPUT | GPIO_PULL_DOWN)

static void set_all_leds(int value)
{
	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		(void)gpio_pin_set_dt(&leds[i], value);
	}
}

static void button_ISR(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_sem_give(&button_sem);
}

int main(void)
{
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(&leds[i])) {
			printf("LED%u GPIO device not ready\n", (unsigned)i);
			return 0;
		}

		ret = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			printf("Failed to configure LED%u (err %d)\n", (unsigned)i, ret);
			return 0;
		}
	}

	if (!gpio_is_ready_dt(&button)) {
		printf("Button GPIO device not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&button, BUTTON_FLAGS);
	if (ret < 0) {
		printf("Failed to configure button (err %d)\n", ret);
		return 0;
	}

	gpio_init_callback(&button_cb_data, button_ISR, BIT(button.pin));
	ret = gpio_add_callback(button.port, &button_cb_data);
	if (ret < 0) {
		printf("Failed to add button callback (err %d)\n", ret);
		return 0;
	}

	printf("Button on %s pin %u configured: PULL_DOWN + ACTIVE_HIGH (dt_flags=0x%x)\n",
	       button.port->name,
	       (unsigned)button.pin,
	       (unsigned)button.dt_flags);

	/* Start by waiting for a press edge. */
	ret = gpio_pin_interrupt_configure_dt(&button, BUTTON_IRQ_FLAGS_PRESS);
	if (ret < 0) {
		printf("Failed to configure button press interrupt (err %d)\n", ret);
		return 0;
	}

	printk("Sleep demo started. Waiting for button press...\n");

	while (1) {
		static bool leds_on;
		static bool waiting_for_release;
		int val;
		int want;

		/* Block forever; when power management is enabled this allows the idle
		 * thread to put the MCU into a low-power state until an interrupt occurs.
		 */
		if (!waiting_for_release) {
			printk("SLEEP: waiting for press\n");
		} else {
			printk("SLEEP: waiting for release\n");
		}
		(void)k_sem_take(&button_sem, K_FOREVER);
		printk("WAKE: interrupt\n");

		/* Simple debounce: wait a bit then sample pin.
		 * This avoids mechanical bounce causing double toggles or inconsistent
		 * observed behavior.
		 */
		k_msleep(BUTTON_DEBOUNCE_MS);
		val = gpio_pin_get_dt(&button);
		if (val < 0) {
			printk("ERR: gpio_pin_get_dt failed (%d)\n", val);
			continue;
		}

		want = waiting_for_release ? 0 : 1;
		if (val != want) {
			/* Spurious edge or bounce; keep waiting in the same state. */
			printk("IGN: unexpected pin=%d (want %d)\n", val, want);
			continue;
		}

		if (!waiting_for_release) {
			/* Stable press confirmed: toggle LEDs and then wait for release edge. */
			leds_on = !leds_on;
			set_all_leds(leds_on ? 1 : 0);
			printk("BTN: pressed -> LEDs %s\n", leds_on ? "ON" : "OFF");

			waiting_for_release = true;
			ret = gpio_pin_interrupt_configure_dt(&button, BUTTON_IRQ_FLAGS_RELEASE);
			if (ret < 0) {
				printk("ERR: configure release interrupt failed (%d)\n", ret);
				return 0;
			}
		} else {
			/* Stable release confirmed: go back to waiting for press edge. */
			printk("BTN: released\n");
			waiting_for_release = false;
			ret = gpio_pin_interrupt_configure_dt(&button, BUTTON_IRQ_FLAGS_PRESS);
			if (ret < 0) {
				printk("ERR: configure press interrupt failed (%d)\n", ret);
				return 0;
			}
		}
	}
}
