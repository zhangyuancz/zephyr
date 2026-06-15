/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO relay / contactor with weld (output-feedback) detection.
 */

#define DT_DRV_COMPAT zephyr_gpio_relay

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/misc/relay/relay.h>

LOG_MODULE_REGISTER(relay, CONFIG_RELAY_LOG_LEVEL);

/* Time for the contacts to physically open and the feedback to settle before a
 * commanded-open state is judged for a welded contact.
 */
#define RELAY_WELD_SETTLE	K_MSEC(50)
#define RELAY_WELD_DEBOUNCE	K_MSEC(20)

struct relay_config {
	struct gpio_dt_spec enable;
	const struct gpio_dt_spec *controls;
	uint8_t num_controls;
	struct gpio_dt_spec weld;
	uint32_t settle_ms;
};

struct relay_data {
	const struct device *dev;
	struct k_mutex lock;
	uint32_t mask;
	bool weld_fault;
	struct gpio_callback weld_cb;
	struct k_work_delayable weld_work;
	relay_weld_handler_t handler;
	void *user_data;
};

static void relay_weld_eval(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct relay_data *data = CONTAINER_OF(dwork, struct relay_data, weld_work);
	const struct device *dev = data->dev;
	const struct relay_config *cfg = dev->config;
	relay_weld_handler_t handler;
	void *user_data;
	bool welded;
	int live;

	live = gpio_pin_get_dt(&cfg->weld);
	if (live < 0) {
		LOG_ERR("weld feedback read failed: %d", live);
		return;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	/* Welded: the output is live while the relay is commanded fully open. */
	welded = (data->mask == 0U) && (live != 0);
	if (welded == data->weld_fault) {
		k_mutex_unlock(&data->lock);
		return;
	}
	data->weld_fault = welded;
	handler = data->handler;
	user_data = data->user_data;
	k_mutex_unlock(&data->lock);

	if (welded) {
		LOG_WRN("welded contact detected (output live while open)");
	}
	if (handler != NULL) {
		handler(dev, welded, user_data);
	}
}

static void relay_weld_isr(const struct device *port, struct gpio_callback *cb,
			   gpio_port_pins_t pins)
{
	struct relay_data *data = CONTAINER_OF(cb, struct relay_data, weld_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	(void)k_work_reschedule(&data->weld_work, RELAY_WELD_DEBOUNCE);
}

int relay_set(const struct device *dev, uint32_t channel_mask)
{
	const struct relay_config *cfg = dev->config;
	struct relay_data *data = dev->data;
	int ret;

	if (cfg->num_controls < 32U && (channel_mask >> cfg->num_controls) != 0U) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Break: drop the enable gate and every channel. */
	ret = gpio_pin_set_dt(&cfg->enable, 0);
	for (uint8_t i = 0U; i < cfg->num_controls; i++) {
		ret |= gpio_pin_set_dt(&cfg->controls[i], 0);
	}
	if (ret < 0) {
		goto out;
	}

	k_sleep(K_MSEC(cfg->settle_ms));

	/* Make: drive the selected channels, then assert the enable gate. */
	if (channel_mask != 0U) {
		for (uint8_t i = 0U; i < cfg->num_controls; i++) {
			ret |= gpio_pin_set_dt(&cfg->controls[i], (channel_mask >> i) & 1U);
		}
		ret |= gpio_pin_set_dt(&cfg->enable, 1);
		if (ret < 0) {
			goto out;
		}
	}

	data->mask = channel_mask;

out:
	k_mutex_unlock(&data->lock);
	if (ret == 0) {
		(void)k_work_reschedule(&data->weld_work, RELAY_WELD_SETTLE);
	}
	return ret;
}

uint32_t relay_get(const struct device *dev)
{
	struct relay_data *data = dev->data;

	return data->mask;
}

bool relay_weld_fault(const struct device *dev)
{
	struct relay_data *data = dev->data;

	return data->weld_fault;
}

int relay_set_weld_handler(const struct device *dev, relay_weld_handler_t handler,
			   void *user_data)
{
	struct relay_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->handler = handler;
	data->user_data = user_data;
	k_mutex_unlock(&data->lock);

	return 0;
}

static int relay_init(const struct device *dev)
{
	const struct relay_config *cfg = dev->config;
	struct relay_data *data = dev->data;
	int live;
	int ret;

	data->dev = dev;
	k_mutex_init(&data->lock);
	k_work_init_delayable(&data->weld_work, relay_weld_eval);

	if (!gpio_is_ready_dt(&cfg->enable) || !gpio_is_ready_dt(&cfg->weld)) {
		LOG_ERR("GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	for (uint8_t i = 0U; i < cfg->num_controls; i++) {
		if (!gpio_is_ready_dt(&cfg->controls[i])) {
			LOG_ERR("control GPIO %u not ready", i);
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->controls[i], GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}
	}

	ret = gpio_pin_configure_dt(&cfg->weld, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	gpio_init_callback(&data->weld_cb, relay_weld_isr, BIT(cfg->weld.pin));
	ret = gpio_add_callback_dt(&cfg->weld, &data->weld_cb);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->weld, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		return ret;
	}

	live = gpio_pin_get_dt(&cfg->weld);
	if (live < 0) {
		return live;
	}
	data->weld_fault = (live != 0);

	return 0;
}

#define RELAY_CONTROL_SPEC(idx, inst) GPIO_DT_SPEC_INST_GET_BY_IDX(inst, control_gpios, idx)

#define RELAY_INIT(inst)								\
	static const struct gpio_dt_spec relay_controls_##inst[] = {			\
		LISTIFY(DT_INST_PROP_LEN(inst, control_gpios),				\
			RELAY_CONTROL_SPEC, (,), inst)					\
	};										\
											\
	static struct relay_data relay_data_##inst;					\
											\
	static const struct relay_config relay_config_##inst = {			\
		.enable = GPIO_DT_SPEC_INST_GET(inst, enable_gpios),			\
		.controls = relay_controls_##inst,					\
		.num_controls = ARRAY_SIZE(relay_controls_##inst),			\
		.weld = GPIO_DT_SPEC_INST_GET(inst, weld_gpios),			\
		.settle_ms = DT_INST_PROP(inst, settle_time_ms),			\
	};										\
											\
	DEVICE_DT_INST_DEFINE(inst, relay_init, NULL,					\
			      &relay_data_##inst, &relay_config_##inst,			\
			      POST_KERNEL, CONFIG_RELAY_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(RELAY_INIT)
