/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for CMIS pluggable module drivers
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_CMIS_H_
#define ZEPHYR_INCLUDE_DRIVERS_CMIS_H_

#include <errno.h>

#include <zephyr/cmis/cmis.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Interfaces for CMIS managed pluggable transceivers.
 * @defgroup cmis_interface CMIS pluggable module
 * @since 4.6
 * @version 0.1.0
 * @ingroup io_interfaces
 * @{
 */

/** @cond INTERNAL_HIDDEN */
__subsystem struct cmis_driver_api {
	int (*read_id)(const struct device *dev, struct cmis_id *id);
	int (*read_monitors)(const struct device *dev, struct cmis_monitors *mon);
	int (*read_thresholds)(const struct device *dev, struct cmis_thresholds *thr);
	int (*read_lanes)(const struct device *dev, struct cmis_lane_status *lanes);
};
/** @endcond */

/**
 * @brief Read and decode the module identification (Page 00h).
 *
 * Selects Page 00h. The latched flags in Lower Memory bytes 8..11 are not
 * read, so this call does not clear them.
 *
 * @param dev CMIS cage device.
 * @param id Destination for the decoded identification data.
 *
 * @retval 0 Success.
 * @retval -ENODEV The cage reports that no module is inserted.
 * @retval -ENOSYS The driver does not implement this call.
 * @retval -errno Negative errno from the presence input or the bus transfer.
 */
__syscall int cmis_read_id(const struct device *dev, struct cmis_id *id);

static inline int z_impl_cmis_read_id(const struct device *dev, struct cmis_id *id)
{
	const struct cmis_driver_api *api = (const struct cmis_driver_api *)dev->api;

	if (api->read_id == NULL) {
		return -ENOSYS;
	}

	return api->read_id(dev, id);
}

/**
 * @brief Read and decode the module-level monitors (Lower Memory).
 *
 * The temperature and supply-voltage flags (Lower Memory byte 9) are latched
 * in the module and cleared by this read.
 * Other latched flags (bytes 8, 10 and 11) are not read and stay pending.
 *
 * @param dev CMIS cage device.
 * @param mon Destination for the decoded monitors.
 *
 * @retval 0 Success.
 * @retval -ENODEV The cage reports that no module is inserted.
 * @retval -ENOSYS The driver does not implement this call.
 * @retval -errno Negative errno from the presence input or the bus transfer.
 */
__syscall int cmis_read_monitors(const struct device *dev, struct cmis_monitors *mon);

static inline int z_impl_cmis_read_monitors(const struct device *dev, struct cmis_monitors *mon)
{
	const struct cmis_driver_api *api = (const struct cmis_driver_api *)dev->api;

	if (api->read_monitors == NULL) {
		return -ENOSYS;
	}

	return api->read_monitors(dev, mon);
}

/**
 * @brief Read and decode the monitor thresholds (Page 02h).
 *
 * Selects Page 01h for the Tx bias scaling factor, then Page 02h.
 *
 * @param dev CMIS cage device.
 * @param thr Destination for the decoded thresholds.
 *
 * @retval 0 Success.
 * @retval -ENODEV The cage reports that no module is inserted.
 * @retval -ENOSYS The driver does not implement this call.
 * @retval -ENOTSUP The module has flat memory (no Page 02h) or advertises a
 *                  reserved Tx bias scaling factor.
 * @retval -errno Negative errno from the presence input or the bus transfer.
 */
__syscall int cmis_read_thresholds(const struct device *dev, struct cmis_thresholds *thr);

static inline int z_impl_cmis_read_thresholds(const struct device *dev,
					      struct cmis_thresholds *thr)
{
	const struct cmis_driver_api *api = (const struct cmis_driver_api *)dev->api;

	if (api->read_thresholds == NULL) {
		return -ENOSYS;
	}

	return api->read_thresholds(dev, thr);
}

/**
 * @brief Read and decode the status of lanes 1-8 (Page 11h, bank 0).
 *
 * Reports the data path state, the Tx power, Tx bias and Rx power monitors and
 * the latched lane flags of each lane. The module clears the lane flags (Page
 * 11h bytes 134..152) when they are read, so they are reported by one call
 * only.
 *
 * @param dev CMIS cage device.
 * @param lanes Destination array of @ref CMIS_LANES_PER_BANK entries.
 *
 * @retval 0 Success.
 * @retval -ENODEV The cage reports that no module is inserted.
 * @retval -ENOSYS The driver does not implement this call.
 * @retval -ENOTSUP The module has flat memory (no Page 11h) or advertises a
 *                  reserved Tx bias scaling factor.
 * @retval -errno Negative errno from the presence input or the bus transfer.
 */
__syscall int cmis_read_lanes(const struct device *dev, struct cmis_lane_status *lanes);

static inline int z_impl_cmis_read_lanes(const struct device *dev,
					 struct cmis_lane_status *lanes)
{
	const struct cmis_driver_api *api = (const struct cmis_driver_api *)dev->api;

	if (api->read_lanes == NULL) {
		return -ENOSYS;
	}

	return api->read_lanes(dev, lanes);
}

/** @} */

#ifdef __cplusplus
}
#endif

#include <zephyr/syscalls/cmis.h>

#endif /* ZEPHYR_INCLUDE_DRIVERS_CMIS_H_ */
