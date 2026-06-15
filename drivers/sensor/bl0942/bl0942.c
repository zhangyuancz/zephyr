/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shanghai Belling BL0942 single-phase energy metering IC, UART interface.
 * Datasheet: BL0942 datasheet v1.07.
 */

#define DT_DRV_COMPAT belling_bl0942

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/drivers/sensor/bl0942.h>

LOG_MODULE_REGISTER(bl0942, CONFIG_SENSOR_LOG_LEVEL);

/* Full electrical-parameter read: command identifier + packet address (3.2.5). */
#define BL0942_CMD_READ_ID	0x58U
#define BL0942_CMD_PACKET	0xAAU

/* Returned full data packet (3.2.5). */
#define BL0942_PACKET_LEN	23U
#define BL0942_PACKET_HEAD	0x55U

/* Byte offsets inside the packet. */
#define BL0942_OFF_I_RMS	1U
#define BL0942_OFF_V_RMS	4U
#define BL0942_OFF_I_FAST_RMS	7U
#define BL0942_OFF_WATT		10U
#define BL0942_OFF_CF_CNT	13U
#define BL0942_OFF_FREQ		16U
#define BL0942_OFF_STATUS	19U
#define BL0942_OFF_CHECKSUM	22U

/*
 * Fixed conversion constants (datasheet typical values, 1.218 V reference):
 *   I_RMS = 305978 * I_pin(mV) / Vref      (2.5)
 *   V_RMS = 73989  * V_pin(mV) / Vref      (2.5)
 *   WATT  = 3537   * I_pin * V_pin * cos / Vref^2 (2.2)
 *   freq  = 2 * 500kHz / FREQ_reg          (2.8)
 * Vref is folded in as integer milli/micro factors below.
 */
#define BL0942_VREF_MILLI	1218U	/* 1.218 V */
#define BL0942_I_RMS_COEFF	305978U
#define BL0942_V_RMS_COEFF	73989U
#define BL0942_WATT_VREF2	1483524U /* (1.218^2) * 1e6 */
#define BL0942_WATT_COEFF	3537U
#define BL0942_FREQ_NUM		1000000000000ULL /* 1e6 * 1e6, micro-Hz numerator */

#define BL0942_RESP_TIMEOUT	K_MSEC(200)

struct bl0942_config {
	const struct device *uart;
	uart_irq_callback_user_data_t cb;
	uint32_t v_divider;
	uint32_t shunt_uohm;
};

struct bl0942_data {
	struct k_sem bus_sem; /* serialises transactions */
	struct k_sem rx_sem;  /* signalled when a full packet is received */

	uint8_t tx_buf[2];
	uint8_t tx_pos;
	uint8_t rx_buf[BL0942_PACKET_LEN];
	uint8_t rx_pos;

	/* Latest decoded registers. */
	uint32_t i_rms;
	uint32_t v_rms;
	int32_t watt;
	uint32_t cf_cnt;
	uint16_t freq;
	uint8_t status;
};

static void bl0942_uart_isr(const struct device *uart, void *user_data)
{
	const struct device *dev = user_data;
	struct bl0942_data *data = dev->data;

	uart_irq_update(uart);

	while (uart_irq_rx_ready(uart)) {
		data->rx_pos += uart_fifo_read(uart, &data->rx_buf[data->rx_pos],
					       BL0942_PACKET_LEN - data->rx_pos);

		if (data->rx_pos >= BL0942_PACKET_LEN) {
			uart_irq_rx_disable(uart);
			k_sem_give(&data->rx_sem);
			break;
		}
	}

	if (uart_irq_tx_ready(uart)) {
		if (data->tx_pos < sizeof(data->tx_buf)) {
			data->tx_pos += uart_fifo_fill(uart, &data->tx_buf[data->tx_pos],
						       sizeof(data->tx_buf) - data->tx_pos);
		}

		if (data->tx_pos >= sizeof(data->tx_buf)) {
			uart_irq_tx_disable(uart);
		}
	}
}

static bool bl0942_checksum_ok(const uint8_t *packet)
{
	uint8_t sum = BL0942_CMD_READ_ID;

	/* checksum = ~(cmd_id + head + data[1..21]) (3.2.5). */
	for (uint8_t i = 0U; i < BL0942_OFF_CHECKSUM; i++) {
		sum += packet[i];
	}

	return (uint8_t)~sum == packet[BL0942_OFF_CHECKSUM];
}

static int32_t bl0942_sign_extend_24(uint32_t value)
{
	if (value & BIT(23)) {
		value |= 0xFF000000U;
	}

	return (int32_t)value;
}

static int bl0942_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct bl0942_config *cfg = dev->config;
	struct bl0942_data *data = dev->data;
	uint8_t byte;
	int ret;

	if (chan != SENSOR_CHAN_ALL) {
		return -ENOTSUP;
	}

	ret = k_sem_take(&data->bus_sem, BL0942_RESP_TIMEOUT);
	if (ret < 0) {
		return ret;
	}

	/* Drain any stale RX bytes before issuing the request. */
	uart_irq_rx_disable(cfg->uart);
	while (uart_fifo_read(cfg->uart, &byte, 1) == 1) {
	}

	data->tx_buf[0] = BL0942_CMD_READ_ID;
	data->tx_buf[1] = BL0942_CMD_PACKET;
	data->tx_pos = 0U;
	data->rx_pos = 0U;
	k_sem_reset(&data->rx_sem);

	uart_irq_rx_enable(cfg->uart);
	uart_irq_tx_enable(cfg->uart);

	ret = k_sem_take(&data->rx_sem, BL0942_RESP_TIMEOUT);
	if (ret < 0) {
		uart_irq_tx_disable(cfg->uart);
		uart_irq_rx_disable(cfg->uart);
		LOG_DBG("response timeout");
		goto out;
	}

	if (data->rx_buf[0] != BL0942_PACKET_HEAD || !bl0942_checksum_ok(data->rx_buf)) {
		LOG_DBG("bad packet (head 0x%02x)", data->rx_buf[0]);
		ret = -EBADMSG;
		goto out;
	}

	data->i_rms = sys_get_le24(&data->rx_buf[BL0942_OFF_I_RMS]);
	data->v_rms = sys_get_le24(&data->rx_buf[BL0942_OFF_V_RMS]);
	data->watt = bl0942_sign_extend_24(sys_get_le24(&data->rx_buf[BL0942_OFF_WATT]));
	data->cf_cnt = sys_get_le24(&data->rx_buf[BL0942_OFF_CF_CNT]);
	data->freq = sys_get_le16(&data->rx_buf[BL0942_OFF_FREQ]);
	data->status = data->rx_buf[BL0942_OFF_STATUS];
	ret = 0;

out:
	k_sem_give(&data->bus_sem);
	return ret;
}

static void bl0942_micro_to_val(int64_t micro, struct sensor_value *val)
{
	val->val1 = (int32_t)(micro / 1000000);
	val->val2 = (int32_t)(micro % 1000000);
}

static int bl0942_channel_get(const struct device *dev, enum sensor_channel chan,
			      struct sensor_value *val)
{
	const struct bl0942_config *cfg = dev->config;
	struct bl0942_data *data = dev->data;
	int64_t micro;

	switch ((int)chan) {
	case SENSOR_CHAN_VOLTAGE:
		/* uV = V_RMS * Vref_milli * divider / V_RMS_COEFF */
		micro = (int64_t)((uint64_t)data->v_rms * BL0942_VREF_MILLI * cfg->v_divider /
				  BL0942_V_RMS_COEFF);
		break;
	case SENSOR_CHAN_CURRENT:
		/* uA = I_RMS * Vref_milli * 1e6 / (I_RMS_COEFF * shunt_uohm) */
		micro = (int64_t)((uint64_t)data->i_rms * BL0942_VREF_MILLI * 1000000ULL /
				  ((uint64_t)BL0942_I_RMS_COEFF * cfg->shunt_uohm));
		break;
	case SENSOR_CHAN_POWER:
		/* uW = WATT * Vref^2(1e6) * divider / (WATT_COEFF * shunt_uohm) */
		micro = (int64_t)data->watt * BL0942_WATT_VREF2 * cfg->v_divider /
			((int64_t)BL0942_WATT_COEFF * cfg->shunt_uohm);
		break;
	case SENSOR_CHAN_FREQUENCY:
		/* uHz = 2 * 500kHz / FREQ_reg, expressed in micro-Hz. */
		micro = (data->freq != 0U) ? (int64_t)(BL0942_FREQ_NUM / data->freq) : 0;
		break;
	case SENSOR_CHAN_BL0942_CF_CNT:
		val->val1 = (int32_t)data->cf_cnt;
		val->val2 = 0;
		return 0;
	default:
		return -ENOTSUP;
	}

	bl0942_micro_to_val(micro, val);
	return 0;
}

static DEVICE_API(sensor, bl0942_api) = {
	.sample_fetch = bl0942_sample_fetch,
	.channel_get = bl0942_channel_get,
};

static int bl0942_init(const struct device *dev)
{
	const struct bl0942_config *cfg = dev->config;
	struct bl0942_data *data = dev->data;
	uint8_t byte;

	if (!device_is_ready(cfg->uart)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	k_sem_init(&data->bus_sem, 1, 1);
	k_sem_init(&data->rx_sem, 0, 1);

	uart_irq_rx_disable(cfg->uart);
	uart_irq_tx_disable(cfg->uart);
	while (uart_fifo_read(cfg->uart, &byte, 1) == 1) {
	}

	return uart_irq_callback_user_data_set(cfg->uart, cfg->cb, (void *)dev);
}

#define BL0942_INIT(inst)								\
	static struct bl0942_data bl0942_data_##inst;					\
											\
	static const struct bl0942_config bl0942_config_##inst = {			\
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),				\
		.cb = bl0942_uart_isr,							\
		.v_divider = DT_INST_PROP(inst, voltage_divider_ratio),			\
		.shunt_uohm = DT_INST_PROP(inst, current_shunt_microohm),		\
	};										\
											\
	SENSOR_DEVICE_DT_INST_DEFINE(inst, bl0942_init, NULL,				\
				     &bl0942_data_##inst, &bl0942_config_##inst,	\
				     POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,		\
				     &bl0942_api);

DT_INST_FOREACH_STATUS_OKAY(BL0942_INIT)
