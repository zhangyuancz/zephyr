.. zephyr:code-sample:: acboard_relay_test
   :name: ACBoard relay control test

ACBoard relay control test
##########################

This sample validates the relay interface on ACBoard-F527. All outputs are
inactive at startup and only change in response to a console command.

* ``PE1``: active-high relay driver enable
* ``PE2``: active-high relay control 1
* ``PE3``: active-high relay control 2
* ``PE4``: active-low relay weld-detection input; low indicates a welded relay

Console commands are ``0`` (all off), ``1`` (control 1), ``2`` (control 2),
``3`` (both controls), and ``s`` (print status). Control outputs are always
cleared before the driver enable is removed.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/relay_test
   :board: acboard_f527
   :goals: build flash
