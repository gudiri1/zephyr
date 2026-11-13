/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_SFP_EMUL_CMIS_H_
#define ZEPHYR_DRIVERS_SFP_EMUL_CMIS_H_

#include <stdbool.h>

#include <zephyr/drivers/emul.h>
#include <zephyr/sys/util.h>

/** Lower Memory byte 8: latched module flags. */
#define CMIS_EMUL_MODULE_STATE_FLAGS 8U
/** Byte 8 bit 0: ModuleStateChanged, set in the emulated power-up image. */
#define CMIS_EMUL_STATE_CHANGED      BIT(0)

/** Restore the emulated module, including its latched flags. */
void cmis_emul_reset(const struct emul *target);

/**
 * Yield the calling thread after every bank- or page-select write, so a test
 * can interleave two threads between a page select and the read that depends
 * on it.
 */
void cmis_emul_set_yield_on_select(const struct emul *target, bool enable);

#endif /* ZEPHYR_DRIVERS_SFP_EMUL_CMIS_H_ */
