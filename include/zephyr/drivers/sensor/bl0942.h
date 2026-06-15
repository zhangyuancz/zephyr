/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_BL0942_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_BL0942_H_

#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

/** BL0942 specific sensor channels. */
enum sensor_channel_bl0942 {
	/**
	 * Active energy pulse count (CF_CNT register, raw counts). Each pulse
	 * corresponds to a fixed amount of active energy; see the datasheet to
	 * convert to watt-hours for a given shunt and divider.
	 */
	SENSOR_CHAN_BL0942_CF_CNT = SENSOR_CHAN_PRIV_START,
};

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_SENSOR_BL0942_H_ */
