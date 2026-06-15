.. zephyr:code-sample:: acboard_ws1850t_test
   :name: ACBoard WS1850T RFID test

ACBoard WS1850T RFID test
#########################

This sample reads the WiseSun WS1850T 13.56 MHz contactless card reader on
ACBoard-F527 through the in-tree ``wisesun,ws1850t`` driver. The reader is
described as a child of ``USART1`` (9600 baud, 8N1; ``PD5`` TX, ``PD6`` RX) with
a reset GPIO on ``PD4``. The driver owns the UART register interface and the
reset line, performs the reset and antenna/timer initialisation, and exposes a
device-specific API (there is no generic RFID subsystem in Zephyr).

The sample reads the version register, then polls
:c:func:`ws1850t_read_card` and prints the ATQA and cascade-level-one UID of any
ISO 14443-A card presented to the field.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/ws1850t_test
   :board: acboard_f527
   :goals: build flash
