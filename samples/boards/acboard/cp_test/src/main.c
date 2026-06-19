/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/misc/control_pilot/control_pilot.h>

#define CP_REPORT_INTERVAL K_MSEC(500)

static const struct device *const cp = DEVICE_DT_GET_ONE(zephyr_control_pilot);
static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static uint16_t commanded_duty = 1000U;

static const char *state_str(enum cp_state state)
{
	switch (state) {
	case CP_STATE_0:
		return "0";
	case CP_STATE_1:
		return "1";
	case CP_STATE_1_PRIME:
		return "1'";
	case CP_STATE_2:
		return "2";
	case CP_STATE_2_PRIME:
		return "2'";
	case CP_STATE_3:
		return "3";
	case CP_STATE_3_PRIME:
		return "3'";
	case CP_STATE_4:
		return "4";
	case CP_STATE_INVALID:
		return "invalid";
	case CP_STATE_UNKNOWN:
	default:
		return "?";
	}
}

static void print_help(void)
{
	printk("Commands: 0=state4, 1=100%%, 2=53%%, 3=10%%, +=+1%%, -=-1%%, h=help\n");
}

static void handle_console(void)
{
	unsigned char ch;
	uint16_t duty = commanded_duty;
	int ret;

	if (uart_poll_in(console, &ch) != 0) {
		return;
	}

	switch (ch) {
	case '0':
		duty = 0U;
		break;
	case '1':
		duty = 1000U;
		break;
	case '2':
		duty = 530U;
		break;
	case '3':
		duty = 100U;
		break;
	case '+':
		duty = MIN((uint16_t)(duty + 10U), 1000U);
		break;
	case '-':
		duty = duty >= 10U ? duty - 10U : 0U;
		break;
	case 'h':
	case '?':
		print_help();
		return;
	default:
		return;
	}

	ret = cp_set_duty(cp, duty);
	if (ret < 0) {
		printk("CP set duty failed: %d\n", ret);
	} else {
		commanded_duty = duty;
		printk("CP duty=%u.%u%%\n", duty / 10U, duty % 10U);
	}
}

int main(void)
{
	struct cp_status status;
	int ret;

	printk("\nACBoard control-pilot test\n");

	if (!device_is_ready(cp) || !device_is_ready(console)) {
		printk("CP device not ready\n");
		return 0;
	}

	print_help();

	while (true) {
		handle_console();

		ret = cp_read(cp, &status);
		if (ret < 0) {
			printk("CP set=%u.%u%% read failed: %d\n", commanded_duty / 10U,
			       commanded_duty % 10U, ret);
			k_sleep(CP_REPORT_INTERVAL);
			continue;
		}

		printk("CP set=%u.%u%% state=%s voltage=%d mV diode=%d",
		       status.duty_permille / 10U, status.duty_permille % 10U,
		       state_str(status.state), status.voltage_mv, status.diode_present);
		if (status.feedback_valid) {
			printk(" pwm_feedback=%uHz/%u.%u%%", status.feedback_hz,
			       status.feedback_permille / 10U, status.feedback_permille % 10U);
		}
		printk("\n");

		k_sleep(CP_REPORT_INTERVAL);
	}

	return 0;
}
