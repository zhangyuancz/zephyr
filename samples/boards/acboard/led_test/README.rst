.. zephyr:code-sample:: acboard_led_test
   :name: ACBoard onboard LED test

ACBoard onboard LED test
########################

This sample alternately blinks the two active-low onboard LEDs on ACBoard-F527.
``PE8`` drives LED 0 and ``PE9`` drives LED 1. A low GPIO level turns a LED on.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/led_test
   :board: acboard_f527
   :goals: build flash
