/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/sys/printk.h>

#define USER_NODE DT_PATH(zephyr_user)

#define UART_RX_BUFFER_SIZE 512U
#define UART_TX_BUFFER_SIZE 128U
#define CHAT_RX_BUFFER_SIZE 128U
#define CHAT_ARGV_SIZE 8U
#define COMMAND_TIMEOUT_MS 1500U
#define COMMAND_SCRIPT_TIMEOUT_S 3U
#define READY_TIMEOUT_MS 3000U

static const struct device *const bluetooth_uart =
	DEVICE_DT_GET(DT_PHANDLE(USER_NODE, bluetooth_uart));
static const struct gpio_dt_spec bluetooth_reset =
	GPIO_DT_SPEC_GET(USER_NODE, bluetooth_reset_gpios);

struct i2616e_context {
	struct modem_backend_uart backend;
	struct modem_pipe *pipe;
	struct modem_chat chat;
	uint8_t uart_rx_buffer[UART_RX_BUFFER_SIZE];
	uint8_t uart_tx_buffer[UART_TX_BUFFER_SIZE];
	uint8_t chat_rx_buffer[CHAT_RX_BUFFER_SIZE];
	uint8_t *chat_argv[CHAT_ARGV_SIZE];
};

static struct i2616e_context i2616e;
static uint8_t chat_delimiter[] = "\r";
static uint8_t chat_filter[] = "\n";
K_SEM_DEFINE(module_ready, 0, 1);

static void on_ready(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(argv);
	ARG_UNUSED(argc);
	ARG_UNUSED(user_data);

	printk("URC: IM_READY\n");
	k_sem_give(&module_ready);
}

static void on_line(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(user_data);

	if (argc >= 2U) {
		printk("RX: %s\n", argv[1]);
	}
}

static void on_ok(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(argv);
	ARG_UNUSED(argc);
	ARG_UNUSED(user_data);

	printk("RX: OK\n");
}

static void on_error(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(argv);
	ARG_UNUSED(argc);
	ARG_UNUSED(user_data);

	printk("RX: ERROR\n");
}

MODEM_CHAT_MATCH_DEFINE(ok_match, "OK", "", on_ok);
MODEM_CHAT_MATCHES_DEFINE(abort_matches,
	MODEM_CHAT_MATCH("ERROR", "", on_error));
MODEM_CHAT_MATCHES_DEFINE(unsol_matches,
	MODEM_CHAT_MATCH("IM_READY", "", on_ready),
	MODEM_CHAT_MATCH("", "", on_line));

static int i2616e_chat_init(void)
{
	const struct modem_backend_uart_config backend_config = {
		.uart = bluetooth_uart,
		.receive_buf = i2616e.uart_rx_buffer,
		.receive_buf_size = sizeof(i2616e.uart_rx_buffer),
		.transmit_buf = i2616e.uart_tx_buffer,
		.transmit_buf_size = sizeof(i2616e.uart_tx_buffer),
	};
	const struct modem_chat_config chat_config = {
		.user_data = &i2616e,
		.receive_buf = i2616e.chat_rx_buffer,
		.receive_buf_size = sizeof(i2616e.chat_rx_buffer),
		.delimiter = chat_delimiter,
		.delimiter_size = 1U,
		.filter = chat_filter,
		.filter_size = 1U,
		.argv = i2616e.chat_argv,
		.argv_size = ARRAY_SIZE(i2616e.chat_argv),
		.unsol_matches = unsol_matches,
		.unsol_matches_size = ARRAY_SIZE(unsol_matches),
	};
	int ret;

	i2616e.pipe = modem_backend_uart_init(&i2616e.backend, &backend_config);
	if (i2616e.pipe == NULL) {
		return -ENODEV;
	}

	ret = modem_chat_init(&i2616e.chat, &chat_config);
	if (ret < 0) {
		return ret;
	}

	ret = modem_chat_attach(&i2616e.chat, i2616e.pipe);
	if (ret < 0) {
		return ret;
	}

	return modem_pipe_open(i2616e.pipe, K_MSEC(100));
}

static int i2616e_run_command(const char *command)
{
	const struct modem_chat_script_chat script_chat = {
		.request = (const uint8_t *)command,
		.request_size = strlen(command),
		.response_matches = &ok_match,
		.response_matches_size = 1U,
		.timeout = COMMAND_TIMEOUT_MS,
	};
	const struct modem_chat_script script = {
		.name = command,
		.script_chats = &script_chat,
		.script_chats_size = 1U,
		.abort_matches = abort_matches,
		.abort_matches_size = ARRAY_SIZE(abort_matches),
		.timeout = COMMAND_SCRIPT_TIMEOUT_S,
	};
	int ret;

	printk("TX: %s\n", command);
	ret = modem_chat_run_script(&i2616e.chat, &script);
	if (ret < 0) {
		printk("Command failed: %d\n", ret);
	}

	return ret;
}

int main(void)
{
	static const char *const commands[] = {
		"AT+GFWVER?",
		"AT+GVER?",
		"AT+NAME?",
		"AT+LBDADDR?",
		"AT+BAUD?",
		"AT+FLOWCTRL?",
		"AT+BTMODE?",
		"AT+ADV?",
		"AT+ADV=1",
		"AT+ADV?",
	};
	int ret;

	printk("\nACBoard i2616e BLE module test\n");

	if (!device_is_ready(bluetooth_uart) || !gpio_is_ready_dt(&bluetooth_reset)) {
		printk("Bluetooth device resource not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&bluetooth_reset, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("Bluetooth reset GPIO setup failed: %d\n", ret);
		return 0;
	}

	ret = i2616e_chat_init();
	if (ret < 0) {
		printk("Bluetooth modem chat setup failed: %d\n", ret);
		return 0;
	}

	printk("Reset i2616e: PD12 low for 200 ms\n");
	(void)gpio_pin_set_dt(&bluetooth_reset, 1);
	k_sleep(K_MSEC(200));
	(void)gpio_pin_set_dt(&bluetooth_reset, 0);

	if (k_sem_take(&module_ready, K_MSEC(READY_TIMEOUT_MS)) < 0) {
		printk("IM_READY timeout\n");
		return 0;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(commands); ++i) {
		if (i2616e_run_command(commands[i]) < 0) {
			return 0;
		}
	}

	printk("i2616e modem_chat bring-up sequence complete\n");

	while (true) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
