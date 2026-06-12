.. zephyr:code-sample:: acboard_sp706_test
   :name: ACBoard SP706 external watchdog test

ACBoard SP706 external watchdog test
####################################

This sample validates the SP706 external watchdog connected to ``PD11``. It
toggles WDI every 500 ms for five seconds, then intentionally stops feeding.
The SP706 nominal timeout is 1.6 seconds, with a specified range of 1.0 to
2.25 seconds at 3.3 V.

If the board connects the SP706 watchdog output into its reset path, the MCU
will restart after feeding stops. Repeated boot banners therefore indicate a
successful end-to-end watchdog reset test.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/sp706_test
   :board: acboard_f527
   :goals: build flash
