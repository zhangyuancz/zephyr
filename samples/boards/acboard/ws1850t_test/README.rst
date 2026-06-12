.. zephyr:code-sample:: acboard_ws1850t_test
   :name: ACBoard WS1850T RFID test

ACBoard WS1850T RFID test
#########################

This sample validates interrupt-driven UART communication with the WS1850T
RFID reader IC on ACBoard-F527. It uses ``USART1`` at 9600 baud, 8 data bits,
no parity, and one stop bit. ``PD5`` is USART TX and ``PD6`` is USART RX.
``PD4`` drives the board-level active-high reset signal.

The sample reads the chip version register, initializes the ISO/IEC 14443A
frontend, and polls for a card. When a card is present it prints the ATQA and
the cascade-level-one UID bytes returned by anticollision.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/ws1850t_test
   :board: acboard_f527
   :goals: build flash
