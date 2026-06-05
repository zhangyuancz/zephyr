/*
 * OCPP Bridge — Zephyr WebSocket adapter for MicroOcpp
 * SPDX-License-Identifier: Apache-2.0
 */

extern "C" {
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/net/websocket.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>
}

#include <functional>
#include <memory>
#include <MicroOcpp.h>
#include <MicroOcpp/Core/Connection.h>
#include <MicroOcpp/Core/FilesystemAdapter.h>

/* ===== MO_CUSTOM_CONSOLE ===== */

char _mo_console_msg_buf[MO_CUSTOM_CONSOLE_MAXMSGSIZE];
extern "C" void _mo_console_out(const char *msg) {
	/* Print ERROR, WARN, info, and key OCPP events (Heartbeat, Boot, etc.) */
	bool show = false;
	if (strstr(msg, "ERROR") || strstr(msg, "WARN")) { show = true; }
	if (strstr(msg, "[MO] info")) { show = true; }
	if (strstr(msg, "request has been")) { show = true; }
	if (strstr(msg, "initialized")) { show = true; }
	if (strstr(msg, "send conf")) { show = true; }

	if (!show) { return; }

	static const struct device *uart;
	if (!uart) { uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console)); }
	for (const char *p = msg; *p && *p != '\n'; p++) {
		uart_poll_out(uart, *p);
	}
	uart_poll_out(uart, '\r');
	uart_poll_out(uart, '\n');
}

/* ===== MO_CUSTOM_TIMER ===== */

static unsigned long (*mo_tick_fn)() = nullptr;
extern "C" void mocpp_set_timer(unsigned long (*get_ms)()) { mo_tick_fn = get_ms; }
extern "C" unsigned long mocpp_tick_ms_custom() {
	return mo_tick_fn ? mo_tick_fn() : (unsigned long)k_uptime_get();
}

/* ===== MO_CUSTOM_RNG ===== */
static uint32_t mo_rng_seed = 12345;
extern "C" uint32_t mocpp_rng_custom() {
	mo_rng_seed = mo_rng_seed * 1103515245 + 12345;
	return mo_rng_seed;
}

/* ===== Zephyr WS Connection ===== */

static void uart_puts(const char *s) {
	static const struct device *uart;
	if (!uart) { uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console)); }
	for (const char *p = s; *p; p++) { uart_poll_out(uart, *p); }
	uart_poll_out(uart, '\r'); uart_poll_out(uart, '\n');
}

class ZephyrWSConnection : public MicroOcpp::Connection {
public:
	ZephyrWSConnection(int ws_fd) : ws_fd_(ws_fd) {}

	void loop() override {
		uint8_t buf[1024];
		uint64_t remaining = UINT64_MAX;
		uint32_t msg_type = 0;
		int ret = websocket_recv_msg(ws_fd_, buf, sizeof(buf)-1,
					     &msg_type, &remaining, 0);
		if (ret > 0) {
			if (msg_type & WEBSOCKET_FLAG_CLOSE) {
				printk("ocpp: ws close\n"); connected_ = false; return;
			}
			if (msg_type & WEBSOCKET_FLAG_PING) {
				websocket_send_msg(ws_fd_, nullptr, 0,
						   WEBSOCKET_OPCODE_PONG, true, true, 1000);
				return;
			}
			if (msg_type & (WEBSOCKET_FLAG_TEXT | WEBSOCKET_FLAG_BINARY)) {
				buf[ret] = '\0';
				/* Print received message (first 256 chars) */
				printk("OCPP<- %.*s%s\n", (ret > 256 ? 256 : ret),
				       (const char *)buf, ret > 256 ? "..." : "");
				if (recv_cb_) { recv_cb_((const char *)buf, (size_t)ret); }
			}
		} else if (ret == -EAGAIN) {
		} else if (ret == 0 || ret == -ENOTCONN) {
			connected_ = false;
		} else if (ret < 0) {
			printk("ocpp: ws err %d\n", ret);
		}
	}

	bool sendTXT(const char *msg, size_t length) override {
		/* Print sent message (first 256 chars) */
		printk("OCPP-> %.*s%s\n", ((int)length > 256 ? 256 : (int)length),
		       msg, length > 256 ? "..." : "");
		int ret = websocket_send_msg(ws_fd_, (const uint8_t *)msg, length,
					     WEBSOCKET_OPCODE_DATA_TEXT, true, true, 3000);
		return ret > 0;
	}

	void setReceiveTXTcallback(MicroOcpp::ReceiveTXTcallback &cb) override { recv_cb_ = cb; }
	unsigned long getLastConnected() override { return last_connected_; }
	bool isConnected() override { return connected_; }
	void setConnected(bool c) { connected_ = c; last_connected_ = k_uptime_get(); }

private:
	int ws_fd_;
	bool connected_ = true;
	unsigned long last_connected_ = 0;
	MicroOcpp::ReceiveTXTcallback recv_cb_;
};

/* ===== Filesystem adapter (stub, no persistence) ===== */

class ZephyrFileAdapter : public MicroOcpp::FileAdapter {
public:
	size_t read(char *buf, size_t len) override { return 0; }
	size_t write(const char *buf, size_t len) override { return 0; }
	size_t seek(size_t offset) override { return 0; }
	int read() override { return -1; }
};

class ZephyrFS : public MicroOcpp::FilesystemAdapter {
public:
	int stat(const char *path, size_t *size) override { return 0; }
	std::unique_ptr<MicroOcpp::FileAdapter> open(const char *fn, const char *mode) override {
		return nullptr;
	}
	bool remove(const char *fn) override { return false; }
	int ftw_root(std::function<int(const char *fpath)> fn) override { return 0; }
};

/* ===== Entry point ===== */

extern "C" void ocpp_bridge_start(int ws_fd, const char *server_host,
				   const char *charge_box_id)
{
	printk("ocpp: init box=%s\n", charge_box_id);

	static ZephyrWSConnection conn(ws_fd);
	conn.setConnected(true);
	static ZephyrFS fs;

	/* Route MO allocations via Zephyr heap (coalescing, less fragmentation) */
	mo_mem_set_malloc_free(
		[](size_t sz) -> void* { return k_malloc(sz); },
		[](void* p) { k_free(p); }
	);

	mocpp_initialize(conn,
		ChargerCredentials::v201("H1", "CAMS", "V1.1.1", "812345678"),
		std::shared_ptr<MicroOcpp::FilesystemAdapter>(&fs, [](MicroOcpp::FilesystemAdapter* p){}),
		false,
		MicroOcpp::ProtocolVersion(2, 0, 1));

	printk("ocpp: running\n");

	while (1) {
		mocpp_loop();
		conn.loop();
		k_msleep(10);
	}
}
