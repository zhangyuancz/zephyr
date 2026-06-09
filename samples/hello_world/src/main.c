/*
 * USART0 PB6/PB7 interrupt-driven loopback test
 * Connect PB6 (TX) to PB7 (RX) with a jumper wire.
 *
 * Uses Zephyr UART interrupt API (tests the usart_gd32 driver).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <string.h>

static uint8_t rx_buf[128];
static volatile int rx_count;

static void uart_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);
	while (uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t c;
			while (uart_fifo_read(dev, &c, 1) == 1) {
				if (rx_count < sizeof(rx_buf)) {
					rx_buf[rx_count++] = c;
				}
			}
		}
	}
}

int main(void)
{
	const struct device *uart = DEVICE_DT_GET(DT_ALIAS(uart0));

	if (!device_is_ready(uart)) {
		printk("USART0 device not ready\n");
		return 0;
	}

	printk("\n=== USART0 PB6/PB7 Zephyr Interrupt Loopback ===\n");

	/* Set up interrupt-driven RX */
	uart_irq_callback_set(uart, uart_cb);
	uart_irq_rx_enable(uart);
	printk("USART0 interrupt mode ready\n");

	/* Send test data - RX happens in callback via loopback wire */
	const char *test = "Hello GD32F527!\r\n";
	const int tx_len = strlen(test);

	printk("TX: %s", test);
	for (const char *p = test; *p; p++) {
		uart_poll_out(uart, *p);
	}
	printk("TX done\n");

	/* Wait for ISR to receive all bytes */
	int64_t deadline = k_uptime_get() + 3000;
	while (k_uptime_get() < deadline && rx_count < tx_len) {
		k_msleep(10);
	}

	printk("RX: %d/%d bytes\n", rx_count, tx_len);
	if (rx_count > 0) {
		printk("Data: ");
		for (int i = 0; i < rx_count; i++) {
			if (rx_buf[i] >= 0x20 && rx_buf[i] < 0x7F)
				printk("%c", rx_buf[i]);
			else
				printk("[%02X]", rx_buf[i]);
		}
		printk("\n");
	}

	if (rx_count == tx_len && memcmp(test, rx_buf, tx_len) == 0) {
		printk("=== ZEPHYR INTERRUPT LOOPBACK OK ===\n");
	} else {
		printk("=== ZEPHYR INTERRUPT LOOPBACK FAIL ===\n");
	}

	return 0;
}
