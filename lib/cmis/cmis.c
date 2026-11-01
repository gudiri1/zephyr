/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/cmis/cmis.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* Milli-degrees Celsius per temperature LSB numerator/denominator (1/256 C). */
#define CMIS_TEMP_MDEGC_NUM 1000
#define CMIS_TEMP_MDEGC_DEN 256

/* Engineering-unit scale factors applied to the raw module monitors. */
#define CMIS_VCC_UV_PER_LSB    100U /* 100 uV/LSB */
#define CMIS_MAX_POWER_MW_PER_LSB 250U /* 0.25 W/LSB == 250 mW/LSB */

/* Module state occupies bits 3..1 of Lower Memory byte 3. */
#define CMIS_MODULE_STATE_SHIFT 1U
#define CMIS_MODULE_STATE_MASK  0x07U

/* Module power class occupies bits 7..5 of Upper Page 00h byte 200. */
#define CMIS_POWER_CLASS_SHIFT 5U
#define CMIS_POWER_CLASS_MASK  0x07U

/* Copy a fixed-length, space-padded ASCII field and trim trailing blanks. */
static void cmis_copy_ascii(char *dst, const uint8_t *src, size_t len)
{
	size_t trimmed = len;

	(void)memcpy(dst, src, len);
	dst[len] = '\0';

	while (trimmed > 0U) {
		char c = dst[trimmed - 1U];

		if ((c != ' ') && (c != '\0')) {
			break;
		}
		trimmed--;
	}

	dst[trimmed] = '\0';
}

/*
 * Collapse the CMIS module monitor flag byte (Lower Memory byte 9) into the
 * normalized bitmask. Byte 9 bits 0..7 are, in order: temperature high alarm,
 * temperature low alarm, temperature high warning, temperature low warning,
 * supply voltage high alarm, low alarm, high warning, low warning.
 */
static uint8_t cmis_decode_flags(uint8_t byte)
{
	uint8_t flags = 0U;

	if ((byte & BIT(0)) != 0U) {
		flags |= CMIS_FLAG_TEMP_HIGH_ALARM;
	}
	if ((byte & BIT(1)) != 0U) {
		flags |= CMIS_FLAG_TEMP_LOW_ALARM;
	}
	if ((byte & BIT(2)) != 0U) {
		flags |= CMIS_FLAG_TEMP_HIGH_WARNING;
	}
	if ((byte & BIT(3)) != 0U) {
		flags |= CMIS_FLAG_TEMP_LOW_WARNING;
	}
	if ((byte & BIT(4)) != 0U) {
		flags |= CMIS_FLAG_VCC_HIGH_ALARM;
	}
	if ((byte & BIT(5)) != 0U) {
		flags |= CMIS_FLAG_VCC_LOW_ALARM;
	}
	if ((byte & BIT(6)) != 0U) {
		flags |= CMIS_FLAG_VCC_HIGH_WARNING;
	}
	if ((byte & BIT(7)) != 0U) {
		flags |= CMIS_FLAG_VCC_LOW_WARNING;
	}

	return flags;
}

int cmis_parse_id(const uint8_t *page00, struct cmis_id *id)
{
	uint8_t power_class;
	uint8_t revision;
	uint8_t state;

	if ((page00 == NULL) || (id == NULL)) {
		return -EINVAL;
	}

	(void)memset(id, 0, sizeof(*id));

	id->identifier = page00[CMIS_LOWER_IDENTIFIER];

	revision = page00[CMIS_LOWER_REVISION];
	id->cmis_major = revision >> 4;
	id->cmis_minor = revision & 0x0FU;

	id->flat_memory = (page00[CMIS_LOWER_MEMORY_MODEL] & CMIS_MEMORY_MODEL_FLAT_MEM) != 0U;

	state = page00[CMIS_LOWER_MODULE_STATE];
	id->module_state = (state >> CMIS_MODULE_STATE_SHIFT) & CMIS_MODULE_STATE_MASK;

	id->media_type = page00[CMIS_LOWER_MEDIA_TYPE];

	/* Encoded 0..7 stands for power class 1..8. */
	power_class =
		(page00[CMIS_PAGE00_POWER_CLASS] >> CMIS_POWER_CLASS_SHIFT) & CMIS_POWER_CLASS_MASK;
	id->power_class = power_class + 1U;
	id->max_power_mw = (uint16_t)page00[CMIS_PAGE00_MAX_POWER] * CMIS_MAX_POWER_MW_PER_LSB;

	(void)memcpy(id->vendor_oui, &page00[CMIS_PAGE00_VENDOR_OUI], CMIS_VENDOR_OUI_LEN);

	cmis_copy_ascii(id->vendor_name, &page00[CMIS_PAGE00_VENDOR_NAME], CMIS_VENDOR_NAME_LEN);
	cmis_copy_ascii(id->vendor_pn, &page00[CMIS_PAGE00_VENDOR_PN], CMIS_VENDOR_PN_LEN);
	cmis_copy_ascii(id->vendor_rev, &page00[CMIS_PAGE00_VENDOR_REV], CMIS_VENDOR_REV_LEN);
	cmis_copy_ascii(id->vendor_sn, &page00[CMIS_PAGE00_VENDOR_SN], CMIS_VENDOR_SN_LEN);
	cmis_copy_ascii(id->date_code, &page00[CMIS_PAGE00_DATE_CODE], CMIS_DATE_CODE_LEN);

	return 0;
}

int cmis_parse_monitors(const uint8_t *lower, struct cmis_monitors *mon)
{
	int16_t temp_raw;

	if ((lower == NULL) || (mon == NULL)) {
		return -EINVAL;
	}

	temp_raw = (int16_t)sys_get_be16(&lower[CMIS_LOWER_TEMPERATURE]);
	mon->temp_mdegc = ((int32_t)temp_raw * CMIS_TEMP_MDEGC_NUM) / CMIS_TEMP_MDEGC_DEN;

	mon->vcc_uv = (uint32_t)sys_get_be16(&lower[CMIS_LOWER_VCC]) * CMIS_VCC_UV_PER_LSB;

	mon->flags = cmis_decode_flags(lower[CMIS_LOWER_MODULE_FLAGS]);

	return 0;
}

const char *cmis_identifier_name(uint8_t identifier)
{
	switch (identifier) {
	case CMIS_ID_QSFP_DD:
		return "QSFP-DD";
	case CMIS_ID_OSFP:
		return "OSFP";
	case CMIS_ID_SFP_DD:
		return "SFP-DD";
	case CMIS_ID_DSFP:
		return "DSFP";
	case CMIS_ID_QSFP:
		return "QSFP+ (CMIS)";
	default:
		return "Reserved or vendor specific";
	}
}

const char *cmis_module_state_name(uint8_t state)
{
	switch (state) {
	case CMIS_MODULE_STATE_LOW_PWR:
		return "Low power";
	case CMIS_MODULE_STATE_PWR_UP:
		return "Power up";
	case CMIS_MODULE_STATE_READY:
		return "Ready";
	case CMIS_MODULE_STATE_PWR_DOWN:
		return "Power down";
	case CMIS_MODULE_STATE_FAULT:
		return "Fault";
	default:
		return "Reserved";
	}
}

const char *cmis_media_type_name(uint8_t media_type)
{
	switch (media_type) {
	case CMIS_MEDIA_UNDEFINED:
		return "Undefined";
	case CMIS_MEDIA_OPTICAL_MMF:
		return "Multimode fiber";
	case CMIS_MEDIA_OPTICAL_SMF:
		return "Single-mode fiber";
	case CMIS_MEDIA_PASSIVE_COPPER:
		return "Passive copper cable";
	case CMIS_MEDIA_ACTIVE_CABLE:
		return "Active cable";
	case CMIS_MEDIA_BASE_T:
		return "BASE-T";
	default:
		return "Reserved or vendor specific";
	}
}

/* Tx bias current: 2 uA per LSB, times the Page 01h scaling factor. */
#define CMIS_TX_BIAS_UA_PER_LSB     2U
#define CMIS_TX_BIAS_SCALE_SHIFT    3U
#define CMIS_TX_BIAS_SCALE_MASK     0x03U
#define CMIS_TX_BIAS_SCALE_RESERVED 0x03U

/* Optical power: 0.1 uW per LSB == 100 nW per LSB. */
#define CMIS_POWER_NW_PER_LSB 100U

static bool cmis_multiplier_valid(uint8_t multiplier)
{
	return (multiplier == 1U) || (multiplier == 2U) || (multiplier == 4U);
}

int cmis_parse_tx_bias_multiplier(const uint8_t *page01, uint8_t *multiplier)
{
	uint8_t code;

	if ((page01 == NULL) || (multiplier == NULL)) {
		return -EINVAL;
	}

	code = (page01[CMIS_UPPER(CMIS_PAGE01_LANE_MON_ADV)] >> CMIS_TX_BIAS_SCALE_SHIFT) &
	       CMIS_TX_BIAS_SCALE_MASK;
	if (code == CMIS_TX_BIAS_SCALE_RESERVED) {
		return -EINVAL;
	}

	*multiplier = (uint8_t)BIT(code);

	return 0;
}

static void cmis_decode_threshold(const uint8_t *page, uint8_t addr, bool is_signed, int32_t num,
				  int32_t den, struct cmis_threshold *t)
{
	int32_t raw[4];

	for (size_t i = 0U; i < ARRAY_SIZE(raw); i++) {
		uint16_t v = sys_get_be16(&page[CMIS_UPPER(addr) + (2U * i)]);

		raw[i] = is_signed ? (int32_t)(int16_t)v : (int32_t)v;
		raw[i] = (raw[i] * num) / den;
	}

	t->high_alarm = raw[0];
	t->low_alarm = raw[1];
	t->high_warning = raw[2];
	t->low_warning = raw[3];
}

int cmis_parse_thresholds(const uint8_t *page02, uint8_t tx_bias_multiplier,
			  struct cmis_thresholds *thr)
{
	if ((page02 == NULL) || (thr == NULL) || !cmis_multiplier_valid(tx_bias_multiplier)) {
		return -EINVAL;
	}

	cmis_decode_threshold(page02, CMIS_PAGE02_TEMP, true, CMIS_TEMP_MDEGC_NUM,
			      CMIS_TEMP_MDEGC_DEN, &thr->temp_mdegc);
	cmis_decode_threshold(page02, CMIS_PAGE02_VCC, false, CMIS_VCC_UV_PER_LSB, 1, &thr->vcc_uv);
	cmis_decode_threshold(page02, CMIS_PAGE02_TX_POWER, false, CMIS_POWER_NW_PER_LSB, 1,
			      &thr->tx_power_nw);
	cmis_decode_threshold(page02, CMIS_PAGE02_TX_BIAS, false,
			      (int32_t)(CMIS_TX_BIAS_UA_PER_LSB * tx_bias_multiplier), 1,
			      &thr->tx_bias_ua);
	cmis_decode_threshold(page02, CMIS_PAGE02_RX_POWER, false, CMIS_POWER_NW_PER_LSB, 1,
			      &thr->rx_power_nw);

	return 0;
}

int cmis_parse_lane_status(const uint8_t *page11, uint8_t tx_bias_multiplier,
			   struct cmis_lane_status lanes[CMIS_LANES_PER_BANK])
{
	if ((page11 == NULL) || (lanes == NULL) || !cmis_multiplier_valid(tx_bias_multiplier)) {
		return -EINVAL;
	}

	for (uint8_t lane = 0U; lane < CMIS_LANES_PER_BANK; lane++) {
		struct cmis_lane_status *st = &lanes[lane];
		uint8_t dp = page11[CMIS_UPPER(CMIS_PAGE11_DP_STATE) + (lane / 2U)];
		size_t mon = 2U * lane;

		st->dp_state = ((lane & 1U) != 0U) ? (dp >> 4) : (dp & 0x0FU);
		st->tx_power_nw =
			(uint32_t)sys_get_be16(&page11[CMIS_UPPER(CMIS_PAGE11_TX_POWER) + mon]) *
			CMIS_POWER_NW_PER_LSB;
		st->tx_bias_ua =
			(uint32_t)sys_get_be16(&page11[CMIS_UPPER(CMIS_PAGE11_TX_BIAS) + mon]) *
			CMIS_TX_BIAS_UA_PER_LSB * tx_bias_multiplier;
		st->rx_power_nw =
			(uint32_t)sys_get_be16(&page11[CMIS_UPPER(CMIS_PAGE11_RX_POWER) + mon]) *
			CMIS_POWER_NW_PER_LSB;

		st->flags = 0U;
		for (uint8_t i = 0U; i <= (CMIS_PAGE11_FLAGS_LAST - CMIS_PAGE11_FLAGS_FIRST); i++) {
			if ((page11[CMIS_UPPER(CMIS_PAGE11_FLAGS_FIRST) + i] & BIT(lane)) != 0U) {
				st->flags |= BIT(i);
			}
		}
	}

	return 0;
}

const char *cmis_dp_state_name(uint8_t state)
{
	switch (state) {
	case CMIS_DP_STATE_DEACTIVATED:
		return "Deactivated";
	case CMIS_DP_STATE_INIT:
		return "Init";
	case CMIS_DP_STATE_DEINIT:
		return "Deinit";
	case CMIS_DP_STATE_ACTIVATED:
		return "Activated";
	case CMIS_DP_STATE_TX_TURN_ON:
		return "TxTurnOn";
	case CMIS_DP_STATE_TX_TURN_OFF:
		return "TxTurnOff";
	case CMIS_DP_STATE_INITIALIZED:
		return "Initialized";
	default:
		return "Reserved";
	}
}
