/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define CP_SAMPLE_COUNT 32U
#define CP_REPORT_INTERVAL_MS 500U

#define CP_STATE_A_MIN_RAW 3500U
#define CP_STATE_B_MIN_RAW 2600U
#define CP_STATE_C_MIN_RAW 1700U
#define CP_STATE_E_MAX_RAW 500U

#define CP_USER_NODE DT_PATH(zephyr_user)

static const struct adc_dt_spec cp_adc = ADC_DT_SPEC_GET_BY_IDX(CP_USER_NODE, 0);
static const struct pwm_dt_spec cp_pwm = PWM_DT_SPEC_GET_BY_IDX(CP_USER_NODE, 0);
static const struct pwm_dt_spec cp_pwm_feedback = PWM_DT_SPEC_GET_BY_IDX(CP_USER_NODE, 1);
static const struct gpio_dt_spec cp_diode =
	GPIO_DT_SPEC_GET(CP_USER_NODE, cp_diode_gpios);
static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static uint16_t cp_duty_permille = 530U;
static uint16_t cp_samples[CP_SAMPLE_COUNT];

static const char *cp_state_name(uint16_t peak_raw)
{
	if (peak_raw >= CP_STATE_A_MIN_RAW) {
		return "A";
	}
	if (peak_raw >= CP_STATE_B_MIN_RAW) {
		return "B";
	}
	if (peak_raw >= CP_STATE_C_MIN_RAW) {
		return "C";
	}
	if (peak_raw <= CP_STATE_E_MAX_RAW) {
		return "E";
	}

	return "D/invalid";
}

static int cp_pwm_set(uint16_t duty_permille)
{
	uint32_t pulse;

	if (duty_permille > 1000U) {
		return -EINVAL;
	}

	pulse = ((uint64_t)cp_pwm.period * duty_permille) / 1000U;
	return pwm_set_dt(&cp_pwm, cp_pwm.period, pulse);
}

static void print_help(void)
{
	printk("Commands: 0=off, 1=100%%, 2=53%%, 3=10%%, +=+1%%, -=-1%%, h=help\n");
}

static void handle_console(void)
{
	unsigned char ch;
	uint16_t requested = cp_duty_permille;
	int ret;

	if (uart_poll_in(console, &ch) != 0) {
		return;
	}

	switch (ch) {
	case '0':
		requested = 0U;
		break;
	case '1':
		requested = 1000U;
		break;
	case '2':
		requested = 530U;
		break;
	case '3':
		requested = 100U;
		break;
	case '+':
		requested = MIN((uint16_t)(cp_duty_permille + 10U), 1000U);
		break;
	case '-':
		requested = cp_duty_permille >= 10U ? cp_duty_permille - 10U : 0U;
		break;
	case 'h':
	case '?':
		print_help();
		return;
	default:
		return;
	}

	ret = cp_pwm_set(requested);
	if (ret == 0) {
		cp_duty_permille = requested;
		printk("CP PWM duty=%u.%u%%\n", requested / 10U, requested % 10U);
	} else {
		printk("CP PWM update failed: %d\n", ret);
	}
}

static int cp_adc_measure(uint16_t *minimum, uint16_t *average, uint16_t *maximum)
{
	struct adc_sequence_options options = {
		.extra_samplings = CP_SAMPLE_COUNT - 1U,
	};
	struct adc_sequence sequence = {
		.options = &options,
		.buffer = cp_samples,
		.buffer_size = sizeof(cp_samples),
	};
	uint32_t sum = 0U;
	int ret;

	adc_sequence_init_dt(&cp_adc, &sequence);
	ret = adc_read(cp_adc.dev, &sequence);
	if (ret < 0) {
		return ret;
	}

	*minimum = UINT16_MAX;
	*maximum = 0U;
	for (size_t i = 0U; i < ARRAY_SIZE(cp_samples); ++i) {
		*minimum = MIN(*minimum, cp_samples[i]);
		*maximum = MAX(*maximum, cp_samples[i]);
		sum += cp_samples[i];
	}
	*average = (uint16_t)(sum / ARRAY_SIZE(cp_samples));
	return 0;
}

static bool cp_pwm_feedback_read(uint32_t *frequency_hz, uint16_t *duty_permille)
{
	uint32_t period;
	uint32_t pulse;
	uint64_t cycles_per_sec;
	int ret;

	ret = pwm_capture_cycles(cp_pwm_feedback.dev, cp_pwm_feedback.channel,
				 PWM_CAPTURE_TYPE_BOTH, &period, &pulse, K_MSEC(5));
	if (ret < 0 || period == 0U || pulse > period) {
		return false;
	}
	ret = pwm_get_cycles_per_sec(cp_pwm_feedback.dev, cp_pwm_feedback.channel,
				     &cycles_per_sec);
	if (ret < 0) {
		return false;
	}

	*frequency_hz = (uint32_t)(cycles_per_sec / period);
	*duty_permille = (uint16_t)(((uint64_t)pulse * 1000U) / period);
	return true;
}

static const char *cp_pwm_feedback_state(bool valid)
{
	if (valid) {
		return "valid";
	}
	if (cp_duty_permille == 0U) {
		return "constant-low";
	}
	if (cp_duty_permille == 1000U) {
		return "constant-high";
	}

	return "missing";
}

int main(void)
{
	uint16_t minimum = 0U;
	uint16_t average = 0U;
	uint16_t maximum = 0U;
	uint16_t feedback_duty = 0U;
	uint32_t feedback_frequency = 0U;
	int ret;

	printk("\nACBoard CP Zephyr driver test\n");

	if (!adc_is_ready_dt(&cp_adc) || !pwm_is_ready_dt(&cp_pwm) ||
	    !pwm_is_ready_dt(&cp_pwm_feedback) || !gpio_is_ready_dt(&cp_diode) ||
	    !device_is_ready(console)) {
		printk("CP device not ready\n");
		return 0;
	}

	ret = adc_channel_setup_dt(&cp_adc);
	if (ret < 0) {
		printk("CP ADC setup failed: %d\n", ret);
		return 0;
	}
	ret = gpio_pin_configure_dt(&cp_diode, GPIO_INPUT);
	if (ret < 0) {
		printk("CP diode GPIO setup failed: %d\n", ret);
		return 0;
	}
	ret = cp_pwm_set(cp_duty_permille);
	if (ret < 0) {
		printk("CP PWM setup failed: %d\n", ret);
		return 0;
	}

	printk("PWM and capture use Zephyr PWM API; ADC uses hardware-triggered adc_read()\n");
	print_help();

	while (true) {
		handle_console();
		ret = cp_adc_measure(&minimum, &average, &maximum);
		bool feedback_valid = cp_pwm_feedback_read(&feedback_frequency, &feedback_duty);

		printk("CP set=%u.%u%% pwm_feedback=%s",
		       cp_duty_permille / 10U, cp_duty_permille % 10U,
		       cp_pwm_feedback_state(feedback_valid));
		if (feedback_valid) {
			printk(" %uHz %u.%u%%", feedback_frequency,
			       feedback_duty / 10U, feedback_duty % 10U);
		}
		if (ret == 0) {
			printk(" voltage_raw[min/avg/max]=%u/%u/%u state=%s",
			       minimum, average, maximum, cp_state_name(maximum));
		} else if (ret == -ETIMEDOUT) {
			printk(" voltage=trigger-timeout");
		} else {
			printk(" voltage=error(%d)", ret);
		}
		printk(" diode=%d\n", gpio_pin_get_dt(&cp_diode));
		k_sleep(K_MSEC(CP_REPORT_INTERVAL_MS));
	}

	return 0;
}
