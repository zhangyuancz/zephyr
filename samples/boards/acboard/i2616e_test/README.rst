.. zephyr:code-sample:: acboard_i2616e_test
   :name: ACBoard i2616e BLE module test

ACBoard i2616e BLE module test
################################

This sample validates the UART and control signals connected to the BARROT
i2616e BLE 5.4 module on ACBoard-F527.

The sample uses these signals:

* ``PD8 / USART2_TX``: module UART receive
* ``PD9 / USART2_RX``: module UART transmit
* ``PD12``: module hardware reset, active low

The module is continuously powered. The sample drives reset low for 200 ms,
returns it high, prints asynchronous startup data, and
queries its firmware version, configured name, local Bluetooth address, mode,
and advertising state. It then sends ``AT+ADV=1`` and confirms the advertising
state. Commands end with carriage return as required by the module.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/i2616e_test
   :board: acboard_f527
   :goals: build flash
