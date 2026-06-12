.. zephyr:code-sample:: acboard_emergency_stop_test
   :name: ACBoard emergency-stop input test

ACBoard emergency-stop input test
#################################

This sample validates the active-low emergency-stop input on ``PA4``. Both
edges generate a Zephyr GPIO interrupt. A low level immediately latches the
emergency-stop event, and delayed work confirms the physical level after 5 ms.

Returning PA4 high reports that the input has recovered, but deliberately does
not clear the latched emergency-stop event. Production control logic must use
an explicit safety reset procedure before re-enabling power outputs.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/emergency_stop_test
   :board: acboard_f527
   :goals: build flash
