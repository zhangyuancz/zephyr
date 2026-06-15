/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mega-senway MastCurr MC001 residual-current detection module.
 * Datasheet: MC001-006-M1J1 v0.2.1.
 */

#define DT_DRV_COMPAT megasenway_mc001

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/sensor/mc001.h>

LOG_MODULE_REGISTER(mc001, CONFIG_SENSOR_LOG_LEVEL);

/* Power-on / zero-calibration timing (datasheet, in-use sequence). */
#define MC001_POWER_ON_SETTLE	K_MSEC(100) /* T1 >= 100 ms */
#define MC001_ZERO_CAL_PULSE	K_MSEC(75)  /* 50 ms <= T2 <= 100 ms */
#define MC001_ZERO_CAL_SETTLE	K_MSEC(500) /* T3 >= 500 ms */
#define MC001_TRIP_DEBOUNCE	K_MSEC(5)

/* Self-test: assert TEST and expect TRIP to follow (datasheet T4/T5/T6). */
#define MC001_SELF_TEST_POLL	K_MSEC(10)
#define MC001_SELF_TEST_TRIES	50	/* up to ~500 ms for TRIP to assert */
#define MC001_SELF_TEST_HOLD	K_MSEC(100) /* keep TEST asserted (T4) */
#define MC001_SELF_TEST_RECOVER	K_MSEC(200) /* let TRIP recover (T6) */

struct mc001_config {
	struct gpio_dt_spec trip;
	struct gpio_dt_spec zero_cal;
	struct gpio_dt_spec self_test;
};

struct mc001_data {
	const struct device *dev;
	struct gpio_callback trip_cb;
	struct k_work_delayable debounce_work;
	bool tripped;

	const struct sensor_trigger *trigger;
	sensor_trigger_handler_t handler;
};

static int mc001_zero_calibrate(const struct device *dev)
{
	const struct mc001_config *cfg = dev->config;
	int ret;

	ret = gpio_pin_set_dt(&cfg->zero_cal, 1);
	if (ret < 0) {
		return ret;
	}
	k_sleep(MC001_ZERO_CAL_PULSE);

	ret = gpio_pin_set_dt(&cfg->zero_cal, 0);
	if (ret < 0) {
		return ret;
	}
	k_sleep(MC001_ZERO_CAL_SETTLE);

	return 0;
}

static int mc001_self_test(const struct device *dev)
{
	const struct mc001_config *cfg = dev->config;
	bool tripped = false;
	int ret;

	if (cfg->self_test.port == NULL) {
		return -ENOTSUP;
	}

	/* Inject the simulated residual current. */
	ret = gpio_pin_set_dt(&cfg->self_test, 1);
	if (ret < 0) {
		return ret;
	}

	/* Wait for TRIP to assert in response. */
	for (int i = 0; i < MC001_SELF_TEST_TRIES; i++) {
		k_sleep(MC001_SELF_TEST_POLL);
		ret = gpio_pin_get_dt(&cfg->trip);
		if (ret < 0) {
			goto release;
		}
		if (ret > 0) {
			tripped = true;
			break;
		}
	}

	k_sleep(MC001_SELF_TEST_HOLD);
	ret = tripped ? 0 : -EIO;

release:
	(void)gpio_pin_set_dt(&cfg->self_test, 0);
	k_sleep(MC001_SELF_TEST_RECOVER);
	return ret;
}

static void mc001_debounce_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct mc001_data *data = CONTAINER_OF(dwork, struct mc001_data, debounce_work);
	const struct device *dev = data->dev;
	const struct mc001_config *cfg = dev->config;
	int value;

	value = gpio_pin_get_dt(&cfg->trip);
	if (value < 0) {
		LOG_ERR("trip read failed: %d", value);
		return;
	}

	if ((value != 0) == data->tripped) {
		return;
	}

	data->tripped = value != 0;
	LOG_DBG("trip %s", data->tripped ? "active" : "normal");

	if (data->handler != NULL) {
		data->handler(dev, data->trigger);
	}
}

static void mc001_trip_isr(const struct device *port, struct gpio_callback *cb,
			   gpio_port_pins_t pins)
{
	struct mc001_data *data = CONTAINER_OF(cb, struct mc001_data, trip_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	(void)k_work_reschedule(&data->debounce_work, MC001_TRIP_DEBOUNCE);
}

static int mc001_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct mc001_config *cfg = dev->config;
	struct mc001_data *data = dev->data;
	int value;

	if (chan != SENSOR_CHAN_ALL && (int)chan != SENSOR_CHAN_MC001_TRIP) {
		return -ENOTSUP;
	}

	value = gpio_pin_get_dt(&cfg->trip);
	if (value < 0) {
		return value;
	}

	data->tripped = value != 0;
	return 0;
}

static int mc001_channel_get(const struct device *dev, enum sensor_channel chan,
			     struct sensor_value *val)
{
	struct mc001_data *data = dev->data;

	if ((int)chan != SENSOR_CHAN_MC001_TRIP) {
		return -ENOTSUP;
	}

	val->val1 = data->tripped ? 1 : 0;
	val->val2 = 0;
	return 0;
}

static int mc001_attr_set(const struct device *dev, enum sensor_channel chan,
			  enum sensor_attribute attr, const struct sensor_value *val)
{
	ARG_UNUSED(val);

	if ((int)chan != SENSOR_CHAN_MC001_TRIP) {
		return -ENOTSUP;
	}

	switch ((int)attr) {
	case SENSOR_ATTR_MC001_ZERO_CAL:
		return mc001_zero_calibrate(dev);
	case SENSOR_ATTR_MC001_SELF_TEST:
		return mc001_self_test(dev);
	default:
		return -ENOTSUP;
	}
}

static int mc001_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
			     sensor_trigger_handler_t handler)
{
	struct mc001_data *data = dev->data;

	if (trig->type != SENSOR_TRIG_THRESHOLD ||
	    (int)trig->chan != SENSOR_CHAN_MC001_TRIP) {
		return -ENOTSUP;
	}

	data->trigger = trig;
	data->handler = handler;
	return 0;
}

static DEVICE_API(sensor, mc001_api) = {
	.sample_fetch = mc001_sample_fetch,
	.channel_get = mc001_channel_get,
	.attr_set = mc001_attr_set,
	.trigger_set = mc001_trigger_set,
};

static int mc001_init(const struct device *dev)
{
	const struct mc001_config *cfg = dev->config;
	struct mc001_data *data = dev->data;
	int value;
	int ret;

	data->dev = dev;

	if (!gpio_is_ready_dt(&cfg->trip) || !gpio_is_ready_dt(&cfg->zero_cal)) {
		LOG_ERR("GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->zero_cal, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_configure_dt(&cfg->trip, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	if (cfg->self_test.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->self_test)) {
			LOG_ERR("self-test GPIO not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->self_test, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}
	}

	k_work_init_delayable(&data->debounce_work, mc001_debounce_work);

	/* Power-on zero-calibration sequence (residual current must be zero). */
	k_sleep(MC001_POWER_ON_SETTLE);
	ret = mc001_zero_calibrate(dev);
	if (ret < 0) {
		LOG_ERR("zero calibration failed: %d", ret);
		return ret;
	}

	gpio_init_callback(&data->trip_cb, mc001_trip_isr, BIT(cfg->trip.pin));
	ret = gpio_add_callback_dt(&cfg->trip, &data->trip_cb);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->trip, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		return ret;
	}

	value = gpio_pin_get_dt(&cfg->trip);
	if (value < 0) {
		return value;
	}
	data->tripped = value != 0;

	return 0;
}

#define MC001_INIT(inst)								\
	static struct mc001_data mc001_data_##inst;					\
											\
	static const struct mc001_config mc001_config_##inst = {			\
		.trip = GPIO_DT_SPEC_INST_GET(inst, trip_gpios),			\
		.zero_cal = GPIO_DT_SPEC_INST_GET(inst, zero_cal_gpios),		\
		.self_test = GPIO_DT_SPEC_INST_GET_OR(inst, self_test_gpios, {0}),	\
	};										\
											\
	SENSOR_DEVICE_DT_INST_DEFINE(inst, mc001_init, NULL,				\
				     &mc001_data_##inst, &mc001_config_##inst,		\
				     POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,		\
				     &mc001_api);

DT_INST_FOREACH_STATUS_OKAY(MC001_INIT)
