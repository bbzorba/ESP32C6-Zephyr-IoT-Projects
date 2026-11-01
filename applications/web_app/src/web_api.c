/*
 * HTTP server: the dashboard page and a small JSON API, encoded with
 * Zephyr's JSON library.
 *
 *   GET  /             dashboard (gzipped, served from flash)
 *   GET  /api/status   hub + BME688 readings, Wi-Fi, scan state
 *   GET  /api/devices  LAN device list
 *   POST /api/scan     start a LAN scan now
 *   POST /api/node     sensor report from a client node (nodes.c)
 *   GET  /api/nodes    latest report of every node
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/data/json.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include "app.h"

LOG_MODULE_REGISTER(web_api, LOG_LEVEL_INF);

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

/* --- /api/status --- */

struct env_json {
	bool present;
	bool ok;
	uint32_t at;
	double temp_c;
	double humidity;
	double pressure_hpa;
};

struct wifi_json {
	bool online;
	char ssid[WIFI_SSID_MAX_LEN + 1];
	int32_t rssi;
	uint32_t channel;
	char ip[NET_IPV4_ADDR_LEN];
	char mac[18];
};

struct scan_json {
	bool running;
	uint32_t progress;
	uint32_t total;
	uint32_t last_end;
	uint32_t next_at;
	uint32_t duration_ms;
	uint32_t interval_min;
};

struct status_json {
	uint32_t now;
	char fw[16];
	bool fw_confirmed;
	bool chip_ok;
	double chip_temp_c;
	double cpu_load_pct;
	struct env_json env;
	struct wifi_json wifi;
	struct scan_json scan;
};

static const struct json_obj_descr env_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct env_json, present, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct env_json, ok, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct env_json, at, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct env_json, temp_c, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct env_json, humidity, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct env_json, pressure_hpa, JSON_TOK_DOUBLE_FP),
};

static const struct json_obj_descr wifi_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct wifi_json, online, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct wifi_json, ssid, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct wifi_json, rssi, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct wifi_json, channel, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct wifi_json, ip, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct wifi_json, mac, JSON_TOK_STRING_BUF),
};

static const struct json_obj_descr scan_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct scan_json, running, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct scan_json, progress, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct scan_json, total, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct scan_json, last_end, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct scan_json, next_at, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct scan_json, duration_ms, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct scan_json, interval_min, JSON_TOK_UINT),
};

static const struct json_obj_descr status_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct status_json, now, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct status_json, fw, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct status_json, fw_confirmed, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct status_json, chip_ok, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct status_json, chip_temp_c, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct status_json, cpu_load_pct, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_OBJECT(struct status_json, env, env_descr),
	JSON_OBJ_DESCR_OBJECT(struct status_json, wifi, wifi_descr),
	JSON_OBJ_DESCR_OBJECT(struct status_json, scan, scan_descr),
};

/* --- /api/devices --- */

struct devices_json {
	uint32_t now;
	struct lan_device devices[CONFIG_APP_MAX_DEVICES];
	size_t devices_len;
};

static const struct json_obj_descr device_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct lan_device, ip, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct lan_device, mac, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct lan_device, name, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct lan_device, online, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct lan_device, self, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM_NAMED(struct lan_device, "first", first_seen, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM_NAMED(struct lan_device, "last", last_seen, JSON_TOK_UINT),
};

static const struct json_obj_descr devices_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct devices_json, now, JSON_TOK_UINT),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct devices_json, devices, CONFIG_APP_MAX_DEVICES,
				 devices_len, device_descr, ARRAY_SIZE(device_descr)),
};

/* Responses are encoded into static buffers: the server handles one request
 * at a time, and the body must outlive the handler call.
 */
static struct status_json status;
static struct devices_json devices;
static char status_buf[768];
static char devices_buf[CONFIG_APP_MAX_DEVICES * 160 + 64];

static void fill_status(void)
{
	struct net_if *iface = wifi_iface();
	struct wifi_iface_status wst = { 0 };
	struct lan_scan_info scan;
	struct readings r;

	memset(&status, 0, sizeof(status));
	status.now = k_uptime_seconds();
	strncpy(status.fw, ota_version(), sizeof(status.fw) - 1);
	status.fw_confirmed = ota_confirmed();

	sensors_get(&r);
	status.chip_ok = r.chip_ok;
	status.chip_temp_c = r.chip_temp_c;
	status.cpu_load_pct = r.cpu_load_pct;
	status.env = (struct env_json){
		.present = r.env_state != -ENODEV,
		.ok = r.env_state == 0,
		.at = r.at,
		.temp_c = r.temp_c,
		.humidity = r.humidity,
		.pressure_hpa = r.pressure_hpa,
	};

	status.wifi.online = wifi_is_online();
	if (iface != NULL) {
		struct net_linkaddr *ll = net_if_get_link_addr(iface);
		struct in_addr *ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

		snprintf(status.wifi.mac, sizeof(status.wifi.mac), "%02x:%02x:%02x:%02x:%02x:%02x",
			 ll->addr[0], ll->addr[1], ll->addr[2], ll->addr[3], ll->addr[4],
			 ll->addr[5]);
		if (ip != NULL) {
			net_addr_ntop(AF_INET, ip, status.wifi.ip, sizeof(status.wifi.ip));
		}
		if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &wst, sizeof(wst)) == 0 &&
		    wst.state >= WIFI_STATE_ASSOCIATED) {
			memcpy(status.wifi.ssid, wst.ssid, MIN(wst.ssid_len, WIFI_SSID_MAX_LEN));
			status.wifi.rssi = wst.rssi;
			status.wifi.channel = wst.channel;
		}
	}

	lan_scan_snapshot(NULL, 0, &scan);
	status.scan = (struct scan_json){
		.running = scan.running,
		.progress = scan.progress,
		.total = scan.total,
		.last_end = scan.last_end,
		.next_at = sampler_next_scan_at(),
		.duration_ms = scan.duration_ms,
		.interval_min = CONFIG_APP_SCAN_INTERVAL_MIN,
	};
}

static const struct http_header json_headers[] = {
	{ .name = "Content-Type", .value = "application/json" },
	{ .name = "Cache-Control", .value = "no-store" },
};

static void json_response(struct http_response_ctx *rsp, const char *body, int ret)
{
	if (ret < 0) {
		LOG_ERR("JSON encoding failed (%d)", ret);
		rsp->status = HTTP_500_INTERNAL_SERVER_ERROR;
		rsp->body_len = 0;
	} else {
		rsp->status = HTTP_200_OK;
		rsp->headers = json_headers;
		rsp->header_count = ARRAY_SIZE(json_headers);
		rsp->body = (const uint8_t *)body;
		rsp->body_len = strlen(body);
	}
	rsp->final_chunk = true;
}

/* GET handlers ignore any payload and answer on the final callback */
static int status_handler(struct http_client_ctx *client, enum http_data_status status_,
			  const struct http_request_ctx *req, struct http_response_ctx *rsp,
			  void *user_data)
{
	if (status_ == HTTP_SERVER_DATA_FINAL) {
		fill_status();
		json_response(rsp, status_buf,
			      json_obj_encode_buf(status_descr, ARRAY_SIZE(status_descr), &status,
						  status_buf, sizeof(status_buf)));
	}
	return 0;
}

static int devices_handler(struct http_client_ctx *client, enum http_data_status status_,
			   const struct http_request_ctx *req, struct http_response_ctx *rsp,
			   void *user_data)
{
	if (status_ == HTTP_SERVER_DATA_FINAL) {
		devices.now = k_uptime_seconds();
		devices.devices_len = lan_scan_snapshot(devices.devices,
							ARRAY_SIZE(devices.devices), NULL);
		json_response(rsp, devices_buf,
			      json_obj_encode_buf(devices_descr, ARRAY_SIZE(devices_descr),
						  &devices, devices_buf, sizeof(devices_buf)));
	}
	return 0;
}

static int scan_handler(struct http_client_ctx *client, enum http_data_status status_,
			const struct http_request_ctx *req, struct http_response_ctx *rsp,
			void *user_data)
{
	if (status_ == HTTP_SERVER_DATA_FINAL) {
		bool started = lan_scan_request();

		LOG_INF("Manual scan requested (%s)", started ? "started" : "already running");
		json_response(rsp, started ? "{\"started\":true}" : "{\"started\":false}", 0);
	}
	return 0;
}

static int node_post_handler(struct http_client_ctx *client, enum http_data_status status_,
			     const struct http_request_ctx *req, struct http_response_ctx *rsp,
			     void *user_data)
{
	/* The body can arrive in several chunks */
	static char body[2048];
	static size_t len;

	if (status_ == HTTP_SERVER_DATA_ABORTED) {
		len = 0;
		return 0;
	}

	if (len + req->data_len >= sizeof(body)) {
		len = 0;
		return -ENOMEM;
	}
	memcpy(body + len, req->data, req->data_len);
	len += req->data_len;

	if (status_ == HTTP_SERVER_DATA_FINAL) {
		int ret = nodes_report(body, len);

		len = 0;
		if (ret) {
			rsp->status = HTTP_400_BAD_REQUEST;
			rsp->final_chunk = true;
		} else {
			json_response(rsp, "{\"ok\":true}", 0);
		}
	}
	return 0;
}

static int nodes_get_handler(struct http_client_ctx *client, enum http_data_status status_,
			     const struct http_request_ctx *req, struct http_response_ctx *rsp,
			     void *user_data)
{
	static char buf[4 * 1400];

	if (status_ == HTTP_SERVER_DATA_FINAL) {
		json_response(rsp, buf, nodes_encode(buf, sizeof(buf)));
	}
	return 0;
}

static struct http_resource_detail_dynamic node_post_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
	},
	.cb = node_post_handler,
};

static struct http_resource_detail_dynamic nodes_get_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = nodes_get_handler,
};

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
HTTP_RESOURCE_DEFINE(node_res, hub_http_service, "/api/node", &node_post_detail);
HTTP_RESOURCE_DEFINE(nodes_res, hub_http_service, "/api/nodes", &nodes_get_detail);
