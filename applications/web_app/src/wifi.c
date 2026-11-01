/*
 * Wi-Fi station.
 *
 * SSID and password come from wifi.conf through Zephyr's wifi_credentials
 * library (CONFIG_WIFI_CREDENTIALS_STATIC); a CONNECT_STORED request joins
 * the network. The ESP32 driver then runs DHCP and reconnects after drops,
 * and the connection manager reports when we are online (associated and
 * holding an IPv4 address).
 *
 * After a power-on reset the driver often gets stuck in "connecting": the
 * first attempt never ends, every further request is refused with
 * -EALREADY, and a disconnect request doesn't clear it either; a software
 * reset always does. A genuine failed attempt (no AP, wrong password) ends
 * with a disconnect event, so "connecting with no disconnect event for
 * STUCK_REBOOT_S" means stuck, and the hub reboots at once. While offline we
 * re-request every RETRY_AFTER, and after OFFLINE_REBOOT_S without a
 * connection the hub reboots anyway.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>

#include "app.h"

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

#define RETRY_AFTER      K_SECONDS(30)
#define STUCK_REBOOT_S   60
#define OFFLINE_REBOOT_S (5 * 60)

static struct net_if *sta_iface;
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback wifi_cb;
static struct k_work_delayable connect_work;
static atomic_t online;
static uint32_t offline_since;
static uint32_t last_attempt_end;

static void reboot(const char *why, const struct wifi_iface_status *st)
{
	LOG_ERR("%s (wifi %s, dhcp %s), rebooting", why, wifi_state_txt(st->state),
		net_dhcpv4_state_name(sta_iface->config.dhcpv4.state));
	LOG_PANIC(); /* flush the deferred log before the reset */
	sys_reboot(SYS_REBOOT_COLD);
}

static void connect_work_handler(struct k_work *work)
{
	struct wifi_iface_status st = { 0 };
	int ret;

	ARG_UNUSED(work);

	if (atomic_get(&online)) {
		return;
	}

	(void)net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, sta_iface, &st, sizeof(st));
	if (k_uptime_seconds() - offline_since >= OFFLINE_REBOOT_S) {
		reboot("Offline for 5 min", &st);
	}
	/* The ESP32 driver reports "connecting" as SCANNING */
	if (st.state == WIFI_STATE_SCANNING &&
	    k_uptime_seconds() - MAX(offline_since, last_attempt_end) >= STUCK_REBOOT_S) {
		reboot("Driver stuck connecting", &st);
	}

	LOG_INF("Connecting to \"%s\"", CONFIG_WIFI_CREDENTIALS_STATIC_SSID);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, sta_iface, NULL, 0);
	if (ret && ret != -EALREADY) {
		LOG_WRN("Connect request failed (%d)", ret);
	}
	k_work_reschedule(&connect_work, RETRY_AFTER);
}

static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			     struct net_if *iface)
{
	char addr[NET_IPV4_ADDR_LEN] = "?";
	struct in_addr *ip;

	ARG_UNUSED(cb);

	if (event == NET_EVENT_L4_CONNECTED) {
		ip = net_if_ipv4_get_global_addr(sta_iface, NET_ADDR_PREFERRED);
		if (ip != NULL) {
			net_addr_ntop(AF_INET, ip, addr, sizeof(addr));
		}
		atomic_set(&online, 1);
		status_led_set(STATUS_LED_ONLINE);
		LOG_INF("Online: http://%s/", addr);
		ota_online();
	} else {
		atomic_set(&online, 0);
		status_led_set(STATUS_LED_CONNECTING);
		LOG_WRN("Offline, driver is reconnecting");
		offline_since = k_uptime_seconds();
		k_work_reschedule(&connect_work, RETRY_AFTER);
	}
}

/* Every attempt of the driver that fails ends with a disconnect event */
static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(event);
	ARG_UNUSED(iface);

	last_attempt_end = k_uptime_seconds();
}

int wifi_start(void)
{
	sta_iface = net_if_get_first_wifi();
	if (sta_iface == NULL) {
		LOG_ERR("No Wi-Fi interface");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler,
				     NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&l4_cb);
	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	status_led_set(STATUS_LED_CONNECTING);
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
