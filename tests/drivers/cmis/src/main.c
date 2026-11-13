/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/cmis/cmis.h>
#include <zephyr/device.h>
#include <zephyr/drivers/cmis.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "emul_cmis.h"

#define CAGE_NODE DT_NODELABEL(cmis0)

static const struct device *const cmis_dev = DEVICE_DT_GET(CAGE_NODE);
static const struct emul *const cmis_emul = EMUL_DT_GET(DT_NODELABEL(cmis_module));
static const struct i2c_dt_spec cmis_bus = I2C_DT_SPEC_GET(DT_NODELABEL(cmis_module));
static const struct device *const presence_port = DEVICE_DT_GET(DT_NODELABEL(cmis_gpio));
static const gpio_pin_t presence_pin = DT_GPIO_PIN(CAGE_NODE, modprsl_gpios);

/* ModPrsL is pulled low by an inserted module (active low). */
static void set_module_present(bool present)
{
	zassert_ok(gpio_emul_input_set(presence_port, presence_pin, present ? 0 : 1));
}

/* Read one Lower Memory byte the way a host interrupt handler would. */
static uint8_t raw_read(uint8_t offset)
{
	uint8_t val = 0xFFU;

	zassert_ok(i2c_write_read_dt(&cmis_bus, &offset, 1U, &val, 1U));
	return val;
}

static void *cmis_setup(void)
{
	if (IS_ENABLED(CONFIG_USERSPACE)) {
		k_object_access_all_grant(cmis_dev);
	}

	return NULL;
}

/* Runs in supervisor mode, also for user-mode tests. */
static void cmis_before(void *fixture)
{
	ARG_UNUSED(fixture);
	set_module_present(true);
	cmis_emul_reset(cmis_emul);
}

ZTEST(cmis_driver, test_device_ready)
{
	zassert_true(device_is_ready(cmis_dev));
}

ZTEST_USER(cmis_driver, test_read_id)
{
	struct cmis_id id;

	zassert_ok(cmis_read_id(cmis_dev, &id));
	zassert_equal(id.identifier, CMIS_ID_QSFP_DD);
	zassert_equal(id.cmis_major, 5U);
	zassert_equal(id.cmis_minor, 0U);
	zassert_equal(id.module_state, CMIS_MODULE_STATE_READY);
	zassert_equal(id.media_type, CMIS_MEDIA_OPTICAL_SMF);
	zassert_equal(id.power_class, 8U);
	zassert_equal(id.max_power_mw, 14000U, "got %u", id.max_power_mw);
	zassert_equal(strcmp(id.vendor_name, "ACME PHOTONICS"), 0, "got '%s'", id.vendor_name);
	zassert_equal(strcmp(id.vendor_pn, "QSFP-DD-400G"), 0, "got '%s'", id.vendor_pn);
	zassert_equal(strcmp(id.vendor_sn, "QD2400001234"), 0, "got '%s'", id.vendor_sn);
}

ZTEST_USER(cmis_driver, test_read_monitors)
{
	struct cmis_monitors mon;

	zassert_ok(cmis_read_monitors(cmis_dev, &mon));
	zassert_equal(mon.temp_mdegc, 45500, "got %d", mon.temp_mdegc);
	zassert_equal(mon.vcc_uv, 3300000U, "got %u", mon.vcc_uv);
	zassert_equal(mon.flags, CMIS_FLAG_TEMP_HIGH_ALARM | CMIS_FLAG_VCC_HIGH_ALARM,
		      "got 0x%02x", mon.flags);
}

ZTEST(cmis_driver, test_monitor_flags_reported_once)
{
	struct cmis_monitors mon;

	zassert_ok(cmis_read_monitors(cmis_dev, &mon));
	zassert_equal(mon.flags, CMIS_FLAG_TEMP_HIGH_ALARM | CMIS_FLAG_VCC_HIGH_ALARM);

	/* Latched flags are cleared by the read: the next read sees no event. */
	zassert_ok(cmis_read_monitors(cmis_dev, &mon));
	zassert_equal(mon.flags, 0U, "got 0x%02x", mon.flags);
	zassert_equal(mon.temp_mdegc, 45500, "monitor values are not latched");
}

ZTEST(cmis_driver, test_reads_keep_unreported_flags)
{
	struct cmis_monitors mon;
	struct cmis_id id;

	zassert_ok(cmis_read_id(cmis_dev, &id));
	zassert_ok(cmis_read_monitors(cmis_dev, &mon));

	/* ModuleStateChanged (byte 8) is not returned by either call, so it
	 * must still be pending for the host's interrupt handling.
	 */
	zassert_equal(raw_read(CMIS_EMUL_MODULE_STATE_FLAGS) & CMIS_EMUL_STATE_CHANGED,
		      CMIS_EMUL_STATE_CHANGED);

	/* read_id() must not have consumed the byte 9 flags either. */
	zassert_equal(mon.flags, CMIS_FLAG_TEMP_HIGH_ALARM | CMIS_FLAG_VCC_HIGH_ALARM);
}

ZTEST(cmis_driver, test_empty_cage)
{
	struct cmis_monitors mon;
	struct cmis_id id;

	set_module_present(false);
	zassert_equal(cmis_read_id(cmis_dev, &id), -ENODEV);
	zassert_equal(cmis_read_monitors(cmis_dev, &mon), -ENODEV);

	set_module_present(true);
	zassert_ok(cmis_read_id(cmis_dev, &id));
}

ZTEST_USER(cmis_driver, test_read_thresholds)
{
	struct cmis_thresholds thr;

	zassert_ok(cmis_read_thresholds(cmis_dev, &thr));
	zassert_equal(thr.temp_mdegc.high_alarm, 75000);
	zassert_equal(thr.temp_mdegc.low_alarm, -5000, "got %d", thr.temp_mdegc.low_alarm);
	zassert_equal(thr.vcc_uv.low_alarm, 2970000);
	zassert_equal(thr.tx_bias_ua.high_alarm, 100000, "x2 scaling from Page 01h");
	zassert_equal(thr.rx_power_nw.low_alarm, 63100);
}

ZTEST_USER(cmis_driver, test_read_lanes)
{
	struct cmis_lane_status lanes[CMIS_LANES_PER_BANK];

	zassert_ok(cmis_read_lanes(cmis_dev, lanes));

	for (uint8_t i = 0U; i < CMIS_LANES_PER_BANK; i++) {
		zassert_equal(lanes[i].dp_state, CMIS_DP_STATE_ACTIVATED, "lane %u", i + 1U);
	}
	zassert_equal(lanes[0].tx_power_nw, 1258900U);
	zassert_equal(lanes[0].tx_bias_ua, 56000U, "got %u", lanes[0].tx_bias_ua);
	zassert_equal(lanes[0].rx_power_nw, 501200U);
	zassert_equal(lanes[0].flags, 0U);
	zassert_equal(lanes[2].rx_power_nw, 1000U);
	zassert_equal(lanes[2].flags,
		      CMIS_LANE_FLAG_RX_LOS | CMIS_LANE_FLAG_RX_CDR_LOL |
			      CMIS_LANE_FLAG_RX_POWER_LOW_ALARM,
		      "got 0x%x", lanes[2].flags);
}

ZTEST(cmis_driver, test_lane_flags_reported_once)
{
	struct cmis_lane_status lanes[CMIS_LANES_PER_BANK];

	zassert_ok(cmis_read_lanes(cmis_dev, lanes));
	zassert_not_equal(lanes[2].flags, 0U);

	zassert_ok(cmis_read_lanes(cmis_dev, lanes));
	zassert_equal(lanes[2].flags, 0U, "lane flags are cleared on read");
	zassert_equal(lanes[2].rx_power_nw, 1000U, "monitor values are not latched");
}

ZTEST(cmis_driver, test_paged_reads_on_empty_cage)
{
	struct cmis_lane_status lanes[CMIS_LANES_PER_BANK];
	struct cmis_thresholds thr;

	set_module_present(false);
	zassert_equal(cmis_read_thresholds(cmis_dev, &thr), -ENODEV);
	zassert_equal(cmis_read_lanes(cmis_dev, lanes), -ENODEV);
}

ZTEST(cmis_driver, test_page_switching)
{
	struct cmis_lane_status lanes[CMIS_LANES_PER_BANK];
	struct cmis_id id;

	/* Page 00h content must be read correctly after Page 11h was selected. */
	zassert_ok(cmis_read_lanes(cmis_dev, lanes));
	zassert_ok(cmis_read_id(cmis_dev, &id));
	zassert_equal(strcmp(id.vendor_pn, "QSFP-DD-400G"), 0, "got '%s'", id.vendor_pn);
}

#define RACE_LOOPS 25
#define RACE_STACK 2048

static K_THREAD_STACK_DEFINE(race_stack_a, RACE_STACK);
static K_THREAD_STACK_DEFINE(race_stack_b, RACE_STACK);
static struct k_thread race_thread_a;
static struct k_thread race_thread_b;
static atomic_t race_errors;

static void race_read_id(void *p1, void *p2, void *p3)
{
	struct cmis_id id;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (int i = 0; i < RACE_LOOPS; i++) {
		if ((cmis_read_id(cmis_dev, &id) != 0) ||
		    (strcmp(id.vendor_pn, "QSFP-DD-400G") != 0)) {
			atomic_inc(&race_errors);
		}
	}
}

static void race_read_lanes(void *p1, void *p2, void *p3)
{
	struct cmis_lane_status lanes[CMIS_LANES_PER_BANK];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (int i = 0; i < RACE_LOOPS; i++) {
		if ((cmis_read_lanes(cmis_dev, lanes) != 0) || (lanes[0].tx_power_nw != 1258900U)) {
			atomic_inc(&race_errors);
		}
	}
}

ZTEST(cmis_driver, test_concurrent_paged_reads)
{
	/* The emulator yields after each page select, so without serialization
	 * in the driver the two threads read each other's page.
	 */
	cmis_emul_set_yield_on_select(cmis_emul, true);
	atomic_set(&race_errors, 0);

	k_thread_create(&race_thread_a, race_stack_a, RACE_STACK, race_read_id, NULL, NULL, NULL,
			K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	k_thread_create(&race_thread_b, race_stack_b, RACE_STACK, race_read_lanes, NULL, NULL,
			NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	zassert_ok(k_thread_join(&race_thread_a, K_SECONDS(5)));
	zassert_ok(k_thread_join(&race_thread_b, K_SECONDS(5)));

	cmis_emul_set_yield_on_select(cmis_emul, false);
	zassert_equal(atomic_get(&race_errors), 0, "%d reads returned another page",
		      (int)atomic_get(&race_errors));
}

static const char *shell_run(const char *cmd)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	size_t size;

	zassert_true(WAIT_FOR(shell_ready(sh), 20000, k_msleep(1)), "dummy shell not ready");

	shell_backend_dummy_clear_output(sh);
	zassert_ok(shell_execute_cmd(sh, cmd), "'%s' failed", cmd);

	return shell_backend_dummy_get_output(sh, &size);
}

ZTEST(cmis_driver, test_shell_lanes_and_thresholds)
{
	char cmd[48];
	const char *out;

	(void)snprintf(cmd, sizeof(cmd), "cmis lanes %s", cmis_dev->name);
	out = shell_run(cmd);
	zassert_not_null(strstr(out, "rx-los rx-cdr-lol rx-power-low-alarm"), "%s", out);
	zassert_not_null(strstr(out, "1258.9"), "%s", out);

	(void)snprintf(cmd, sizeof(cmd), "cmis thresholds %s", cmis_dev->name);
	out = shell_run(cmd);
	zassert_not_null(strstr(out, "100000"), "%s", out);
}

ZTEST_SUITE(cmis_driver, NULL, cmis_setup, cmis_before, NULL, NULL);
