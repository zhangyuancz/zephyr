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
 * @brief IEC 61851 control-pilot (CP) interface.
 *
 * Drives the CP PWM, measures the CP positive-peak voltage (hardware-triggered
 * ADC) to classify the IEC 61851 state, reads the diode-check input, and reads
 * back the generated PWM via capture. There is no generic EVSE subsystem in
 * Zephyr, so these device-specific functions form the public API.
 * @defgroup control_pilot_interface Control pilot
 * @{
 */

/** IEC 61851 control-pilot state (from the CP positive-peak voltage). */
enum cp_state {
	CP_STATE_A,	/**< +12 V: vehicle not connected. */
	CP_STATE_B,	/**< +9 V: vehicle connected, not ready. */
	CP_STATE_C,	/**< +6 V: vehicle ready, charging. */
	CP_STATE_D,	/**< +3 V: vehicle ready, ventilation required. */
	CP_STATE_E,	/**< 0 V: no power / error. */
	CP_STATE_UNKNOWN,
};

/** Snapshot of the control-pilot state. */
struct cp_status {
	enum cp_state state;		/**< Classified IEC 61851 state. */
	int32_t voltage_mv;		/**< CP positive-peak voltage. */
	bool diode_present;		/**< Vehicle diode detected. */
	uint16_t duty_permille;		/**< Commanded PWM duty (per mille). */
	bool feedback_valid;		/**< PWM capture feedback is valid. */
	uint32_t feedback_hz;		/**< Measured PWM frequency. */
	uint16_t feedback_permille;	/**< Measured PWM duty (per mille). */
};

/**
 * @brief Set the control-pilot PWM duty cycle.
 *
 * The duty advertises the available current to the vehicle (IEC 61851).
 *
 * @param dev CP device.
 * @param permille Duty cycle in per mille (0..1000).
 *
 * @retval 0 on success, -EINVAL if out of range, other negative errno on error.
 */
int cp_set_duty(const struct device *dev, uint16_t permille);

/**
 * @brief Get the last commanded PWM duty cycle (per mille).
 */
uint16_t cp_get_duty(const struct device *dev);

/**
 * @brief Sample the control pilot and fill a status snapshot.
 *
 * @param dev CP device.
 * @param status Output snapshot.
 *
 * @retval 0 on success, negative errno on a measurement error.
 */
int cp_read(const struct device *dev, struct cp_status *status);

/**
 * @brief Return a short string for a CP state ("A".."E", "?").
 */
const char *cp_state_str(enum cp_state state);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_CONTROL_PILOT_H_ */
