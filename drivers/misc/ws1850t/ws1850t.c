/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiseSun WS1850T 13.56 MHz contactless card reader, UART register interface.
 * MFRC522 register-compatible; datasheet WS1850T Rev 1.3.
 */

#define DT_DRV_COMPAT wisesun_ws1850t

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/misc/ws1850t/ws1850t.h>

LOG_MODULE_REGISTER(ws1850t, CONFIG_WS1850T_LOG_LEVEL);

/* MFRC522-compatible register map. */
#define REG_COMMAND	0x01U
#define REG_COM_IRQ	0x04U
#define REG_ERROR	0x06U
#define REG_FIFO_DATA	0x09U
#define REG_FIFO_LEVEL	0x0AU
#define REG_BIT_FRAMING	0x0DU
#define REG_COLL	0x0EU
#define REG_MODE	0x11U
#define REG_TX_CONTROL	0x14U
#define REG_TX_ASK	0x15U
#define REG_T_MODE	0x2AU
#define REG_T_PRESCALER	0x2BU
#define REG_T_RELOAD_H	0x2CU
#define REG_T_RELOAD_L	0x2DU
#define REG_VERSION	0x37U

#define CMD_IDLE	0x00U
#define CMD_TRANSCEIVE	0x0CU
#define CMD_SOFT_RESET	0x0FU

#define PICC_REQA	0x26U
#define PICC_ANTICOLL_CL1 0x93U

#define IRQ_RX_IDLE	0x30U
#define IRQ_TIMER	0x01U
#define FIFO_FLUSH	BIT(7)
#define START_SEND	BIT(7)

#define WS1850T_RX_QUEUE_LEN	64U
#define WS1850T_UART_TIMEOUT	K_MSEC(20)
#define WS1850T_TRANSCEIVE_TRIES 50

struct ws1850t_config {
	const struct device *uart;
	struct gpio_dt_spec reset;
	uart_irq_callback_user_data_t cb;
};

struct ws1850t_data {
	struct k_msgq rx_queue;
	uint8_t rx_buffer[WS1850T_RX_QUEUE_LEN];
	struct k_sem tx_done;
	const uint8_t *tx_data;
	size_t tx_len;
	size_t tx_pos;
	struct k_mutex lock;
};

static void ws1850t_uart_isr(const struct device *uart, void *user_data)
{
	const struct device *dev = user_data;
	struct ws1850t_data *data = dev->data;
	uint8_t byte;

	uart_irq_update(uart);

	while (uart_irq_rx_ready(uart) > 0) {
		while (uart_fifo_read(uart, &byte, 1) == 1) {
			(void)k_msgq_put(&data->rx_queue, &byte, K_NO_WAIT);
		}
	}

	if (uart_irq_tx_ready(uart) > 0) {
		while (data->tx_pos < data->tx_len &&
		       uart_fifo_fill(uart, &data->tx_data[data->tx_pos], 1) == 1) {
			data->tx_pos++;
		}

		if (data->tx_pos == data->tx_len && uart_irq_tx_complete(uart) > 0) {
			uart_irq_tx_disable(uart);
			k_sem_give(&data->tx_done);
		}
	}
}

static int ws1850t_uart_transfer(const struct device *dev, const uint8_t *tx, size_t tx_len)
{
	const struct ws1850t_config *cfg = dev->config;
	struct ws1850t_data *data = dev->data;

	k_sem_reset(&data->tx_done);
	data->tx_data = tx;
	data->tx_len = tx_len;
	data->tx_pos = 0U;
	uart_irq_tx_enable(cfg->uart);

	if (k_sem_take(&data->tx_done, WS1850T_UART_TIMEOUT) < 0) {
		uart_irq_tx_disable(cfg->uart);
		return -ETIMEDOUT;
	}

	return 0;
}

static int ws1850t_read_reg(const struct device *dev, uint8_t reg, uint8_t *value)
{
	struct ws1850t_data *data = dev->data;
	uint8_t address = 0x80U | (reg & 0x3FU);
	int ret;

	k_msgq_purge(&data->rx_queue);
	ret = ws1850t_uart_transfer(dev, &address, 1U);
	if (ret < 0) {
		return ret;
	}

	return k_msgq_get(&data->rx_queue, value, WS1850T_UART_TIMEOUT);
}

static int ws1850t_write_reg(const struct device *dev, uint8_t reg, uint8_t value)
{
	struct ws1850t_data *data = dev->data;
	uint8_t frame[] = {reg & 0x3FU, value};
	uint8_t discard;
	int ret;

	k_msgq_purge(&data->rx_queue);
	ret = ws1850t_uart_transfer(dev, frame, sizeof(frame));
	if (ret < 0) {
		return ret;
	}

	while (k_msgq_get(&data->rx_queue, &discard, K_NO_WAIT) == 0) {
	}

	return 0;
}

static int ws1850t_set_bits(const struct device *dev, uint8_t reg, uint8_t mask)
{
	uint8_t value;
	int ret = ws1850t_read_reg(dev, reg, &value);

	return ret < 0 ? ret : ws1850t_write_reg(dev, reg, value | mask);
}

static int ws1850t_transceive(const struct device *dev, const uint8_t *tx, size_t tx_len,
			      uint8_t tx_last_bits, uint8_t *rx, size_t rx_size, size_t *rx_len)
{
	uint8_t irq = 0U;
	uint8_t error;
	uint8_t level;
	int ret;

	ret = ws1850t_write_reg(dev, REG_COMMAND, CMD_IDLE);
	ret |= ws1850t_write_reg(dev, REG_COM_IRQ, 0x7FU);
	ret |= ws1850t_write_reg(dev, REG_FIFO_LEVEL, FIFO_FLUSH);
	ret |= ws1850t_write_reg(dev, REG_BIT_FRAMING, tx_last_bits & 0x07U);
	for (size_t i = 0U; i < tx_len && ret == 0; ++i) {
		ret = ws1850t_write_reg(dev, REG_FIFO_DATA, tx[i]);
	}
	ret |= ws1850t_write_reg(dev, REG_COMMAND, CMD_TRANSCEIVE);
	ret |= ws1850t_set_bits(dev, REG_BIT_FRAMING, START_SEND);
	if (ret < 0) {
		return ret;
	}

	for (int i = 0; i < WS1850T_TRANSCEIVE_TRIES; ++i) {
		ret = ws1850t_read_reg(dev, REG_COM_IRQ, &irq);
		if (ret < 0) {
			return ret;
		}
		if ((irq & (IRQ_RX_IDLE | IRQ_TIMER)) != 0U) {
			break;
		}
		k_sleep(K_MSEC(1));
	}

	(void)ws1850t_write_reg(dev, REG_BIT_FRAMING, tx_last_bits & 0x07U);
	if ((irq & IRQ_TIMER) != 0U || (irq & IRQ_RX_IDLE) == 0U) {
		return -ETIMEDOUT;
	}

	ret = ws1850t_read_reg(dev, REG_ERROR, &error);
	if (ret < 0) {
		return ret;
	}
	if ((error & 0x13U) != 0U) {
		return -EIO;
	}

	ret = ws1850t_read_reg(dev, REG_FIFO_LEVEL, &level);
	if (ret < 0) {
		return ret;
	}
	if (level > rx_size) {
		return -EMSGSIZE;
	}

	for (size_t i = 0U; i < level; ++i) {
		ret = ws1850t_read_reg(dev, REG_FIFO_DATA, &rx[i]);
		if (ret < 0) {
			return ret;
		}
	}
	*rx_len = level;

	return 0;
}

int ws1850t_get_version(const struct device *dev, uint8_t *version)
{
	struct ws1850t_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = ws1850t_read_reg(dev, REG_VERSION, version);
	k_mutex_unlock(&data->lock);

	return ret;
}

int ws1850t_read_card(const struct device *dev, uint8_t atqa[2], uint8_t uid[4])
{
	static const uint8_t anticoll[] = {PICC_ANTICOLL_CL1, 0x20U};
	struct ws1850t_data *data = dev->data;
	uint8_t response[5];
	uint8_t bcc = 0U;
	size_t response_len;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = ws1850t_transceive(dev, (const uint8_t[]){PICC_REQA}, 1U, 7U,
				 atqa, 2U, &response_len);
	if (ret < 0 || response_len != 2U) {
		ret = (ret == -ETIMEDOUT) ? -EAGAIN : (ret < 0 ? ret : -EBADMSG);
		goto out;
	}

	ret = ws1850t_write_reg(dev, REG_COLL, 0x80U);
	if (ret < 0) {
		goto out;
	}

	ret = ws1850t_transceive(dev, anticoll, sizeof(anticoll), 0U,
				 response, sizeof(response), &response_len);
	if (ret < 0 || response_len != sizeof(response)) {
		ret = ret < 0 ? ret : -EBADMSG;
		goto out;
	}

	for (size_t i = 0U; i < 4U; ++i) {
		uid[i] = response[i];
		bcc ^= response[i];
	}

	ret = (bcc == response[4]) ? 0 : -EBADMSG;

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int ws1850t_hw_reset(const struct device *dev)
{
	const struct ws1850t_config *cfg = dev->config;
	int ret;

	ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_set_dt(&cfg->reset, 1);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(1));

	ret = gpio_pin_set_dt(&cfg->reset, 0);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(10));

	return 0;
}

static int ws1850t_configure(const struct device *dev)
{
	int ret;

	ret = ws1850t_write_reg(dev, REG_COMMAND, CMD_SOFT_RESET);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(50));

	ret = ws1850t_write_reg(dev, REG_T_MODE, 0x8DU);
	ret |= ws1850t_write_reg(dev, REG_T_PRESCALER, 0x3EU);
	ret |= ws1850t_write_reg(dev, REG_T_RELOAD_L, 30U);
	ret |= ws1850t_write_reg(dev, REG_T_RELOAD_H, 0U);
	ret |= ws1850t_write_reg(dev, REG_TX_ASK, 0x40U);
	ret |= ws1850t_write_reg(dev, REG_MODE, 0x3DU);
	ret |= ws1850t_set_bits(dev, REG_TX_CONTROL, 0x03U); /* antenna on */

	return ret;
}

static int ws1850t_init(const struct device *dev)
{
	const struct ws1850t_config *cfg = dev->config;
	struct ws1850t_data *data = dev->data;
	uint8_t version;
	int ret;

	if (!device_is_ready(cfg->uart) || !gpio_is_ready_dt(&cfg->reset)) {
		LOG_ERR("UART or reset GPIO not ready");
		return -ENODEV;
	}

	k_msgq_init(&data->rx_queue, data->rx_buffer, sizeof(uint8_t), WS1850T_RX_QUEUE_LEN);
	k_sem_init(&data->tx_done, 0, 1);
	k_mutex_init(&data->lock);

	ret = ws1850t_hw_reset(dev);
	if (ret < 0) {
		LOG_ERR("hardware reset failed: %d", ret);
		return ret;
	}

	ret = uart_irq_callback_user_data_set(cfg->uart, cfg->cb, (void *)dev);
	if (ret < 0) {
		return ret;
	}
	uart_irq_rx_enable(cfg->uart);
	k_sleep(K_MSEC(20));

	ret = ws1850t_configure(dev);
	if (ret < 0) {
		LOG_ERR("initialisation failed: %d", ret);
		return ret;
	}

	ret = ws1850t_read_reg(dev, REG_VERSION, &version);
	if (ret == 0) {
		LOG_INF("WS1850T version 0x%02x", version);
	}

	return 0;
}

#define WS1850T_INIT(inst)								\
	static struct ws1850t_data ws1850t_data_##inst;					\
											\
	static const struct ws1850t_config ws1850t_config_##inst = {			\
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),				\
		.reset = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),			\
		.cb = ws1850t_uart_isr,							\
	};										\
											\
	DEVICE_DT_INST_DEFINE(inst, ws1850t_init, NULL,					\
			      &ws1850t_data_##inst, &ws1850t_config_##inst,		\
			      POST_KERNEL, CONFIG_WS1850T_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(WS1850T_INIT)
