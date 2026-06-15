.. zephyr:code-sample:: acboard_relay_test
   :name: ACBoard relay control test

ACBoard relay control test
##########################

This sample drives the ACBoard-F527 charging contactor through the in-tree
``zephyr,gpio-relay`` driver. The contactor has two poles (channel 0 = L / K1,
channel 1 = N / K2), a master enable gate, and an output-feedback input used for
weld detection. The driver owns those GPIOs, applies break-before-make
sequencing, and reports welded contacts (output live while commanded open)
through a callback.

The application registers the weld callback and drives the relay from the
console: ``0`` (open), ``1`` (L pole only), ``2`` (N pole only), ``3`` (both),
``s`` (status), ``h`` (help).

.. note::

   Weld detection senses mains voltage on the relay output, so it only triggers
   with the high-voltage side connected. Without mains it always reads normal.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/relay_test
   :board: acboard_f527
   :goals: build flash
