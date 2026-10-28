// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2016 The Android Open Source Project
 */

#include <env.h>
#include <fastboot.h>
#include <fastboot-internal.h>
#include <fb_mmc.h>
#include <fb_block.h>
#include <fb_nand.h>
#include <fb_spi_flash.h>
#include <fs.h>
#include <part.h>
#include <version.h>
#include <vsprintf.h>
#include <linux/printk.h>

static void getvar_version(char *var_parameter, char *response);
static void getvar_version_bootloader(char *var_parameter, char *response);
static void getvar_downloadsize(char *var_parameter, char *response);
static void getvar_serialno(char *var_parameter, char *response);
static void getvar_version_baseband(char *var_parameter, char *response);
static void getvar_product(char *var_parameter, char *response);
static void getvar_platform(char *var_parameter, char *response);
static void getvar_current_slot(char *var_parameter, char *response);
static void getvar_has_slot(char *var_parameter, char *response);
static void getvar_partition_type(char *part_name, char *response);
static void getvar_partition_size(char *part_name, char *response);
static void getvar_is_userspace(char *var_parameter, char *response);

static const struct {
	const char *variable;
	bool list;
	void (*dispatch)(char *var_parameter, char *response);
} getvar_dispatch[] = {
	{
		.variable = "version",
		.dispatch = getvar_version,
		.list = true,
	}, {
		.variable = "version-bootloader",
		.dispatch = getvar_version_bootloader,
		.list = true
	}, {
		.variable = "downloadsize",
		.dispatch = getvar_downloadsize,
		.list = true
	}, {
		.variable = "max-download-size",
		.dispatch = getvar_downloadsize,
		.list = true
	}, {
		.variable = "serialno",
		.dispatch = getvar_serialno,
		.list = true
	}, {
		.variable = "version-baseband",
		.dispatch = getvar_version_baseband,
		.list = true
	}, {
		.variable = "product",
		.dispatch = getvar_product,
		.list = true
	}, {
		.variable = "platform",
		.dispatch = getvar_platform,
		.list = true
	}, {
		.variable = "current-slot",
		.dispatch = getvar_current_slot,
		.list = true
#if CONFIG_IS_ENABLED(FASTBOOT_FLASH)
	}, {
		.variable = "has-slot",
		.dispatch = getvar_has_slot,
		.list = false
#endif
#if CONFIG_IS_ENABLED(FASTBOOT_FLASH_MMC)
	}, {
		.variable = "partition-type",
		.dispatch = getvar_partition_type,
		.list = false
#endif
#if CONFIG_IS_ENABLED(FASTBOOT_FLASH)
	}, {
		.variable = "partition-size",
		.dispatch = getvar_partition_size,
		.list = false
#endif
	}, {
		.variable = "is-userspace",
		.dispatch = getvar_is_userspace,
		.list = true
	}
};

/**
 * Get partition number and size for any storage type.
 *
 * Can be used to check if partition with specified name exists.
 *
 * If error occurs, this function guarantees to fill @p response with fail
 * string. @p response can be rewritten in caller, if needed.
 *
 * @param[in] part_name Info for which partition name to look for
 * @param[in,out] response Pointer to fastboot response buffer
 * @param[out] size If not NULL, will contain partition size
 * Return: Partition number or negative value on error
 */
static int getvar_get_part_info(const char *part_name, char *response,
				size_t *size)
{
	int r;
	struct blk_desc *dev_desc;
	struct disk_partition disk_part;
	struct part_info *part_info;

	if (CONFIG_IS_ENABLED(FASTBOOT_FLASH_BLOCK)) {
		r = fastboot_block_get_part_info(part_name, &dev_desc, &disk_part,
						 response);
		if (r >= 0 && size)
			*size = disk_part.size * disk_part.blksz;
	} else if (CONFIG_IS_ENABLED(FASTBOOT_FLASH_MMC)) {
		r = fastboot_mmc_get_part_info(part_name, &dev_desc, &disk_part,
					       response);
		if (r >= 0 && size)
			*size = disk_part.size * disk_part.blksz;
	} else if (CONFIG_IS_ENABLED(FASTBOOT_FLASH_NAND)) {
		r = fastboot_nand_get_part_info(part_name, &part_info, response);
		if (r >= 0 && size)
			*size = part_info->size;
	} else if (CONFIG_IS_ENABLED(FASTBOOT_FLASH_SPI)) {
		r = fastboot_spi_flash_get_part_info(part_name, &disk_part,
						     response);
		if (r >= 0 && size)
			*size = disk_part.size * disk_part.blksz;
	} else {
		fastboot_fail("this storage is not supported in bootloader", response);
		r = -ENODEV;
	}

	return r;
}

static void getvar_version(char *var_parameter, char *response)
{
	fastboot_okay(FASTBOOT_VERSION, response);
}

static void getvar_version_bootloader(char *var_parameter, char *response)
{
	fastboot_okay(U_BOOT_VERSION, response);
}

static void getvar_downloadsize(char *var_parameter, char *response)
{
	fastboot_response("OKAY", response, "0x%08x", fastboot_buf_size);
}

static void getvar_serialno(char *var_parameter, char *response)
{
	const char *tmp = CONFIG_IS_ENABLED(ENV_SUPPORT,
					    (env_get("serial#")), (NULL));

	if (tmp)
		fastboot_okay(tmp, response);
	else
		fastboot_fail("Value not set", response);
}

static void getvar_version_baseband(char *var_parameter, char *response)
{
	fastboot_okay("N/A", response);
}

static void getvar_product(char *var_parameter, char *response)
{
	const char *board = CONFIG_IS_ENABLED(ENV_SUPPORT,
					      (env_get("board")), (NULL));

	if (board)
		fastboot_okay(board, response);
	else
		fastboot_fail("Board not set", response);
}

static void getvar_platform(char *var_parameter, char *response)
{
	const char *p = CONFIG_IS_ENABLED(ENV_SUPPORT,
					  (env_get("platform")), (NULL));

	if (p)
		fastboot_okay(p, response);
	else
		fastboot_fail("platform not set", response);
}

static void getvar_current_slot(char *var_parameter, char *response)
{
	/*
	 * Upstream always answered "a" here unconditionally (A/B isn't
	 * really implemented in mainline U-Boot) - but adt1061 is a
	 * confirmed non-A/B device (GPT has a plain "boot", not
	 * "boot_a"/"boot_b", and none of its other partitions are slotted
	 * either), so claiming slot "a" would be lying to tools that key
	 * off this getvar. Fail the same way a real non-A/B bootloader
	 * does, the way "fastboot getvar current-slot" is supposed to
	 * behave on a device that doesn't have slots at all.
	 *
	 * This used to detect that dynamically via getvar_get_part_info()
	 * looking for "boot_a" - correct in principle, but each failed
	 * partition-type probe prints its own "Bad device specification"/
	 * "Couldn't find partition" lines, and a real MMC partition-table
	 * scan on every single "getvar all" round-trip was slow enough to
	 * make the host time out before the rest of the variables ever
	 * got sent. Just answer directly - this board's slot status is
	 * fixed and known, not something to re-probe on every query.
	 */
	fastboot_fail("A/B not supported", response);
}

static void __maybe_unused getvar_has_slot(char *part_name, char *response)
{
	char part_name_wslot[PART_NAME_LEN];
	size_t len;
	int r;

	if (!part_name || part_name[0] == '\0')
		goto fail;

	/* part_name_wslot = part_name + "_a" */
	len = strlcpy(part_name_wslot, part_name, PART_NAME_LEN - 3);
	if (len >= PART_NAME_LEN - 3)
		goto fail;
	strcat(part_name_wslot, "_a");

	r = getvar_get_part_info(part_name_wslot, response, NULL);
	if (r >= 0) {
		fastboot_okay("yes", response); /* part exists and slotted */
		return;
	}

	r = getvar_get_part_info(part_name, response, NULL);
	if (r >= 0)
		fastboot_okay("no", response); /* part exists but not slotted */

	/* At this point response is filled with okay or fail string */
	return;

fail:
	fastboot_fail("invalid partition name", response);
}

static void __maybe_unused getvar_partition_type(char *part_name, char *response)
{
	int r;
	struct blk_desc *dev_desc;
	struct disk_partition part_info;

	r = fastboot_mmc_get_part_info(part_name, &dev_desc, &part_info,
				       response);
	if (r >= 0) {
#ifdef CONFIG_XPL_BUILD
		/* SPL does not pull in filesystem probing just for this getvar. */
		fastboot_okay("raw", response);
#else
		r = fs_set_blk_dev_with_part(dev_desc, r);
		if (r < 0)
			/* If we don't know then just default to raw */
			fastboot_okay("raw", response);
		else
			fastboot_okay(fs_get_type_name(), response);
#endif
	}
}

static void __maybe_unused getvar_partition_size(char *part_name, char *response)
{
	int r;
	size_t size;

	r = getvar_get_part_info(part_name, response, &size);
	if (r >= 0)
		fastboot_response("OKAY", response, "0x%016zx", size);
}

static void getvar_is_userspace(char *var_parameter, char *response)
{
	fastboot_okay("no", response);
}

#if CONFIG_IS_ENABLED(FASTBOOT_FLASH_MMC)
/*
 * Real-world fastboot clients/users expect "getvar all" to also list
 * every partition's "partition-type"/"partition-size", the way vendor
 * bootloaders do - mainline's dispatch table above only answers those
 * for an explicit "getvar partition-type:<name>"/"partition-size:<name>"
 * query (list = false), it never walks the partition table on its own.
 * adt1061 is a plain non-A/B device whose GPT already matches the
 * vendor's official ums512_1h10.xml BMAConfig layout 1:1 (42 partitions,
 * none "_a"/"_b" suffixed - see getvar_current_slot() above), so just
 * walk that same GPT here and report each entry the same way.
 */
static int current_all_partition = 1;
static int current_all_partition_phase; /* 0 = type line, 1 = size line */

static bool getvar_all_next_partition(char *response)
{
	struct blk_desc *dev_desc;
	struct disk_partition info;

	dev_desc = blk_get_dev("mmc", CONFIG_VAL(FASTBOOT_FLASH_MMC_DEV));
	if (!dev_desc)
		return false;

	if (part_get_info(dev_desc, current_all_partition, &info))
		return false; /* no more partitions on the GPT */

	if (current_all_partition_phase == 0) {
		fastboot_response("INFO", response, "partition-type:%s: raw",
				  info.name);
		current_all_partition_phase = 1;
	} else {
		size_t size = (size_t)info.size * info.blksz;

		fastboot_response("INFO", response,
				  "partition-size:%s: 0x%016zx",
				  info.name, size);
		current_all_partition_phase = 0;
		current_all_partition++;
	}

	return true;
}
#endif

static int current_all_dispatch;
void fastboot_getvar_all(char *response)
{
	/*
	 * Find a dispatch getvar that can be listed and send
	 * it as INFO until we reach the end.
	 */
	while (current_all_dispatch < ARRAY_SIZE(getvar_dispatch)) {
		if (!getvar_dispatch[current_all_dispatch].list) {
			current_all_dispatch++;
			continue;
		}

		char envstr[FASTBOOT_RESPONSE_LEN] = { 0 };

		getvar_dispatch[current_all_dispatch].dispatch(NULL, envstr);

		char *envstr_start = envstr;

		if (!strncmp("OKAY", envstr, 4) || !strncmp("FAIL", envstr, 4))
			envstr_start += 4;

		fastboot_response("INFO", response, "%s: %s",
				  getvar_dispatch[current_all_dispatch].variable,
				  envstr_start);

		current_all_dispatch++;
		return;
	}

#if CONFIG_IS_ENABLED(FASTBOOT_FLASH_MMC)
	if (getvar_all_next_partition(response))
		return;

	current_all_partition = 1;
	current_all_partition_phase = 0;
#endif

	fastboot_response("OKAY", response, NULL);
	current_all_dispatch = 0;
}

/**
 * fastboot_getvar() - Writes variable indicated by cmd_parameter to response.
 *
 * @cmd_parameter: Pointer to command parameter
 * @response: Pointer to fastboot response buffer
 *
 * Look up cmd_parameter first as an environment variable of the form
 * fastboot.<cmd_parameter>, if that exists return use its value to set
 * response.
 *
 * Otherwise lookup the name of variable and execute the appropriate
 * function to return the requested value.
 */
void fastboot_getvar(char *cmd_parameter, char *response)
{
	if (!cmd_parameter) {
		fastboot_fail("missing var", response);
	} else if (!strncmp("all", cmd_parameter, 3) && strlen(cmd_parameter) == 3) {
		current_all_dispatch = 0;
		fastboot_response(FASTBOOT_MULTIRESPONSE_START, response, NULL);
	} else {
#define FASTBOOT_ENV_PREFIX	"fastboot."
		int i;
		char *var_parameter = cmd_parameter;
		char envstr[FASTBOOT_RESPONSE_LEN];
		const char *s;

		snprintf(envstr, sizeof(envstr) - 1,
			 FASTBOOT_ENV_PREFIX "%s", cmd_parameter);
		s = CONFIG_IS_ENABLED(ENV_SUPPORT, (env_get(envstr)), (NULL));
		if (s) {
			fastboot_response("OKAY", response, "%s", s);
			return;
		}

		strsep(&var_parameter, ":");
		for (i = 0; i < ARRAY_SIZE(getvar_dispatch); ++i) {
			if (!strcmp(getvar_dispatch[i].variable,
				    cmd_parameter)) {
				getvar_dispatch[i].dispatch(var_parameter,
							    response);
				return;
			}
		}
		pr_warn("WARNING: unknown variable: %s\n", cmd_parameter);
		fastboot_fail("Variable not implemented", response);
	}
}
