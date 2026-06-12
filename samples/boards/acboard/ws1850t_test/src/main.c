/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define USER_NODE DT_PATH(zephyr_user)

#define REG_COMMAND 0x01U
#define REG_COM_IRQ 0x04U
#define REG_ERROR 0x06U
#define REG_FIFO_DATA 0x09U
#define REG_FIFO_LEVEL 0x0AU
#define REG_CONTROL 0x0CU
#define REG_BIT_FRAMING 0x0DU
#define REG_COLL 0x0EU
#define REG_MODE 0x11U
#define REG_TX_CONTROL 0x14U
#define REG_TX_ASK 0x15U
#define REG_T_MODE 0x2AU
#define REG_T_PRESCALER 0x2BU
#define REG_T_RELOAD_H 0x2CU
#define REG_T_RELOAD_L 0x2DU
#define REG_VERSION 0x37U

#define CMD_IDLE 0x00U
#define CMD_TRANSCEIVE 0x0CU
#define CMD_SOFT_RESET 0x0FU

#define PICC_REQA 0x26U
#define PICC_ANTICOLL_CL1 0x93U

#define IRQ_RX_IDLE 0x30U
#define IRQ_TIMER 0x01U
#define FIFO_FLUSH BIT(7)
#define START_SEND BIT(7)
#define UART_TIMEOUT K_MSEC(20)
#define CARD_REMOVAL_POLLS 3U

static const struct device *const rfid_uart =
	DEVICE_DT_GET(DT_PHANDLE(USER_NODE, ws1850t_uart));
static const struct gpio_dt_spec rfid_reset =
	GPIO_DT_SPEC_GET(USER_NODE, ws1850t_reset_gpios);

K_MSGQ_DEFINE(rx_queue, sizeof(uint8_t), 32, 1);
K_SEM_DEFINE(tx_done, 0, 1);

struct uart_tx_state {
	const uint8_t *data;
	size_t length;
	size_t position;
};

static struct uart_tx_state tx_state;

struct ws1850t_diagnostics {
	uint32_t magic;
	int32_t version_result;
	uint32_t version;
	int32_t initialization_result;
	int32_t card_result;
	uint32_t poll_count;
	uint32_t card_count;
	uint32_t atqa;
	uint32_t uid;
};

volatile struct ws1850t_diagnostics ws1850t_diagnostics;

static int ws1850t_hardware_reset(void)
{
	int ret;

	ret = gpio_pin_configure_dt(&rfid_reset, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_set_dt(&rfid_reset, 1);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(1));

	ret = gpio_pin_set_dt(&rfid_reset, 0);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(10));

	return 0;
}

static void rfid_uart_isr(const struct device *dev, void *user_data)
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

static int uart_transfer(const uint8_t *tx, size_t tx_len)
{
	k_sem_reset(&tx_done);
	tx_state.data = tx;
	tx_state.length = tx_len;
	tx_state.position = 0U;
	uart_irq_tx_enable(rfid_uart);

	if (k_sem_take(&tx_done, UART_TIMEOUT) < 0) {
		uart_irq_tx_disable(rfid_uart);
		return -ETIMEDOUT;
	}

	return 0;
}

static int uart_receive(uint8_t *byte)
{
	return k_msgq_get(&rx_queue, byte, UART_TIMEOUT);
}

static int ws1850t_read_reg(uint8_t reg, uint8_t *value)
{
	uint8_t address = 0x80U | (reg & 0x3FU);
	int ret;

	k_msgq_purge(&rx_queue);
	ret = uart_transfer(&address, 1U);
	if (ret < 0) {
		return ret;
	}

	return uart_receive(value);
}

static int ws1850t_write_reg(uint8_t reg, uint8_t value)
{
	uint8_t frame[] = {reg & 0x3FU, value};
	uint8_t discard;
	int ret;

	k_msgq_purge(&rx_queue);
	ret = uart_transfer(frame, sizeof(frame));
	if (ret < 0) {
		return ret;
	}

	while (k_msgq_get(&rx_queue, &discard, K_NO_WAIT) == 0) {
	}

	return 0;
}

static int ws1850t_set_bits(uint8_t reg, uint8_t mask)
{
	uint8_t value;
	int ret = ws1850t_read_reg(reg, &value);

	return ret < 0 ? ret : ws1850t_write_reg(reg, value | mask);
}

static int ws1850t_init(void)
{
	int ret;

	ret = ws1850t_write_reg(REG_COMMAND, CMD_SOFT_RESET);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(50));

	ret = ws1850t_write_reg(REG_T_MODE, 0x8DU);
	ret |= ws1850t_write_reg(REG_T_PRESCALER, 0x3EU);
	ret |= ws1850t_write_reg(REG_T_RELOAD_L, 30U);
	ret |= ws1850t_write_reg(REG_T_RELOAD_H, 0U);
	ret |= ws1850t_write_reg(REG_TX_ASK, 0x40U);
	ret |= ws1850t_write_reg(REG_MODE, 0x3DU);
	ret |= ws1850t_set_bits(REG_TX_CONTROL, 0x03U);

	return ret;
}

static int ws1850t_transceive(const uint8_t *tx, size_t tx_len, uint8_t tx_last_bits,
			      uint8_t *rx, size_t rx_size, size_t *rx_len)
{
	uint8_t irq;
	uint8_t error;
	uint8_t level;
	int ret;

	ret = ws1850t_write_reg(REG_COMMAND, CMD_IDLE);
	ret |= ws1850t_write_reg(REG_COM_IRQ, 0x7FU);
	ret |= ws1850t_write_reg(REG_FIFO_LEVEL, FIFO_FLUSH);
	ret |= ws1850t_write_reg(REG_BIT_FRAMING, tx_last_bits & 0x07U);
	for (size_t i = 0U; i < tx_len && ret == 0; ++i) {
		ret = ws1850t_write_reg(REG_FIFO_DATA, tx[i]);
	}
	ret |= ws1850t_write_reg(REG_COMMAND, CMD_TRANSCEIVE);
	ret |= ws1850t_set_bits(REG_BIT_FRAMING, START_SEND);
	if (ret < 0) {
		return ret;
	}

	for (int i = 0; i < 50; ++i) {
		ret = ws1850t_read_reg(REG_COM_IRQ, &irq);
		if (ret < 0) {
			return ret;
		}
		if ((irq & (IRQ_RX_IDLE | IRQ_TIMER)) != 0U) {
			break;
		}
		k_sleep(K_MSEC(1));
	}

	(void)ws1850t_write_reg(REG_BIT_FRAMING, tx_last_bits & 0x07U);
	if ((irq & IRQ_TIMER) != 0U || (irq & IRQ_RX_IDLE) == 0U) {
		return -ETIMEDOUT;
	}

	ret = ws1850t_read_reg(REG_ERROR, &error);
	if (ret < 0) {
		return ret;
	}
	if ((error & 0x13U) != 0U) {
		return -EIO;
	}

	ret = ws1850t_read_reg(REG_FIFO_LEVEL, &level);
	if (ret < 0) {
		return ret;
	}
	if (level > rx_size) {
		return -EMSGSIZE;
	}

	for (size_t i = 0U; i < level; ++i) {
		ret = ws1850t_read_reg(REG_FIFO_DATA, &rx[i]);
		if (ret < 0) {
			return ret;
		}
	}
	*rx_len = level;

	return 0;
}

static int ws1850t_read_card(uint8_t atqa[2], uint8_t uid[4])
{
	static const uint8_t anticoll[] = {PICC_ANTICOLL_CL1, 0x20U};
	uint8_t response[5];
	uint8_t bcc = 0U;
	size_t response_len;
	int ret;

	ret = ws1850t_transceive((const uint8_t[]){PICC_REQA}, 1U, 7U,
				  atqa, 2U, &response_len);
	if (ret < 0 || response_len != 2U) {
		return ret < 0 ? ret : -EBADMSG;
	}

	ret = ws1850t_write_reg(REG_COLL, 0x80U);
	if (ret < 0) {
		return ret;
	}
	ret = ws1850t_transceive(anticoll, sizeof(anticoll), 0U,
				  response, sizeof(response), &response_len);
	if (ret < 0 || response_len != sizeof(response)) {
		return ret < 0 ? ret : -EBADMSG;
	}

	for (size_t i = 0U; i < 4U; ++i) {
		uid[i] = response[i];
		bcc ^= response[i];
	}

	return bcc == response[4] ? 0 : -EBADMSG;
}

int main(void)
{
	uint8_t version;
	uint8_t atqa[2];
	uint8_t uid[4];
	uint32_t active_uid = 0U;
	uint32_t uid_value;
	uint32_t missed_polls = 0U;
	bool card_present = false;
	int ret;

	printk("\nACBoard WS1850T RFID UART interrupt test\n");
	printk("USART1: PD5 TX, PD6 RX, 9600 8N1\n");
	ws1850t_diagnostics.magic = 0x1850D1A6U;

	if (!device_is_ready(rfid_uart) || !gpio_is_ready_dt(&rfid_reset)) {
		printk("WS1850T device resource not ready\n");
		return 0;
	}

	ret = ws1850t_hardware_reset();
	if (ret < 0) {
		printk("WS1850T hardware reset failed: %d\n", ret);
		return 0;
	}
	printk("WS1850T hardware reset: PD4 high then low\n");

	ret = uart_irq_callback_user_data_set(rfid_uart, rfid_uart_isr, NULL);
	if (ret < 0) {
		printk("UART IRQ callback setup failed: %d\n", ret);
		return 0;
	}
	uart_irq_rx_enable(rfid_uart);
	k_sleep(K_MSEC(20));

	ret = ws1850t_read_reg(REG_VERSION, &version);
	ws1850t_diagnostics.version_result = ret;
	if (ret < 0) {
		printk("WS1850T version read failed: %d\n", ret);
		return 0;
	}
	ws1850t_diagnostics.version = version;
	printk("WS1850T version: 0x%02x\n", version);

	ret = ws1850t_init();
	ws1850t_diagnostics.initialization_result = ret;
	if (ret < 0) {
		printk("WS1850T initialization failed: %d\n", ret);
		return 0;
	}
	printk("WS1850T initialized, waiting for ISO14443A card\n");

	while (true) {
		ret = ws1850t_read_card(atqa, uid);
		ws1850t_diagnostics.card_result = ret;
		ws1850t_diagnostics.poll_count++;
		if (ret == 0) {
			uid_value = (uint32_t)uid[0] |
				    ((uint32_t)uid[1] << 8) |
				    ((uint32_t)uid[2] << 16) |
				    ((uint32_t)uid[3] << 24);
			missed_polls = 0U;
			ws1850t_diagnostics.atqa = (uint32_t)atqa[0] |
						  ((uint32_t)atqa[1] << 8);
			ws1850t_diagnostics.uid = uid_value;

			if (!card_present || uid_value != active_uid) {
				active_uid = uid_value;
				card_present = true;
				ws1850t_diagnostics.card_count++;
				printk("Card: ATQA=%02x%02x UID=%02x%02x%02x%02x\n",
				       atqa[0], atqa[1], uid[0], uid[1], uid[2], uid[3]);
			}
		} else if (ret == -ETIMEDOUT) {
			if (card_present && ++missed_polls >= CARD_REMOVAL_POLLS) {
				card_present = false;
				missed_polls = 0U;
				printk("Card removed\n");
			}
		} else if (ret != -ETIMEDOUT) {
			printk("Card read failed: %d\n", ret);
		}
		k_sleep(K_MSEC(500));
	}

	return 0;
}
