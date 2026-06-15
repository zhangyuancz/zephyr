.. zephyr:code-sample:: acboard_mc001_test
   :name: ACBoard MC001 residual-current detector test

ACBoard MC001 residual-current detector test
############################################

This sample reads the Mega-senway MastCurr MC001 residual-current (leakage)
detection module on ACBoard-F527 through the Zephyr :ref:`sensor` API. The
module is described with the ``megasenway,mc001`` binding and driven by the
in-tree driver, which owns the TRIP and Zero Cal. GPIOs.

At init the driver runs the power-on zero-calibration sequence. The sample then
reads the initial trip state with :c:func:`sensor_sample_fetch` /
:c:func:`sensor_channel_get` and registers a :c:enumerator:`SENSOR_TRIG_THRESHOLD`
trigger, so every debounced TRIP edge is reported through the callback.

It then runs the module self-test (``SENSOR_ATTR_MC001_SELF_TEST``), which
injects a simulated residual current and verifies that TRIP asserts. This
exercises the full trip path, including the trigger callback, without needing a
real fault current.

The TRIP and Zero Cal. GPIO polarities in the overlay account for the board NPN
buffers between the module and the MCU (``CAMS_A+6mA`` schematic).

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/mc001_test
   :board: acboard_f527
   :goals: build flash
