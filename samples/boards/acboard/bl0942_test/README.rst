.. zephyr:code-sample:: acboard_bl0942_test
   :name: ACBoard BL0942 energy-meter sensor test

ACBoard BL0942 energy-meter sensor test
#######################################

This sample reads the Shanghai Belling BL0942 single-phase energy metering IC
on ACBoard-F527 through the Zephyr :ref:`sensor` API. The BL0942 is described
as a child of ``UART4`` (9600 baud, 8N1; ``PC12`` TX, ``PD2`` RX) and is driven
by the in-tree ``belling,bl0942`` driver, which owns the UART, issues the full
measurement packet command, validates the response checksum, and converts the
raw registers to physical units.

The sample periodically calls :c:func:`sensor_sample_fetch` and prints voltage,
current, active power, line frequency, and the active-energy pulse count.

.. note::

   The reported voltage, current, and power depend on the board analog
   front-end. The overlay values match the ACBoard ``CAMS_METER`` schematic: a
   2000:1 current transformer with a 1.5 ohm burden (0.75 mOhm effective) and a
   ZMPT107 voltage transformer (ratio ~4016). Adjust them for other hardware.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/bl0942_test
   :board: acboard_f527
   :goals: build flash
