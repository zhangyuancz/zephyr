/*
 * OCPP Bridge - C interface to MicroOcpp C++ adapter
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OCPP_BRIDGE_H
#define OCPP_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/** Start the OCPP stack. Blocks forever (runs the event loop). */
void ocpp_bridge_start(int ws_fd, const char *server_host, const char *charge_box_id);

#ifdef __cplusplus
}
#endif

#endif
