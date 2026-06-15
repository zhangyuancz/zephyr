/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_MC001_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_MC001_H_

#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MC001 specific sensor channels. */
enum sensor_channel_mc001 {
	/** Residual-current trip state: 1 when tripped, 0 when normal. */
	SENSOR_CHAN_MC001_TRIP = SENSOR_CHAN_PRIV_START,
};

/** MC001 specific sensor attributes. */
enum sensor_attribute_mc001 {
	/**
	 * Run the zero-current calibration sequence. Write to channel
	 * SENSOR_CHAN_MC001_TRIP with any value; the residual current must be
	 * zero (no load) while this runs.
	 */
	SENSOR_ATTR_MC001_ZERO_CAL = SENSOR_ATTR_PRIV_START,
	/**
	 * Run the module self-test: inject a simulated residual current and
	 * verify the TRIP output asserts. attr_set returns 0 if the device
	 * tripped as expected, -EIO otherwise, or -ENOTSUP if no self-test GPIO
	 * is wired. Requires the residual current to be zero while it runs.
	 */
	SENSOR_ATTR_MC001_SELF_TEST,
};

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_SENSOR_MC001_H_ */
