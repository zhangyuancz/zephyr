/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * L511C OCPP on plain WebSocket (no TLS)
 * ACBoard + GD32E513
 */

#include <ctype.h>
#include <stdio.h>
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
#include <zephyr/net/websocket.h>
#include <zephyr/sys/printk.h>

#include "ocpp_bridge.h"

/* ===== Board / Modem DT ===== */
#define USER_NODE       DT_PATH(zephyr_user)
#define MODEM_UART_NODE DT_CHOSEN(zephyr_modem_uart)

#define MODEM_RESET_PULSE_MS      DT_PROP(USER_NODE, modem_reset_pulse_ms)
#define MODEM_BOOT_DRAIN_MS       3000
#define MODEM_AT_WAIT_MS          5000
#define MODEM_AT_IDLE_TIMEOUT_MS  2000
#define MODEM_DIAL_TIMEOUT_MS     30000
#define MODEM_NET_POLL_COUNT      12
#define MODEM_NET_POLL_INTERVAL_MS 5000

#define MODEM_BACKEND_RX_BUF_SIZE 2048
#define MODEM_BACKEND_TX_BUF_SIZE 2048
#define MODEM_RESP_BUF_SIZE       768
#define MODEM_PDP_APN             "cmnet"
#define MODEM_PPP_BUF_SIZE        512
#define MODEM_PPP_MTU             1500

/* OCPP server (plain WS) */
#define OCPP_SERVER_HOST  "47.111.208.192"
#define OCPP_SERVER_PORT  "6671"
#define OCPP_SERVER_PATH  "/ocppj/812345678"
#define OCPP_CHARGEBOX_ID "812345678"

/* WS buffers */
#define WS_RECV_BUF_LEN  512
#define WS_TEMP_BUF_LEN  1024

/* PPP events */
#define PPP_EVENT_CONNECTED     BIT(0)
#define PPP_EVENT_DISCONNECTED  BIT(1)
#define PPP_EVENT_DNS_READY     BIT(2)

/* GPIOs */
static const struct gpio_dt_spec modem_powerkey =
	GPIO_DT_SPEC_GET(USER_NODE, modem_powerkey_gpios);
static const struct gpio_dt_spec modem_reset =
	GPIO_DT_SPEC_GET(USER_NODE, modem_reset_gpios);
static const struct device *const modem_uart = DEVICE_DT_GET(MODEM_UART_NODE);

/* App data */
struct app_data {
	struct { uint8_t uart_rx[MODEM_BACKEND_RX_BUF_SIZE];
		 uint8_t uart_tx[MODEM_BACKEND_TX_BUF_SIZE]; } buffers;
	struct modem_backend_uart uart_backend;
	struct modem_pipe *uart_pipe;
};
static struct app_data app;
static bool modem_registered;

/* PPP */
MODEM_PPP_DEFINE(cell_ppp, NULL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		 MODEM_PPP_MTU, MODEM_PPP_BUF_SIZE);
#define CELL_IFACE modem_ppp_get_iface(&cell_ppp)
K_EVENT_DEFINE(ppp_events);

/* Net mgmt handlers */
static void l4_event_handler(uint64_t event, struct net_if *iface,
			     void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info); ARG_UNUSED(info_length); ARG_UNUSED(user_data);
	if (iface != CELL_IFACE) { return; }
	switch (event) {
	case NET_EVENT_L4_CONNECTED:   k_event_post(&ppp_events, PPP_EVENT_CONNECTED); break;
	case NET_EVENT_DNS_SERVER_ADD:
	case NET_EVENT_DNS_SERVERS_RECONFIGURED: k_event_post(&ppp_events, PPP_EVENT_DNS_READY); break;
	case NET_EVENT_L4_DISCONNECTED: k_event_post(&ppp_events, PPP_EVENT_DISCONNECTED); break;
	}
}
NET_MGMT_REGISTER_EVENT_HANDLER(ppp_l4, NET_EVENT_L4_CONNECTED |
	NET_EVENT_L4_DISCONNECTED | NET_EVENT_DNS_SERVER_ADD |
	NET_EVENT_DNS_SERVERS_RECONFIGURED, l4_event_handler, NULL);

static void ppp_event_handler(uint64_t event, struct net_if *iface,
			      void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info); ARG_UNUSED(info_length); ARG_UNUSED(user_data);
	if (iface != CELL_IFACE) { return; }
	switch (event) {
	case NET_EVENT_PPP_PHASE_DEAD:   printk("ppp: DEAD\n"); break;
	case NET_EVENT_PPP_PHASE_RUNNING: printk("ppp: RUN\n"); break;
	case NET_EVENT_PPP_CARRIER_ON:   printk("ppp: CARRIER ON\n"); break;
	case NET_EVENT_PPP_CARRIER_OFF:  printk("ppp: CARRIER OFF\n"); break;
	}
}
NET_MGMT_REGISTER_EVENT_HANDLER(ppp_phase, NET_EVENT_PPP_PHASE_DEAD |
	NET_EVENT_PPP_PHASE_RUNNING | NET_EVENT_PPP_CARRIER_ON |
	NET_EVENT_PPP_CARRIER_OFF, ppp_event_handler, NULL);

/* ===== Modem AT helpers ===== */

static void modem_dump_lines(const char *tag, const uint8_t *buf, size_t len)
{
	char line[160]; size_t line_len = 0;
	printk("%s: %uB\n", tag, (unsigned)len);
	for (size_t i = 0; i < len; i++) {
		uint8_t c = buf[i];
		if (c == '\r') { continue; }
		if (c == '\n') {
			if (line_len > 0U) {
				line[line_len] = '\0'; printk("%s: %s\n", tag, line);
				if (strstr(line, "+CEREG: 0,1")) { modem_registered = true; }
				line_len = 0;
			} continue;
		}
		if (line_len < sizeof(line)-1U) { line[line_len++] = isprint(c)?(char)c:'.'; }
	}
}

static int modem_prepare_gpios(void)
{
	if (!gpio_is_ready_dt(&modem_powerkey) || !gpio_is_ready_dt(&modem_reset))
		{ return -ENODEV; }
	gpio_pin_configure_dt(&modem_powerkey, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&modem_reset, GPIO_OUTPUT_INACTIVE);
	return 0;
}

static int modem_backend_open(void)
{
	const struct modem_backend_uart_config cfg = {
		.uart = modem_uart, .receive_buf = app.buffers.uart_rx,
		.receive_buf_size = sizeof(app.buffers.uart_rx),
		.transmit_buf = app.buffers.uart_tx,
		.transmit_buf_size = sizeof(app.buffers.uart_tx),
	};
	app.uart_pipe = modem_backend_uart_init(&app.uart_backend, &cfg);
	if (!app.uart_pipe) { return -ENODEV; }
	return modem_pipe_open(app.uart_pipe, K_MSEC(100));
}

static int modem_write_all(const uint8_t *buf, size_t len)
{
	size_t off = 0; int64_t dl = k_uptime_get() + 1000;
	while (off < len) {
		int r = modem_pipe_transmit(app.uart_pipe, &buf[off], len-off);
		if (r < 0) { return r; }
		if (r == 0) { if (k_uptime_get() >= dl) return -ETIMEDOUT; k_msleep(1); continue; }
		off += r; dl = k_uptime_get() + 1000;
	}
	return 0;
}

static size_t modem_collect_response(uint8_t *buf, size_t size, int idle_ms)
{
	size_t t = 0; int64_t dl = k_uptime_get() + idle_ms;
	if (!size) { return 0; }
	while (k_uptime_get() < dl && t < size-1) {
		int r = modem_pipe_receive(app.uart_pipe, &buf[t], size-1-t);
		if (r < 0) { break; }
		if (r == 0) { k_msleep(5); continue; }
		t += r; dl = k_uptime_get() + idle_ms;
	}
	buf[t] = '\0'; return t;
}

static int modem_run_cmd(const char *cmd, int timeout_ms, const char *expect)
{
	uint8_t rsp[MODEM_RESP_BUF_SIZE];
	printk("AT: %s", cmd);
	if (modem_write_all((const uint8_t *)cmd, strlen(cmd)) < 0) { return -EIO; }
	size_t len = modem_collect_response(rsp, sizeof(rsp), timeout_ms);
	modem_dump_lines("AT", rsp, len);
	if (expect && !strstr((const char *)rsp, expect)) { return -EIO; }
	return 0;
}

static int modem_dial_ppp(void)
{
	char line[160]; size_t ll = 0; uint8_t b;
	printk("AT: ATD*99#\n");
	if (modem_write_all((const uint8_t *)"ATD*99#\r\n", 9) < 0) { return -EIO; }
	int64_t dl = k_uptime_get() + MODEM_DIAL_TIMEOUT_MS;
	while (k_uptime_get() < dl) {
		int r = modem_pipe_receive(app.uart_pipe, &b, 1);
		if (r < 0) { return r; }
		if (r == 0) { k_msleep(5); continue; }
		if (b == '\r') { continue; }
		if (b == '\n') {
			if (!ll) { continue; }
			line[ll] = '\0'; printk("AT: %s\n", line);
			if (!strcmp(line, "CONNECT")) { return 0; }
			ll = 0; continue;
		}
		if (ll < sizeof(line)-1) { line[ll++] = isprint(b)?(char)b:'.'; }
	}
	return -ETIMEDOUT;
}

static void modem_power_on_and_reset(void)
{
	gpio_pin_set_dt(&modem_powerkey, 1);
	gpio_pin_set_dt(&modem_reset, 1);
	k_msleep(MODEM_RESET_PULSE_MS);
	gpio_pin_set_dt(&modem_reset, 0);
}

static int modem_init_sequence(void)
{
	static const char *cmds[] = {
		"AT\r\n", "ATE0\r\n", "ATI\r\n", "AT+CPIN?\r\n", "AT+CSQ\r\n",
		"AT+CEREG?\r\n", "AT+ECICCID\r\n", "AT+CIMI\r\n", "AT+CGATT?\r\n",
		"AT+CGDCONT=1,\"IP\",\"" MODEM_PDP_APN "\"\r\n",
	};
	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++) {
		if (modem_run_cmd(cmds[i], MODEM_AT_IDLE_TIMEOUT_MS, "OK") < 0) { return -EIO; }
	}
	for (size_t i = 0; i < MODEM_NET_POLL_COUNT; i++) {
		if (modem_registered) { return 0; }
		printk("net poll %u/%u\n", (unsigned)(i+1), (unsigned)MODEM_NET_POLL_COUNT);
		modem_run_cmd("AT+CSQ\r\n", MODEM_AT_IDLE_TIMEOUT_MS, "OK");
		modem_run_cmd("AT+CEREG?\r\n", MODEM_AT_IDLE_TIMEOUT_MS, "OK");
		if (modem_registered) { return 0; }
		k_msleep(MODEM_NET_POLL_INTERVAL_MS);
	}
	return -ETIMEDOUT;
}

/* ===== Plain WS connect (no TLS) ===== */

static uint8_t ws_recv_buf[WS_RECV_BUF_LEN];
static uint8_t ws_temp_buf[WS_TEMP_BUF_LEN];

static int ws_connect(void)
{
	struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *ai = NULL;
	struct sockaddr_in peer = {0};
	struct websocket_request req;
	char addr[NET_IPV4_ADDR_LEN];
	int sock = -1, ws_fd = -1, ret;

	printk("resolve %s:%s\n", OCPP_SERVER_HOST, OCPP_SERVER_PORT);
	ret = zsock_getaddrinfo(OCPP_SERVER_HOST, OCPP_SERVER_PORT, &hints, &ai);
	if (ret != 0 || !ai) { ret = -ENOENT; goto out; }
	memcpy(&peer, ai->ai_addr, sizeof(peer));
	zsock_freeaddrinfo(ai); ai = NULL;
	zsock_inet_ntop(AF_INET, &peer.sin_addr, addr, sizeof(addr));
	printk("-> %s:%u\n", addr, ntohs(peer.sin_port));

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) { ret = -errno; goto out; }
	{ struct timeval tv = {.tv_sec = 10}; zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }
	ret = zsock_connect(sock, (struct sockaddr *)&peer, sizeof(peer));
	if (ret < 0) { ret = -errno; goto out; }
	printk("TCP ok\n");

	const char *ocpp_headers[] = {
		"Sec-WebSocket-Protocol: ocpp2.0\r\n",
		NULL
	};
	memset(&req, 0, sizeof(req));
	req.host = OCPP_SERVER_HOST; req.url = OCPP_SERVER_PATH;
	req.optional_headers = ocpp_headers;
	req.tmp_buf = ws_temp_buf; req.tmp_buf_len = sizeof(ws_temp_buf);
	ws_fd = websocket_connect(sock, &req, 10000, NULL);
	if (ws_fd < 0) { printk("ws fail: %d\n", ws_fd); ret = ws_fd; goto out; }
	printk("WS ok (fd=%d)\n", ws_fd);
	return ws_fd;

out:
	if (ws_fd >= 0) { websocket_disconnect(ws_fd); }
	if (sock >= 0)  { zsock_close(sock); }
	if (ai) { zsock_freeaddrinfo(ai); }
	return ret;
}

/* ===== Main ===== */

int main(void)
{
	printk("\n=== L511C OCPP on WS ===\n\n");

	if (!device_is_ready(modem_uart)) { return 0; }
	if (modem_prepare_gpios() < 0) { return 0; }
	if (modem_backend_open() < 0) { return 0; }

	modem_power_on_and_reset();
	{ uint8_t b[MODEM_RESP_BUF_SIZE]; modem_collect_response(b, sizeof(b), MODEM_BOOT_DRAIN_MS); }
	k_msleep(MODEM_AT_WAIT_MS);

	if (modem_init_sequence() < 0) { return 0; }
	if (modem_dial_ppp() < 0) { return 0; }
	if (modem_ppp_attach(&cell_ppp, app.uart_pipe) < 0) { return 0; }
	net_if_carrier_on(CELL_IFACE); net_if_up(CELL_IFACE); net_if_dormant_off(CELL_IFACE);

	printk("wait PPP ...\n");
	if (k_event_wait(&ppp_events, PPP_EVENT_CONNECTED, false, K_SECONDS(120)) != PPP_EVENT_CONNECTED)
		{ printk("PPP timeout\n"); return 0; }
	printk("PPP ok\n");
	k_event_wait(&ppp_events, PPP_EVENT_DNS_READY, false, K_SECONDS(30));
	k_msleep(1000);

	int ws_fd = ws_connect();
	if (ws_fd < 0) { printk("WS fail: %d\n", ws_fd); return 0; }

	printk("OCPP bridge start ...\n");
	ocpp_bridge_start(ws_fd, OCPP_SERVER_HOST, OCPP_CHARGEBOX_ID);
	return 0;
}
