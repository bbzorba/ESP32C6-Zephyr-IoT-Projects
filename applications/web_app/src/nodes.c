/*
 * Client nodes (i.MX 8M Plus today, more later) report their sensors to
 * the hub by POSTing JSON to /api/node every 10 s:
 *
 *   {"id":"imx8mpevk","model":"NXP FRDM-IMX8MPLUS",
 *    "sensors":[{"name":"CPU temperature","value":46.0,"unit":"°C"}, ...]}
 *
 * The hub keeps the latest report per node id. A node counts as online
 * while its last report is younger than ONLINE_WINDOW_S.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/data/json.h>

#include "app.h"

LOG_MODULE_REGISTER(nodes, LOG_LEVEL_INF);

#define MAX_NODES        4
#define MAX_SENSORS      16
/* Three missed reports */
#define ONLINE_WINDOW_S  30

struct node_sensor {
	char name[24];
	double value;
	char unit[8];
};

struct node {
	char id[32];
	char model[40];
	uint32_t last;
	bool online;
	struct node_sensor sensors[MAX_SENSORS];
	size_t sensors_len;
};

struct nodes_json {
	uint32_t now;
	struct node nodes[MAX_NODES];
	size_t nodes_len;
};

static const struct json_obj_descr sensor_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct node_sensor, name, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct node_sensor, value, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct node_sensor, unit, JSON_TOK_STRING_BUF),
};

/* What a node sends */
static const struct json_obj_descr report_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct node, id, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct node, model, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct node, sensors, MAX_SENSORS, sensors_len, sensor_descr,
				 ARRAY_SIZE(sensor_descr)),
};

/* What the dashboard gets */
static const struct json_obj_descr node_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct node, id, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct node, model, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct node, last, JSON_TOK_UINT),
	JSON_OBJ_DESCR_PRIM(struct node, online, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct node, sensors, MAX_SENSORS, sensors_len, sensor_descr,
				 ARRAY_SIZE(sensor_descr)),
};

static const struct json_obj_descr nodes_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct nodes_json, now, JSON_TOK_UINT),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct nodes_json, nodes, MAX_NODES, nodes_len, node_descr,
				 ARRAY_SIZE(node_descr)),
};

static struct nodes_json table;
static struct node incoming;
static K_MUTEX_DEFINE(lock);

int nodes_report(char *json, size_t len)
{
	struct node *slot = NULL;
	int ret;

	memset(&incoming, 0, sizeof(incoming));
	ret = json_obj_parse(json, len, report_descr, ARRAY_SIZE(report_descr), &incoming);
	if (ret < 0 || !(ret & BIT(0)) || incoming.id[0] == '\0') {
		LOG_WRN("Bad node report (%d)", ret);
		return -EINVAL;
	}

	incoming.last = k_uptime_seconds();

	k_mutex_lock(&lock, K_FOREVER);
	for (size_t i = 0; i < table.nodes_len; i++) {
		if (strcmp(table.nodes[i].id, incoming.id) == 0) {
			slot = &table.nodes[i];
			break;
		}
	}
	if (slot == NULL) {
		if (table.nodes_len < MAX_NODES) {
			slot = &table.nodes[table.nodes_len++];
		} else {
			/* Full: replace the node silent for longest */
			slot = &table.nodes[0];
			for (size_t i = 1; i < MAX_NODES; i++) {
				if (table.nodes[i].last < slot->last) {
					slot = &table.nodes[i];
				}
			}
		}
		LOG_INF("New node \"%s\" (%s)", incoming.id, incoming.model);
	}
	*slot = incoming;
	k_mutex_unlock(&lock);

	return 0;
}

int nodes_encode(char *buf, size_t len)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	table.now = k_uptime_seconds();
	for (size_t i = 0; i < table.nodes_len; i++) {
		table.nodes[i].online = table.now - table.nodes[i].last <= ONLINE_WINDOW_S;
	}
	ret = json_obj_encode_buf(nodes_descr, ARRAY_SIZE(nodes_descr), &table, buf, len);
	k_mutex_unlock(&lock);

	return ret;
}
