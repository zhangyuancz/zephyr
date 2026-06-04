/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/ppp.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>

#define USER_NODE DT_PATH(zephyr_user)
#define MODEM_UART_NODE DT_CHOSEN(zephyr_modem_uart)

#define MODEM_RESET_PULSE_MS DT_PROP(USER_NODE, modem_reset_pulse_ms)
#define MODEM_BOOT_DRAIN_MS 3000
#define MODEM_AT_WAIT_MS 5000
#define MODEM_AT_IDLE_TIMEOUT_MS 2000
#define MODEM_DIAL_TIMEOUT_MS 30000
#define MODEM_NET_POLL_COUNT 12
#define MODEM_NET_POLL_INTERVAL_MS 5000

#define MODEM_BACKEND_RX_BUF_SIZE 2048
#define MODEM_BACKEND_TX_BUF_SIZE 2048
#define MODEM_RESP_BUF_SIZE 768
#define MODEM_PDP_APN "cmnet"
#define MODEM_PPP_BUF_SIZE 512
#define MODEM_PPP_MTU 1500
#define TEST_SERVER_HOST "47.111.208.192"
#define TEST_SERVER_PORT 8443
#define TEST_ECHO_PAYLOAD "acboard-ppp-echo"

#define PPP_EVENT_CONNECTED BIT(0)
#define PPP_EVENT_DISCONNECTED BIT(1)
#define PPP_EVENT_DNS_READY BIT(2)

static const struct gpio_dt_spec modem_powerkey =
	GPIO_DT_SPEC_GET(USER_NODE, modem_powerkey_gpios);
static const struct gpio_dt_spec modem_reset =
	GPIO_DT_SPEC_GET(USER_NODE, modem_reset_gpios);
static const struct device *const modem_uart = DEVICE_DT_GET(MODEM_UART_NODE);

struct app_data {
	struct {
		uint8_t uart_rx[MODEM_BACKEND_RX_BUF_SIZE];
		uint8_t uart_tx[MODEM_BACKEND_TX_BUF_SIZE];
	} buffers;

	struct modem_backend_uart uart_backend;
	struct modem_pipe *uart_pipe;
};

static struct app_data app;
static bool modem_registered;

MODEM_PPP_DEFINE(cell_ppp, NULL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, MODEM_PPP_MTU,
		 MODEM_PPP_BUF_SIZE);
#define CELL_IFACE modem_ppp_get_iface(&cell_ppp)

K_EVENT_DEFINE(ppp_events);

static void l4_event_handler(uint64_t event, struct net_if *iface, void *info, size_t info_length,
			     void *user_data)
{
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (iface != CELL_IFACE) {
		return;
	}

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		k_event_post(&ppp_events, PPP_EVENT_CONNECTED);
		break;
	case NET_EVENT_DNS_SERVER_ADD:
	case NET_EVENT_DNS_SERVERS_RECONFIGURED:
		k_event_post(&ppp_events, PPP_EVENT_DNS_READY);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		k_event_post(&ppp_events, PPP_EVENT_DISCONNECTED);
		break;
	default:
		break;
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(ppp_l4_handler,
				NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED |
				NET_EVENT_DNS_SERVER_ADD | NET_EVENT_DNS_SERVERS_RECONFIGURED,
				l4_event_handler, NULL);

static void ppp_event_handler(uint64_t event, struct net_if *iface, void *info, size_t info_length,
			      void *user_data)
{
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (iface != CELL_IFACE) {
		return;
	}

	switch (event) {
	case NET_EVENT_PPP_PHASE_DEAD:
		printk("ppp phase: DEAD\n");
		break;
	case NET_EVENT_PPP_PHASE_RUNNING:
		printk("ppp phase: RUNNING\n");
		break;
	case NET_EVENT_PPP_CARRIER_ON:
		printk("ppp carrier: ON\n");
		break;
	case NET_EVENT_PPP_CARRIER_OFF:
		printk("ppp carrier: OFF\n");
		break;
	default:
		break;
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(ppp_phase_handler,
				NET_EVENT_PPP_PHASE_DEAD | NET_EVENT_PPP_PHASE_RUNNING |
				NET_EVENT_PPP_CARRIER_ON | NET_EVENT_PPP_CARRIER_OFF,
				ppp_event_handler, NULL);

static void modem_dump_lines(const char *tag, const uint8_t *buf, size_t len)
{
	char line[160];
	size_t line_len = 0;

	printk("%s captured %u bytes\n", tag, (unsigned int)len);

	for (size_t i = 0; i < len; i++) {
		uint8_t c = buf[i];

		if (c == '\r') {
			continue;
		}

		if (c == '\n') {
			if (line_len > 0U) {
				line[line_len] = '\0';
				printk("%s: %s\n", tag, line);
				if (strstr(line, "+CEREG: 0,1") != NULL) {
					modem_registered = true;
				}
				line_len = 0;
			}
			continue;
		}

		if (line_len < sizeof(line) - 1U) {
			line[line_len++] = isprint((int)c) ? (char)c : '.';
		}
	}

	if (line_len > 0U) {
		line[line_len] = '\0';
		printk("%s: %s\n", tag, line);
		if (strstr(line, "+CEREG: 0,1") != NULL) {
			modem_registered = true;
		}
	}
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

static int modem_backend_open(void)
{
	const struct modem_backend_uart_config uart_backend_config = {
		.uart = modem_uart,
		.receive_buf = app.buffers.uart_rx,
		.receive_buf_size = sizeof(app.buffers.uart_rx),
		.transmit_buf = app.buffers.uart_tx,
		.transmit_buf_size = sizeof(app.buffers.uart_tx),
	};
	int ret;

	app.uart_pipe = modem_backend_uart_init(&app.uart_backend, &uart_backend_config);
	if (app.uart_pipe == NULL) {
		printk("modem backend init failed\n");
		return -ENODEV;
	}

	ret = modem_pipe_open(app.uart_pipe, K_MSEC(100));
	if (ret < 0) {
		printk("modem pipe open failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int modem_write_all(const uint8_t *buf, size_t len)
{
	size_t offset = 0;
	int64_t deadline = k_uptime_get() + 1000;

	while (offset < len) {
		int ret = modem_pipe_transmit(app.uart_pipe, &buf[offset], len - offset);

		if (ret < 0) {
			return ret;
		}

		if (ret == 0) {
			if (k_uptime_get() >= deadline) {
				return -ETIMEDOUT;
			}
			k_msleep(1);
			continue;
		}

		offset += (size_t)ret;
		deadline = k_uptime_get() + 1000;
	}

	return 0;
}

static size_t modem_collect_response(uint8_t *buf, size_t size, int idle_timeout_ms)
{
	size_t total = 0;
	int64_t deadline = k_uptime_get() + idle_timeout_ms;

	if (size == 0U) {
		return 0;
	}

	while (k_uptime_get() < deadline && total < (size - 1U)) {
		int ret = modem_pipe_receive(app.uart_pipe, &buf[total], size - 1U - total);

		if (ret < 0) {
			break;
		}

		if (ret == 0) {
			k_msleep(5);
			continue;
		}

		total += (size_t)ret;
		deadline = k_uptime_get() + idle_timeout_ms;
	}

	buf[total] = '\0';
	return total;
}

static void modem_drain_boot_output(void)
{
	uint8_t buf[MODEM_RESP_BUF_SIZE];

	printk("draining modem boot bytes for %d ms\n", MODEM_BOOT_DRAIN_MS);
	(void)modem_collect_response(buf, sizeof(buf), MODEM_BOOT_DRAIN_MS);
}

static int modem_run_cmd(const char *cmd, int timeout_ms, const char *expect)
{
	uint8_t response[MODEM_RESP_BUF_SIZE];
	size_t len;
	int ret;

	printk("send: %s", cmd);
	ret = modem_write_all((const uint8_t *)cmd, strlen(cmd));
	if (ret < 0) {
		printk("tx failed: %d\n", ret);
		return ret;
	}

	len = modem_collect_response(response, sizeof(response), timeout_ms);
	modem_dump_lines("at", response, len);

	if (expect != NULL && strstr((const char *)response, expect) == NULL) {
		printk("expected response missing: %s\n", expect);
		return -EIO;
	}

	return 0;
}

static int modem_dial_ppp(void)
{
	char line[160];
	size_t line_len = 0;
	uint8_t byte;
	int ret;
	int64_t deadline;

	printk("send: ATD*99#\r\n");
	ret = modem_write_all((const uint8_t *)"ATD*99#\r\n", strlen("ATD*99#\r\n"));
	if (ret < 0) {
		printk("tx failed: %d\n", ret);
		return ret;
	}

	deadline = k_uptime_get() + MODEM_DIAL_TIMEOUT_MS;

	while (k_uptime_get() < deadline) {
		ret = modem_pipe_receive(app.uart_pipe, &byte, 1);
		if (ret < 0) {
			return ret;
		}

		if (ret == 0) {
			k_msleep(5);
			continue;
		}

		if (byte == '\r') {
			continue;
		}

		if (byte == '\n') {
			if (line_len == 0U) {
				continue;
			}

			line[line_len] = '\0';
			printk("at: %s\n", line);
			if (strcmp(line, "CONNECT") == 0) {
				return 0;
			}
			line_len = 0;
			continue;
		}

		if (line_len < sizeof(line) - 1U) {
			line[line_len++] = isprint((int)byte) ? (char)byte : '.';
		}
	}

	printk("dial timeout waiting for CONNECT\n");
	return -ETIMEDOUT;
}

static void modem_power_on_and_reset(void)
{
	printk("drive powerkey high and hold\n");
	gpio_pin_set_dt(&modem_powerkey, 1);

	gpio_pin_set_dt(&modem_reset, 1);
	k_msleep(MODEM_RESET_PULSE_MS);
	gpio_pin_set_dt(&modem_reset, 0);
}

static void ppp_dump_ipv4(void)
{
	struct net_in_addr *addr;
	struct net_in_addr gw;
	char text[NET_IPV4_ADDR_LEN];

	addr = net_if_ipv4_get_global_addr(CELL_IFACE, NET_ADDR_PREFERRED);
	if (addr != NULL && net_addr_ntop(AF_INET, addr, text, sizeof(text)) != NULL) {
		printk("ppp ipv4: %s\n", text);
	}

	gw = net_if_ipv4_get_gw(CELL_IFACE);
	if (!net_ipv4_is_addr_unspecified(&gw) &&
	    net_addr_ntop(AF_INET, &gw, text, sizeof(text)) != NULL) {
		printk("ppp gateway: %s\n", text);
	}
}

static int tcp_echo_test(void)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *ai = NULL;
	struct sockaddr_in peer = {0};
	char port[6];
	char addr[NET_IPV4_ADDR_LEN];
	uint8_t rx[128];
	int sock = -1;
	int ret;

	snprintk(port, sizeof(port), "%u", TEST_SERVER_PORT);
	printk("resolve: %s:%u\n", TEST_SERVER_HOST, TEST_SERVER_PORT);

	ret = zsock_getaddrinfo(TEST_SERVER_HOST, port, &hints, &ai);
	if (ret != 0 || ai == NULL) {
		printk("dns failed: %d errno=%d\n", ret, errno);
		return ret != 0 ? ret : -ENOENT;
	}

	memcpy(&peer, ai->ai_addr, sizeof(peer));
	zsock_freeaddrinfo(ai);

	if (zsock_inet_ntop(AF_INET, &peer.sin_addr, addr, sizeof(addr)) == NULL) {
		printk("addr format failed\n");
		return -EINVAL;
	}

	printk("connect: %s:%u\n", addr, ntohs(peer.sin_port));

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		printk("socket failed: errno=%d\n", errno);
		return -errno;
	}

	{
		struct timeval tv = {.tv_sec = 10, .tv_usec = 0};

		(void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	ret = zsock_connect(sock, (struct sockaddr *)&peer, sizeof(peer));
	if (ret < 0) {
		printk("connect failed: errno=%d\n", errno);
		ret = -errno;
		goto out;
	}

	printk("tcp connected\n");

	ret = zsock_send(sock, TEST_ECHO_PAYLOAD, sizeof(TEST_ECHO_PAYLOAD) - 1, 0);
	if (ret < 0) {
		printk("send failed: errno=%d\n", errno);
		ret = -errno;
		goto out;
	}

	printk("sent %d bytes: %s\n", ret, TEST_ECHO_PAYLOAD);

	ret = zsock_recv(sock, rx, sizeof(rx) - 1, 0);
	if (ret < 0) {
		printk("recv failed: errno=%d\n", errno);
		ret = -errno;
		goto out;
	}

	rx[ret] = '\0';
	printk("recv %d bytes: %s\n", ret, rx);
	ret = 0;

out:
	(void)zsock_close(sock);
	return ret;
}

static int modem_init_sequence(void)
{
	static const char *const init_cmds[] = {
		"AT\r\n",
		"ATE0\r\n",
		"ATI\r\n",
		"AT+CPIN?\r\n",
		"AT+CSQ\r\n",
		"AT+CEREG?\r\n",
		"AT+ECICCID\r\n",
		"AT+CIMI\r\n",
		"AT+CGATT?\r\n",
		"AT+CGDCONT=1,\"IP\",\"" MODEM_PDP_APN "\"\r\n",
	};

	for (size_t i = 0; i < ARRAY_SIZE(init_cmds); i++) {
		if (modem_run_cmd(init_cmds[i], MODEM_AT_IDLE_TIMEOUT_MS, "OK") < 0) {
			return -EIO;
		}
	}

	for (size_t i = 0; i < MODEM_NET_POLL_COUNT; i++) {
		if (modem_registered) {
			printk("network registered, stop polling\n");
			return 0;
		}

		printk("network poll %u/%u\n",
		       (unsigned int)(i + 1),
		       (unsigned int)MODEM_NET_POLL_COUNT);

		if (modem_run_cmd("AT+CSQ\r\n", MODEM_AT_IDLE_TIMEOUT_MS, "OK") < 0) {
			return -EIO;
		}

		if (modem_run_cmd("AT+CEREG?\r\n", MODEM_AT_IDLE_TIMEOUT_MS, "OK") < 0) {
			return -EIO;
		}

		if (modem_registered) {
			printk("network registered, stop polling\n");
			return 0;
		}

		k_msleep(MODEM_NET_POLL_INTERVAL_MS);
	}

	printk("network registration timeout\n");
	return -ETIMEDOUT;
}

int main(void)
{
	int ret;

	if (!device_is_ready(modem_uart)) {
		printk("modem uart not ready\n");
		return 0;
	}

	ret = modem_prepare_gpios();
	if (ret < 0) {
		return 0;
	}

	ret = modem_backend_open();
	if (ret < 0) {
		return 0;
	}

	modem_power_on_and_reset();
	modem_drain_boot_output();

	printk("waiting %d ms before AT\n", MODEM_AT_WAIT_MS);
	k_msleep(MODEM_AT_WAIT_MS);

	ret = modem_init_sequence();
	if (ret < 0) {
		printk("modem init sequence failed: %d\n", ret);
		return 0;
	}

	ret = modem_dial_ppp();
	if (ret < 0) {
		printk("dial failed: %d\n", ret);
		return 0;
	}

	ret = modem_ppp_attach(&cell_ppp, app.uart_pipe);
	if (ret < 0) {
		printk("ppp attach failed: %d\n", ret);
		return 0;
	}

	net_if_carrier_on(CELL_IFACE);

	ret = net_if_up(CELL_IFACE);
	if (ret < 0) {
		printk("ppp if up failed: %d\n", ret);
		return 0;
	}

	net_if_dormant_off(CELL_IFACE);

	printk("waiting for PPP L4 connected\n");
	ret = k_event_wait(&ppp_events, PPP_EVENT_CONNECTED, false, K_SECONDS(120));
	if (ret != PPP_EVENT_CONNECTED) {
		printk("ppp connect timeout\n");
		return 0;
	}

	printk("ppp connected\n");
	ppp_dump_ipv4();

	printk("waiting for DNS server\n");
	ret = k_event_wait(&ppp_events, PPP_EVENT_DNS_READY, false, K_SECONDS(30));
	if (ret != PPP_EVENT_DNS_READY) {
		printk("dns server wait timeout\n");
		return 0;
	}

	ret = tcp_echo_test();
	if (ret < 0) {
		printk("tcp echo test failed: %d\n", ret);
	}

	while (1) {
		k_sleep(K_SECONDS(30));
	}

	return 0;
}
