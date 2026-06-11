/*
 * Copyright (c) 2021 Teslabs Engineering S.L.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gd_gd32_pwm

#include <errno.h>

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util_macro.h>

#include <gd32_timer.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pwm_gd32, CONFIG_PWM_LOG_LEVEL);

/** PWM data. */
struct pwm_gd32_data {
	/** Timer clock (Hz). */
	uint32_t tim_clk;
#ifdef CONFIG_PWM_CAPTURE
	pwm_capture_callback_handler_t capture_callback;
	void *capture_user_data;
	pwm_flags_t capture_flags;
	bool capture_configured;
	bool capture_active;
	bool capture_synced;
#endif
};

/** PWM configuration. */
struct pwm_gd32_config {
	/** Timer register. */
	uint32_t reg;
	/** Number of channels */
	uint8_t channels;
	/** Flag to indicate if timer has 32-bit counter */
	bool is_32bit;
	/** Flag to indicate if timer is advanced */
	bool is_advanced;
	/** Prescaler. */
	uint16_t prescaler;
	/** Clock id. */
	uint16_t clkid;
	/** Reset. */
	struct reset_dt_spec reset;
	/** pinctrl configurations. */
	const struct pinctrl_dev_config *pcfg;
	uint8_t auxiliary_source_channel;
	uint8_t auxiliary_channel;
	uint16_t auxiliary_ratio_numerator;
	uint16_t auxiliary_ratio_denominator;
	bool has_auxiliary_channel;
#ifdef CONFIG_PWM_CAPTURE
	void (*irq_config_func)(const struct device *dev);
#endif
};

/** Obtain channel enable bit for the given channel */
#define TIMER_CHCTL2_CHXEN(ch) BIT(4U * (ch))
/** Obtain polarity bit for the given channel */
#define TIMER_CHCTL2_CHXP(ch) BIT(1U + (4U * (ch)))
/** Obtain CHCTL0/1 mask for the given channel (0 or 1) */
#define TIMER_CHCTLX_MSK(ch) (0xFU << (8U * (ch)))

/** Obtain RCU register offset from RCU clock value */
#define RCU_CLOCK_OFFSET(rcu_clock) ((rcu_clock) >> 6U)

static void pwm_gd32_set_compare(uint32_t reg, uint32_t channel, uint32_t value)
{
	switch (channel) {
	case 0U:
		TIMER_CH0CV(reg) = value;
		break;
	case 1U:
		TIMER_CH1CV(reg) = value;
		break;
	case 2U:
		TIMER_CH2CV(reg) = value;
		break;
	case 3U:
		TIMER_CH3CV(reg) = value;
		break;
	default:
		break;
	}
}

static void pwm_gd32_update_auxiliary(const struct pwm_gd32_config *config,
				      uint32_t channel, uint32_t pulse_cycles)
{
	volatile uint32_t *chctl;
	uint32_t auxiliary_channel;
	uint32_t auxiliary_pulse;

	if (!config->has_auxiliary_channel || channel != config->auxiliary_source_channel) {
		return;
	}

	auxiliary_channel = config->auxiliary_channel;
	auxiliary_pulse = (uint32_t)(((uint64_t)pulse_cycles *
				       config->auxiliary_ratio_numerator) /
				      config->auxiliary_ratio_denominator);
	pwm_gd32_set_compare(config->reg, auxiliary_channel, auxiliary_pulse);
	chctl = auxiliary_channel < 2U ? &TIMER_CHCTL0(config->reg) :
					       &TIMER_CHCTL1(config->reg);
	*chctl &= ~TIMER_CHCTLX_MSK(auxiliary_channel % 2U);
	*chctl |= (TIMER_OC_MODE_PWM0 | TIMER_OC_SHADOW_ENABLE) <<
		  (8U * (auxiliary_channel % 2U));
	TIMER_CHCTL2(config->reg) |= TIMER_CHCTL2_CHXEN(auxiliary_channel) |
				    TIMER_CHCTL2_CHXP(auxiliary_channel);
}

static int pwm_gd32_set_cycles(const struct device *dev, uint32_t channel,
			      uint32_t period_cycles, uint32_t pulse_cycles,
			      pwm_flags_t flags)
{
	const struct pwm_gd32_config *config = dev->config;

	if (channel >= config->channels) {
		return -EINVAL;
	}

	/* 16-bit timers can count up to UINT16_MAX */
	if (!config->is_32bit && (period_cycles > UINT16_MAX)) {
		return -ENOTSUP;
	}

	/* disable channel output if period is zero */
	if (period_cycles == 0U) {
		TIMER_CHCTL2(config->reg) &= ~TIMER_CHCTL2_CHXEN(channel);
		return 0;
	}

	/* update polarity */
	if ((flags & PWM_POLARITY_INVERTED) != 0U) {
		TIMER_CHCTL2(config->reg) |= TIMER_CHCTL2_CHXP(channel);
	} else {
		TIMER_CHCTL2(config->reg) &= ~TIMER_CHCTL2_CHXP(channel);
	}

	/* update pulse */
	pwm_gd32_set_compare(config->reg, channel, pulse_cycles);
	pwm_gd32_update_auxiliary(config, channel, pulse_cycles);

	/* update period */
	TIMER_CAR(config->reg) = period_cycles - 1U;

	/* channel not enabled: configure it */
	if ((TIMER_CHCTL2(config->reg) & TIMER_CHCTL2_CHXEN(channel)) == 0U) {
		volatile uint32_t *chctl;

		/* Select PWM0 so pulse_cycles is the active portion of the period. */
		if (channel < 2U) {
			chctl = &TIMER_CHCTL0(config->reg);
		} else {
			chctl = &TIMER_CHCTL1(config->reg);
		}

		*chctl &= ~TIMER_CHCTLX_MSK(channel % 2U);
		*chctl |= (TIMER_OC_MODE_PWM0 | TIMER_OC_SHADOW_ENABLE) <<
			  (8U * (channel % 2U));

		/* enable channel output */
		TIMER_CHCTL2(config->reg) |= TIMER_CHCTL2_CHXEN(channel);

		/* generate update event (to load shadow values) */
		TIMER_SWEVG(config->reg) |= TIMER_SWEVG_UPG;
	}

	return 0;
}

#ifdef CONFIG_PWM_CAPTURE
static void pwm_gd32_isr(const struct device *dev)
{
	const struct pwm_gd32_config *config = dev->config;
	struct pwm_gd32_data *data = dev->data;
	uint32_t period;
	uint32_t pulse;

	if (!data->capture_active || (TIMER_INTF(config->reg) & TIMER_INTF_CH0IF) == 0U) {
		return;
	}

	period = TIMER_CH0CV(config->reg) + 1U;
	pulse = TIMER_CH1CV(config->reg) + 1U;
	TIMER_INTF(config->reg) &= ~TIMER_INTF_CH0IF;

	/* The first rising edge only synchronizes restart mode. Its capture
	 * contains the time from enable to that edge, not a complete period.
	 */
	if (!data->capture_synced) {
		data->capture_synced = true;
		return;
	}

	if ((data->capture_flags & PWM_CAPTURE_MODE_MASK) == PWM_CAPTURE_MODE_SINGLE) {
		TIMER_DMAINTEN(config->reg) &= ~TIMER_DMAINTEN_CH0IE;
		data->capture_active = false;
	}

	data->capture_callback(dev, 0U, period, pulse, 0, data->capture_user_data);
}

static int pwm_gd32_configure_capture(const struct device *dev, uint32_t channel,
				      pwm_flags_t flags,
				      pwm_capture_callback_handler_t cb,
				      void *user_data)
{
	struct pwm_gd32_data *data = dev->data;

	if (channel != 0U || cb == NULL ||
	    (flags & PWM_CAPTURE_TYPE_MASK) != PWM_CAPTURE_TYPE_BOTH ||
	    (flags & PWM_POLARITY_INVERTED) != 0U) {
		return -ENOTSUP;
	}
	if (data->capture_active) {
		return -EBUSY;
	}

	data->capture_callback = cb;
	data->capture_user_data = user_data;
	data->capture_flags = flags;
	data->capture_configured = true;
	return 0;
}

static int pwm_gd32_enable_capture(const struct device *dev, uint32_t channel)
{
	const struct pwm_gd32_config *config = dev->config;
	struct pwm_gd32_data *data = dev->data;

	if (channel != 0U || !data->capture_configured) {
		return -EINVAL;
	}
	if (data->capture_active) {
		return -EBUSY;
	}

	TIMER_CTL0(config->reg) &= ~TIMER_CTL0_CEN;
	TIMER_CAR(config->reg) = config->is_32bit ? UINT32_MAX : UINT16_MAX;
	TIMER_CNT(config->reg) = 0U;
	TIMER_CHCTL0(config->reg) &=
		~(TIMER_CHCTL0_CH0MS | TIMER_CHCTL0_CH0CAPFLT | TIMER_CHCTL0_CH0CAPPSC |
		  TIMER_CHCTL0_CH1MS | TIMER_CHCTL0_CH1CAPFLT | TIMER_CHCTL0_CH1CAPPSC);
	TIMER_CHCTL0(config->reg) |= TIMER_IC_SELECTION_DIRECTTI |
				      ((uint32_t)TIMER_IC_SELECTION_INDIRECTTI << 8U);
	TIMER_CHCTL2(config->reg) &=
		~(TIMER_CHCTL2_CH0P | TIMER_CHCTL2_CH0NP |
		  TIMER_CHCTL2_CH1P | TIMER_CHCTL2_CH1NP);
	TIMER_CHCTL2(config->reg) |= TIMER_CHCTL2_CH0EN | TIMER_CHCTL2_CH1EN |
				      ((uint32_t)TIMER_IC_POLARITY_FALLING << 4U);
	TIMER_SMCFG(config->reg) &= ~(TIMER_SMCFG_TRGS | TIMER_SMCFG_SMC);
	TIMER_SMCFG(config->reg) |= TIMER_SMCFG_TRGSEL_CI0FE0 | TIMER_SLAVE_MODE_RESTART;
	TIMER_INTF(config->reg) &= ~TIMER_INTF_CH0IF;
	TIMER_DMAINTEN(config->reg) |= TIMER_DMAINTEN_CH0IE;
	data->capture_active = true;
	data->capture_synced = false;
	TIMER_CTL0(config->reg) |= TIMER_CTL0_CEN;
	return 0;
}

static int pwm_gd32_disable_capture(const struct device *dev, uint32_t channel)
{
	const struct pwm_gd32_config *config = dev->config;
	struct pwm_gd32_data *data = dev->data;

	if (channel != 0U) {
		return -EINVAL;
	}
	TIMER_DMAINTEN(config->reg) &= ~TIMER_DMAINTEN_CH0IE;
	data->capture_active = false;
	data->capture_synced = false;
	return 0;
}
#endif

static int pwm_gd32_get_cycles_per_sec(const struct device *dev,
				       uint32_t channel, uint64_t *cycles)
{
	struct pwm_gd32_data *data = dev->data;
	const struct pwm_gd32_config *config = dev->config;

	*cycles = (uint64_t)(data->tim_clk / (config->prescaler + 1U));

	return 0;
}

static DEVICE_API(pwm, pwm_gd32_driver_api) = {
	.set_cycles = pwm_gd32_set_cycles,
	.get_cycles_per_sec = pwm_gd32_get_cycles_per_sec,
#ifdef CONFIG_PWM_CAPTURE
	.configure_capture = pwm_gd32_configure_capture,
	.enable_capture = pwm_gd32_enable_capture,
	.disable_capture = pwm_gd32_disable_capture,
#endif
};

static int pwm_gd32_init(const struct device *dev)
{
	const struct pwm_gd32_config *config = dev->config;
	struct pwm_gd32_data *data = dev->data;
	int ret;

	if (config->has_auxiliary_channel &&
	    (config->auxiliary_source_channel >= config->channels ||
	     config->auxiliary_channel >= config->channels ||
	     config->auxiliary_source_channel == config->auxiliary_channel ||
	     config->auxiliary_ratio_denominator == 0U)) {
		return -EINVAL;
	}

	(void)clock_control_on(GD32_CLOCK_CONTROLLER,
			       (clock_control_subsys_t)&config->clkid);

	(void)reset_line_toggle_dt(&config->reset);

	/* apply pin configuration */
	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	/* cache timer clock value */
	(void)clock_control_get_rate(GD32_CLOCK_CONTROLLER,
				     (clock_control_subsys_t)&config->clkid,
				     &data->tim_clk);

	/* basic timer operation: edge aligned, up counting, shadowed CAR */
	TIMER_CTL0(config->reg) = TIMER_CKDIV_DIV1 | TIMER_COUNTER_EDGE |
				  TIMER_COUNTER_UP | TIMER_CTL0_ARSE;
	TIMER_PSC(config->reg) = config->prescaler;

	/* enable primary output for advanced timers */
	if (config->is_advanced) {
		TIMER_CCHP(config->reg) |= TIMER_CCHP_POEN;
	}

	/* enable timer counter */
	TIMER_CTL0(config->reg) |= TIMER_CTL0_CEN;

#ifdef CONFIG_PWM_CAPTURE
	config->irq_config_func(dev);
#endif

	return 0;
}

#ifdef CONFIG_PWM_CAPTURE
#define PWM_GD32_IRQ_CONFIG(i)                                                   \
	static void pwm_gd32_irq_config_##i(const struct device *dev)             \
	{                                                                           \
		IRQ_CONNECT(DT_IRQN(DT_INST_PARENT(i)),                              \
			    DT_IRQ(DT_INST_PARENT(i), priority), pwm_gd32_isr,        \
			    DEVICE_DT_INST_GET(i), 0);                                  \
		irq_enable(DT_IRQN(DT_INST_PARENT(i)));                              \
	}
#define PWM_GD32_IRQ_CONFIG_INIT(i) .irq_config_func = pwm_gd32_irq_config_##i,
#else
#define PWM_GD32_IRQ_CONFIG(i)
#define PWM_GD32_IRQ_CONFIG_INIT(i)
#endif

#define PWM_GD32_AUX_RATIO(i, idx, default_value)                                    \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(i, gd_auxiliary_pulse_ratio),                 \
		    (DT_INST_PROP_BY_IDX(i, gd_auxiliary_pulse_ratio, idx)),             \
		    (default_value))

#define PWM_GD32_DEFINE(i)						       \
	static struct pwm_gd32_data pwm_gd32_data_##i;			       \
	PWM_GD32_IRQ_CONFIG(i)						       \
	BUILD_ASSERT(DT_INST_PROP_LEN_OR(i, gd_auxiliary_pulse_ratio, 2) == 2,   \
		     "gd,auxiliary-pulse-ratio must contain numerator and denominator"); \
									       \
	PINCTRL_DT_INST_DEFINE(i);					       \
									       \
	static const struct pwm_gd32_config pwm_gd32_config_##i = {	       \
		.reg = DT_REG_ADDR(DT_INST_PARENT(i)),			       \
		.clkid = DT_CLOCKS_CELL(DT_INST_PARENT(i), id),		       \
		.reset = RESET_DT_SPEC_GET(DT_INST_PARENT(i)),		       \
		.prescaler = DT_PROP(DT_INST_PARENT(i), prescaler),	       \
		.channels = DT_PROP(DT_INST_PARENT(i), channels),	       \
		.is_32bit = DT_PROP(DT_INST_PARENT(i), is_32bit),	       \
		.is_advanced = DT_PROP(DT_INST_PARENT(i), is_advanced),	       \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(i),		       \
		.auxiliary_source_channel =                                      \
			DT_INST_PROP_OR(i, gd_auxiliary_source_channel, 0),        \
		.auxiliary_channel = DT_INST_PROP_OR(i, gd_auxiliary_channel, 0), \
		.auxiliary_ratio_numerator = PWM_GD32_AUX_RATIO(i, 0, 1),       \
		.auxiliary_ratio_denominator = PWM_GD32_AUX_RATIO(i, 1, 1),     \
		.has_auxiliary_channel =                                        \
			DT_INST_NODE_HAS_PROP(i, gd_auxiliary_channel),           \
		PWM_GD32_IRQ_CONFIG_INIT(i)				       \
	};								       \
									       \
	DEVICE_DT_INST_DEFINE(i, &pwm_gd32_init, NULL, &pwm_gd32_data_##i,     \
			      &pwm_gd32_config_##i, POST_KERNEL,	       \
			      CONFIG_PWM_INIT_PRIORITY,			       \
			      &pwm_gd32_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_GD32_DEFINE)
