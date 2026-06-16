.. zephyr:code-sample:: acboard_bt_manager
   :name: ACBoard Bluetooth manager

ACBoard Bluetooth manager
#########################

This sample builds the application-facing Bluetooth port on top of the BARROT
i2616e driver. It is the ``IBluetoothPort`` / ``BluetoothManager`` layer of the
wallbox firmware: the APP protocol talks to it by logical channel number and
never sees the module CID strings.

The manager (``src/bt_manager.c``):

* maps i2616e connection ids (CIDs) to logical channels (0..2),
* normalises module events into ``CONNECTED`` / ``AUTHENTICATED`` /
  ``DISCONNECTED`` / ``DATA`` delivered through a single callback,
* enforces the bring-up pairing policy (accept while fewer than three devices
  are paired),
* persists the paired-device table through the Settings subsystem
  (``cfg/bt`` subtree on the Bank1 LittleFS) and mirrors authorisations into
  the module whitelist,
* exposes channel send, paired-device query/update, plug-and-charge flag, and
  forget operations.

The APP protocol layer (``src/bt_app.c``) implements the wallbox phone-app frame
format (``0xff, node, command, payload length, payload, checksum``) and handles:

* authentication check,
* pile status query,
* charge start/stop command response,
* paired-device list query,
* APP device-name report,
* delete paired/current device,
* plug-and-charge enable/disable/query.

Pair a phone with the advertised device ``ID. UNYX Pro AC999``. The manager
records the device, the APP protocol can fill in its name and plug-and-charge
flag, and the record survives resets. Charging control is currently a bring-up
stub inside the sample; replace the local simulated status with the real charger
service when this layer is moved into the product application.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/bt_manager
   :board: acboard_f527
   :goals: build flash
