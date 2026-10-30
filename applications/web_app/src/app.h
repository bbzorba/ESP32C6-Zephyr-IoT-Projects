/*
 * Home Hub - shared declarations between the app modules.
 */

#ifndef APP_H_
#define APP_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>

/* --- main.c: onboard RGB LED used as a status indicator --- */
enum status_led {
	STATUS_LED_NO_CONFIG,   /* red: wifi.conf missing */
	STATUS_LED_CONNECTING,  /* blue */
	STATUS_LED_ONLINE,      /* green */
	STATUS_LED_SCANNING,    /* purple */
};

void status_led_set(enum status_led state);

/* --- wifi.c: station mode, DHCP, automatic reconnect --- */
int wifi_start(void);
bool wifi_is_online(void);
struct net_if *wifi_iface(void);

/* --- lan_scan.c: ARP sweep of the local /24 --- */
struct lan_device {
	struct in_addr ip;
	uint8_t mac[6];
	bool online;
	bool self;
	uint32_t first_seen; /* uptime, seconds */
	uint32_t last_seen;  /* uptime, seconds */
};

struct lan_scan_info {
	bool running;
	uint32_t last_scan_end; /* uptime seconds, 0 = never */
	uint32_t next_scan_at;  /* uptime seconds */
	uint32_t duration_ms;
	uint16_t progress;      /* hosts probed in the running scan */
	uint16_t total;         /* hosts to probe in the running scan */
};

/* Returns false if a scan is already running. */
bool lan_scan_request(void);
/* Copies up to max devices into out, sorted by IP. Returns the count. */
int lan_scan_snapshot(struct lan_device *out, int max, struct lan_scan_info *info);

/* --- sensors.c --- */
/* On-die temperature in milli-degrees Celsius. */
int sensors_die_temp(int32_t *milli_c);

/* --- bme688.c: BME688 on I2C0, sampled every 10 s --- */
struct env_reading {
	int32_t temp_mc;     /* milli-degrees Celsius */
	uint32_t humidity_m; /* milli-%RH */
	uint32_t press_pa;   /* Pa */
	uint32_t gas_ohm;    /* gas sensor resistance */
	uint32_t at;         /* uptime seconds of the reading */
};

/* 0 with *out filled, -EAGAIN before the first reading, -ENODEV if no sensor */
int bme688_latest(struct env_reading *out);

#endif /* APP_H_ */
