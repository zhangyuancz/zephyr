.. zephyr:code-sample:: acboard_cp_test
   :name: ACBoard control pilot test

ACBoard control pilot test
##########################

This standalone sample validates the AC charging control pilot hardware without
the modem, TLS, WebSocket, OCPP, or external storage stacks.

The sample uses these ACBoard signals:

* ``PA2 / TIMER1_CH2``: 1 kHz CP PWM output
* ``PA1 / TIMER1_CH1``: inverted auxiliary compare output at half the CP high time
* ``PA6 / TIMER2_CH0``: actual CP PWM frequency and duty feedback
* ``PC1 / ADC0_CH11``: CP voltage feedback
* ``PC2``: CP diode detection input

ADC0 regular conversions are triggered by the ``TIMER1_CH1`` compare event, so
each reading is taken at the midpoint of the CP PWM high level. The CH1 compare
value is updated whenever the CP duty changes. CH2 uses PWM0 with active-high
polarity and CH1 uses PWM0 with active-low polarity, matching the production
BSP timing. The UART console prints the
minimum, average, and maximum of 32 synchronized samples every 500 ms. The
maximum value is classified with the provisional raw thresholds already used by
the charger application: A >= 3500, B >= 2600, C >= 1700, and E <= 500. These
thresholds must be calibrated against the production board.

The application uses only Zephyr ``pwm_set_dt()``, ``pwm_capture_cycles()``,
``adc_read()``, and GPIO APIs. GD32 register programming for the paired compare,
PWM input capture, and ADC hardware trigger is contained in the respective
Zephyr drivers and configured through devicetree.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/cp_test
   :board: acboard
   :goals: build flash

For the GD32F527 board, use ``acboard_f527`` as the board name. The sample
keeps the same physical CP signals while applying the F527 timer and ADC clock
configuration in its board-specific overlay.

Console commands
****************

The test uses polling UART input instead of the Zephyr shell to keep RAM usage
small.

* ``0``: set 0% duty
* ``1``: set 100% duty
* ``2``: set 53% duty
* ``3``: set 10% duty
* ``+`` / ``-``: adjust duty by 1%
* ``h`` or ``?``: print command help
