/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT sff_cmis

#include <errno.h>
#include <string.h>

#include <zephyr/cmis/cmis.h>
#include <zephyr/device.h>
#include <zephyr/drivers/cmis.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

/* CMIS modules answer at a fixed management address. */
#define CMIS_MODULE_ADDR 0x50U

/* CMIS bank- and page-select registers in Lower Memory. */
#define CMIS_REG_BANK_SELECT 0x7EU
#define CMIS_REG_PAGE_SELECT 0x7FU

/* Pages 10h-1Fh are banked; this driver reads bank 0 (lanes 1-8). */
#define CMIS_PAGE_BANKED_FIRST 0x10U

/*
 * Lower Memory bytes 8..11 hold latched flags that the module clears when
 * they are read. Reads below skip them unless the caller receives them, so
 * flags such as ModuleStateChanged (byte 8) stay pending for the host's
 * interrupt handling.
 */
#define CMIS_LOWER_STATUS_LEN 8U /* bytes 0..7: identifier .. module state */
#define CMIS_MONITOR_LEN      4U /* bytes 14..17: temperature and Vcc */

struct cmis_module_config {
	const struct device *i2c;
	struct gpio_dt_spec modprsl; /* optional module presence input */
};

struct cmis_module_data {
	/* The page select and the reads that follow it must not interleave
	 * with another thread's access to the same module.
	 */
	struct k_mutex lock;
};

static int cmis_module_check_present(const struct device *dev)
{
	const struct cmis_module_config *cfg = dev->config;
	int ret;

	if (cfg->modprsl.port == NULL) {
		return 0;
	}

	ret = gpio_pin_get_dt(&cfg->modprsl);
	if (ret < 0) {
		return ret;
	}

	return (ret == 1) ? 0 : -ENODEV;
}

static int cmis_module_select_page(const struct device *dev, uint8_t page)
{
	const struct cmis_module_config *cfg = dev->config;
	uint8_t banked[3] = {CMIS_REG_BANK_SELECT, 0U, page};
	uint8_t paged[2] = {CMIS_REG_PAGE_SELECT, page};

	if (page >= CMIS_PAGE_BANKED_FIRST) {
		/* Bank and page in one write, so the pair takes effect together. */
		return i2c_write(cfg->i2c, banked, sizeof(banked), CMIS_MODULE_ADDR);
	}

	return i2c_write(cfg->i2c, paged, sizeof(paged), CMIS_MODULE_ADDR);
}

static int cmis_module_read(const struct device *dev, uint8_t offset, uint8_t *buf, size_t len)
{
	const struct cmis_module_config *cfg = dev->config;

	return i2c_write_read(cfg->i2c, CMIS_MODULE_ADDR, &offset, sizeof(offset), buf, len);
}

static int cmis_module_read_page00(const struct device *dev, uint8_t *page00)
{
	int ret;

	/* Page 00h is the reset default; select it so a stale page cannot leak. */
	ret = cmis_module_select_page(dev, 0U);
	if (ret != 0) {
		return ret;
	}

	ret = cmis_module_read(dev, 0U, &page00[0], CMIS_LOWER_STATUS_LEN);
	if (ret != 0) {
		return ret;
	}

	ret = cmis_module_read(dev, CMIS_LOWER_MEDIA_TYPE, &page00[CMIS_LOWER_MEDIA_TYPE], 1U);
	if (ret != 0) {
		return ret;
	}

	return cmis_module_read(dev, CMIS_PAGE_SIZE, &page00[CMIS_PAGE_SIZE], CMIS_PAGE_SIZE);
}

static int cmis_module_read_id(const struct device *dev, struct cmis_id *id)
{
	struct cmis_module_data *data = dev->data;
	uint8_t page00[CMIS_PAGE_00H_SIZE] = {0};
	int ret;

	ret = cmis_module_check_present(dev);
	if (ret != 0) {
		return ret;
	}

	(void)k_mutex_lock(&data->lock, K_FOREVER);
	ret = cmis_module_read_page00(dev, page00);
	(void)k_mutex_unlock(&data->lock);
	if (ret != 0) {
		return ret;
	}

	return cmis_parse_id(page00, id);
}

/* Paged content beyond Page 00h exists only on modules with paged memory. */
static int cmis_module_check_paged(const struct device *dev)
{
	uint8_t model;
	int ret;

	ret = cmis_module_read(dev, CMIS_LOWER_MEMORY_MODEL, &model, 1U);
	if (ret != 0) {
		return ret;
	}

	return ((model & CMIS_MEMORY_MODEL_FLAT_MEM) != 0U) ? -ENOTSUP : 0;
}

/* Select @p page and read its 128 upper bytes. The caller holds the lock. */
static int cmis_module_read_upper(const struct device *dev, uint8_t page, uint8_t *buf)
{
	int ret;

	ret = cmis_module_select_page(dev, page);
	if (ret != 0) {
		return ret;
	}

	return cmis_module_read(dev, CMIS_PAGE_SIZE, buf, CMIS_PAGE_SIZE);
}

/* Tx bias scaling factor from Page 01h. The caller holds the lock. */
static int cmis_module_read_bias_multiplier(const struct device *dev, uint8_t *multiplier)
{
	uint8_t page01[CMIS_PAGE_SIZE] = {0};
	int ret;

	ret = cmis_module_select_page(dev, 0x01U);
	if (ret != 0) {
		return ret;
	}

	ret = cmis_module_read(dev, CMIS_PAGE01_LANE_MON_ADV,
			       &page01[CMIS_UPPER(CMIS_PAGE01_LANE_MON_ADV)], 1U);
	if (ret != 0) {
		return ret;
	}

	/* A reserved scaling code would make every bias value wrong. */
	ret = cmis_parse_tx_bias_multiplier(page01, multiplier);

	return (ret == -EINVAL) ? -ENOTSUP : ret;
}

static int cmis_module_read_paged(const struct device *dev, uint8_t page, uint8_t *buf,
				  uint8_t *multiplier)
{
	struct cmis_module_data *data = dev->data;
	int ret;

	ret = cmis_module_check_present(dev);
	if (ret != 0) {
		return ret;
	}

	(void)k_mutex_lock(&data->lock, K_FOREVER);

	ret = cmis_module_check_paged(dev);
	if (ret == 0) {
		ret = cmis_module_read_bias_multiplier(dev, multiplier);
	}
	if (ret == 0) {
		ret = cmis_module_read_upper(dev, page, buf);
	}

	(void)k_mutex_unlock(&data->lock);

	return ret;
}

static int cmis_module_read_thresholds(const struct device *dev, struct cmis_thresholds *thr)
{
	uint8_t page02[CMIS_PAGE_SIZE];
	uint8_t multiplier;
	int ret;

	ret = cmis_module_read_paged(dev, 0x02U, page02, &multiplier);
	if (ret != 0) {
		return ret;
	}

	return cmis_parse_thresholds(page02, multiplier, thr);
}

static int cmis_module_read_lanes(const struct device *dev, struct cmis_lane_status *lanes)
{
	uint8_t page11[CMIS_PAGE_SIZE];
	uint8_t multiplier;
	int ret;

	/* One transfer, so states, latched flags and monitors are coherent. */
	ret = cmis_module_read_paged(dev, 0x11U, page11, &multiplier);
	if (ret != 0) {
		return ret;
	}

	return cmis_parse_lane_status(page11, multiplier, lanes);
}

static int cmis_module_read_monitors(const struct device *dev, struct cmis_monitors *mon)
{
	struct cmis_module_data *data = dev->data;
	uint8_t lower[CMIS_LOWER_VCC + 2U] = {0};
	int ret;

	ret = cmis_module_check_present(dev);
	if (ret != 0) {
		return ret;
	}

	(void)k_mutex_lock(&data->lock, K_FOREVER);

	/* Byte 9 is the only latched flag byte reported through this API. */
	ret = cmis_module_read(dev, CMIS_LOWER_MODULE_FLAGS, &lower[CMIS_LOWER_MODULE_FLAGS], 1U);
	if (ret == 0) {
		/* Temperature and Vcc in one transfer, so the pair is coherent. */
		ret = cmis_module_read(dev, CMIS_LOWER_TEMPERATURE, &lower[CMIS_LOWER_TEMPERATURE],
				       CMIS_MONITOR_LEN);
	}

	(void)k_mutex_unlock(&data->lock);
	if (ret != 0) {
		return ret;
	}

	return cmis_parse_monitors(lower, mon);
}

static int cmis_module_init(const struct device *dev)
{
	const struct cmis_module_config *cfg = dev->config;
	struct cmis_module_data *data = dev->data;

	k_mutex_init(&data->lock);

	if (!device_is_ready(cfg->i2c)) {
		return -ENODEV;
	}

	if (cfg->modprsl.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->modprsl)) {
			return -ENODEV;
		}

		return gpio_pin_configure_dt(&cfg->modprsl, GPIO_INPUT);
	}

	return 0;
}

static DEVICE_API(cmis, cmis_module_api) = {
	.read_id = cmis_module_read_id,
	.read_monitors = cmis_module_read_monitors,
	.read_thresholds = cmis_module_read_thresholds,
	.read_lanes = cmis_module_read_lanes,
};

#define CMIS_MODULE_INIT(n)                                                                        \
	static struct cmis_module_data cmis_module_data_##n;                                       \
	static const struct cmis_module_config cmis_module_config_##n = {                          \
		.i2c = DEVICE_DT_GET(DT_INST_PHANDLE(n, i2c_bus)),                                 \
		.modprsl = GPIO_DT_SPEC_INST_GET_OR(n, modprsl_gpios, {0}),                        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, cmis_module_init, NULL, &cmis_module_data_##n,                    \
			      &cmis_module_config_##n, POST_KERNEL, CONFIG_SFP_INIT_PRIORITY,      \
			      &cmis_module_api);

DT_INST_FOREACH_STATUS_OKAY(CMIS_MODULE_INIT)
