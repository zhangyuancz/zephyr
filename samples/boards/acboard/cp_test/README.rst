.. zephyr:code-sample:: acboard_cp_test
   :name: ACBoard control pilot test

ACBoard control pilot test
##########################

This sample drives the AC charging control pilot (CP) through the in-tree
``zephyr,control-pilot`` driver, standalone from the modem, TLS, WebSocket,
OCPP, and storage stacks.

The CP node wires these ACBoard signals:

* ``PA2 / TIMER1_CH2``: 1 kHz CP PWM output (name ``cp``)
* ``PA1 / TIMER1_CH1``: inverted auxiliary compare that triggers the ADC at the
  midpoint of the CP PWM high level
* ``PA6 / TIMER2_CH0``: CP PWM frequency/duty feedback capture (name ``feedback``)
* ``PC1 / ADC0_CH11``: CP voltage feedback
* ``PC2``: CP diode detection input

The driver owns the PWM, ADC, capture, and diode GPIO. ADC and PWM capture run
continuously from hardware-triggered interrupts; each read copies their latest
complete values without waiting. The driver converts the positive level to a real
CP voltage using ``adc-reference-millivolt`` and the actual values in
``sense-divider-resistors-ohms``, then classifies the GB/T 18487.1 state (0
through 4, including PWM prime states).

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/cp_test
   :board: acboard_f527
   :goals: build flash

Console commands
****************

* ``0``: 0% duty (continuous -12 V / state 4)
* ``1``: 100% duty (constant +12 V, standby)
* ``2``: 53% duty (advertises ~32 A)
* ``3``: 10% duty
* ``+`` / ``-``: adjust duty by 1%
* ``h`` or ``?``: print command help
