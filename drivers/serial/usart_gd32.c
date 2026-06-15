/*
 * Copyright (c) 2021, ATL Electronics
 * Copyright (c) 2025 Aleksandr Senin <al@meshium.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gd_gd32_usart

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#ifdef CONFIG_UART_ASYNC_API
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_gd32.h>
#endif
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>

#include <gd32_usart.h>

#ifdef CONFIG_UART_ASYNC_API
struct gd32_usart_dma_config {
	const struct device *dev;
	uint32_t channel;
	uint32_t slot;
	uint32_t config;
};
#endif

struct gd32_usart_config {
	uint32_t reg;
	uint16_t clkid;
	struct reset_dt_spec reset;
	const struct pinctrl_dev_config *pcfg;
	uint32_t parity;
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_UART_ASYNC_API)
	uart_irq_config_func_t irq_config_func;
#endif
#ifdef CONFIG_UART_ASYNC_API
	struct gd32_usart_dma_config dma_rx;
	struct gd32_usart_dma_config dma_tx;
#endif
};

struct gd32_usart_data {
	uint32_t baud_rate;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_user_data_t user_cb;
	void *user_data;
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
#ifdef CONFIG_UART_ASYNC_API
	const struct device *dev;
	struct k_spinlock async_lock;
	uart_callback_t async_cb;
	void *async_user_data;
	struct {
		const uint8_t *buf;
		size_t len;
		bool active;
	} tx;
	struct {
		uint8_t *buf;
		size_t len;
		size_t offset;
		uint8_t *next_buf;
		size_t next_len;
		bool active;
	} rx;
#endif
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	enum uart_config_parity parity;
	enum uart_config_stop_bits stop_bits;
	enum uart_config_data_bits data_bits;
	enum uart_config_flow_control flow_ctrl;
	bool initialized;
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */
};

#ifdef CONFIG_UART_ASYNC_API
static void usart_gd32_async_rx_idle(const struct device *dev);
#endif /* CONFIG_UART_ASYNC_API */

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_UART_ASYNC_API)
static void usart_gd32_isr(const struct device *dev)
{
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	struct gd32_usart_data *const data = dev->data;

	if (data->user_cb) {
		data->user_cb(dev, data->user_data);
	}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

#ifdef CONFIG_UART_ASYNC_API
	const struct gd32_usart_config *const cfg = dev->config;

	/* RX line idle: flush whatever the DMA has landed so far. The IDLE
	 * flag has no write-clear bit; it is cleared by reading STAT0 (done by
	 * usart_interrupt_flag_get) followed by a read of DATA.
	 */
	if (usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_IDLE) == SET) {
		(void)usart_data_receive(cfg->reg);
		usart_gd32_async_rx_idle(dev);
	}
#endif /* CONFIG_UART_ASYNC_API */
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN || CONFIG_UART_ASYNC_API */

#ifdef CONFIG_UART_ASYNC_API
static void usart_gd32_async_rx_dma_done(const struct device *dma_dev,
					 void *user_data, uint32_t channel,
					 int status);
static void usart_gd32_async_tx_dma_done(const struct device *dma_dev,
					 void *user_data, uint32_t channel,
					 int status);

static bool usart_gd32_async_supported(const struct gd32_usart_config *cfg)
{
	return cfg->dma_rx.dev != NULL && cfg->dma_tx.dev != NULL;
}

static void usart_gd32_async_event(const struct device *dev, struct uart_event *event)
{
	struct gd32_usart_data *data = dev->data;
	uart_callback_t callback;
	void *user_data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->async_lock);
	callback = data->async_cb;
	user_data = data->async_user_data;
	k_spin_unlock(&data->async_lock, key);

	if (callback != NULL) {
		callback(dev, event, user_data);
	}
}

static int usart_gd32_dma_configure(const struct device *dev, bool tx,
				    const uint8_t *buf, size_t len,
				    dma_callback_t callback)
{
	const struct gd32_usart_config *cfg = dev->config;
	const struct gd32_usart_dma_config *dma = tx ? &cfg->dma_tx : &cfg->dma_rx;
	struct dma_block_config block = {0};
	struct dma_config dma_cfg = {0};

	dma_cfg.channel_direction = tx ? MEMORY_TO_PERIPHERAL : PERIPHERAL_TO_MEMORY;
	dma_cfg.source_data_size = 1U;
	dma_cfg.dest_data_size = 1U;
	dma_cfg.source_burst_length = 1U;
	dma_cfg.dest_burst_length = 1U;
	dma_cfg.channel_priority = GD32_DMA_CONFIG_PRIORITY(dma->config);
	dma_cfg.dma_slot = dma->slot;
	dma_cfg.block_count = 1U;
	dma_cfg.head_block = &block;
	dma_cfg.dma_callback = callback;
	dma_cfg.user_data = (void *)dev;

	block.block_size = len;
	if (tx) {
		block.source_address = (uintptr_t)buf;
		block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		block.dest_address = (uintptr_t)&USART_DATA(cfg->reg);
		block.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	} else {
		block.source_address = (uintptr_t)&USART_DATA(cfg->reg);
		block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
		block.dest_address = (uintptr_t)buf;
		block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	}

	return dma_config(dma->dev, dma->channel, &dma_cfg);
}

static int usart_gd32_async_rx_start(const struct device *dev, uint8_t *buf,
				     size_t len)
{
	const struct gd32_usart_config *cfg = dev->config;
	int ret;

	ret = usart_gd32_dma_configure(dev, false, buf, len,
				       usart_gd32_async_rx_dma_done);
	if (ret < 0) {
		return ret;
	}

	ret = dma_start(cfg->dma_rx.dev, cfg->dma_rx.channel);
	if (ret < 0) {
		return ret;
	}

	usart_dma_receive_config(cfg->reg, USART_DENR_ENABLE);
	/* Clear any stale IDLE flag (read STAT0 + DATA) before arming the
	 * IDLE interrupt, so we do not take a spurious idle event at start.
	 */
	(void)usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_IDLE);
	(void)usart_data_receive(cfg->reg);
	usart_interrupt_enable(cfg->reg, USART_INT_IDLE);
	return 0;
}

static void usart_gd32_async_rx_dma_done(const struct device *dma_dev,
					 void *user_data, uint32_t channel,
					 int status)
{
	const struct device *dev = user_data;
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	struct uart_event event = {0};
	uint8_t *released;
	uint8_t *next_buf;
	size_t current_len;
	size_t current_offset;
	size_t next_len;
	k_spinlock_key_t key;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	usart_dma_receive_config(cfg->reg, USART_DENR_DISABLE);
	usart_interrupt_disable(cfg->reg, USART_INT_IDLE);

	key = k_spin_lock(&data->async_lock);
	if (!data->rx.active) {
		k_spin_unlock(&data->async_lock, key);
		return;
	}
	released = data->rx.buf;
	current_len = data->rx.len;
	current_offset = data->rx.offset;
	next_buf = data->rx.next_buf;
	next_len = data->rx.next_len;
	data->rx.buf = next_buf;
	data->rx.len = next_len;
	data->rx.offset = 0U;
	data->rx.next_buf = NULL;
	data->rx.next_len = 0U;
	data->rx.active = next_buf != NULL && status == 0;
	k_spin_unlock(&data->async_lock, key);

	if (status == 0 && current_len > current_offset) {
		event.type = UART_RX_RDY;
		event.data.rx.buf = released;
		event.data.rx.offset = current_offset;
		event.data.rx.len = current_len - current_offset;
		usart_gd32_async_event(dev, &event);
	}

	event.type = UART_RX_BUF_RELEASED;
	event.data.rx_buf.buf = released;
	usart_gd32_async_event(dev, &event);

	if (data->rx.active) {
		if (usart_gd32_async_rx_start(dev, next_buf, next_len) < 0) {
			key = k_spin_lock(&data->async_lock);
			data->rx.active = false;
			k_spin_unlock(&data->async_lock, key);
		} else {
			event.type = UART_RX_BUF_REQUEST;
			usart_gd32_async_event(dev, &event);
			return;
		}
	}

	event.type = status == 0 ? UART_RX_DISABLED : UART_RX_STOPPED;
	if (status != 0) {
		event.data.rx_stop.reason = UART_ERROR_OVERRUN;
		event.data.rx_stop.data.buf = released;
		event.data.rx_stop.data.offset = current_offset;
		event.data.rx_stop.data.len = current_len - current_offset;
	}
	usart_gd32_async_event(dev, &event);
}

static void usart_gd32_async_tx_dma_done(const struct device *dma_dev,
					 void *user_data, uint32_t channel,
					 int status)
{
	const struct device *dev = user_data;
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	struct uart_event event = {0};
	k_spinlock_key_t key;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	usart_dma_transmit_config(cfg->reg, USART_DENT_DISABLE);

	key = k_spin_lock(&data->async_lock);
	event.data.tx.buf = data->tx.buf;
	event.data.tx.len = status == 0 ? data->tx.len : 0U;
	data->tx.active = false;
	data->tx.buf = NULL;
	data->tx.len = 0U;
	k_spin_unlock(&data->async_lock, key);

	event.type = status == 0 ? UART_TX_DONE : UART_TX_ABORTED;
	usart_gd32_async_event(dev, &event);
}

/* Called from the USART IDLE interrupt: the RX line has gone quiet, so deliver
 * whatever the RX DMA has landed since the last delivery. Runs in ISR context.
 */
static void usart_gd32_async_rx_idle(const struct device *dev)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	struct dma_status status;
	struct uart_event event = {0};
	size_t received;
	size_t offset;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->async_lock);
	if (!data->rx.active) {
		k_spin_unlock(&data->async_lock, key);
		return;
	}
	if (dma_get_status(cfg->dma_rx.dev, cfg->dma_rx.channel, &status) < 0) {
		k_spin_unlock(&data->async_lock, key);
		return;
	}
	received = data->rx.len - status.pending_length;
	offset = data->rx.offset;
	data->rx.offset = received;
	k_spin_unlock(&data->async_lock, key);

	if (received > offset) {
		event.type = UART_RX_RDY;
		event.data.rx.buf = data->rx.buf;
		event.data.rx.offset = offset;
		event.data.rx.len = received - offset;
		usart_gd32_async_event(dev, &event);
	}
}
#endif /* CONFIG_UART_ASYNC_API */

static int usart_gd32_init(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;
	struct gd32_usart_data *const data = dev->data;
	uint32_t word_length;
	uint32_t parity;
	int ret;

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	/**
	 * In order to keep the transfer data size to 8 bits(1 byte),
	 * append word length to 9BIT if parity bit enabled.
	 */
	switch (cfg->parity) {
	case UART_CFG_PARITY_NONE:
		parity = USART_PM_NONE;
		word_length = USART_WL_8BIT;
		break;
	case UART_CFG_PARITY_ODD:
		parity = USART_PM_ODD;
		word_length = USART_WL_9BIT;
		break;
	case UART_CFG_PARITY_EVEN:
		parity = USART_PM_EVEN;
		word_length = USART_WL_9BIT;
		break;
	default:
		return -ENOTSUP;
	}

	(void)clock_control_on(GD32_CLOCK_CONTROLLER,
			       (clock_control_subsys_t)&cfg->clkid);

	(void)reset_line_toggle_dt(&cfg->reset);

	usart_baudrate_set(cfg->reg, data->baud_rate);
	usart_parity_config(cfg->reg, parity);
	usart_word_length_set(cfg->reg, word_length);
	/* Default to 1 stop bit */
	usart_stop_bit_set(cfg->reg, USART_STB_1BIT);
	usart_receive_config(cfg->reg, USART_RECEIVE_ENABLE);
	usart_transmit_config(cfg->reg, USART_TRANSMIT_ENABLE);
	usart_enable(cfg->reg);

#ifdef CONFIG_UART_ASYNC_API
	data->dev = dev;
#endif

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_UART_ASYNC_API)
	cfg->irq_config_func(dev);
#endif
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	/* Initialize runtime configuration from Devicetree defaults */
	data->parity = cfg->parity;
	data->data_bits = UART_CFG_DATA_BITS_8;
	data->stop_bits = UART_CFG_STOP_BITS_1;
	data->flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
	data->initialized = true;
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */
	return 0;
}

static int usart_gd32_poll_in(const struct device *dev, unsigned char *c)
{
	const struct gd32_usart_config *const cfg = dev->config;
	uint32_t status;

	status = usart_flag_get(cfg->reg, USART_FLAG_RBNE);

	if (!status) {
		return -EPERM;
	}

	*c = usart_data_receive(cfg->reg);

	return 0;
}

static void usart_gd32_poll_out(const struct device *dev, unsigned char c)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_data_transmit(cfg->reg, c);

	while (usart_flag_get(cfg->reg, USART_FLAG_TBE) == RESET) {
		;
	}
}

static int usart_gd32_err_check(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;
	int errors = 0;
	bool need_data_read = false;

	/*
	 * Use HAL usart_flag_get() instead of raw register reads.
	 * The HAL flag enums encode (register_offset << 6) | bit_position,
	 * NOT raw bitmasks. Direct comparison against USART_STAT() register
	 * values would compare against the wrong bits (e.g. USART_FLAG_ORERR
	 * is 3, not BIT(3)=8, causing ORERR to never be detected).
	 *
	 * FERR, NERR, and ORERR are cleared by the standard USART sequence:
	 * read STAT0, then read DATA (matching the GD32F527 reference demo).
	 * PERR is cleared via usart_flag_clear write to STAT0.
	 */
	if (usart_flag_get(cfg->reg, USART_FLAG_ORERR)) {
		need_data_read = true;
		errors |= UART_ERROR_OVERRUN;
	}

	if (usart_flag_get(cfg->reg, USART_FLAG_PERR)) {
		usart_flag_clear(cfg->reg, USART_FLAG_PERR);

		errors |= UART_ERROR_PARITY;
	}

	if (usart_flag_get(cfg->reg, USART_FLAG_FERR)) {
		need_data_read = true;
		errors |= UART_ERROR_FRAMING;
	}

	if (usart_flag_get(cfg->reg, USART_FLAG_NERR)) {
		need_data_read = true;
	}

	/* Clear FERR/NERR/ORERR: read STAT0 (done by usart_flag_get above)
	 * then read DATA to complete the clear sequence.
	 */
	if (need_data_read) {
		(void)usart_data_receive(cfg->reg);
	}

	return errors;
}

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
static int usart_gd32_configure(const struct device *dev, const struct uart_config *cfg_new)
{
	const struct gd32_usart_config *const cfg = dev->config;
	struct gd32_usart_data *const data = dev->data;
	uint32_t parity_bits;
	uint32_t word_length;
	uint32_t stop_bits_hw;

	if (cfg_new == NULL) {
		return -EINVAL;
	}

	if (cfg_new->baudrate == 0U) {
		return -EINVAL;
	}

	if (cfg_new->flow_ctrl != UART_CFG_FLOW_CTRL_NONE) {
		return -ENOTSUP;
	}

	switch (cfg_new->parity) {
	case UART_CFG_PARITY_NONE:
		parity_bits = USART_PM_NONE;
		break;
	case UART_CFG_PARITY_ODD:
		parity_bits = USART_PM_ODD;
		break;
	case UART_CFG_PARITY_EVEN:
		parity_bits = USART_PM_EVEN;
		break;
	default:
		return -EINVAL;
	}

	switch (cfg_new->data_bits) {
	case UART_CFG_DATA_BITS_8:
	case UART_CFG_DATA_BITS_7:
		break;
	default:
		return -EINVAL;
	}

	if (cfg_new->data_bits == UART_CFG_DATA_BITS_7 && cfg_new->parity == UART_CFG_PARITY_NONE) {
		return -EINVAL;
	}

	/* Map word length depending on requested data bits and parity */
	if (cfg_new->parity == UART_CFG_PARITY_NONE) {
		/* 8N* uses 8-bit word length */
		word_length = USART_WL_8BIT;
	} else {
		/* With parity: 8 data bits -> 9-bit word length, 7 data bits -> 8-bit */
		word_length = (cfg_new->data_bits == UART_CFG_DATA_BITS_8) ? USART_WL_9BIT
									   : USART_WL_8BIT;
	}

	switch (cfg_new->stop_bits) {
	case UART_CFG_STOP_BITS_1:
		stop_bits_hw = USART_STB_1BIT;
		break;
	case UART_CFG_STOP_BITS_2:
		stop_bits_hw = USART_STB_2BIT;
		break;
	default:
		return -EINVAL;
	}

	if (data->baud_rate == cfg_new->baudrate && data->parity == cfg_new->parity &&
	    data->data_bits == cfg_new->data_bits && data->stop_bits == cfg_new->stop_bits &&
	    data->flow_ctrl == cfg_new->flow_ctrl) {
		return 0;
	}

	unsigned int key = irq_lock();

	usart_disable(cfg->reg);

	usart_parity_config(cfg->reg, parity_bits);
	usart_word_length_set(cfg->reg, word_length);
	usart_stop_bit_set(cfg->reg, stop_bits_hw);
	usart_baudrate_set(cfg->reg, cfg_new->baudrate);

	usart_receive_config(cfg->reg, USART_RECEIVE_ENABLE);
	usart_transmit_config(cfg->reg, USART_TRANSMIT_ENABLE);
	usart_enable(cfg->reg);

	irq_unlock(key);

	data->baud_rate = cfg_new->baudrate;
	data->parity = cfg_new->parity;
	data->data_bits = cfg_new->data_bits;
	data->stop_bits = cfg_new->stop_bits;
	data->flow_ctrl = cfg_new->flow_ctrl;

	return 0;
}

static int usart_gd32_config_get(const struct device *dev, struct uart_config *cfg_out)
{
	struct gd32_usart_data *const data = dev->data;

	if (cfg_out == NULL) {
		return -EINVAL;
	}

	if (!data->initialized) {
		return -ENODEV;
	}

	cfg_out->baudrate = data->baud_rate;
	cfg_out->parity = data->parity;
	cfg_out->stop_bits = data->stop_bits;
	cfg_out->data_bits = data->data_bits;
	cfg_out->flow_ctrl = data->flow_ctrl;

	return 0;
}
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
int usart_gd32_fifo_fill(const struct device *dev, const uint8_t *tx_data,
			 int len)
{
	const struct gd32_usart_config *const cfg = dev->config;
	int num_tx = 0U;

	while ((len - num_tx > 0) &&
	       usart_flag_get(cfg->reg, USART_FLAG_TBE)) {
		usart_data_transmit(cfg->reg, tx_data[num_tx++]);
	}

	return num_tx;
}

int usart_gd32_fifo_read(const struct device *dev, uint8_t *rx_data,
			 const int size)
{
	const struct gd32_usart_config *const cfg = dev->config;
	int num_rx = 0U;

	while ((size - num_rx > 0) &&
	       usart_flag_get(cfg->reg, USART_FLAG_RBNE)) {
		rx_data[num_rx++] = usart_data_receive(cfg->reg);
	}

	return num_rx;
}

void usart_gd32_irq_tx_enable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_enable(cfg->reg, USART_INT_TC);
}

void usart_gd32_irq_tx_disable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_disable(cfg->reg, USART_INT_TC);
}

int usart_gd32_irq_tx_ready(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	return usart_flag_get(cfg->reg, USART_FLAG_TBE) &&
	       usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_TC);
}

int usart_gd32_irq_tx_complete(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	return usart_flag_get(cfg->reg, USART_FLAG_TC);
}

void usart_gd32_irq_rx_enable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_enable(cfg->reg, USART_INT_RBNE);
}

void usart_gd32_irq_rx_disable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_disable(cfg->reg, USART_INT_RBNE);
}

int usart_gd32_irq_rx_ready(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	return usart_flag_get(cfg->reg, USART_FLAG_RBNE);
}

void usart_gd32_irq_err_enable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_enable(cfg->reg, USART_INT_ERR);
	usart_interrupt_enable(cfg->reg, USART_INT_PERR);
}

void usart_gd32_irq_err_disable(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	usart_interrupt_disable(cfg->reg, USART_INT_ERR);
	usart_interrupt_disable(cfg->reg, USART_INT_PERR);
}

int usart_gd32_irq_is_pending(const struct device *dev)
{
	const struct gd32_usart_config *const cfg = dev->config;

	return ((usart_flag_get(cfg->reg, USART_FLAG_RBNE) &&
		 usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_RBNE)) ||
		(usart_flag_get(cfg->reg, USART_FLAG_TC) &&
		 usart_interrupt_flag_get(cfg->reg, USART_INT_FLAG_TC)));
}

void usart_gd32_irq_callback_set(const struct device *dev,
				 uart_irq_callback_user_data_t cb,
				 void *user_data)
{
	struct gd32_usart_data *const data = dev->data;

	data->user_cb = cb;
	data->user_data = user_data;
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

#ifdef CONFIG_UART_ASYNC_API
static int usart_gd32_async_callback_set(const struct device *dev,
					 uart_callback_t callback,
					 void *user_data)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	k_spinlock_key_t key;

	if (!usart_gd32_async_supported(cfg)) {
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->async_lock);
	data->async_cb = callback;
	data->async_user_data = user_data;
	k_spin_unlock(&data->async_lock, key);
	return 0;
}

static int usart_gd32_async_tx(const struct device *dev, const uint8_t *buf,
			       size_t len, int32_t timeout)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	ARG_UNUSED(timeout);

	if (!usart_gd32_async_supported(cfg)) {
		return -ENOTSUP;
	}
	if (buf == NULL || len == 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->async_lock);
	if (data->tx.active) {
		k_spin_unlock(&data->async_lock, key);
		return -EBUSY;
	}
	data->tx.buf = buf;
	data->tx.len = len;
	data->tx.active = true;
	k_spin_unlock(&data->async_lock, key);

	ret = usart_gd32_dma_configure(dev, true, buf, len,
				       usart_gd32_async_tx_dma_done);
	if (ret == 0) {
		ret = dma_start(cfg->dma_tx.dev, cfg->dma_tx.channel);
	}
	if (ret < 0) {
		key = k_spin_lock(&data->async_lock);
		data->tx.active = false;
		data->tx.buf = NULL;
		data->tx.len = 0U;
		k_spin_unlock(&data->async_lock, key);
		return ret;
	}

	usart_dma_transmit_config(cfg->reg, USART_DENT_ENABLE);
	return 0;
}

static int usart_gd32_async_tx_abort(const struct device *dev)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	struct dma_status status;
	struct uart_event event = {0};
	k_spinlock_key_t key;
	size_t sent;

	key = k_spin_lock(&data->async_lock);
	if (!data->tx.active) {
		k_spin_unlock(&data->async_lock, key);
		return -EFAULT;
	}
	(void)dma_get_status(cfg->dma_tx.dev, cfg->dma_tx.channel, &status);
	usart_dma_transmit_config(cfg->reg, USART_DENT_DISABLE);
	(void)dma_stop(cfg->dma_tx.dev, cfg->dma_tx.channel);
	sent = data->tx.len - MIN(data->tx.len, status.pending_length);
	event.data.tx.buf = data->tx.buf;
	event.data.tx.len = sent;
	data->tx.active = false;
	data->tx.buf = NULL;
	data->tx.len = 0U;
	k_spin_unlock(&data->async_lock, key);

	event.type = UART_TX_ABORTED;
	usart_gd32_async_event(dev, &event);
	return 0;
}

static int usart_gd32_async_rx_enable(const struct device *dev, uint8_t *buf,
				      size_t len, int32_t timeout)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	struct uart_event event = {.type = UART_RX_BUF_REQUEST};
	k_spinlock_key_t key;
	int ret;

	/* RX-ready delivery is driven by the hardware IDLE-line interrupt (and
	 * the DMA buffer-full callback as the back-stop for continuous streams),
	 * so the caller-supplied timeout is not used.
	 */
	ARG_UNUSED(timeout);

	if (!usart_gd32_async_supported(cfg)) {
		return -ENOTSUP;
	}
	if (buf == NULL || len == 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->async_lock);
	if (data->rx.active) {
		k_spin_unlock(&data->async_lock, key);
		return -EBUSY;
	}
	data->rx.buf = buf;
	data->rx.len = len;
	data->rx.offset = 0U;
	data->rx.next_buf = NULL;
	data->rx.next_len = 0U;
	data->rx.active = true;
	k_spin_unlock(&data->async_lock, key);

	ret = usart_gd32_async_rx_start(dev, buf, len);
	if (ret < 0) {
		key = k_spin_lock(&data->async_lock);
		data->rx.active = false;
		k_spin_unlock(&data->async_lock, key);
		return ret;
	}

	usart_gd32_async_event(dev, &event);
	return 0;
}

static int usart_gd32_async_rx_buf_rsp(const struct device *dev, uint8_t *buf,
				       size_t len)
{
	struct gd32_usart_data *data = dev->data;
	k_spinlock_key_t key;
	int ret = 0;

	if (buf == NULL || len == 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->async_lock);
	if (!data->rx.active) {
		ret = -EACCES;
	} else if (data->rx.next_buf != NULL) {
		ret = -EBUSY;
	} else {
		data->rx.next_buf = buf;
		data->rx.next_len = len;
	}
	k_spin_unlock(&data->async_lock, key);
	return ret;
}

static int usart_gd32_async_rx_disable(const struct device *dev)
{
	const struct gd32_usart_config *cfg = dev->config;
	struct gd32_usart_data *data = dev->data;
	struct dma_status status = {0};
	struct uart_event event = {0};
	uint8_t *buf;
	uint8_t *next_buf;
	size_t received;
	size_t offset;
	k_spinlock_key_t key;

	usart_interrupt_disable(cfg->reg, USART_INT_IDLE);

	key = k_spin_lock(&data->async_lock);
	if (!data->rx.active) {
		k_spin_unlock(&data->async_lock, key);
		return -EFAULT;
	}
	(void)dma_get_status(cfg->dma_rx.dev, cfg->dma_rx.channel, &status);
	usart_dma_receive_config(cfg->reg, USART_DENR_DISABLE);
	(void)dma_stop(cfg->dma_rx.dev, cfg->dma_rx.channel);
	buf = data->rx.buf;
	next_buf = data->rx.next_buf;
	received = data->rx.len - MIN(data->rx.len, status.pending_length);
	offset = data->rx.offset;
	data->rx.active = false;
	data->rx.buf = NULL;
	data->rx.len = 0U;
	data->rx.next_buf = NULL;
	data->rx.next_len = 0U;
	k_spin_unlock(&data->async_lock, key);

	if (received > offset) {
		event.type = UART_RX_RDY;
		event.data.rx.buf = buf;
		event.data.rx.offset = offset;
		event.data.rx.len = received - offset;
		usart_gd32_async_event(dev, &event);
	}
	event.type = UART_RX_BUF_RELEASED;
	event.data.rx_buf.buf = buf;
	usart_gd32_async_event(dev, &event);
	if (next_buf != NULL) {
		event.data.rx_buf.buf = next_buf;
		usart_gd32_async_event(dev, &event);
	}
	event.type = UART_RX_DISABLED;
	usart_gd32_async_event(dev, &event);
	return 0;
}
#endif /* CONFIG_UART_ASYNC_API */

static DEVICE_API(uart, usart_gd32_driver_api) = {
	.poll_in = usart_gd32_poll_in,
	.poll_out = usart_gd32_poll_out,
	.err_check = usart_gd32_err_check,
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	.configure = usart_gd32_configure,
	.config_get = usart_gd32_config_get,
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = usart_gd32_fifo_fill,
	.fifo_read = usart_gd32_fifo_read,
	.irq_tx_enable = usart_gd32_irq_tx_enable,
	.irq_tx_disable = usart_gd32_irq_tx_disable,
	.irq_tx_ready = usart_gd32_irq_tx_ready,
	.irq_tx_complete = usart_gd32_irq_tx_complete,
	.irq_rx_enable = usart_gd32_irq_rx_enable,
	.irq_rx_disable = usart_gd32_irq_rx_disable,
	.irq_rx_ready = usart_gd32_irq_rx_ready,
	.irq_err_enable = usart_gd32_irq_err_enable,
	.irq_err_disable = usart_gd32_irq_err_disable,
	.irq_is_pending = usart_gd32_irq_is_pending,
	.irq_callback_set = usart_gd32_irq_callback_set,
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
#ifdef CONFIG_UART_ASYNC_API
	.callback_set = usart_gd32_async_callback_set,
	.tx = usart_gd32_async_tx,
	.tx_abort = usart_gd32_async_tx_abort,
	.rx_enable = usart_gd32_async_rx_enable,
	.rx_buf_rsp = usart_gd32_async_rx_buf_rsp,
	.rx_disable = usart_gd32_async_rx_disable,
#endif /* CONFIG_UART_ASYNC_API */
};

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_UART_ASYNC_API)
#define GD32_USART_IRQ_HANDLER(n)						\
	static void usart_gd32_config_func_##n(const struct device *dev)	\
	{									\
		IRQ_CONNECT(DT_INST_IRQN(n),					\
			    DT_INST_IRQ(n, priority),				\
			    usart_gd32_isr,					\
			    DEVICE_DT_INST_GET(n),				\
			    0);							\
		irq_enable(DT_INST_IRQN(n));					\
	}
#define GD32_USART_IRQ_HANDLER_FUNC_INIT(n)					\
	.irq_config_func = usart_gd32_config_func_##n,
#else
#define GD32_USART_IRQ_HANDLER(n)
#define GD32_USART_IRQ_HANDLER_FUNC_INIT(n)
#endif

#ifdef CONFIG_UART_ASYNC_API
#define GD32_USART_DMA_DEVICE(n, name)                                        \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, dmas),                           \
		    (DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, name))), (NULL))
#define GD32_USART_DMA_CELL(n, name, cell)                                    \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, dmas),                           \
		    (DT_INST_DMAS_CELL_BY_NAME(n, name, cell)), (0U))
#define GD32_USART_DMA_INIT(n)                                                \
	.dma_rx = {                                                            \
		.dev = GD32_USART_DMA_DEVICE(n, rx),                            \
		.channel = GD32_USART_DMA_CELL(n, rx, channel),                 \
		.slot = GD32_USART_DMA_CELL(n, rx, slot),                       \
		.config = GD32_USART_DMA_CELL(n, rx, config),                   \
	},                                                                       \
	.dma_tx = {                                                            \
		.dev = GD32_USART_DMA_DEVICE(n, tx),                            \
		.channel = GD32_USART_DMA_CELL(n, tx, channel),                 \
		.slot = GD32_USART_DMA_CELL(n, tx, slot),                       \
		.config = GD32_USART_DMA_CELL(n, tx, config),                   \
	},
#else
#define GD32_USART_DMA_INIT(n)
#endif /* CONFIG_UART_ASYNC_API */

#ifdef CONFIG_UART_ASYNC_API
#define GD32_USART_INIT_LEVEL POST_KERNEL
#else
#define GD32_USART_INIT_LEVEL PRE_KERNEL_1
#endif

#define GD32_USART_INIT(n)							\
	PINCTRL_DT_INST_DEFINE(n);						\
	GD32_USART_IRQ_HANDLER(n)						\
	static struct gd32_usart_data usart_gd32_data_##n = {			\
		.baud_rate = DT_INST_PROP(n, current_speed),			\
	};									\
	static const struct gd32_usart_config usart_gd32_config_##n = {		\
		.reg = DT_INST_REG_ADDR(n),					\
		.clkid = DT_INST_CLOCKS_CELL(n, id),				\
		.reset = RESET_DT_SPEC_INST_GET(n),				\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),			\
		.parity = DT_INST_ENUM_IDX(n, parity),				\
		GD32_USART_IRQ_HANDLER_FUNC_INIT(n)				\
		GD32_USART_DMA_INIT(n)						\
	};									\
	DEVICE_DT_INST_DEFINE(n, usart_gd32_init,				\
			      NULL,						\
			      &usart_gd32_data_##n,				\
			      &usart_gd32_config_##n, GD32_USART_INIT_LEVEL,	\
			      CONFIG_SERIAL_INIT_PRIORITY,			\
			      &usart_gd32_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GD32_USART_INIT)
