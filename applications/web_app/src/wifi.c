/*
 * Wi-Fi station: connect with the credentials from wifi.conf, get an
 * address over DHCP and stay connected.
 *
 * The ESP32 driver starts DHCP after association
 * (CONFIG_ESP32_WIFI_STA_AUTO_DHCPV4) and reconnects by itself after a drop
 * (CONFIG_ESP32_WIFI_STA_RECONNECT), so this module only issues the first
 * connect request and tracks whether we have an IPv4 address.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>

#include "app.h"

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

#define RETRY_DELAY K_SECONDS(10)

static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct k_work_delayable connect_work;
static atomic_t online;

static void connect_work_handler(struct k_work *work)
{
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)CONFIG_APP_WIFI_SSID,
		.ssid_length = strlen(CONFIG_APP_WIFI_SSID),
		.psk = (const uint8_t *)CONFIG_APP_WIFI_PSK,
		.psk_length = strlen(CONFIG_APP_WIFI_PSK),
		.security = strlen(CONFIG_APP_WIFI_PSK) ? WIFI_SECURITY_TYPE_PSK
							: WIFI_SECURITY_TYPE_NONE,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.mfp = WIFI_MFP_OPTIONAL,
	};
	int ret;

	ARG_UNUSED(work);

	LOG_INF("Connecting to \"%s\"", CONFIG_APP_WIFI_SSID);
	status_led_set(STATUS_LED_CONNECTING);

	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("Connect request failed (%d), retrying", ret);
		k_work_reschedule(&connect_work, RETRY_DELAY);
	}
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		atomic_set(&online, 0);
		status_led_set(STATUS_LED_CONNECTING);
		LOG_WRN("Wi-Fi disconnected, driver is reconnecting");
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			       struct net_if *iface)
{
	char addr[NET_IPV4_ADDR_LEN];
	struct in_addr *ip;

	ARG_UNUSED(cb);

	if (event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (ip == NULL) {
		return;
	}

	atomic_set(&online, 1);
	status_led_set(STATUS_LED_ONLINE);
	LOG_INF("Online: http://%s/", net_addr_ntop(AF_INET, ip, addr, sizeof(addr)));

	/* First LAN scan as soon as we have an address */
	lan_scan_request();
}

int wifi_start(void)
{
	sta_iface = net_if_get_first_wifi();
	if (sta_iface == NULL) {
		LOG_ERR("No Wi-Fi interface");
		return -ENODEV;
	}

	if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
		LOG_ERR("No Wi-Fi credentials: copy wifi.conf.example to wifi.conf and rebuild");
		status_led_set(STATUS_LED_NO_CONFIG);
		return -EINVAL;
	}

	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	k_work_init_delayable(&connect_work, connect_work_handler);
	k_work_schedule(&connect_work, K_NO_WAIT);

	return 0;
}

bool wifi_is_online(void)
{
	return atomic_get(&online) != 0;
}

struct net_if *wifi_iface(void)
{
	return sta_iface;
}
