/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT maxlinear_sp706

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wdt_maxlinear_sp706, CONFIG_WDT_LOG_LEVEL);

struct sp706_config {
	struct gpio_dt_spec wdi_gpio;
	uint32_t timeout;
};

static int sp706_init(const struct device *dev)
{
	const struct sp706_config *config = dev->config;

	if (!gpio_is_ready_dt(&config->wdi_gpio)) {
		LOG_ERR("WDI GPIO not ready");
		return -ENODEV;
	}

	/* Tri-state WDI until the application enables the watchdog. */
	return gpio_pin_configure_dt(&config->wdi_gpio, GPIO_INPUT);
}

static int sp706_setup(const struct device *dev, uint8_t options)
{
	const struct sp706_config *config = dev->config;

	ARG_UNUSED(options);
	return gpio_pin_configure_dt(&config->wdi_gpio, GPIO_OUTPUT_INACTIVE);
}

static int sp706_disable(const struct device *dev)
{
	const struct sp706_config *config = dev->config;

	return gpio_pin_configure_dt(&config->wdi_gpio, GPIO_INPUT);
}

static int sp706_install_timeout(const struct device *dev,
				 const struct wdt_timeout_cfg *cfg)
{
	const struct sp706_config *config = dev->config;

	if (cfg->window.min != 0U || cfg->window.max != config->timeout) {
		return -EINVAL;
	}
	if (cfg->callback != NULL) {
		return -ENOTSUP;
	}
	if ((cfg->flags & WDT_FLAG_RESET_MASK) != WDT_FLAG_RESET_SOC) {
		return -ENOTSUP;
	}

	return 0;
}

static int sp706_feed(const struct device *dev, int channel_id)
{
	const struct sp706_config *config = dev->config;

	if (channel_id != 0) {
		return -EINVAL;
	}

	return gpio_pin_toggle_dt(&config->wdi_gpio);
}

static DEVICE_API(wdt, sp706_api) = {
	.setup = sp706_setup,
	.disable = sp706_disable,
	.install_timeout = sp706_install_timeout,
	.feed = sp706_feed,
};

#define SP706_DEFINE(inst)                                                                          \
	static const struct sp706_config sp706_config_##inst = {                                     \
		.wdi_gpio = GPIO_DT_SPEC_INST_GET(inst, wdi_gpios),                                  \
		.timeout = DT_INST_PROP(inst, timeout_period),                                       \
	};                                                                                             \
	DEVICE_DT_INST_DEFINE(inst, sp706_init, NULL, NULL, &sp706_config_##inst, POST_KERNEL,         \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &sp706_api);

DT_INST_FOREACH_STATUS_OKAY(SP706_DEFINE)
