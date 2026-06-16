.. zephyr:code-sample:: acboard_settings_config
   :name: ACBoard settings config store

ACBoard settings config store
#############################

This sample uses the Zephyr :ref:`settings_api` subsystem as the ACBoard
configuration store. The ``SETTINGS_FILE`` backend persists settings into the
LittleFS on Bank1 (``/lfs/settings``), so the same 2MB flash region backs both
files and configuration.

A Bluetooth paired-device table is modeled under the ``cfg/bt`` subtree, one
record per key ``cfg/bt/dev/<i>``. The handler's ``h_set`` loads records into
RAM at boot and ``h_export`` writes them back; records also carry a version
byte so fields can grow later.

The sample:

#. initialises the settings subsystem and loads the ``cfg/bt`` subtree,
#. prints the paired-device table restored from flash,
#. on the first boot enrolls two demo devices (``settings_save_one``),
#. on later boots toggles ``dev0``'s plug-and-charge flag and persists it.

Reset the board: the device table is restored from flash and ``dev0``'s
``pnc`` flag flips each boot, demonstrating that both whole records and field
updates survive resets and power cycles. This is the configuration backbone
intended for the Bluetooth paired-device policy and other runtime settings.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/settings_config
   :board: acboard_f527
   :goals: build flash
