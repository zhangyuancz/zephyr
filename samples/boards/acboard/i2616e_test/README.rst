.. zephyr:code-sample:: acboard_i2616e_test
   :name: ACBoard i2616e BLE module test

ACBoard i2616e BLE module test
################################

This sample exercises the BARROT i2616e BLE 5.4 module driver
(:kconfig:option:`CONFIG_I2616E`) on ACBoard-F527. The module is described as a
``barrot,i2616e`` child of ``usart2`` in the board overlay.

The sample uses these signals:

* ``PD8 / USART2_TX``: module UART receive
* ``PD9 / USART2_RX``: module UART transmit
* ``PD12``: module hardware reset, active low

The module is continuously powered. The sample resets the module through the
driver and waits for the ``IM_READY`` indication, reads the module information
(firmware/config version, name, local Bluetooth address, baudrate, flow control
and BLE mode), applies the bring-up settings (device name, peripheral mode, PDU
command mode, multi-connection, auto-unlock and advertising), prints the
whitelist, and then loops delivering asynchronous events. Pairing requests are
auto-accepted and received PDUs are echoed back to the peer.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/i2616e_test
   :board: acboard_f527
   :goals: build flash
