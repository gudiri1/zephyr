/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulator for a CMIS managed pluggable module on its single I2C address.
 * Lower Memory is page-independent; Upper Memory (offset 0x80 and above)
 * returns the page selected through the bank- and page-select registers.
 * Pages 00h, 01h, 02h and 11h (bank 0) are modeled; other pages read as 0.
 * Latched flags (Lower Memory bytes 8..11, Page 11h bytes 134..152) are
 * cleared by the read that returns them, as on a real module, so tests can
 * check which flags a driver consumes.
 */

#define DT_DRV_COMPAT zephyr_cmis_module_emul

#include <string.h>

#include <zephyr/cmis/cmis.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/emul_stub_device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "emul_cmis.h"

/* CMIS bank- and page-select registers in Lower Memory. */
#define CMIS_REG_BANK_SELECT 0x7EU
#define CMIS_REG_PAGE_SELECT 0x7FU

/* Latched, clear-on-read flag bytes in Lower Memory. */
#define CMIS_LATCHED_FIRST 8U
#define CMIS_LATCHED_LAST  11U

enum cmis_emul_page {
	EMUL_PAGE_01,
	EMUL_PAGE_02,
	EMUL_PAGE_11,
	EMUL_PAGE_COUNT,
};

struct cmis_emul_data {
	uint8_t mem[CMIS_PAGE_00H_SIZE];                /* Lower Memory plus Upper Page 00h. */
	uint8_t upper[EMUL_PAGE_COUNT][CMIS_PAGE_SIZE]; /* Pages 01h, 02h, 11h (bank 0). */
	uint8_t bank;                                   /* Currently selected bank. */
	uint8_t page;                                   /* Currently selected upper page. */
	bool yield_on_select;                           /* Test hook, see emul_cmis.h. */
};

static void emul_set_ascii(uint8_t *buf, size_t off, const char *str, size_t field_len)
{
	size_t len = strlen(str);

	for (size_t i = 0U; i < field_len; i++) {
		buf[off + i] = (i < len) ? (uint8_t)str[i] : (uint8_t)' ';
	}
}

static void emul_put_be16(uint8_t *page, uint8_t addr, uint16_t val)
{
	sys_put_be16(val, &page[CMIS_UPPER(addr)]);
}

/* Thresholds in Page 02h order: high alarm, low alarm, high warning, low warning. */
static void emul_put_thresholds(uint8_t *page, uint8_t addr, const uint16_t thr[4])
{
	for (uint8_t i = 0U; i < 4U; i++) {
		emul_put_be16(page, addr + (2U * i), thr[i]);
	}
}

static void emul_build_module(struct cmis_emul_data *data)
{
	static const uint16_t temp_thr[4] = {75U * 256U, (uint16_t)(-5 * 256), 70U * 256U, 0U};
	static const uint16_t vcc_thr[4] = {36300U, 29700U, 34650U, 31350U};
	static const uint16_t txp_thr[4] = {31623U, 2512U, 25119U, 3162U};
	static const uint16_t bias_thr[4] = {25000U, 2500U, 22500U, 3750U};
	static const uint16_t rxp_thr[4] = {31623U, 631U, 25119U, 1000U};
	uint8_t *m = data->mem;
	uint8_t *p01 = data->upper[EMUL_PAGE_01];
	uint8_t *p02 = data->upper[EMUL_PAGE_02];
	uint8_t *p11 = data->upper[EMUL_PAGE_11];
	bool yield = data->yield_on_select;

	(void)memset(data, 0, sizeof(*data));
	data->yield_on_select = yield;

	/* Lower Memory: a fictional CMIS 5.0 QSFP-DD 400G single-mode module. */
	m[CMIS_LOWER_IDENTIFIER] = CMIS_ID_QSFP_DD;
	m[CMIS_LOWER_REVISION] = 0x50U;
	m[CMIS_LOWER_MEMORY_MODEL] = 0x00U;
	m[CMIS_LOWER_MODULE_STATE] = CMIS_MODULE_STATE_READY << 1U;
	m[CMIS_EMUL_MODULE_STATE_FLAGS] = CMIS_EMUL_STATE_CHANGED; /* ModuleStateChanged */
	m[CMIS_LOWER_MODULE_FLAGS] = BIT(0) | BIT(4); /* temp high alarm, vcc high alarm */
	sys_put_be16(0x2D80U, &m[CMIS_LOWER_TEMPERATURE]); /* +45.5 C */
	sys_put_be16(33000U, &m[CMIS_LOWER_VCC]);          /* 3.300 V */
	m[CMIS_LOWER_MEDIA_TYPE] = CMIS_MEDIA_OPTICAL_SMF;

	/* Upper Page 00h: administrative information. */
	emul_set_ascii(m, CMIS_PAGE00_VENDOR_NAME, "ACME PHOTONICS", CMIS_VENDOR_NAME_LEN);
	m[CMIS_PAGE00_VENDOR_OUI + 0U] = 0x00U;
	m[CMIS_PAGE00_VENDOR_OUI + 1U] = 0x90U;
	m[CMIS_PAGE00_VENDOR_OUI + 2U] = 0x65U;
	emul_set_ascii(m, CMIS_PAGE00_VENDOR_PN, "QSFP-DD-400G", CMIS_VENDOR_PN_LEN);
	emul_set_ascii(m, CMIS_PAGE00_VENDOR_REV, "B2", CMIS_VENDOR_REV_LEN);
	emul_set_ascii(m, CMIS_PAGE00_VENDOR_SN, "QD2400001234", CMIS_VENDOR_SN_LEN);
	emul_set_ascii(m, CMIS_PAGE00_DATE_CODE, "240620", CMIS_DATE_CODE_LEN);
	m[CMIS_PAGE00_POWER_CLASS] = 7U << 5U; /* power class 8 (> 5 W) */
	m[CMIS_PAGE00_MAX_POWER] = 56U;        /* 56 * 0.25 W == 14 W */

	/* Page 01h: Tx bias scaling factor x2. */
	p01[CMIS_UPPER(CMIS_PAGE01_LANE_MON_ADV)] = 0x01U << 3;

	/* Page 02h: monitor thresholds. */
	emul_put_thresholds(p02, CMIS_PAGE02_TEMP, temp_thr);
	emul_put_thresholds(p02, CMIS_PAGE02_VCC, vcc_thr);
	emul_put_thresholds(p02, CMIS_PAGE02_TX_POWER, txp_thr);
	emul_put_thresholds(p02, CMIS_PAGE02_TX_BIAS, bias_thr);
	emul_put_thresholds(p02, CMIS_PAGE02_RX_POWER, rxp_thr);

	/* Page 11h: a 400G-DR4 data path on host lanes 1-8, media lanes 1-4. */
	(void)memset(&p11[CMIS_UPPER(CMIS_PAGE11_DP_STATE)], 0x44, 4U);
	for (uint8_t lane = 0U; lane < 4U; lane++) {
		emul_put_be16(p11, CMIS_PAGE11_TX_POWER + (2U * lane), 12589U); /* 1258.9 uW */
		emul_put_be16(p11, CMIS_PAGE11_TX_BIAS + (2U * lane), 14000U);  /* 56 mA */
		emul_put_be16(p11, CMIS_PAGE11_RX_POWER + (2U * lane), 5012U);  /* 501.2 uW */
	}
	/* Media lane 3 has lost its input: Rx LOS, Rx CDR LOL, Rx power low alarm. */
	emul_put_be16(p11, CMIS_PAGE11_RX_POWER + 4U, 10U);
	p11[CMIS_UPPER(147U)] = BIT(2);
	p11[CMIS_UPPER(148U)] = BIT(2);
	p11[CMIS_UPPER(150U)] = BIT(2);
}

static uint8_t *emul_upper(struct cmis_emul_data *data)
{
	if (data->page == 0x00U) {
		return &data->mem[CMIS_PAGE_SIZE];
	}
	if (data->page == 0x01U) {
		return data->upper[EMUL_PAGE_01];
	}
	if (data->page == 0x02U) {
		return data->upper[EMUL_PAGE_02];
	}
	if ((data->page == 0x11U) && (data->bank == 0U)) {
		return data->upper[EMUL_PAGE_11];
	}

	return NULL;
}

static bool emul_is_latched(const struct cmis_emul_data *data, size_t addr)
{
	if (addr < CMIS_PAGE_SIZE) {
		return (addr >= CMIS_LATCHED_FIRST) && (addr <= CMIS_LATCHED_LAST);
	}

	return (data->page == 0x11U) && (addr >= CMIS_PAGE11_FLAGS_FIRST) &&
	       (addr <= CMIS_PAGE11_FLAGS_LAST);
}

/* Only the bank- and page-select registers are writable. */
static int emul_write(struct cmis_emul_data *data, const struct i2c_msg *msg)
{
	if (msg->len < 2U) {
		return -EIO;
	}

	for (size_t i = 1U; i < msg->len; i++) {
		size_t addr = (size_t)msg->buf[0] + i - 1U;

		if (addr == CMIS_REG_BANK_SELECT) {
			data->bank = msg->buf[i];
		} else if (addr == CMIS_REG_PAGE_SELECT) {
			data->page = msg->buf[i];
		} else {
			return -EIO;
		}
	}

	if (data->yield_on_select) {
		k_yield();
	}

	return 0;
}

static int cmis_emul_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
			      int addr)
{
	struct cmis_emul_data *data = target->data;
	uint8_t *upper;
	uint8_t offset;

	ARG_UNUSED(addr);

	/* A lone write is a register write: a bank and/or page select. */
	if (num_msgs == 1) {
		if ((msgs[0].flags & I2C_MSG_READ) != 0U) {
			return -EIO;
		}
		return emul_write(data, &msgs[0]);
	}

	/* Otherwise model an EEPROM read: a one-byte offset write, then a read. */
	if (num_msgs != 2) {
		return -EIO;
	}
	if (((msgs[0].flags & I2C_MSG_READ) != 0U) || (msgs[0].len != 1U)) {
		return -EIO;
	}
	if ((msgs[1].flags & I2C_MSG_READ) == 0U) {
		return -EIO;
	}

	offset = msgs[0].buf[0];
	if (((size_t)offset + msgs[1].len) > CMIS_PAGE_00H_SIZE) {
		return -EIO;
	}

	upper = emul_upper(data);
	for (size_t i = 0U; i < msgs[1].len; i++) {
		size_t a = (size_t)offset + i;
		uint8_t *cell = NULL;

		if (a < CMIS_PAGE_SIZE) {
			cell = &data->mem[a];
		} else if (upper != NULL) {
			cell = &upper[a - CMIS_PAGE_SIZE];
		}

		msgs[1].buf[i] = (cell != NULL) ? *cell : 0U;
		if ((cell != NULL) && emul_is_latched(data, a)) {
			*cell = 0U; /* clear on read */
		}
	}

	return 0;
}

void cmis_emul_reset(const struct emul *target)
{
	emul_build_module(target->data);
}

void cmis_emul_set_yield_on_select(const struct emul *target, bool enable)
{
	struct cmis_emul_data *data = target->data;

	data->yield_on_select = enable;
}

static const struct i2c_emul_api cmis_emul_api = {
	.transfer = cmis_emul_transfer,
};

static int cmis_emul_init(const struct emul *target, const struct device *parent)
{
	struct cmis_emul_data *data = target->data;

	ARG_UNUSED(parent);

	emul_build_module(data);

	return 0;
}

#define CMIS_EMUL(n)                                                                               \
	static struct cmis_emul_data cmis_emul_data_##n;                                           \
	EMUL_DT_INST_DEFINE(n, cmis_emul_init, &cmis_emul_data_##n, NULL, &cmis_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(CMIS_EMUL)

/* The module node has no driver of its own: the sff,cmis cage driver reads it. */
DT_INST_FOREACH_STATUS_OKAY(EMUL_STUB_DEVICE)
