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
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/websocket.h>
#include <zephyr/net/sntp.h>
#include <time.h>
#include "tls_certificates.h"
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
/* WSS config */
#define WSS_HOST "47.111.208.192"
#define WSS_PORT "8443"
#define WSS_PATH "/"
#define WSS_SEND_PAYLOAD "acboard-wss-hello"
#define WSS_RECV_BUF_LEN 512
#define WSS_TEMP_BUF_LEN 1024

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

static uint8_t wss_recv_buf[WSS_RECV_BUF_LEN];
static uint8_t wss_temp_buf[WSS_TEMP_BUF_LEN];
static int wss_tls_sock = -1;

static int wss_connect(void)
{
	struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *ai = NULL;
	struct sockaddr_in peer = {0};
	struct websocket_request ws_req;
	char addr_str[NET_IPV4_ADDR_LEN];
	sec_tag_t sec_tags[] = { CA_CERTIFICATE_TAG, CLIENT_CERT_TAG };
	int sock = -1, ws_fd = -1, ret;
	int32_t timeout = 15000;

	ret = tls_credential_add(CA_CERTIFICATE_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
				 ca_certificate, sizeof(ca_certificate));
	if (ret < 0) { printk("CA cert add failed: %d\n", ret); return ret; }
	printk("CA cert reg (tag=%d)\n", CA_CERTIFICATE_TAG);

	ret = tls_credential_add(CLIENT_CERT_TAG, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
				 client_certificate, sizeof(client_certificate));
	if (ret < 0) { printk("Client cert add failed: %d\n", ret); return ret; }
	printk("Client cert reg (tag=%d)\n", CLIENT_CERT_TAG);

	ret = tls_credential_add(CLIENT_CERT_TAG, TLS_CREDENTIAL_PRIVATE_KEY,
				 client_private_key, sizeof(client_private_key));
	if (ret < 0) { printk("Client key add failed: %d\n", ret); return ret; }
	printk("Client key reg (tag=%d)\n", CLIENT_CERT_TAG);

	{
		struct sntp_time sntp_ts;
		struct timespec ts;
		printk("SNTP ...\n");
		ret = sntp_simple("203.107.6.88", 5000, &sntp_ts);
		if (ret < 0) { ret = sntp_simple("ntp.aliyun.com", 8000, &sntp_ts); }
		if (ret >= 0) {
			ts.tv_sec = (time_t)sntp_ts.seconds; ts.tv_nsec = 0;
			clock_settime(CLOCK_REALTIME, &ts);
		} else {
			ts.tv_sec = 1769097600; ts.tv_nsec = 0;
			clock_settime(CLOCK_REALTIME, &ts);
			printk("SNTP failed (%d), hardcoded time\n", ret);
		}
	}

	printk("resolving %s:%s\n", WSS_HOST, WSS_PORT);
	ret = zsock_getaddrinfo(WSS_HOST, WSS_PORT, &hints, &ai);
	if (ret != 0 || ai == NULL) { ret = -ENOENT; goto out; }
	memcpy(&peer, ai->ai_addr, sizeof(peer));
	zsock_freeaddrinfo(ai); ai = NULL;
	zsock_inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
	printk("resolved %s:%u\n", addr_str, ntohs(peer.sin_port));

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (sock < 0) { ret = -errno; goto out; }
	printk("TLS sock fd=%d\n", sock);

	zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, sec_tags, sizeof(sec_tags));
	zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, WSS_HOST, sizeof(WSS_HOST)-1);
	{ struct timeval tv = {.tv_sec = 15}; zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)); }

	printk("TLS connecting ...\n");
	ret = zsock_connect(sock, (struct sockaddr *)&peer, sizeof(peer));
	if (ret < 0) { printk("TLS connect failed: %d\n", -errno); ret = -errno; goto out; }
	printk("TLS connected\n");

	memset(&ws_req, 0, sizeof(ws_req));
	ws_req.host = WSS_HOST; ws_req.url = WSS_PATH;
	ws_req.tmp_buf = wss_temp_buf; ws_req.tmp_buf_len = sizeof(wss_temp_buf);
	ws_fd = websocket_connect(sock, &ws_req, timeout, NULL);
	if (ws_fd < 0) { printk("ws upgrade failed: %d\n", ws_fd); ret = ws_fd; goto out; }
	printk("WSS connected (ws_fd=%d)\n", ws_fd);

	printk("WSS send: %s\n", WSS_SEND_PAYLOAD);
	ret = websocket_send_msg(ws_fd, (const uint8_t *)WSS_SEND_PAYLOAD,
				 sizeof(WSS_SEND_PAYLOAD)-1,
				 WEBSOCKET_OPCODE_DATA_TEXT, true, true, 5000);
	if (ret < 0) { printk("send failed: %d\n", ret); goto out; }
	printk("sent %d bytes\n", ret);

	wss_tls_sock = sock;
	return ws_fd;

out:
	if (ws_fd >= 0) { websocket_disconnect(ws_fd); }
	if (sock >= 0) { zsock_close(sock); }
	if (ai != NULL) { zsock_freeaddrinfo(ai); }
	return ret;
}

static void wss_recv_loop(int ws_fd)
{
	int total_read, ret, seq = 0;
	int64_t last_ping = k_uptime_get();
	int64_t last_test = 0;
	char test_msg[64];

	while (1) {
		uint64_t remaining = UINT64_MAX;
		uint32_t msg_type = 0;
		total_read = 0;

		/* Send test message every 5s */
		if (k_uptime_get() - last_test > 5000) {
			seq++;
			snprintk(test_msg, sizeof(test_msg),
				 "{\"seq\":%d,\"msg\":\"acboard-test\"}", seq);
			ret = websocket_send_msg(ws_fd, (uint8_t *)test_msg,
						 strlen(test_msg),
						 WEBSOCKET_OPCODE_DATA_TEXT,
						 true, true, 3000);
			if (ret > 0) { printk("wss tx [%d]: %s\n", seq, test_msg); }
			else { printk("wss tx fail: %d\n", ret); }
			last_test = k_uptime_get();
		}

		/* Send ping every 30s */
		if (k_uptime_get() - last_ping > 30000) {
			ret = websocket_send_msg(ws_fd, NULL, 0,
						 WEBSOCKET_OPCODE_PING,
						 true, true, 3000);
			if (ret >= 0) { printk("wss ping\n"); }
			last_ping = k_uptime_get();
		}

		while (remaining > 0 && total_read < (int)sizeof(wss_recv_buf)-1) {
			ret = websocket_recv_msg(ws_fd, wss_recv_buf+total_read,
						 sizeof(wss_recv_buf)-1-total_read,
						 &msg_type, &remaining, 2000);
			if (ret < 0) {
				if (ret == -EAGAIN) { break; }
				printk("wss err: %d\n", ret); return;
			}

			/* Check flags BEFORE ret==0 (close frame may return 0) */
			if (msg_type & WEBSOCKET_FLAG_CLOSE) {
				printk("wss close frame\n"); return;
			}
			if (msg_type & WEBSOCKET_FLAG_PING) {
				websocket_send_msg(ws_fd, NULL, 0,
						   WEBSOCKET_OPCODE_PONG,
						   true, true, 3000);
				printk("wss pong\n");
				continue;
			}
			if (msg_type & WEBSOCKET_FLAG_PONG) {
				printk("wss pong recv\n");
				continue;
			}
			if (ret == 0) { printk("wss closed\n"); return; }

			total_read += ret;
		}

		if (total_read > 0) {
			wss_recv_buf[total_read] = '\0';
			printk("wss recv: %s\n", wss_recv_buf);
		}
	}
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
	int ws_fd = -1;
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

	ws_fd = wss_connect();
	if (ws_fd >= 0) {
		printk("WSS loop start (ws_fd=%d)\n", ws_fd);
		wss_recv_loop(ws_fd);
	}
	if (ws_fd >= 0) { websocket_disconnect(ws_fd); }
	if (wss_tls_sock >= 0) { zsock_close(wss_tls_sock); }

	while (1) {
		k_sleep(K_SECONDS(30));
	}

	return 0;
}
