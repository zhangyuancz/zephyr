/*
 * OCPP Bridge — Zephyr WebSocket adapter for MicroOcpp
 * SPDX-License-Identifier: Apache-2.0
 */

extern "C" {
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fs/fs.h>
#include <zephyr/net/websocket.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>
#include <string.h>
}

#include <functional>
#include <memory>
#include <string>
#include <MicroOcpp.h>
#include <MicroOcpp/Core/Connection.h>
#include <MicroOcpp/Core/FilesystemAdapter.h>

/* MicroOcpp files are stored under this mount point */
#define MO_FS_PREFIX "/lfs1/"

/* ===== MO_CUSTOM_CONSOLE ===== */

char _mo_console_msg_buf[MO_CUSTOM_CONSOLE_MAXMSGSIZE];
extern "C" void _mo_console_out(const char *msg) {
	/* Show all messages during bringup */
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

/* ===== Filesystem adapter (LittleFS on SPI NOR flash) ===== */

static std::string mo_full_path(const char *path)
{
	return std::string(MO_FS_PREFIX) + path;
}

class ZephyrFileAdapter : public MicroOcpp::FileAdapter {
public:
	ZephyrFileAdapter(const char *path, const char *mode)
	{
		fs_file_t_init(&file_);

		fs_mode_t flags = FS_O_CREATE;
		if (strchr(mode, 'w')) {
			flags = FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC;
		} else if (strchr(mode, 'a')) {
			flags = FS_O_CREATE | FS_O_WRITE | FS_O_APPEND;
		} else {
			flags = FS_O_READ;
		}
		if (strchr(mode, '+')) {
			flags |= FS_O_RDWR;
		}

		int ret = fs_open(&file_, path, flags);
		if (ret < 0) {
			printk("ocpp-fs: open(%s) err %d\n", path, ret);
			valid_ = false;
		} else {
			valid_ = true;
		}
	}

	~ZephyrFileAdapter() override
	{
		if (valid_) {
			fs_close(&file_);
		}
	}

	size_t read(char *buf, size_t len) override
	{
		if (!valid_) { return 0; }
		ssize_t ret = fs_read(&file_, buf, len);
		return ret > 0 ? (size_t)ret : 0;
	}

	size_t write(const char *buf, size_t len) override
	{
		if (!valid_) { return 0; }
		ssize_t ret = fs_write(&file_, buf, len);
		return ret > 0 ? (size_t)ret : 0;
	}

	size_t seek(size_t offset) override
	{
		if (!valid_) { return 0; }
		int ret = fs_seek(&file_, (off_t)offset, FS_SEEK_SET);
		if (ret < 0) { return 0; }
		return (size_t)fs_tell(&file_);
	}

	int read() override
	{
		if (!valid_) { return -1; }
		unsigned char c;
		ssize_t ret = fs_read(&file_, &c, 1);
		return ret == 1 ? (int)c : -1;
	}

	bool isValid() const { return valid_; }

private:
	struct fs_file_t file_;
	bool valid_ = false;
};

class ZephyrFS : public MicroOcpp::FilesystemAdapter {
public:
	int stat(const char *path, size_t *size) override
	{
		struct fs_dirent entry;
		std::string full = mo_full_path(path);
		int ret = fs_stat(full.c_str(), &entry);
		if (ret < 0) { return ret; }
		if (size) { *size = entry.size; }
		return 0;
	}

	std::unique_ptr<MicroOcpp::FileAdapter> open(const char *fn, const char *mode) override
	{
		std::string full = mo_full_path(fn);
		auto adapter = std::unique_ptr<ZephyrFileAdapter>(
			new ZephyrFileAdapter(full.c_str(), mode));
		if (!adapter->isValid()) {
			return nullptr;
		}
		return adapter;
	}

	bool remove(const char *fn) override
	{
		std::string full = mo_full_path(fn);
		int ret = fs_unlink(full.c_str());
		return ret == 0;
	}

	int ftw_root(std::function<int(const char *fpath)> fn) override
	{
		struct fs_dir_t dir;
		fs_dir_t_init(&dir);
		int count = 0;

		int ret = fs_opendir(&dir, MO_FS_PREFIX);
		if (ret < 0) {
			printk("ocpp-fs: opendir(%s) err %d\n", MO_FS_PREFIX, ret);
			return ret;  /* non-zero = error, as MO expects */
		}

		struct fs_dirent entry;
		while (fs_readdir(&dir, &entry) == 0) {
			if (entry.name[0] == '\0') { break; }
			if (entry.type == FS_DIR_ENTRY_DIR) { continue; }
			count++;
			fn(entry.name);
		}
		fs_closedir(&dir);

		/* MO expects 0 = success */
		return 0;
	}
};

/* ===== Zephyr WS Connection ===== */

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

/* ===== Print MO config at boot ===== */

static void print_mo_config(void)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;
	char buf[256];

	fs_dir_t_init(&dir);

	int ret = fs_opendir(&dir, MO_FS_PREFIX);
	if (ret < 0) {
		printk("--- MO config: opendir err %d ---\n", ret);
		return;
	}

	printk("\n--- MO config on flash (mount: %s) ---\n", MO_FS_PREFIX);

	while (fs_readdir(&dir, &entry) == 0) {
		if (entry.name[0] == '\0') { break; }
		if (entry.type == FS_DIR_ENTRY_DIR) { continue; }

		printk("  [%s] (%u bytes)\n", entry.name, (unsigned)entry.size);

		/* Read and print file content (up to 240 chars) */
		struct fs_file_t file;
		fs_file_t_init(&file);
		char fpath[260];
		snprintf(fpath, sizeof(fpath), "%s%s", MO_FS_PREFIX, entry.name);
		ret = fs_open(&file, fpath, FS_O_READ);
		if (ret == 0) {
			ssize_t n = fs_read(&file, buf, sizeof(buf) - 1);
			if (n > 0) {
				buf[n] = '\0';
				printk("    %.*s%s\n",
				       (n > 240 ? 240 : (int)n), buf,
				       n > 240 ? "..." : "");
			}
			fs_close(&file);
		}
	}

	fs_closedir(&dir);

	/* FS usage */
	struct fs_statvfs st;
	if (fs_statvfs(MO_FS_PREFIX, &st) == 0) {
		unsigned long total_kb = (st.f_blocks * st.f_frsize) / 1024;
		unsigned long free_kb  = (st.f_bfree * st.f_frsize) / 1024;
		printk("  [FS] %lu / %lu KB used\n", total_kb - free_kb, total_kb);
	}
	printk("--- end MO config ---\n\n");
}

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

	/* Dump MO configuration persisted on flash */
	print_mo_config();

	while (1) {
		mocpp_loop();
		conn.loop();
		k_msleep(10);
	}
}
