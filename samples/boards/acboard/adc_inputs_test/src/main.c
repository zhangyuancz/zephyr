/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define USER_NODE DT_PATH(zephyr_user)
#define SAMPLE_COUNT 16U
#define REPORT_INTERVAL K_SECONDS(1)
#define ADC_NOMINAL_REFERENCE_MV 3300U
#define ADC_FULL_SCALE 4095U

struct adc_input {
	const char *name;
	struct adc_dt_spec spec;
};

static const struct adc_input inputs[] = {
	{.name = "PA0", .spec = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, pa0)},
	{.name = "PA3", .spec = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, pa3)},
	{.name = "PC0", .spec = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, pc0)},
	{.name = "PC3", .spec = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, pc3)},
};

static uint16_t samples[SAMPLE_COUNT];

static int sample_input(const struct adc_input *input, uint16_t *minimum,
			uint16_t *average, uint16_t *maximum, int32_t *millivolts)
{
	struct adc_sequence_options options = {
		.extra_samplings = SAMPLE_COUNT - 1U,
	};
	struct adc_sequence sequence = {
		.options = &options,
		.buffer = samples,
		.buffer_size = sizeof(samples),
	};
	uint32_t sum = 0U;
	int ret;

	adc_sequence_init_dt(&input->spec, &sequence);
	ret = adc_read(input->spec.dev, &sequence);
	if (ret < 0) {
		return ret;
	}

	*minimum = UINT16_MAX;
	*maximum = 0U;
	for (size_t i = 0U; i < ARRAY_SIZE(samples); ++i) {
		*minimum = MIN(*minimum, samples[i]);
		*maximum = MAX(*maximum, samples[i]);
		sum += samples[i];
	}

	*average = (uint16_t)(sum / ARRAY_SIZE(samples));
	*millivolts = (int32_t)(((uint32_t)*average * ADC_NOMINAL_REFERENCE_MV +
				 ADC_FULL_SCALE / 2U) / ADC_FULL_SCALE);
	return 0;
}

int main(void)
{
	uint16_t minimum;
	uint16_t average;
	uint16_t maximum;
	int32_t millivolts;
	int ret;

	printk("\nACBoard general ADC inputs test\n");
	printk("12-bit ADC, nominal reference 3300 mV\n");

	for (size_t i = 0U; i < ARRAY_SIZE(inputs); ++i) {
		if (!adc_is_ready_dt(&inputs[i].spec)) {
			printk("%s ADC device not ready\n", inputs[i].name);
			return 0;
		}

		ret = adc_channel_setup_dt(&inputs[i].spec);
		if (ret < 0) {
			printk("%s ADC setup failed: %d\n", inputs[i].name, ret);
			return 0;
		}
	}

	while (true) {
		for (size_t i = 0U; i < ARRAY_SIZE(inputs); ++i) {
			ret = sample_input(&inputs[i], &minimum, &average, &maximum,
					   &millivolts);
			if (ret < 0) {
				printk("%s read failed: %d\n", inputs[i].name, ret);
				continue;
			}

			printk("%s: raw min=%u avg=%u max=%u, estimated=%d mV\n",
			       inputs[i].name, minimum, average, maximum, millivolts);
		}
		printk("\n");
		k_sleep(REPORT_INTERVAL);
	}

	return 0;
}
