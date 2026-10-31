/*
 * Home Hub - shared declarations between the app modules.
 */

#ifndef APP_H_
#define APP_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>

/* --- main.c: one 10 s sampler tick drives sensors and LAN scans --- */
#define SAMPLE_PERIOD_S  10
#define SCAN_EVERY_TICKS (CONFIG_APP_SCAN_INTERVAL_MIN * 60 / SAMPLE_PERIOD_S)

enum status_led {
	STATUS_LED_CONNECTING,  /* blue */
	STATUS_LED_ONLINE,      /* green */
	STATUS_LED_SCANNING,    /* purple */
};

void status_led_set(enum status_led state);
/* Uptime (s) of the next scheduled LAN scan */
uint32_t sampler_next_scan_at(void);

/* --- wifi.c: station mode via wifi_credentials, online state via conn_mgr --- */
int wifi_start(void);
bool wifi_is_online(void);
struct net_if *wifi_iface(void);

/* --- lan_scan.c: ARP sweep of the local /24 + reverse DNS names --- */
struct lan_device {
	struct in_addr addr;
	char ip[NET_IPV4_ADDR_LEN];
	char mac[18];
	char name[32];          /* from the router's DNS, "" if unknown */
	bool online;
	bool self;
	uint32_t first_seen;    /* uptime, seconds */
	uint32_t last_seen;     /* uptime, seconds */
};

struct lan_scan_info {
	bool running;
	uint32_t last_end;      /* uptime seconds, 0 = never */
	uint32_t duration_ms;
	uint32_t progress;      /* hosts probed in the running scan */
	uint32_t total;         /* hosts to probe in the running scan */
};

/* Returns false if a scan is already running. */
bool lan_scan_request(void);
/* Copies up to max devices into out, sorted by IP. Returns the count. */
int lan_scan_snapshot(struct lan_device *out, int max, struct lan_scan_info *info);

/* --- sensors.c: readings cached by the sampler tick --- */
struct readings {
	uint32_t at;            /* uptime seconds of the sample */
	bool chip_ok;
	double chip_temp_c;
	double cpu_load_pct;    /* CPU busy share since the previous tick */
	int env_state;          /* 0 ok, -ENODEV no sensor, -EAGAIN none yet */
	double temp_c;
	double humidity;
	double pressure_hpa;
};

void sensors_sample(void);
void sensors_get(struct readings *out);

/* --- nodes.c: reports POSTed by client nodes --- */
/* Parses one report (json is modified in place). 0 or -EINVAL. */
int nodes_report(char *json, size_t len);
/* Encodes all nodes as JSON into buf. 0 or a negative error. */
int nodes_encode(char *buf, size_t len);

/* --- mqtt_pub.c: readings to the MQTT broker for the live web page --- */
/* Called on each sampler tick; the publish happens in the MQTT thread. */
void mqtt_pub_trigger(void);

/* --- bme688.c: BME688 on I2C0 --- */
/* Fills the environment fields of r. 0 on success, -ENODEV if no sensor. */
int bme688_read(struct readings *r);

#endif /* APP_H_ */
