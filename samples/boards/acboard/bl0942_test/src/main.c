/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define USER_NODE DT_PATH(zephyr_user)

#define BL0942_READ_COMMAND 0x58U
#define BL0942_PACKET_ADDRESS 0xAAU
#define BL0942_PACKET_HEADER 0x55U
#define BL0942_PACKET_SIZE 23U
#define BL0942_RESPONSE_TIMEOUT K_MSEC(150)
#define BL0942_POLL_INTERVAL K_SECONDS(2)

static const struct device *const bl0942_uart =
	DEVICE_DT_GET(DT_PHANDLE(USER_NODE, bl0942_uart));

K_MSGQ_DEFINE(rx_queue, sizeof(uint8_t), 64, 1);
K_SEM_DEFINE(tx_done, 0, 1);

struct uart_tx_state {
	const uint8_t *data;
	size_t length;
	size_t position;
};

static struct uart_tx_state tx_state;

static void bl0942_uart_isr(const struct device *dev, void *user_data)
{
	uint8_t byte;

	ARG_UNUSED(user_data);

	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev) > 0) {
		while (uart_fifo_read(dev, &byte, 1) == 1) {
			(void)k_msgq_put(&rx_queue, &byte, K_NO_WAIT);
		}
	}

	if (uart_irq_tx_ready(dev) > 0) {
		while (tx_state.position < tx_state.length &&
		       uart_fifo_fill(dev, &tx_state.data[tx_state.position], 1) == 1) {
			tx_state.position++;
		}

		if (tx_state.position == tx_state.length && uart_irq_tx_complete(dev) > 0) {
			uart_irq_tx_disable(dev);
			k_sem_give(&tx_done);
		}
	}
}

static int uart_write_irq(const uint8_t *data, size_t length)
{
	if (length == 0U) {
		return 0;
	}

	k_sem_reset(&tx_done);
	tx_state.data = data;
	tx_state.length = length;
	tx_state.position = 0U;
	uart_irq_tx_enable(bl0942_uart);

	if (k_sem_take(&tx_done, K_MSEC(20)) < 0) {
		uart_irq_tx_disable(bl0942_uart);
		return -ETIMEDOUT;
	}

	return 0;
}

static uint32_t get_le24(const uint8_t *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16);
}

static int32_t get_le24_signed(const uint8_t *data)
{
	uint32_t value = get_le24(data);

	if ((value & BIT(23)) != 0U) {
		value |= 0xFF000000U;
	}

	return (int32_t)value;
}

static bool packet_checksum_valid(const uint8_t *packet)
{
	uint8_t sum = BL0942_READ_COMMAND;

	for (size_t i = 0U; i < BL0942_PACKET_SIZE - 1U; ++i) {
		sum += packet[i];
	}

	return packet[BL0942_PACKET_SIZE - 1U] == (uint8_t)(~sum);
}

static int receive_packet(uint8_t *packet)
{
	uint8_t byte;
	size_t position = 0U;
	int ret;

	while (position < BL0942_PACKET_SIZE) {
		ret = k_msgq_get(&rx_queue, &byte, BL0942_RESPONSE_TIMEOUT);
		if (ret < 0) {
			return -ETIMEDOUT;
		}

		if (position == 0U && byte != BL0942_PACKET_HEADER) {
			printk("Discard RX byte: %02x\n", byte);
			continue;
		}

		packet[position++] = byte;
	}

	return packet_checksum_valid(packet) ? 0 : -EBADMSG;
}

static void print_packet(const uint8_t *packet)
{
	printk("BL0942: I_RMS=%u V_RMS=%u I_FAST_RMS=%u WATT=%d "
	       "CF_CNT=%u FREQ=%u STATUS=0x%02x\n",
	       get_le24(&packet[1]), get_le24(&packet[4]),
	       get_le24(&packet[7]), get_le24_signed(&packet[10]),
	       get_le24(&packet[13]),
	       (uint32_t)packet[16] | ((uint32_t)packet[17] << 8), packet[19]);
}

int main(void)
{
	static const uint8_t packet_command[] = {
		BL0942_READ_COMMAND,
		BL0942_PACKET_ADDRESS,
	};
	uint8_t packet[BL0942_PACKET_SIZE];
	int ret;

	printk("\nACBoard BL0942 UART interrupt test\n");
	printk("UART4: PC12 TX, PD2 RX, 9600 8N1\n");

	if (!device_is_ready(bl0942_uart)) {
		printk("BL0942 UART device not ready\n");
		return 0;
	}

	ret = uart_irq_callback_user_data_set(bl0942_uart, bl0942_uart_isr, NULL);
	if (ret < 0) {
		printk("UART IRQ callback setup failed: %d\n", ret);
		return 0;
	}

	uart_irq_rx_enable(bl0942_uart);

	while (true) {
		k_msgq_purge(&rx_queue);
		printk("TX: 58 aa\n");

		ret = uart_write_irq(packet_command, sizeof(packet_command));
		if (ret < 0) {
			printk("UART TX failed: %d\n", ret);
		} else {
			ret = receive_packet(packet);
			if (ret == -ETIMEDOUT) {
				printk("BL0942 response timeout\n");
			} else if (ret == -EBADMSG) {
				printk("BL0942 checksum mismatch\n");
			} else {
				print_packet(packet);
			}
		}

		k_sleep(BL0942_POLL_INTERVAL);
	}

	return 0;
}
