.. zephyr:code-sample:: acboard_bl0942_test
   :name: ACBoard BL0942 UART test

ACBoard BL0942 UART test
########################

This sample validates interrupt-driven UART communication with the BL0942
metering IC on ACBoard-F527.

The sample uses ``UART4`` at 9600 baud, 8 data bits, no parity, and one stop
bit. ``PC12`` is UART TX and ``PD2`` is UART RX. It periodically sends the
device-address-zero full measurement packet command (``58 AA``), waits for the
23-byte response beginning with ``55``, validates its checksum, and prints the
raw measurement registers.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/bl0942_test
   :board: acboard_f527
   :goals: build flash
