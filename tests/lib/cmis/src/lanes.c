/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/cmis/cmis.h>

/* Upper Pages 01h, 02h and 11h, each 128 bytes with byte 128 at index 0. */
static uint8_t page01[CMIS_PAGE_SIZE];
static uint8_t page02[CMIS_PAGE_SIZE];
static uint8_t page11[CMIS_PAGE_SIZE];

static void put_be16(uint8_t *page, uint8_t addr, uint16_t val)
{
	page[CMIS_UPPER(addr)] = (uint8_t)(val >> 8);
	page[CMIS_UPPER(addr) + 1U] = (uint8_t)(val & 0xFFU);
}

static void put_thresholds(uint8_t addr, uint16_t ha, uint16_t la, uint16_t hw, uint16_t lw)
{
	put_be16(page02, addr + 0U, ha);
	put_be16(page02, addr + 2U, la);
	put_be16(page02, addr + 4U, hw);
	put_be16(page02, addr + 6U, lw);
}

static void build_pages(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)memset(page01, 0, sizeof(page01));
	(void)memset(page02, 0, sizeof(page02));
	(void)memset(page11, 0, sizeof(page11));

	/* Tx bias scaling factor x2 (bits 4..3 = 01b). */
	page01[CMIS_UPPER(CMIS_PAGE01_LANE_MON_ADV)] = 0x01U << 3;

	put_thresholds(CMIS_PAGE02_TEMP, 75U * 256U, (uint16_t)(-5 * 256), 70U * 256U, 0U);
	put_thresholds(CMIS_PAGE02_VCC, 36300U, 29700U, 34650U, 31350U);
	put_thresholds(CMIS_PAGE02_TX_POWER, 31623U, 2512U, 25119U, 3162U);
	put_thresholds(CMIS_PAGE02_TX_BIAS, 25000U, 2500U, 22500U, 3750U);
	put_thresholds(CMIS_PAGE02_RX_POWER, 31623U, 631U, 25119U, 1000U);

	/* Host lanes 1-8 DPActivated, except lane 8 in DPInitialized. */
	page11[CMIS_UPPER(CMIS_PAGE11_DP_STATE) + 0U] = 0x44U;
	page11[CMIS_UPPER(CMIS_PAGE11_DP_STATE) + 1U] = 0x44U;
	page11[CMIS_UPPER(CMIS_PAGE11_DP_STATE) + 2U] = 0x44U;
	page11[CMIS_UPPER(CMIS_PAGE11_DP_STATE) + 3U] = 0x74U;

	for (uint8_t lane = 0U; lane < 4U; lane++) {
		put_be16(page11, CMIS_PAGE11_TX_POWER + 2U * lane, 12589U); /* 1258.9 uW */
		put_be16(page11, CMIS_PAGE11_TX_BIAS + 2U * lane, 14000U);  /* 56 mA at x2 */
		put_be16(page11, CMIS_PAGE11_RX_POWER + 2U * lane, 5012U);  /* 501.2 uW */
	}

	/* Lane 3: fiber cut. Rx LOS (147), Rx LOL (148), Rx power low alarm (150). */
	put_be16(page11, CMIS_PAGE11_RX_POWER + 4U, 10U);
	page11[CMIS_UPPER(147U)] = BIT(2);
	page11[CMIS_UPPER(148U)] = BIT(2);
	page11[CMIS_UPPER(150U)] = BIT(2);
	/* Lane 2: Tx fault (135). Every lane: DP state changed (134). */
	page11[CMIS_UPPER(135U)] = BIT(1);
	page11[CMIS_UPPER(CMIS_PAGE11_FLAGS_FIRST)] = 0xFFU;
}

ZTEST_SUITE(cmis_lanes, NULL, NULL, build_pages, NULL, NULL);

ZTEST(cmis_lanes, test_tx_bias_multiplier)
{
	uint8_t mult;

	zassert_ok(cmis_parse_tx_bias_multiplier(page01, &mult));
	zassert_equal(mult, 2U);

	page01[CMIS_UPPER(CMIS_PAGE01_LANE_MON_ADV)] = 0x00U | BIT(0) | BIT(1) | BIT(2);
	zassert_ok(cmis_parse_tx_bias_multiplier(page01, &mult));
	zassert_equal(mult, 1U, "monitor support bits must not leak into the factor");

	page01[CMIS_UPPER(CMIS_PAGE01_LANE_MON_ADV)] = 0x02U << 3;
	zassert_ok(cmis_parse_tx_bias_multiplier(page01, &mult));
	zassert_equal(mult, 4U);

	page01[CMIS_UPPER(CMIS_PAGE01_LANE_MON_ADV)] = 0x03U << 3;
	zassert_equal(cmis_parse_tx_bias_multiplier(page01, &mult), -EINVAL, "reserved code");
}

ZTEST(cmis_lanes, test_thresholds)
{
	struct cmis_thresholds thr;

	zassert_ok(cmis_parse_thresholds(page02, 2U, &thr));

	zassert_equal(thr.temp_mdegc.high_alarm, 75000);
	zassert_equal(thr.temp_mdegc.low_alarm, -5000, "got %d", thr.temp_mdegc.low_alarm);
	zassert_equal(thr.temp_mdegc.high_warning, 70000);
	zassert_equal(thr.temp_mdegc.low_warning, 0);
	zassert_equal(thr.vcc_uv.high_alarm, 3630000);
	zassert_equal(thr.vcc_uv.low_warning, 3135000);
	zassert_equal(thr.tx_power_nw.high_alarm, 3162300);
	zassert_equal(thr.tx_power_nw.low_alarm, 251200);
	zassert_equal(thr.tx_bias_ua.high_alarm, 100000, "got %d", thr.tx_bias_ua.high_alarm);
	zassert_equal(thr.tx_bias_ua.low_warning, 15000);
	zassert_equal(thr.rx_power_nw.low_alarm, 63100);

	/* The scaling factor applies to the bias thresholds only. */
	zassert_ok(cmis_parse_thresholds(page02, 1U, &thr));
	zassert_equal(thr.tx_bias_ua.high_alarm, 50000);
	zassert_equal(thr.tx_power_nw.high_alarm, 3162300);
}

ZTEST(cmis_lanes, test_lane_monitors_and_states)
{
	struct cmis_lane_status lanes[CMIS_LANES_PER_BANK];

	zassert_ok(cmis_parse_lane_status(page11, 2U, lanes));

	for (uint8_t i = 0U; i < 7U; i++) {
		zassert_equal(lanes[i].dp_state, CMIS_DP_STATE_ACTIVATED, "lane %u", i + 1U);
	}
	zassert_equal(lanes[7].dp_state, CMIS_DP_STATE_INITIALIZED);

	zassert_equal(lanes[0].tx_power_nw, 1258900U);
	zassert_equal(lanes[0].tx_bias_ua, 56000U, "got %u", lanes[0].tx_bias_ua);
	zassert_equal(lanes[0].rx_power_nw, 501200U);
	zassert_equal(lanes[2].rx_power_nw, 1000U);
	zassert_equal(lanes[5].tx_power_nw, 0U, "lanes 5-8 carry no media monitors here");
}

ZTEST(cmis_lanes, test_lane_flags)
{
	struct cmis_lane_status lanes[CMIS_LANES_PER_BANK];
	const uint32_t cut = CMIS_LANE_FLAG_RX_LOS | CMIS_LANE_FLAG_RX_CDR_LOL |
			     CMIS_LANE_FLAG_RX_POWER_LOW_ALARM | CMIS_LANE_FLAG_DP_STATE_CHANGED;

	zassert_ok(cmis_parse_lane_status(page11, 1U, lanes));

	zassert_equal(lanes[0].flags, CMIS_LANE_FLAG_DP_STATE_CHANGED, "got 0x%x", lanes[0].flags);
	zassert_equal(lanes[1].flags, CMIS_LANE_FLAG_TX_FAULT | CMIS_LANE_FLAG_DP_STATE_CHANGED);
	zassert_equal(lanes[2].flags, cut, "got 0x%x", lanes[2].flags);

	/* Every flag byte maps to its own bit: set byte 152 for lane 8 only. */
	(void)memset(&page11[CMIS_UPPER(CMIS_PAGE11_FLAGS_FIRST)], 0,
		     CMIS_PAGE11_FLAGS_LAST - CMIS_PAGE11_FLAGS_FIRST + 1U);
	page11[CMIS_UPPER(CMIS_PAGE11_FLAGS_LAST)] = BIT(7);
	zassert_ok(cmis_parse_lane_status(page11, 1U, lanes));
	zassert_equal(lanes[7].flags, CMIS_LANE_FLAG_RX_POWER_LOW_WARNING);
	zassert_equal(lanes[6].flags, 0U);
}

ZTEST(cmis_lanes, test_invalid_arguments)
{
	struct cmis_lane_status lanes[CMIS_LANES_PER_BANK];
	struct cmis_thresholds thr;
	uint8_t mult;

	zassert_equal(cmis_parse_tx_bias_multiplier(NULL, &mult), -EINVAL);
	zassert_equal(cmis_parse_tx_bias_multiplier(page01, NULL), -EINVAL);
	zassert_equal(cmis_parse_thresholds(NULL, 1U, &thr), -EINVAL);
	zassert_equal(cmis_parse_thresholds(page02, 1U, NULL), -EINVAL);
	zassert_equal(cmis_parse_thresholds(page02, 3U, &thr), -EINVAL);
	zassert_equal(cmis_parse_lane_status(NULL, 1U, lanes), -EINVAL);
	zassert_equal(cmis_parse_lane_status(page11, 1U, NULL), -EINVAL);
	zassert_equal(cmis_parse_lane_status(page11, 0U, lanes), -EINVAL);
}

ZTEST(cmis_lanes, test_dp_state_names)
{
	zassert_equal(strcmp(cmis_dp_state_name(CMIS_DP_STATE_ACTIVATED), "Activated"), 0);
	zassert_equal(strcmp(cmis_dp_state_name(CMIS_DP_STATE_INITIALIZED), "Initialized"), 0);
	zassert_not_null(cmis_dp_state_name(0x0U));
	zassert_not_null(cmis_dp_state_name(0xFU));
}
