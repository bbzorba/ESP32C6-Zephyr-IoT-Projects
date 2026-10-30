/*
 * LAN device discovery by ARP sweep, plus names by reverse DNS.
 *
 * Every host of the local /24 gets one tiny UDP datagram (discard port 9).
 * Sending it forces the stack to ARP for that address, and every host on
 * the LAN answers ARP even when it ignores ping. After each batch the ARP
 * table is read back; whatever resolved is online. Zephyr has no LAN
 * discovery API, so this sweep is the one piece written by hand.
 *
 * Names come from a PTR query to the DNS server we got over DHCP. Home
 * routers (FRITZ!Box, OpenWrt, most ISP boxes) answer these with the
 * hostname each device sent when it asked for its address.
 *
 * Devices that stop answering stay in the list marked offline, so the page
 * also shows which devices went away and when they were last seen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dns_resolve.h>

#include "arp.h"
#include "app.h"

LOG_MODULE_REGISTER(lan_scan, LOG_LEVEL_INF);

#define BATCH_SIZE      16
/* Longer than the stack's 2 s ARP request timeout, so a batch's pending
 * entries are gone before the next batch needs table slots.
 */
#define BATCH_WAIT_MS   2100
#define PROBE_PORT      9
#define DNS_TIMEOUT_MS  1000

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

/* Caller holds lock. */
static void upsert(const struct in_addr *ip, const uint8_t *mac, uint32_t now, bool self)
{
	struct lan_device *dev = NULL;
	int i;

	for (i = 0; i < device_count; i++) {
		if (devices[i].addr.s_addr == ip->s_addr) {
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
		dev->addr = *ip;
		net_addr_ntop(AF_INET, ip, dev->ip, sizeof(dev->ip));
		dev->first_seen = now;
		LOG_INF("New device %s", dev->ip);
	}

	snprintf(dev->mac, sizeof(dev->mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
		 mac[2], mac[3], mac[4], mac[5]);
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

struct ptr_query {
	struct k_sem done;
	char name[32];
};

static void ptr_cb(enum dns_resolve_status status, struct dns_addrinfo *info, void *user_data)
{
	struct ptr_query *q = user_data;

	if (status == DNS_EAI_INPROGRESS && info != NULL) {
		/* Keep the host part only: "laptop.fritz.box" -> "laptop" */
		size_t len = strcspn(info->ai_canonname, ".");

		len = MIN(len, sizeof(q->name) - 1);
		memcpy(q->name, info->ai_canonname, len);
		q->name[len] = '\0';
		return;
	}
	k_sem_give(&q->done);
}

/* Reverse DNS for one address; leaves name empty if the server has none. */
static void lookup_name(const struct in_addr *addr, char *name, size_t len)
{
	const uint8_t *b = (const uint8_t *)&addr->s_addr;
	struct ptr_query q = { 0 };
	char query[32];

	k_sem_init(&q.done, 0, 1);
	snprintf(query, sizeof(query), "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);

	if (dns_resolve_name(dns_resolve_get_default(), query, DNS_QUERY_TYPE_PTR, NULL, ptr_cb,
			     &q, DNS_TIMEOUT_MS) == 0) {
		k_sem_take(&q.done, K_MSEC(DNS_TIMEOUT_MS + 500));
	}

	/* Routers answer unnamed hosts with a placeholder like "192-168-178-1" */
	snprintf(query, sizeof(query), "%u-%u-%u-%u", b[0], b[1], b[2], b[3]);
	if (strcmp(q.name, query) == 0) {
		q.name[0] = '\0';
	}
	strncpy(name, q.name, len - 1);
	name[len - 1] = '\0';
}

static void resolve_names(void)
{
	for (int i = 0; i < device_count; i++) {
		struct in_addr addr;
		char name[sizeof(devices[0].name)];

		if (!seen[i]) {
			continue;
		}

		k_mutex_lock(&lock, K_FOREVER);
		addr = devices[i].addr;
		k_mutex_unlock(&lock);

		lookup_name(&addr, name, sizeof(name));

		k_mutex_lock(&lock, K_FOREVER);
		if (name[0] != '\0') {
			strcpy(devices[i].name, name);
		}
		k_mutex_unlock(&lock);
	}
}

static void run_scan(void)
{
	struct net_if *iface = wifi_iface();
	struct collect_ctx ctx = { .iface = iface };
	struct in_addr *own;
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

		ctx.now = k_uptime_seconds();
		net_arp_foreach(arp_collect_cb, &ctx);

		k_mutex_lock(&lock, K_FOREVER);
		scan_info.progress = last;
		k_mutex_unlock(&lock);
	}

	zsock_close(sock);

	k_mutex_lock(&lock, K_FOREVER);
	upsert(own, net_if_get_link_addr(iface)->addr, k_uptime_seconds(), true);
	k_mutex_unlock(&lock);

	resolve_names();

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < device_count; i++) {
		devices[i].online = seen[i];
	}
	scan_info.running = false;
	scan_info.last_end = k_uptime_seconds();
	scan_info.duration_ms = (uint32_t)(k_uptime_get() - start);
	k_mutex_unlock(&lock);

	status_led_set(wifi_is_online() ? STATUS_LED_ONLINE : STATUS_LED_CONNECTING);
	LOG_INF("Scan done in %u ms, %d devices known", scan_info.duration_ms, device_count);
}

/* The sweep takes ~35 s, so it runs in its own thread; the sampler tick in
 * main.c (or the "Scan now" button) only wakes it.
 */
static void scan_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&scan_sem, K_FOREVER);
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
	uint32_t ia = ntohl(((const struct lan_device *)a)->addr.s_addr);
	uint32_t ib = ntohl(((const struct lan_device *)b)->addr.s_addr);

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
