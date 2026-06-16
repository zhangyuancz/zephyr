# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

board_runner_args(jlink "--device=GD32F527VMT7" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
