.. zephyr:code-sample:: acboard_adc_inputs_test
   :name: ACBoard general ADC inputs test

ACBoard general ADC inputs test
################################

This sample validates four general-purpose analog inputs on ACBoard-F527:

* ``PA0``: ADC0 channel 0
* ``PA3``: ADC0 channel 3
* ``PC0``: ADC0 channel 10
* ``PC3``: ADC0 channel 13

Each input is sampled 16 times once per second. The sample prints the minimum,
average, and maximum 12-bit raw values, plus an estimated voltage based on a
nominal 3300 mV ADC reference. Accurate voltage measurements require the
actual board VDDA value or application-level calibration.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/adc_inputs_test
   :board: acboard_f527
   :goals: build flash
