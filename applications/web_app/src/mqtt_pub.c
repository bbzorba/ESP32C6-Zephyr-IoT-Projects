/*
 * Publishes the hub's readings to an MQTT broker for the live page on
 * https://bbzorba.github.io/live (the page subscribes over WebSocket).
 *
 * Topics (all retained, so a new visitor gets the latest at once):
 *   <prefix>/state   JSON with hub, BME688 and node readings, every 10 s
 *   <prefix>/status  "online" while connected; the broker publishes the
 *                    last will "offline" if the hub drops off for longer
 *                    than 1.5x the keepalive
 *
 * Only sensor values are published. The LAN device list, IP/MAC addresses
 * and the Wi-Fi name stay on the hub's local page. The default broker is
 * public: anyone who knows the topic can read it, and could publish too.
 *
 * Publishing is triggered by the sampler tick in main.c, so it happens in
 * the same wake-up as the sensor reads.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/data/json.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/random/random.h>

#include "app.h"

LOG_MODULE_REGISTER(mqtt_pub, LOG_LEVEL_INF);

#define TOPIC_STATE   CONFIG_APP_MQTT_TOPIC_PREFIX "/state"
#define TOPIC_STATUS  CONFIG_APP_MQTT_TOPIC_PREFIX "/status"
#define RETRY_AFTER   K_SECONDS(30)
#define CONNACK_WAIT_MS 5000

struct hub_json {
	double chip_temp_c;
	double cpu_load_pct;
	uint32_t uptime_s;
	int32_t rssi;
};

struct env_json {
	bool ok;
	double temp_c;
	double humidity;
	double pressure_hpa;
};

static const struct json_obj_descr hub_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hub_json, chip_temp_c, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct hub_json, cpu_load_pct, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct hub_json, uptime_s, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct hub_json, rssi, JSON_TOK_NUMBER),
};

static const struct json_obj_descr env_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct env_json, ok, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct env_json, temp_c, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct env_json, humidity, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct env_json, pressure_hpa, JSON_TOK_DOUBLE_FP),
};

static struct mqtt_client client;
static struct sockaddr_storage broker;
static struct zsock_pollfd fds[1];
static uint8_t rx_buf[256];
static uint8_t tx_buf[512];
static char payload[4 * 1400 + 512];
static bool connected;

static K_SEM_DEFINE(publish_sem, 0, 1);

static struct mqtt_topic will_topic = {
	.topic = { .utf8 = (uint8_t *)TOPIC_STATUS, .size = sizeof(TOPIC_STATUS) - 1 },
	.qos = MQTT_QOS_0_AT_MOST_ONCE,
};
static struct mqtt_utf8 will_message = MQTT_UTF8_LITERAL("offline");

void mqtt_pub_trigger(void)
{
	k_sem_give(&publish_sem);
}

static void evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		connected = evt->result == 0;
		if (!connected) {
			LOG_ERR("Broker refused connection (%d)", evt->result);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		connected = false;
		break;
	default:
		break;
	}
}

static int publish(const char *topic, const char *data, size_t len)
{
	struct mqtt_publish_param p = {
		.message.topic.topic = { .utf8 = (uint8_t *)topic, .size = strlen(topic) },
		.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.message.payload = { .data = (uint8_t *)data, .len = len },
		.message_id = sys_rand16_get(),
		.retain_flag = 1,
	};

	return mqtt_publish(&client, &p);
}

static int resolve_broker(void)
{
	struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res;
	char port[8];
	int ret;

	snprintf(port, sizeof(port), "%d", CONFIG_APP_MQTT_BROKER_PORT);
	ret = zsock_getaddrinfo(CONFIG_APP_MQTT_BROKER, port, &hints, &res);
	if (ret) {
		return -EHOSTUNREACH;
	}
	memcpy(&broker, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	return 0;
}

static int broker_connect(void)
{
	static char client_id[24];
	int ret;

	ret = resolve_broker();
	if (ret) {
		LOG_WRN("Cannot resolve %s", CONFIG_APP_MQTT_BROKER);
		return ret;
	}

	mqtt_client_init(&client);
	snprintf(client_id, sizeof(client_id), "home-hub-%08x", sys_rand32_get());
	client.broker = &broker;
	client.evt_cb = evt_handler;
	client.client_id = (struct mqtt_utf8){ .utf8 = (uint8_t *)client_id,
					       .size = strlen(client_id) };
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.will_topic = &will_topic;
	client.will_message = &will_message;
	client.will_retain = 1;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	ret = mqtt_connect(&client);
	if (ret) {
		LOG_WRN("mqtt_connect failed (%d)", ret);
		return ret;
	}

	fds[0].fd = client.transport.tcp.sock;
	fds[0].events = ZSOCK_POLLIN;
	if (zsock_poll(fds, 1, CONNACK_WAIT_MS) > 0) {
		mqtt_input(&client);
	}
	if (!connected) {
		mqtt_abort(&client);
		return -ECONNREFUSED;
	}

	LOG_INF("Connected to %s, publishing to %s", CONFIG_APP_MQTT_BROKER, TOPIC_STATE);
	return publish(TOPIC_STATUS, "online", 6);
}

static int publish_state(void)
{
	struct net_if *iface = wifi_iface();
	struct wifi_iface_status wst = { 0 };
	struct readings r;
	struct hub_json hub;
	struct env_json env;
	char hub_buf[160], env_buf[128];
	size_t len;
	int ret;

	sensors_get(&r);
	if (iface != NULL) {
		(void)net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &wst, sizeof(wst));
	}

	hub = (struct hub_json){
		.chip_temp_c = r.chip_temp_c,
		.cpu_load_pct = r.cpu_load_pct,
		.uptime_s = k_uptime_seconds(),
		.rssi = wst.rssi,
	};
	env = (struct env_json){
		.ok = r.env_state == 0,
		.temp_c = r.temp_c,
		.humidity = r.humidity,
		.pressure_hpa = r.pressure_hpa,
	};

	ret = json_obj_encode_buf(hub_descr, ARRAY_SIZE(hub_descr), &hub, hub_buf,
				  sizeof(hub_buf));
	ret = ret ?: json_obj_encode_buf(env_descr, ARRAY_SIZE(env_descr), &env, env_buf,
					 sizeof(env_buf));
	if (ret) {
		return ret;
	}

	/* {"hub":{...},"env":{...},"nodes":{"now":..,"nodes":[...]}} */
	len = snprintf(payload, sizeof(payload), "{\"hub\":%s,\"env\":%s,\"nodes\":", hub_buf,
		       env_buf);
	ret = nodes_encode(payload + len, sizeof(payload) - len - 1);
	if (ret) {
		return ret;
	}
	len += strlen(payload + len);
	payload[len++] = '}';
	payload[len] = '\0';

	return publish(TOPIC_STATE, payload, len);
}

static void mqtt_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		int ret;

		if (!connected) {
			/* Wait for a tick so we only try while there is work */
			k_sem_take(&publish_sem, K_FOREVER);
			if (!wifi_is_online() || broker_connect() != 0) {
				k_sleep(RETRY_AFTER);
				continue;
			}
		} else {
			int left = mqtt_keepalive_time_left(&client);

			ret = k_sem_take(&publish_sem, K_MSEC(MAX(left, 0)));
			if (ret == 0) {
				ret = publish_state();
				if (ret) {
					LOG_WRN("Publish failed (%d)", ret);
				}
			}
		}

		/* Handle PINGRESP/DISCONNECT, send PINGREQ when due */
		if (zsock_poll(fds, 1, 0) > 0) {
			if (fds[0].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
				connected = false;
			} else if (mqtt_input(&client) != 0) {
				connected = false;
			}
		}
		if (connected) {
			ret = mqtt_live(&client);
			if (ret != 0 && ret != -EAGAIN) {
				connected = false;
			}
		}
		if (!connected) {
			LOG_WRN("Broker connection lost, reconnecting");
			mqtt_abort(&client);
		}
	}
}

K_THREAD_DEFINE(mqtt_tid, 3072, mqtt_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
