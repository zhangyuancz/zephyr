/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * IEC 61851 control-pilot (CP) driver: PWM generation, hardware-triggered ADC
 * voltage measurement with state classification, diode detection and PWM
 * capture feedback.
 */

#define DT_DRV_COMPAT zephyr_control_pilot

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/misc/control_pilot/control_pilot.h>

LOG_MODULE_REGISTER(control_pilot, CONFIG_CONTROL_PILOT_LOG_LEVEL);

#define CP_SAMPLE_COUNT 32U
#define CP_FEEDBACK_TIMEOUT K_MSEC(5)

/* IEC 61851 CP voltage state thresholds (midpoints between the 12/9/6/3/0 V
 * nominal levels), in millivolts.
 */
#define CP_THRESHOLD_AB_MV 10500
#define CP_THRESHOLD_BC_MV 7500
#define CP_THRESHOLD_CD_MV 4500
#define CP_THRESHOLD_DE_MV 1500

struct cp_config {
	struct pwm_dt_spec pwm;
	struct pwm_dt_spec feedback;
	struct adc_dt_spec adc;
	struct gpio_dt_spec diode;
	bool has_feedback;
	uint32_t full_scale_mv;
	uint16_t default_duty;
};

struct cp_data {
	struct k_mutex lock;
	uint16_t duty_permille;
	uint16_t samples[CP_SAMPLE_COUNT];
};

const char *cp_state_str(enum cp_state state)
{
	static const char *const names[] = {"A", "B", "C", "D", "E", "?"};

	return names[MIN((size_t)state, ARRAY_SIZE(names) - 1U)];
}

static enum cp_state cp_classify(int32_t cp_mv)
{
	if (cp_mv >= CP_THRESHOLD_AB_MV) {
		return CP_STATE_A;
	}
	if (cp_mv >= CP_THRESHOLD_BC_MV) {
		return CP_STATE_B;
	}
	if (cp_mv >= CP_THRESHOLD_CD_MV) {
		return CP_STATE_C;
	}
	if (cp_mv >= CP_THRESHOLD_DE_MV) {
		return CP_STATE_D;
	}

	return CP_STATE_E;
}

int cp_set_duty(const struct device *dev, uint16_t permille)
{
	const struct cp_config *cfg = dev->config;
	struct cp_data *data = dev->data;
	uint32_t pulse;
	int ret;

	if (permille > 1000U) {
		return -EINVAL;
	}

	pulse = (uint32_t)(((uint64_t)cfg->pwm.period * permille) / 1000U);

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = pwm_set_dt(&cfg->pwm, cfg->pwm.period, pulse);
	if (ret == 0) {
		data->duty_permille = permille;
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

uint16_t cp_get_duty(const struct device *dev)
{
	struct cp_data *data = dev->data;

	return data->duty_permille;
}

static int cp_measure_peak(const struct device *dev, uint16_t *peak_raw)
{
	const struct cp_config *cfg = dev->config;
	struct cp_data *data = dev->data;
	struct adc_sequence_options options = {
		.extra_samplings = CP_SAMPLE_COUNT - 1U,
	};
	struct adc_sequence sequence = {
		.options = &options,
		.buffer = data->samples,
		.buffer_size = sizeof(data->samples),
	};
	uint16_t peak = 0U;
	int ret;

	adc_sequence_init_dt(&cfg->adc, &sequence);
	ret = adc_read(cfg->adc.dev, &sequence);
	if (ret < 0) {
		return ret;
	}

	for (size_t i = 0U; i < CP_SAMPLE_COUNT; i++) {
		peak = MAX(peak, data->samples[i]);
	}
	*peak_raw = peak;

	return 0;
}

static bool cp_feedback_read(const struct device *dev, uint32_t *freq_hz, uint16_t *permille)
{
	const struct cp_config *cfg = dev->config;
	struct cp_data *data = dev->data;

	if (!cfg->has_feedback || !IS_ENABLED(CONFIG_PWM_CAPTURE)) {
		return false;
	}

	/*
	 * Only meaningful while the CP is actually oscillating. At duty 0% or 100%
	 * the output is DC (no edges), so a capture would just block until it times
	 * out. Skip the feedback read in those states.
	 */
	if (data->duty_permille == 0U || data->duty_permille >= 1000U) {
		return false;
	}

#ifdef CONFIG_PWM_CAPTURE
	uint32_t period;
	uint32_t pulse;
	uint64_t cycles_per_sec;
	int ret;

	ret = pwm_capture_cycles(cfg->feedback.dev, cfg->feedback.channel,
				 PWM_CAPTURE_TYPE_BOTH, &period, &pulse, CP_FEEDBACK_TIMEOUT);
	if (ret < 0 || period == 0U || pulse > period) {
		return false;
	}

	ret = pwm_get_cycles_per_sec(cfg->feedback.dev, cfg->feedback.channel, &cycles_per_sec);
	if (ret < 0) {
		return false;
	}

	*freq_hz = (uint32_t)(cycles_per_sec / period);
	*permille = (uint16_t)(((uint64_t)pulse * 1000U) / period);

	return true;
#endif /* CONFIG_PWM_CAPTURE */
}

int cp_read(const struct device *dev, struct cp_status *status)
{
	const struct cp_config *cfg = dev->config;
	struct cp_data *data = dev->data;
	uint32_t max_raw = BIT(cfg->adc.resolution) - 1U;
	uint16_t peak_raw;
	int diode;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	status->duty_permille = data->duty_permille;

	ret = cp_measure_peak(dev, &peak_raw);
	if (ret < 0) {
		k_mutex_unlock(&data->lock);
		return ret;
	}

	status->voltage_mv = (int32_t)(((uint64_t)peak_raw * cfg->full_scale_mv) / max_raw);
	status->state = cp_classify(status->voltage_mv);

	diode = gpio_pin_get_dt(&cfg->diode);
	status->diode_present = (diode > 0);

	status->feedback_valid = cp_feedback_read(dev, &status->feedback_hz,
						  &status->feedback_permille);

	k_mutex_unlock(&data->lock);

	return 0;
}

static int cp_init(const struct device *dev)
{
	const struct cp_config *cfg = dev->config;
	struct cp_data *data = dev->data;
	int ret;

	k_mutex_init(&data->lock);

	if (!pwm_is_ready_dt(&cfg->pwm) || !adc_is_ready_dt(&cfg->adc) ||
	    !gpio_is_ready_dt(&cfg->diode)) {
		LOG_ERR("PWM, ADC or diode GPIO not ready");
		return -ENODEV;
	}

	if (cfg->has_feedback && !pwm_is_ready_dt(&cfg->feedback)) {
		LOG_ERR("feedback PWM not ready");
		return -ENODEV;
	}

	ret = adc_channel_setup_dt(&cfg->adc);
	if (ret < 0) {
		LOG_ERR("ADC channel setup failed: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&cfg->diode, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	return cp_set_duty(dev, cfg->default_duty);
}

#define CP_INIT(inst)									\
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, pwms) >= 1,					\
		     "control-pilot requires at least the cp PWM");			\
											\
	static struct cp_data cp_data_##inst;						\
											\
	static const struct cp_config cp_config_##inst = {				\
		.pwm = PWM_DT_SPEC_INST_GET_BY_NAME(inst, cp),				\
		.feedback = PWM_DT_SPEC_INST_GET_BY_NAME_OR(inst, feedback, {0}),	\
		.has_feedback = (DT_INST_PROP_LEN(inst, pwms) > 1),			\
		.adc = ADC_DT_SPEC_INST_GET_BY_IDX(inst, 0),				\
		.diode = GPIO_DT_SPEC_INST_GET(inst, diode_gpios),			\
		.full_scale_mv = DT_INST_PROP(inst, full_scale_millivolt),		\
		.default_duty = DT_INST_PROP(inst, default_duty_permille),		\
	};										\
											\
	DEVICE_DT_INST_DEFINE(inst, cp_init, NULL,					\
			      &cp_data_##inst, &cp_config_##inst,			\
			      POST_KERNEL, CONFIG_CONTROL_PILOT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(CP_INIT)
