/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * GB/T 18487.1 control-pilot (CP) driver: PWM generation, hardware-triggered ADC
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

#define CP_SAMPLE_MAX_AGE_MS 5U

#define CP_STATE_1_MIN_MV 11000
#define CP_STATE_2_MAX_MV 10000
#define CP_STATE_2_MIN_MV 8000
#define CP_STATE_3_MAX_MV 7000
#define CP_STATE_3_MIN_MV 4000
#define CP_STATE_0_MAX_MV 1000
#define CP_VOLTAGE_MAX_MV 13000

struct cp_config {
	struct pwm_dt_spec pwm;
	struct pwm_dt_spec feedback;
	struct adc_dt_spec adc;
	struct gpio_dt_spec diode;
	bool has_feedback;
	uint32_t adc_reference_mv;
	uint32_t divider_upper_ohms;
	uint32_t divider_lower_ohms;
	uint16_t default_duty;
};

struct cp_data {
	struct k_mutex lock;
	struct k_spinlock sample_lock;
	uint16_t duty_permille;
	enum cp_state stable_state;
	uint16_t adc_dma_raw;
	uint16_t adc_raw;
	uint32_t adc_timestamp;
	bool adc_valid;
	uint32_t feedback_period;
	uint32_t feedback_pulse;
	uint32_t feedback_timestamp;
	uint64_t feedback_cycles_per_sec;
	bool feedback_valid;
};

static enum cp_state cp_base_state(enum cp_state state)
{
	switch (state) {
	case CP_STATE_1:
	case CP_STATE_1_PRIME:
		return CP_STATE_1;
	case CP_STATE_2:
	case CP_STATE_2_PRIME:
		return CP_STATE_2;
	case CP_STATE_3:
	case CP_STATE_3_PRIME:
		return CP_STATE_3;
	default:
		return state;
	}
}

static enum cp_state cp_apply_pwm(enum cp_state state, bool pwm_active)
{
	if (!pwm_active) {
		return state;
	}

	switch (state) {
	case CP_STATE_1:
		return CP_STATE_1_PRIME;
	case CP_STATE_2:
		return CP_STATE_2_PRIME;
	case CP_STATE_3:
		return CP_STATE_3_PRIME;
	default:
		return state;
	}
}

static enum cp_state cp_classify(int32_t cp_mv, uint16_t duty_permille, enum cp_state previous)
{
	bool pwm_active = duty_permille > 0U && duty_permille < 1000U;
	enum cp_state previous_base = cp_base_state(previous);
	enum cp_state state;

	if (cp_mv > CP_VOLTAGE_MAX_MV || cp_mv < 0) {
		return CP_STATE_INVALID;
	}
	if (cp_mv >= CP_STATE_1_MIN_MV) {
		state = CP_STATE_1;
	} else if (cp_mv > CP_STATE_2_MAX_MV) {
		/* 10 V..11 V: retain state 1 or 2 only when history resolves it. */
		if (previous_base != CP_STATE_1 && previous_base != CP_STATE_2) {
			return CP_STATE_UNKNOWN;
		}
		state = previous_base;
	} else if (cp_mv >= CP_STATE_2_MIN_MV) {
		state = CP_STATE_2;
	} else if (cp_mv > CP_STATE_3_MAX_MV) {
		/* 7 V..8 V: retain state 2 or 3 only when history resolves it. */
		if (previous_base != CP_STATE_2 && previous_base != CP_STATE_3) {
			return CP_STATE_UNKNOWN;
		}
		state = previous_base;
	} else if (cp_mv >= CP_STATE_3_MIN_MV) {
		state = CP_STATE_3;
	} else if (cp_mv <= CP_STATE_0_MAX_MV) {
		/* The unipolar feedback path confirms state 4 from the output command. */
		if (duty_permille == 0U) {
			return CP_STATE_4;
		}
		return pwm_active ? CP_STATE_INVALID : CP_STATE_0;
	} else {
		return CP_STATE_INVALID;
	}

	return cp_apply_pwm(state, pwm_active);
}

int cp_set_duty(const struct device *dev, uint16_t permille)
{
	const struct cp_config *cfg = dev->config;
	struct cp_data *data = dev->data;
	uint32_t pulse;
	int ret;

	if (permille > 1000U || (permille != 0U && permille != 50U && permille < 100U) ||
	    (permille > 900U && permille != 1000U)) {
		return -EINVAL;
	}

	pulse = (uint32_t)(((uint64_t)cfg->pwm.period * permille) / 1000U);

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = pwm_set_dt(&cfg->pwm, cfg->pwm.period, pulse);
	if (ret == 0) {
		k_spinlock_key_t key = k_spin_lock(&data->sample_lock);

		data->duty_permille = permille;
		data->adc_valid = false;
		data->feedback_valid = false;
		k_spin_unlock(&data->sample_lock, key);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static enum adc_action cp_adc_callback(const struct device *adc_dev,
				       const struct adc_sequence *sequence,
				       uint16_t sampling_index)
{
	struct cp_data *data = sequence->options->user_data;
	k_spinlock_key_t key;

	ARG_UNUSED(adc_dev);
	ARG_UNUSED(sampling_index);

	key = k_spin_lock(&data->sample_lock);
	data->adc_raw = data->adc_dma_raw;
	data->adc_timestamp = k_uptime_get_32();
	data->adc_valid = true;
	k_spin_unlock(&data->sample_lock, key);

	return ADC_ACTION_REPEAT;
}

#ifdef CONFIG_PWM_CAPTURE
static void cp_feedback_callback(const struct device *pwm_dev, uint32_t channel,
				 uint32_t period, uint32_t pulse, int status,
				 void *user_data)
{
	struct cp_data *data = user_data;
	k_spinlock_key_t key;

	ARG_UNUSED(pwm_dev);
	ARG_UNUSED(channel);

	key = k_spin_lock(&data->sample_lock);
	if (status == 0 && period > 0U && pulse <= period) {
		data->feedback_period = period;
		data->feedback_pulse = pulse;
		data->feedback_timestamp = k_uptime_get_32();
		data->feedback_valid = true;
	} else {
		data->feedback_valid = false;
	}
	k_spin_unlock(&data->sample_lock, key);
}
#endif /* CONFIG_PWM_CAPTURE */

int cp_read(const struct device *dev, struct cp_status *status)
{
	const struct cp_config *cfg = dev->config;
	struct cp_data *data = dev->data;
	uint32_t max_raw = BIT(cfg->adc.resolution) - 1U;
	uint32_t now;
	uint32_t feedback_period;
	uint32_t feedback_pulse;
	uint64_t feedback_cycles_per_sec;
	uint16_t adc_raw;
	bool adc_valid;
	bool feedback_valid;
	k_spinlock_key_t key;
	int diode;

	k_mutex_lock(&data->lock, K_FOREVER);

	now = k_uptime_get_32();
	key = k_spin_lock(&data->sample_lock);
	status->duty_permille = data->duty_permille;
	adc_raw = data->adc_raw;
	adc_valid = data->adc_valid &&
		    (now - data->adc_timestamp <= CP_SAMPLE_MAX_AGE_MS);
	feedback_period = data->feedback_period;
	feedback_pulse = data->feedback_pulse;
	feedback_cycles_per_sec = data->feedback_cycles_per_sec;
	feedback_valid = data->feedback_valid &&
			 (now - data->feedback_timestamp <= CP_SAMPLE_MAX_AGE_MS);
	k_spin_unlock(&data->sample_lock, key);

	if (!adc_valid) {
		k_mutex_unlock(&data->lock);
		return -EAGAIN;
	}

	status->voltage_mv =
		(int32_t)(((uint64_t)adc_raw * cfg->adc_reference_mv *
			   ((uint64_t)cfg->divider_upper_ohms + cfg->divider_lower_ohms)) /
			  ((uint64_t)max_raw * cfg->divider_lower_ohms));
	status->state = cp_classify(status->voltage_mv, status->duty_permille, data->stable_state);
	if (cp_base_state(status->state) == CP_STATE_1 ||
	    cp_base_state(status->state) == CP_STATE_2 ||
	    cp_base_state(status->state) == CP_STATE_3) {
		data->stable_state = status->state;
	}

	diode = gpio_pin_get_dt(&cfg->diode);
	if (diode < 0) {
		k_mutex_unlock(&data->lock);
		return diode;
	}
	status->diode_present = (diode > 0);

	status->feedback_valid = feedback_valid && status->duty_permille > 0U &&
				 status->duty_permille < 1000U;
	if (status->feedback_valid) {
		status->feedback_hz = (uint32_t)(feedback_cycles_per_sec / feedback_period);
		status->feedback_permille =
			(uint16_t)(((uint64_t)feedback_pulse * 1000U) / feedback_period);
	} else {
		status->feedback_hz = 0U;
		status->feedback_permille = 0U;
	}

	k_mutex_unlock(&data->lock);

	return 0;
}

static int cp_init(const struct device *dev)
{
	const struct cp_config *cfg = dev->config;
	struct cp_data *data = dev->data;
	struct adc_sequence_options options = {
		.callback = cp_adc_callback,
		.user_data = data,
	};
	struct adc_sequence sequence = {
		.options = &options,
		.buffer = &data->adc_dma_raw,
		.buffer_size = sizeof(data->adc_dma_raw),
	};
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

	data->stable_state = CP_STATE_UNKNOWN;
	data->adc_valid = false;
	data->feedback_valid = false;

	ret = cp_set_duty(dev, cfg->default_duty);
	if (ret < 0) {
		return ret;
	}

	adc_sequence_init_dt(&cfg->adc, &sequence);
	ret = adc_read_async(cfg->adc.dev, &sequence, NULL);
	if (ret < 0) {
		LOG_ERR("continuous ADC sampling failed: %d", ret);
		return ret;
	}

#ifdef CONFIG_PWM_CAPTURE
	if (cfg->has_feedback) {
		ret = pwm_get_cycles_per_sec(cfg->feedback.dev, cfg->feedback.channel,
					     &data->feedback_cycles_per_sec);
		if (ret < 0) {
			return ret;
		}

		ret = pwm_configure_capture(cfg->feedback.dev, cfg->feedback.channel,
					    PWM_CAPTURE_TYPE_BOTH | PWM_CAPTURE_MODE_CONTINUOUS,
					    cp_feedback_callback, data);
		if (ret < 0) {
			return ret;
		}

		ret = pwm_enable_capture(cfg->feedback.dev, cfg->feedback.channel);
		if (ret < 0) {
			return ret;
		}
	}
#endif

	return 0;
}

#define CP_INIT(inst)									\
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, pwms) >= 1,					\
		     "control-pilot requires at least the cp PWM");			\
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, sense_divider_resistors_ohms) == 2,		\
		     "sense-divider-resistors-ohms must contain upper and lower values");	\
	BUILD_ASSERT(DT_INST_PROP_BY_IDX(inst, sense_divider_resistors_ohms, 0) > 0 &&	\
		     DT_INST_PROP_BY_IDX(inst, sense_divider_resistors_ohms, 1) > 0,	\
		     "sense-divider resistor values must be non-zero");			\
										\
	static struct cp_data cp_data_##inst;						\
										\
	static const struct cp_config cp_config_##inst = {				\
		.pwm = PWM_DT_SPEC_INST_GET_BY_NAME(inst, cp),				\
		.feedback = PWM_DT_SPEC_INST_GET_BY_NAME_OR(inst, feedback, {0}),	\
		.has_feedback = (DT_INST_PROP_LEN(inst, pwms) > 1),			\
		.adc = ADC_DT_SPEC_INST_GET_BY_IDX(inst, 0),				\
		.diode = GPIO_DT_SPEC_INST_GET(inst, diode_gpios),			\
		.adc_reference_mv = DT_INST_PROP(inst, adc_reference_millivolt),		\
		.divider_upper_ohms =						\
			DT_INST_PROP_BY_IDX(inst, sense_divider_resistors_ohms, 0),	\
		.divider_lower_ohms =						\
			DT_INST_PROP_BY_IDX(inst, sense_divider_resistors_ohms, 1),	\
		.default_duty = DT_INST_PROP(inst, default_duty_permille),		\
	};										\
										\
	DEVICE_DT_INST_DEFINE(inst, cp_init, NULL,					\
			      &cp_data_##inst, &cp_config_##inst,			\
			      POST_KERNEL, CONFIG_CONTROL_PILOT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(CP_INIT)
