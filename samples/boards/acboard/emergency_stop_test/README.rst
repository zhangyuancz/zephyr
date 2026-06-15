.. zephyr:code-sample:: acboard_emergency_stop_test
   :name: ACBoard emergency-stop input test

ACBoard emergency-stop input test
#################################

This sample validates the active-low emergency-stop input on ``PA4`` using the
Zephyr :ref:`input` subsystem. The pin is described as a ``gpio-keys`` key, so
the in-tree ``gpio-keys`` driver owns the GPIO, performs the 5 ms debounce, and
publishes :c:enum:`INPUT_EV_KEY` events. The application only registers an input
callback with :c:macro:`INPUT_CALLBACK_DEFINE`.

An asserted input (``value != 0``) immediately latches the emergency-stop event.
Releasing the input (``PA4`` returns high) reports recovery but deliberately does
not clear the latch. Production control logic must use an explicit safety reset
procedure before re-enabling power outputs.

Because the input subsystem only reports transitions, the application also takes
a one-shot read of the pin at start-up so an emergency stop that is already
asserted at boot is latched immediately.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/emergency_stop_test
   :board: acboard_f527
   :goals: build flash
