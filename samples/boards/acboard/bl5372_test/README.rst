.. zephyr:code-sample:: acboard_bl5372_test
   :name: ACBoard BL5372 RTC test

ACBoard BL5372 RTC test
#######################

This sample validates the BL5372 real-time clock on ACBoard-F527 using I2C2 at
100 kHz. ``PA8`` is SCL and ``PC9`` is SDA. The BL5372 uses the 7-bit I2C
address ``0x32``.

The sample reads and prints the retained date and time every second. If the
BL5372 ``XSTP`` flag indicates power loss or oscillator stop, it initializes
the clock once to the fixed test value Friday, June 12, 2026 at 14:30:00.
Subsequent MCU resets preserve an already valid RTC value.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/bl5372_test
   :board: acboard_f527
   :goals: build flash
