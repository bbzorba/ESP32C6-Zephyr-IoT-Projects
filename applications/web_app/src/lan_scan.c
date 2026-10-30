/*
 * LAN device discovery by ARP sweep.
 *
 * Every host of the local /24 gets one tiny UDP datagram (discard port 9).
 * Sending it forces the stack to ARP for that address, and every host on
 * the LAN answers ARP even when it ignores ping. After each batch the ARP
 * table is read back; whatever resolved is online.
 *
 * Devices that stop answering stay in the list marked offline, so the page
 * also shows which devices went away and when they were last seen.
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>

#include "arp.h"
#include "app.h"

LOG_MODULE_REGISTER(lan_scan, LOG_LEVEL_INF);

#define BATCH_SIZE      16
/* Longer than the stack's 2 s ARP request timeout, so a batch's pending
 * entries are gone before the next batch needs table slots.
 */
#define BATCH_WAIT_MS   2100
#define PROBE_PORT      9

struct collect_ctx {
	struct net_if *iface;
	uint32_t net;
	uint32_t mask;
	uint32_t now;
};

static struct lan_device devices[CONFIG_APP_MAX_DEVICES];
static bool seen[CONFIG_APP_MAX_DEVICES];
static int device_count;
static struct lan_scan_info scan_info;

static K_MUTEX_DEFINE(lock);
static K_SEM_DEFINE(scan_sem, 0, 1);

static uint32_t now_s(void)
{
	return (uint32_t)(k_uptime_get() / MSEC_PER_SEC);
}

/* Caller holds lock. */
static void upsert(const struct in_addr *ip, const uint8_t *mac, uint32_t now, bool self)
{
	struct lan_device *dev = NULL;
	int i;

	for (i = 0; i < device_count; i++) {
		if (devices[i].ip.s_addr == ip->s_addr) {
			dev = &devices[i];
			break;
		}
	}

	if (dev == NULL) {
		if (device_count == ARRAY_SIZE(devices)) {
			LOG_WRN("Device list full, ignoring new device");
			return;
		}
		i = device_count++;
		dev = &devices[i];
		dev->ip = *ip;
		dev->first_seen = now;
		LOG_INF("New device %d.%d.%d.%d", ((uint8_t *)&ip->s_addr)[0],
			((uint8_t *)&ip->s_addr)[1], ((uint8_t *)&ip->s_addr)[2],
			((uint8_t *)&ip->s_addr)[3]);
	}

	memcpy(dev->mac, mac, sizeof(dev->mac));
	dev->last_seen = now;
	dev->self = self;
	seen[i] = true;
}

static void arp_collect_cb(struct arp_entry *entry, void *user_data)
{
	struct collect_ctx *ctx = user_data;
	uint32_t ip = ntohl(entry->ip.s_addr);

	if (entry->iface != ctx->iface || (ip & ctx->mask) != ctx->net) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	upsert(&entry->ip, entry->eth.addr, ctx->now, false);
	k_mutex_unlock(&lock);
}

static void run_scan(void)
{
	struct net_if *iface = wifi_iface();
	struct collect_ctx ctx = { .iface = iface };
	struct in_addr *own;
	struct net_linkaddr *lladdr;
	uint32_t self, hosts;
	int64_t start = k_uptime_get();
	int sock;

	own = iface ? net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) : NULL;
	if (own == NULL || !wifi_is_online()) {
		LOG_WRN("Not online, skipping scan");
		return;
	}

	self = ntohl(own->s_addr);
	ctx.mask = ntohl(net_if_ipv4_get_netmask_by_addr(iface, own).s_addr);
	/* Never sweep more than one /24, even on bigger subnets */
	if (ctx.mask < 0xFFFFFF00) {
		ctx.mask = 0xFFFFFF00;
	}
	ctx.net = self & ctx.mask;
	hosts = ~ctx.mask - 1;

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("socket() failed (%d)", errno);
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	memset(seen, 0, sizeof(seen));
	scan_info.running = true;
	scan_info.progress = 0;
	scan_info.total = hosts;
	k_mutex_unlock(&lock);

	status_led_set(STATUS_LED_SCANNING);
	LOG_INF("Scanning %u hosts", hosts);

	/* Drop stale entries so only hosts answering now count as online */
	net_arp_clear_cache(iface);

	for (uint32_t first = 1; first <= hosts; first += BATCH_SIZE) {
		uint32_t last = MIN(first + BATCH_SIZE - 1, hosts);

		for (uint32_t h = first; h <= last; h++) {
			struct sockaddr_in dst = {
				.sin_family = AF_INET,
				.sin_port = htons(PROBE_PORT),
				.sin_addr.s_addr = htonl(ctx.net | h),
			};

			if ((ctx.net | h) == self) {
				continue;
			}
			(void)zsock_sendto(sock, "", 1, 0, (struct sockaddr *)&dst, sizeof(dst));
		}

		k_msleep(BATCH_WAIT_MS);

		ctx.now = now_s();
		net_arp_foreach(arp_collect_cb, &ctx);

		k_mutex_lock(&lock, K_FOREVER);
		scan_info.progress = last;
		k_mutex_unlock(&lock);
	}

	zsock_close(sock);

	lladdr = net_if_get_link_addr(iface);

	k_mutex_lock(&lock, K_FOREVER);
	upsert(own, lladdr->addr, now_s(), true);
	for (int i = 0; i < device_count; i++) {
		devices[i].online = seen[i];
	}
	scan_info.running = false;
	scan_info.last_scan_end = now_s();
	scan_info.duration_ms = (uint32_t)(k_uptime_get() - start);
	k_mutex_unlock(&lock);

	status_led_set(wifi_is_online() ? STATUS_LED_ONLINE : STATUS_LED_CONNECTING);
	LOG_INF("Scan done in %u ms, %d devices known", scan_info.duration_ms, device_count);
}

static void scan_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_mutex_lock(&lock, K_FOREVER);
		scan_info.next_scan_at = now_s() + CONFIG_APP_SCAN_INTERVAL_MIN * 60;
		k_mutex_unlock(&lock);

		/* Wake up on a manual request or when the interval expires */
		(void)k_sem_take(&scan_sem, K_MINUTES(CONFIG_APP_SCAN_INTERVAL_MIN));
		run_scan();
	}
}

K_THREAD_DEFINE(lan_scan_tid, 2048, scan_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

bool lan_scan_request(void)
{
	bool running;

	k_mutex_lock(&lock, K_FOREVER);
	running = scan_info.running;
	k_mutex_unlock(&lock);

	if (!running) {
		k_sem_give(&scan_sem);
	}
	return !running;
}

static int cmp_ip(const void *a, const void *b)
{
	uint32_t ia = ntohl(((const struct lan_device *)a)->ip.s_addr);
	uint32_t ib = ntohl(((const struct lan_device *)b)->ip.s_addr);

	return (ia > ib) - (ia < ib);
}

int lan_scan_snapshot(struct lan_device *out, int max, struct lan_scan_info *info)
{
	int n;

	k_mutex_lock(&lock, K_FOREVER);
	n = MIN(device_count, max);
	memcpy(out, devices, n * sizeof(*out));
	if (info != NULL) {
		*info = scan_info;
	}
	k_mutex_unlock(&lock);

	qsort(out, n, sizeof(*out), cmp_ip);
	return n;
}
