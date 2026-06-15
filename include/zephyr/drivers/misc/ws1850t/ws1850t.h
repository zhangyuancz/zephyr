/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_WS1850T_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_WS1850T_H_

#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiseSun WS1850T contactless card reader (MFRC522-compatible).
 *
 * The driver owns the UART register interface and the reset GPIO and performs
 * the antenna/timer initialisation during init. There is no generic RFID
 * subsystem in Zephyr, so these device-specific functions form the public API.
 * @defgroup ws1850t_interface WS1850T
 * @{
 */

/**
 * @brief Read the firmware/version register.
 *
 * @param dev WS1850T device.
 * @param version Filled with the VERSION register value.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int ws1850t_get_version(const struct device *dev, uint8_t *version);

/**
 * @brief Select an ISO 14443-A card and read its ATQA and 4-byte UID.
 *
 * Performs a REQA followed by cascade level 1 anti-collision and validates the
 * UID BCC.
 *
 * @param dev WS1850T device.
 * @param atqa Filled with the 2-byte ATQA response.
 * @param uid Filled with the 4-byte UID (cascade level 1).
 *
 * @retval 0 if a card was selected and its UID read.
 * @retval -EAGAIN if no card is present in the field.
 * @retval -EBADMSG on an unexpected response or BCC mismatch.
 * @retval other negative errno on a communication error.
 */
int ws1850t_read_card(const struct device *dev, uint8_t atqa[2], uint8_t uid[4]);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_WS1850T_H_ */
