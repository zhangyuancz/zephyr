/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_CONTROL_PILOT_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_CONTROL_PILOT_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GB/T 18487.1 control-pilot (CP) interface.
 *
 * Drives the CP PWM, measures the CP positive-level voltage (hardware-triggered
 * ADC) to classify the GB/T 18487.1 state, reads the diode-check input, and reads
 * back the generated PWM via capture. There is no generic EVSE subsystem in
 * Zephyr, so these device-specific functions form the public API.
 * @defgroup control_pilot_interface Control pilot
 * @{
 */

/** GB/T 18487.1-2023 control-pilot state at detection point 1. */
enum cp_state {
	CP_STATE_0,       /**< 0 V: CP fault; power transfer is prohibited. */
	CP_STATE_1,       /**< +12 V without PWM: vehicle not connected. */
	CP_STATE_1_PRIME, /**< +12 V with PWM. */
	CP_STATE_2,       /**< +9 V without PWM: vehicle connected. */
	CP_STATE_2_PRIME, /**< +9 V with PWM: supply equipment ready. */
	CP_STATE_3,       /**< +6 V without PWM: vehicle ready. */
	CP_STATE_3_PRIME, /**< +6 V with PWM: both sides ready. */
	CP_STATE_4,       /**< -12 V: supply equipment unavailable. */
	CP_STATE_INVALID, /**< Voltage is in a prohibited or fault range. */
	CP_STATE_UNKNOWN,
};

/** Latest control-pilot measurements and classified state. */
struct cp_status {
	enum cp_state state;        /**< Classified GB/T 18487.1 state. */
	int32_t voltage_mv;         /**< Latest CP positive-level voltage. */
	bool diode_present;         /**< Vehicle diode detected. */
	uint16_t duty_permille;     /**< Commanded PWM duty (per mille). */
	bool feedback_valid;        /**< PWM capture feedback is valid. */
	uint32_t feedback_hz;       /**< Measured frequency if feedback_valid is true. */
	uint16_t feedback_permille; /**< Measured duty if feedback_valid is true. */
};

/**
 * @brief Set the control-pilot PWM duty cycle.
 *
 * The duty advertises the available current to the vehicle. Only GB/T
 * 18487.1-defined outputs are accepted: 0%, 5%, 10% through 90%, and 100%.
 * Changing the duty invalidates measurements made under the previous waveform;
 * cp_read() returns -EAGAIN until the next ADC interrupt publishes a new value.
 *
 * @param dev CP device.
 * @param permille Duty cycle in per mille (0..1000).
 *
 * @retval 0 on success, -EINVAL if prohibited by the standard, other negative
 *         errno on error.
 */
int cp_set_duty(const struct device *dev, uint16_t permille);

/**
 * @brief Acquire the current control-pilot status.
 *
 * ADC and PWM capture run continuously in interrupt context. This call copies
 * the latest complete measurements and never starts or waits for a conversion.
 * PWM feedback fields are optional; check cp_status::feedback_valid before use.
 *
 * @param dev CP device.
 * @param status Output status.
 *
 * @retval 0 on success.
 * @retval -EAGAIN The latest ADC sample is unavailable or older than 5 ms.
 * @retval -errno A GPIO or other status acquisition error.
 */
int cp_read(const struct device *dev, struct cp_status *status);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_CONTROL_PILOT_H_ */
