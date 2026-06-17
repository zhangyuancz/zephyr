/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * L511C OCPP over WSS (WebSocket Secure / TLS with mTLS) on ACBoard.
 *
 * The cellular modem bring-up (power, AT init, dial, PPP) is handled by the
 * lynq,l511 driver, which brings up the PPP network interface. This app waits
 * for connectivity, opens a mutually-authenticated TLS WebSocket to the OCPP
 * server, and hands the socket to the OCPP bridge.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/ppp.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/websocket.h>
#include <zephyr/logging/log.h>

#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>

#include "tls_certificates.h"
#include "ocpp_bridge.h"

LOG_MODULE_REGISTER(l511c, LOG_LEVEL_INF);

/* OCPP server (WSS / TLS) */
#define OCPP_SERVER_HOST  "47.111.208.192"
#define OCPP_SERVER_PORT  "6669"
#define OCPP_SERVER_PATH  "/ocppj/812345678"
#define OCPP_CHARGEBOX_ID "812345678"

/* WS temp buffer (used during handshake) */
#define WS_TEMP_BUF_LEN  1024

/* PPP / connectivity events */
#define PPP_EVENT_CONNECTED     BIT(0)
#define PPP_EVENT_DISCONNECTED  BIT(1)
#define PPP_EVENT_DNS_READY     BIT(2)

/* The lynq,l511 driver owns the modem and creates the PPP interface, which is
 * the only (default) network interface.
 */
#define CELL_IFACE net_if_get_default()
K_EVENT_DEFINE(ppp_events);

/* ===== Net mgmt handlers ===== */

static void l4_event_handler(uint64_t event, struct net_if *iface,
			     void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info); ARG_UNUSED(info_length); ARG_UNUSED(user_data);
	if (iface != CELL_IFACE) { return; }
	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		k_event_clear(&ppp_events, PPP_EVENT_DISCONNECTED);
		k_event_post(&ppp_events, PPP_EVENT_CONNECTED);
		break;
	case NET_EVENT_DNS_SERVER_ADD:
	case NET_EVENT_DNS_SERVERS_RECONFIGURED: k_event_post(&ppp_events, PPP_EVENT_DNS_READY); break;
	case NET_EVENT_L4_DISCONNECTED:
		k_event_clear(&ppp_events, PPP_EVENT_CONNECTED | PPP_EVENT_DNS_READY);
		k_event_post(&ppp_events, PPP_EVENT_DISCONNECTED);
		break;
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
	case NET_EVENT_PPP_PHASE_DEAD:   LOG_DBG("ppp: DEAD"); break;
	case NET_EVENT_PPP_PHASE_RUNNING: LOG_DBG("ppp: RUN"); break;
	case NET_EVENT_PPP_CARRIER_ON:   LOG_INF("ppp: carrier on"); break;
	case NET_EVENT_PPP_CARRIER_OFF:
		LOG_INF("ppp: carrier off");
		k_event_clear(&ppp_events, PPP_EVENT_CONNECTED | PPP_EVENT_DNS_READY);
		k_event_post(&ppp_events, PPP_EVENT_DISCONNECTED);
		break;
	}
}
NET_MGMT_REGISTER_EVENT_HANDLER(ppp_phase, NET_EVENT_PPP_PHASE_DEAD |
	NET_EVENT_PPP_PHASE_RUNNING | NET_EVENT_PPP_CARRIER_ON |
	NET_EVENT_PPP_CARRIER_OFF, ppp_event_handler, NULL);

/* ===== WSS connect (TLS + mTLS) ===== */

/* TLS verify callback: skip CN/SAN hostname check since server cert has no CN */
static int tls_verify_cb(void *data, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
	ARG_UNUSED(data); ARG_UNUSED(crt); ARG_UNUSED(depth);
	*flags &= ~MBEDTLS_X509_BADCERT_CN_MISMATCH;
	return 0;
}

static uint8_t ws_temp_buf[WS_TEMP_BUF_LEN];

static int tls_credential_add_once(sec_tag_t tag,
				   enum tls_credential_type type,
				   const void *cred, size_t credlen)
{
	int ret = tls_credential_add(tag, type, cred, credlen);

	return ret == -EEXIST ? 0 : ret;
}

static int ws_connect(void)
{
	struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *ai = NULL;
	struct sockaddr_in peer = {0};
	struct websocket_request req;
	char addr[NET_IPV4_ADDR_LEN];
	sec_tag_t sec_tags[] = { CA_CERTIFICATE_TAG, CLIENT_CERT_TAG };
	int sock = -1, ws_fd = -1, ret;

	/* Register certificates for mTLS */
	ret = tls_credential_add_once(CA_CERTIFICATE_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
				      ca_certificate, sizeof(ca_certificate));
	if (ret < 0) { LOG_ERR("CA cert add err: %d", ret); return ret; }
	LOG_DBG("CA cert ok (tag=%d)", CA_CERTIFICATE_TAG);

	ret = tls_credential_add_once(CLIENT_CERT_TAG, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
				      client_certificate, sizeof(client_certificate));
	if (ret < 0) { LOG_ERR("Client cert add err: %d", ret); return ret; }

	ret = tls_credential_add_once(CLIENT_CERT_TAG, TLS_CREDENTIAL_PRIVATE_KEY,
				      client_private_key, sizeof(client_private_key));
	if (ret < 0) { LOG_ERR("Client key add err: %d", ret); return ret; }
	LOG_DBG("Client cert+key ok (tag=%d)", CLIENT_CERT_TAG);

	/* Sync time for cert validity check */
	{
		struct sntp_time sntp_ts;
		struct timespec ts;
		LOG_DBG("SNTP ...");
		ret = sntp_simple("203.107.6.88", 5000, &sntp_ts);
		if (ret < 0) { ret = sntp_simple("ntp.aliyun.com", 8000, &sntp_ts); }
		if (ret >= 0) {
			ts.tv_sec = (time_t)sntp_ts.seconds; ts.tv_nsec = 0;
			clock_settime(CLOCK_REALTIME, &ts);
			LOG_DBG("SNTP ok");
		} else {
			/* Fallback: June 8, 2026 */
			ts.tv_sec = 1780876800; ts.tv_nsec = 0;
			clock_settime(CLOCK_REALTIME, &ts);
			LOG_WRN("SNTP fail (%d), using hardcoded time", ret);
		}
	}

	/* DNS resolve */
	LOG_DBG("resolve %s:%s", OCPP_SERVER_HOST, OCPP_SERVER_PORT);
	ret = zsock_getaddrinfo(OCPP_SERVER_HOST, OCPP_SERVER_PORT, &hints, &ai);
	if (ret != 0 || !ai) { ret = -ENOENT; goto out; }
	memcpy(&peer, ai->ai_addr, sizeof(peer));
	zsock_freeaddrinfo(ai); ai = NULL;
	zsock_inet_ntop(AF_INET, &peer.sin_addr, addr, sizeof(addr));
	LOG_DBG("-> %s:%u", addr, ntohs(peer.sin_port));

	/* Test plain TCP connectivity first */
	LOG_DBG("TCP test to %s:%u ...", addr, ntohs(peer.sin_port));
	{
		int tsock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (tsock >= 0) {
			struct timeval tv = {.tv_sec = 10};
			zsock_setsockopt(tsock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
			int tret = zsock_connect(tsock, (struct sockaddr *)&peer, sizeof(peer));
			if (tret < 0) {
				LOG_WRN("TCP test fail: %d", -errno);
			} else {
				LOG_DBG("TCP test ok (SYN reached server)");
			}
			zsock_close(tsock);
		} else {
			LOG_WRN("TCP test socket create fail: %d", -errno);
		}
	}

	/* TLS socket — mutual auth: verify server cert, skip hostname check */
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (sock < 0) {
		ret = -errno;
		LOG_ERR("TLS socket err: %d", ret);
		goto out;
	}

	zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, sec_tags, sizeof(sec_tags));
	{
		int verify_req = TLS_PEER_VERIFY_REQUIRED;
		zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
				 &verify_req, sizeof(verify_req));
	}
	/* Set hostname to prevent Zephyr's forced empty-string verification.
	 * Custom verify callback strips CN_MISMATCH — server cert has no CN,
	 * so hostname check against IP would always fail.
	 */
	zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
			 OCPP_SERVER_HOST, sizeof(OCPP_SERVER_HOST) - 1);
	{
		struct zsock_tls_cert_verify_cb vfy_cb = {
			.cb = tls_verify_cb,
			.ctx = NULL,
		};
		zsock_setsockopt(sock, SOL_TLS, TLS_CERT_VERIFY_CALLBACK,
				 &vfy_cb, sizeof(vfy_cb));
	}
	{
		struct timeval tv = {.tv_sec = 15};
		zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}

	LOG_DBG("TLS connecting ...");
	ret = zsock_connect(sock, (struct sockaddr *)&peer, sizeof(peer));
	if (ret < 0) { LOG_ERR("TLS connect err: %d", -errno); ret = -errno; goto out; }
	LOG_INF("TLS ok");

	/* WebSocket upgrade over TLS */
	const char *ocpp_headers[] = {
		"Sec-WebSocket-Protocol: ocpp2.0\r\n",
		NULL
	};
	memset(&req, 0, sizeof(req));
	req.host = OCPP_SERVER_HOST; req.url = OCPP_SERVER_PATH;
	req.optional_headers = ocpp_headers;
	req.tmp_buf = ws_temp_buf; req.tmp_buf_len = sizeof(ws_temp_buf);
	ws_fd = websocket_connect(sock, &req, 15000, NULL);
	if (ws_fd < 0) { LOG_ERR("ws upgrade err: %d", ws_fd); ret = ws_fd; goto out; }
	LOG_INF("WSS ok (fd=%d)", ws_fd);
	return ws_fd;

out:
	if (ws_fd >= 0) { websocket_disconnect(ws_fd); }
	if (sock >= 0)  { zsock_close(sock); }
	if (ai) { zsock_freeaddrinfo(ai); }
	return ret;
}

static bool cellular_ready(void)
{
	struct net_if *iface = CELL_IFACE;

	return iface != NULL && net_if_is_up(iface) &&
	       net_if_is_carrier_ok(iface) && !net_if_is_dormant(iface);
}

static int wait_for_ppp_link(void)
{
	LOG_INF("waiting for cellular PPP link (driver brings it up) ...");

	if (cellular_ready()) {
		k_event_post(&ppp_events, PPP_EVENT_CONNECTED);
		return 0;
	}

	if ((k_event_wait(&ppp_events, PPP_EVENT_CONNECTED, false, K_SECONDS(180)) &
	     PPP_EVENT_CONNECTED) == 0) {
		LOG_ERR("PPP timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

/* ===== Main ===== */

int main(void)
{
	LOG_INF("=== L511C OCPP on WSS ===");

	while (1) {
		if (wait_for_ppp_link() < 0) {
			k_msleep(5000);
			continue;
		}

		LOG_INF("PPP ok");
		if ((k_event_wait(&ppp_events, PPP_EVENT_DNS_READY, false, K_SECONDS(2)) &
		     PPP_EVENT_DNS_READY) == 0) {
			LOG_DBG("DNS not ready yet, continue with IP endpoint");
		}
		k_msleep(200);

		int ws_fd = ws_connect();
		if (ws_fd < 0) {
			LOG_ERR("WS fail: %d", ws_fd);
			k_msleep(5000);
			continue;
		}

		LOG_INF("OCPP bridge start ...");
		int ret = ocpp_bridge_run(ws_fd, OCPP_SERVER_HOST, OCPP_CHARGEBOX_ID);
		LOG_WRN("OCPP bridge stopped: %d", ret);
		k_msleep(5000);
	}
}
