/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/cmis.h>
#include <zephyr/internal/syscall_handler.h>

static inline int z_vrfy_cmis_read_id(const struct device *dev, struct cmis_id *id)
{
	K_OOPS(K_SYSCALL_DRIVER_CMIS(dev, read_id));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(id, sizeof(*id)));

	return z_impl_cmis_read_id(dev, id);
}
#include <zephyr/syscalls/cmis_read_id_mrsh.c>

static inline int z_vrfy_cmis_read_monitors(const struct device *dev, struct cmis_monitors *mon)
{
	K_OOPS(K_SYSCALL_DRIVER_CMIS(dev, read_monitors));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(mon, sizeof(*mon)));

	return z_impl_cmis_read_monitors(dev, mon);
}
#include <zephyr/syscalls/cmis_read_monitors_mrsh.c>

static inline int z_vrfy_cmis_read_thresholds(const struct device *dev,
					      struct cmis_thresholds *thr)
{
	K_OOPS(K_SYSCALL_DRIVER_CMIS(dev, read_thresholds));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(thr, sizeof(*thr)));

	return z_impl_cmis_read_thresholds(dev, thr);
}
#include <zephyr/syscalls/cmis_read_thresholds_mrsh.c>

static inline int z_vrfy_cmis_read_lanes(const struct device *dev,
					 struct cmis_lane_status *lanes)
{
	K_OOPS(K_SYSCALL_DRIVER_CMIS(dev, read_lanes));
	K_OOPS(K_SYSCALL_MEMORY_ARRAY_WRITE(lanes, CMIS_LANES_PER_BANK, sizeof(*lanes)));

	return z_impl_cmis_read_lanes(dev, lanes);
}
#include <zephyr/syscalls/cmis_read_lanes_mrsh.c>
