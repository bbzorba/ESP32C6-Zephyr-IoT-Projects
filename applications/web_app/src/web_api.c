/*
 * HTTP server: the dashboard page and a small JSON API.
 *
 *   GET  /             dashboard (gzipped, served from flash)
 *   GET  /api/status   hub sensors + Wi-Fi + scan state
 *   GET  /api/devices  LAN device list
 *   POST /api/scan     start a LAN scan now
 */

#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include "app.h"

LOG_MODULE_REGISTER(web_api, LOG_LEVEL_INF);

/* One device renders to ~100 bytes of JSON */
#define DEVICE_JSON_LEN 112

static uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};

static struct http_resource_detail_static index_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/html",
	},
	.static_data = index_html_gz,
	.static_data_len = sizeof(index_html_gz),
};

/* Responses are built into static buffers: the server handles one request
 * at a time, and the buffer must outlive the handler call.
 */
static char status_buf[768];
static char devices_buf[CONFIG_APP_MAX_DEVICES * DEVICE_JSON_LEN + 256];
static struct lan_device snapshot[CONFIG_APP_MAX_DEVICES];

static uint32_t now_s(void)
{
	return (uint32_t)(k_uptime_get() / MSEC_PER_SEC);
}

/* Copy src into dst, dropping characters that would break a JSON string */
static void json_safe_copy(char *dst, size_t len, const char *src, size_t src_len)
{
	size_t j = 0;

	for (size_t i = 0; i < src_len && src[i] && j + 1 < len; i++) {
		if (src[i] != '"' && src[i] != '\\' && (uint8_t)src[i] >= 0x20) {
			dst[j++] = src[i];
		}
	}
	dst[j] = '\0';
}

/* Format a milli-unit value with one decimal, e.g. 23456 -> "23.5" */
static void fmt_milli_1dp(char *buf, size_t len, int32_t milli)
{
	int32_t tenths = (milli + (milli < 0 ? -50 : 50)) / 100;

	snprintf(buf, len, "%s%d.%d", tenths < 0 ? "-" : "", abs(tenths) / 10, abs(tenths) % 10);
}

/* "null" or the BME688 object of the status JSON */
static void build_env(char *buf, size_t len)
{
	struct env_reading env;
	char temp[12], hum[12], press[12], gas[12];
	int ret = bme688_latest(&env);

	if (ret) {
		snprintf(buf, len, "{\"present\":%s}", ret == -ENODEV ? "false" : "true");
		return;
	}

	fmt_milli_1dp(temp, sizeof(temp), env.temp_mc);
	fmt_milli_1dp(hum, sizeof(hum), env.humidity_m);
	fmt_milli_1dp(press, sizeof(press), env.press_pa * 10);  /* Pa -> milli-hPa */
	fmt_milli_1dp(gas, sizeof(gas), env.gas_ohm);           /* ohm -> kOhm */

	snprintf(buf, len,
		 "{\"present\":true,\"temp_c\":%s,\"humidity\":%s,\"pressure_hpa\":%s,"
		 "\"gas_kohm\":%s,\"at\":%u}",
		 temp, hum, press, gas, env.at);
}

static int build_status(void)
{
	char env_json[160];
	struct net_if *iface = wifi_iface();
	struct wifi_iface_status wst = { 0 };
	struct lan_scan_info scan;
	struct in_addr *ip = NULL;
	char ip_str[NET_IPV4_ADDR_LEN] = "";
	char ssid[WIFI_SSID_MAX_LEN + 1] = "";
	char temp_str[16] = "null";
	char mac_str[18] = "";
	int32_t milli_c;

	if (sensors_die_temp(&milli_c) == 0) {
		fmt_milli_1dp(temp_str, sizeof(temp_str), milli_c);
	}
	build_env(env_json, sizeof(env_json));

	if (iface != NULL) {
		struct net_linkaddr *ll = net_if_get_link_addr(iface);

		snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
			 ll->addr[0], ll->addr[1], ll->addr[2], ll->addr[3], ll->addr[4],
			 ll->addr[5]);

		ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (ip != NULL) {
			net_addr_ntop(AF_INET, ip, ip_str, sizeof(ip_str));
		}

		if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &wst, sizeof(wst)) == 0 &&
		    wst.state >= WIFI_STATE_ASSOCIATED) {
			json_safe_copy(ssid, sizeof(ssid), wst.ssid, wst.ssid_len);
		} else {
			wst.rssi = 0;
			wst.channel = 0;
		}
	}

	lan_scan_snapshot(snapshot, 0, &scan);

	return snprintf(status_buf, sizeof(status_buf),
			"{\"now\":%u,\"temp_c\":%s,\"env\":%s,"
			"\"wifi\":{\"online\":%s,\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,"
			"\"ip\":\"%s\",\"mac\":\"%s\"},"
			"\"scan\":{\"running\":%s,\"progress\":%u,\"total\":%u,"
			"\"last_end\":%u,\"next_at\":%u,\"duration_ms\":%u,"
			"\"interval_min\":%d}}",
			now_s(), temp_str, env_json, wifi_is_online() ? "true" : "false", ssid, wst.rssi,
			wst.channel, ip_str, mac_str, scan.running ? "true" : "false",
			scan.progress, scan.total, scan.last_scan_end, scan.next_scan_at,
			scan.duration_ms, CONFIG_APP_SCAN_INTERVAL_MIN);
}

static int build_devices(void)
{
	struct lan_scan_info scan;
	int n = lan_scan_snapshot(snapshot, ARRAY_SIZE(snapshot), &scan);
	size_t len = 0;

	len += snprintf(devices_buf + len, sizeof(devices_buf) - len,
			"{\"now\":%u,\"scanning\":%s,\"last_end\":%u,\"devices\":[", now_s(),
			scan.running ? "true" : "false", scan.last_scan_end);

	for (int i = 0; i < n && len < sizeof(devices_buf) - DEVICE_JSON_LEN; i++) {
		const struct lan_device *d = &snapshot[i];
		char ip_str[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET, &d->ip, ip_str, sizeof(ip_str));
		len += snprintf(devices_buf + len, sizeof(devices_buf) - len,
				"%s{\"ip\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
				"\"online\":%s,\"self\":%s,\"first\":%u,\"last\":%u}",
				i ? "," : "", ip_str, d->mac[0], d->mac[1], d->mac[2], d->mac[3],
				d->mac[4], d->mac[5], d->online ? "true" : "false",
				d->self ? "true" : "false", d->first_seen, d->last_seen);
	}

	len += snprintf(devices_buf + len, sizeof(devices_buf) - len, "]}");
	return len;
}

static const struct http_header json_headers[] = {
	{ .name = "Content-Type", .value = "application/json" },
	{ .name = "Cache-Control", .value = "no-store" },
};

static void json_response(struct http_response_ctx *rsp, const char *body, int len)
{
	rsp->status = HTTP_200_OK;
	rsp->headers = json_headers;
	rsp->header_count = ARRAY_SIZE(json_headers);
	rsp->body = (const uint8_t *)body;
	rsp->body_len = len;
	rsp->final_chunk = true;
}

/* GET handlers ignore any payload and answer on the final callback */
static int status_handler(struct http_client_ctx *client, enum http_data_status status,
			  const struct http_request_ctx *req, struct http_response_ctx *rsp,
			  void *user_data)
{
	if (status == HTTP_SERVER_DATA_FINAL) {
		json_response(rsp, status_buf, build_status());
	}
	return 0;
}

static int devices_handler(struct http_client_ctx *client, enum http_data_status status,
			   const struct http_request_ctx *req, struct http_response_ctx *rsp,
			   void *user_data)
{
	if (status == HTTP_SERVER_DATA_FINAL) {
		json_response(rsp, devices_buf, build_devices());
	}
	return 0;
}

static int scan_handler(struct http_client_ctx *client, enum http_data_status status,
			const struct http_request_ctx *req, struct http_response_ctx *rsp,
			void *user_data)
{
	static char buf[32];

	if (status == HTTP_SERVER_DATA_FINAL) {
		bool started = lan_scan_request();

		LOG_INF("Manual scan requested (%s)", started ? "started" : "already running");
		json_response(rsp, buf,
			      snprintf(buf, sizeof(buf), "{\"started\":%s}",
				       started ? "true" : "false"));
	}
	return 0;
}

static struct http_resource_detail_dynamic status_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = status_handler,
};

static struct http_resource_detail_dynamic devices_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = devices_handler,
};

static struct http_resource_detail_dynamic scan_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
	},
	.cb = scan_handler,
};

static uint16_t http_port = CONFIG_APP_HTTP_PORT;

HTTP_SERVICE_DEFINE(hub_http_service, NULL, &http_port, CONFIG_HTTP_SERVER_MAX_CLIENTS, 10,
		    NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(index_res, hub_http_service, "/", &index_detail);
HTTP_RESOURCE_DEFINE(status_res, hub_http_service, "/api/status", &status_detail);
HTTP_RESOURCE_DEFINE(devices_res, hub_http_service, "/api/devices", &devices_detail);
HTTP_RESOURCE_DEFINE(scan_res, hub_http_service, "/api/scan", &scan_detail);
