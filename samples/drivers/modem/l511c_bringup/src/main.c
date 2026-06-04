/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define USER_NODE DT_PATH(zephyr_user)
#define MODEM_UART_NODE DT_CHOSEN(zephyr_modem_uart)

#define MODEM_RESET_PULSE_MS DT_PROP(USER_NODE, modem_reset_pulse_ms)
#define MODEM_RX_POLL_MS 2
#define MODEM_BOOT_LOG_TIMEOUT_MS 3000
#define MODEM_AT_WAIT_MS 5000
#define MODEM_AT_RX_TIMEOUT_MS 2000
#define MODEM_AT_CMD_GAP_MS 300
#define MODEM_NET_POLL_COUNT 12
#define MODEM_NET_POLL_INTERVAL_MS 5000
#define MODEM_BUF_SIZE 512

static const struct gpio_dt_spec modem_powerkey =
	GPIO_DT_SPEC_GET(USER_NODE, modem_powerkey_gpios);
static const struct gpio_dt_spec modem_reset =
	GPIO_DT_SPEC_GET(USER_NODE, modem_reset_gpios);
static const struct device *const modem_uart = DEVICE_DT_GET(MODEM_UART_NODE);
static uint8_t modem_buf[MODEM_BUF_SIZE];
static volatile size_t modem_buf_len;

static void modem_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev)) {
		uint8_t tmp[32];
		int rx = uart_fifo_read(dev, tmp, sizeof(tmp));

		if (rx <= 0) {
			break;
		}

		for (int i = 0; i < rx; i++) {
			if (modem_buf_len < sizeof(modem_buf)) {
				modem_buf[modem_buf_len++] = tmp[i];
			}
		}
	}
}

static void modem_dump_lines(const char *tag, const uint8_t *buf, size_t len)
{
	char line[128];
	size_t line_len = 0;

	printk("%s captured %u bytes\n", tag, (unsigned int)len);

	for (size_t i = 0; i < len; i++) {
		uint8_t c = buf[i];

		if (c == '\r') {
			continue;
		}

		if (c == '\n') {
			if (line_len > 0) {
				line[line_len] = '\0';
				printk("%s: %s\n", tag, line);
				line_len = 0;
			}
			continue;
		}

		if (line_len < sizeof(line) - 1U) {
			line[line_len++] = isprint((int)c) ? (char)c : '.';
		}
	}

	if (line_len > 0) {
		line[line_len] = '\0';
		printk("%s: %s\n", tag, line);
	}
}

static size_t modem_capture_bytes(int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (k_uptime_get() < deadline) {
		k_msleep(MODEM_RX_POLL_MS);
	}

	uart_irq_rx_disable(modem_uart);
	return modem_buf_len;
}

static void modem_send(const char *s)
{
	while (*s != '\0') {
		uart_poll_out(modem_uart, *s++);
	}
}

static void modem_run_cmd(const char *cmd)
{
	size_t len;

	printk("send: %s", cmd);
	modem_buf_len = 0;
	uart_irq_rx_enable(modem_uart);
	modem_send(cmd);
	len = modem_capture_bytes(MODEM_AT_RX_TIMEOUT_MS);
	modem_dump_lines("at", modem_buf, len);
	k_msleep(MODEM_AT_CMD_GAP_MS);
}

static int modem_prepare_gpios(void)
{
	int ret;

	if (!gpio_is_ready_dt(&modem_powerkey) || !gpio_is_ready_dt(&modem_reset)) {
		printk("modem gpio not ready\n");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&modem_powerkey, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("modem powerkey gpio config failed: %d\n", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&modem_reset, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("modem reset gpio config failed: %d\n", ret);
		return ret;
	}

	return 0;
}

int main(void)
{
	static const char *const cmds[] = {
		"AT\r\n",
		"ATE0\r\n",
		"ATI\r\n",
		"AT+CPIN?\r\n",
		"AT+CSQ\r\n",
		"AT+CEREG?\r\n",
		"AT+ECICCID\r\n",
		"AT+CIMI\r\n",
		"AT+COPS?\r\n",
		"AT+CGDCONT?\r\n",
	};
	size_t len;
	int ret;

	if (!device_is_ready(modem_uart)) {
		printk("modem uart not ready\n");
		return 0;
	}

	if (modem_prepare_gpios() < 0) {
		return 0;
	}

	printk("drive powerkey high and hold\n");
	gpio_pin_set_dt(&modem_powerkey, 1);

	/* Trigger reset after powerkey is asserted, then capture immediately. */
	gpio_pin_set_dt(&modem_reset, 1);
	k_msleep(MODEM_RESET_PULSE_MS);
	printk("capturing modem boot bytes for %d ms\n",
	       MODEM_BOOT_LOG_TIMEOUT_MS);
	modem_buf_len = 0;
	ret = uart_irq_callback_user_data_set(modem_uart, modem_uart_isr, NULL);
	if (ret < 0) {
		printk("uart irq callback setup failed: %d\n", ret);
		return 0;
	}
	uart_irq_rx_enable(modem_uart);
	gpio_pin_set_dt(&modem_reset, 0);
	(void)modem_capture_bytes(MODEM_BOOT_LOG_TIMEOUT_MS);

	printk("waiting %d ms before AT\n", MODEM_AT_WAIT_MS);
	k_msleep(MODEM_AT_WAIT_MS);

	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++) {
		modem_run_cmd(cmds[i]);
	}

	for (size_t i = 0; i < MODEM_NET_POLL_COUNT; i++) {
		printk("network poll %u/%u\n",
		       (unsigned int)(i + 1),
		       (unsigned int)MODEM_NET_POLL_COUNT);
		modem_run_cmd("AT+CSQ\r\n");
		modem_run_cmd("AT+CEREG?\r\n");
		modem_run_cmd("AT+COPS?\r\n");
		k_msleep(MODEM_NET_POLL_INTERVAL_MS);
	}

	printk("bring-up sequence done\n");
	return 0;
}
