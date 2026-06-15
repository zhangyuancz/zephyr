/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_RELAY_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_RELAY_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GPIO relay / contactor with weld detection.
 *
 * Controls a multi-channel relay built from a master enable gate and one
 * control line per channel, with break-before-make sequencing. A feedback
 * input reports whether the relay output is live, which is used to detect a
 * welded contact (output live while the relay is commanded open).
 *
 * There is no generic relay subsystem in Zephyr, so these device-specific
 * functions form the public API.
 * @defgroup relay_interface Relay
 * @{
 */

/**
 * @brief Weld-fault callback.
 *
 * @param dev Relay device.
 * @param welded True when a welded contact is detected (output live while
 *               commanded open), false when the condition clears.
 * @param user_data User pointer passed to relay_set_weld_handler().
 */
typedef void (*relay_weld_handler_t)(const struct device *dev, bool welded, void *user_data);

/**
 * @brief Set which relay channels are closed.
 *
 * Performs break-before-make: all channels and the enable gate are dropped,
 * the configured settle time elapses, then the selected channels are driven and
 * the enable gate is asserted.
 *
 * @param dev Relay device.
 * @param channel_mask Bitmask of channels to close (bit i = channel i). Zero
 *                     opens the relay.
 *
 * @retval 0 on success.
 * @retval -EINVAL if the mask references a non-existent channel.
 * @retval other negative errno on a GPIO error.
 */
int relay_set(const struct device *dev, uint32_t channel_mask);

/**
 * @brief Get the currently commanded channel mask.
 *
 * @param dev Relay device.
 * @return The last mask passed to relay_set().
 */
uint32_t relay_get(const struct device *dev);

/**
 * @brief Query the latest weld-fault state.
 *
 * @param dev Relay device.
 * @return True if a welded contact is currently detected.
 */
bool relay_weld_fault(const struct device *dev);

/**
 * @brief Register a callback invoked when the weld-fault state changes.
 *
 * @param dev Relay device.
 * @param handler Callback, or NULL to unregister.
 * @param user_data User pointer passed back to the callback.
 *
 * @retval 0 on success.
 */
int relay_set_weld_handler(const struct device *dev, relay_weld_handler_t handler,
			   void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_RELAY_H_ */
