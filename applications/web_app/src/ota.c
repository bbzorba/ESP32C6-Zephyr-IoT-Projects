/*
 * Firmware updates (see BOOTLOADER.md).
 *
 * New images arrive over Wi-Fi through Zephyr's mcumgr UDP transport
 * (port 1337), which needs no code here. MCUboot swaps an uploaded image in
 * for a trial boot; it is kept only if this firmware confirms it.
 *
 * We confirm once the hub is back online (Wi-Fi + IPv4 address): that proves
 * the new image can still receive the next update. If it never gets online,
 * the Wi-Fi recovery in wifi.c reboots after 5 minutes and MCUboot restores
 * the previous image.
 *
 * A confirmed firmware then installs a bootloader staged in slot 1, if
 * there is one (bl_update.c).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/mcuboot.h>
#include <app_version.h>

#include "app.h"

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

static void confirm_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	if (!boot_is_img_confirmed()) {
		ret = boot_write_img_confirmed();
		if (ret) {
			LOG_ERR("Could not confirm firmware %s (%d)", APP_VERSION_STRING, ret);
			return;
		}
		LOG_INF("Firmware %s confirmed, it stays installed", APP_VERSION_STRING);
	}

	/* Only a confirmed firmware touches the bootloader */
	bl_update_check();
}

/* Flash writes don't belong in the network event callback */
static K_WORK_DEFINE(confirm_work, confirm_work_handler);

void ota_online(void)
{
	k_work_submit(&confirm_work);
}

void ota_log_boot(void)
{
	LOG_INF("Firmware %s (%s)", APP_VERSION_STRING,
		boot_is_img_confirmed() ? "confirmed" : "trial boot, confirms when online");
}

const char *ota_version(void)
{
	return APP_VERSION_STRING;
}

bool ota_confirmed(void)
{
	return boot_is_img_confirmed();
}
