.. zephyr:code-sample:: acboard_l511_test
   :name: ACBoard Lynq L511 modem test

ACBoard Lynq L511 modem test
############################

Brings up the Lynq L511 LTE Cat.1 cellular modem through the ``lynq,l511``
driver (:kconfig:option:`CONFIG_MODEM_LYNQ_L511`) and waits for the PPP network
interface to reach connectivity.

The modem is described as a ``lynq,l511`` child of ``usart0`` with the
power-key and reset GPIOs; the driver powers the module, runs the AT
init/registration/dial scripts and hands the UART over to PPP. The sample only
configures networking and waits for ``NET_EVENT_L4_CONNECTED``, then prints the
assigned IPv4 address — all the modem bring-up that used to live in the
application now lives in the driver.

A SIM card and cellular coverage are required. The APN defaults to ``cmnet``
(:kconfig:option:`CONFIG_MODEM_LYNQ_L511_APN`).

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/l511_test
   :board: acboard_f527
   :goals: build flash
