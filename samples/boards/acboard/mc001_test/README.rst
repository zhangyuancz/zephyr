.. zephyr:code-sample:: acboard_mc001_test
   :name: ACBoard MC001 residual-current detector test

ACBoard MC001 residual-current detector test
################################################

This sample validates the MC001 residual-current detector interface on
ACBoard-F527.

* ``PB8`` is the board-level active-low trip input. Both edges generate an
  interrupt; the level is sampled after a short debounce delay and remains the
  authoritative trip state.
* ``PE0`` is the board-level active-high zero-calibration control. The sample
  asserts it for 75 ms, then waits 500 ms for calibration to complete before
  monitoring the trip input.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/mc001_test
   :board: acboard_f527
   :goals: build flash
