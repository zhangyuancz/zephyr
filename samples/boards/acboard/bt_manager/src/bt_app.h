/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ACBOARD_BT_APP_H_
#define ACBOARD_BT_APP_H_

#include "bt_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

void bt_app_on_event(const struct bt_mgr_event *event);

#ifdef __cplusplus
}
#endif

#endif /* ACBOARD_BT_APP_H_ */
