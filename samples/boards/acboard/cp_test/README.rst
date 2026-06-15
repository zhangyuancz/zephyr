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

The driver owns the PWM, ADC, capture, and diode GPIO. It samples the
hardware-triggered ADC, converts the positive peak to a real CP voltage using
``full-scale-millivolt`` (ADC reference / sense-divider ratio), and classifies
the IEC 61851 state (A..E). The application registers nothing extra: it sets the
duty from the console and prints the state, voltage, diode, and PWM feedback
every 500 ms.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/cp_test
   :board: acboard_f527
   :goals: build flash

Console commands
****************

* ``0``: 0% duty
* ``1``: 100% duty (constant +12 V, standby)
* ``2``: 53% duty (advertises ~32 A)
* ``3``: 10% duty
* ``+`` / ``-``: adjust duty by 1%
* ``h`` or ``?``: print command help
