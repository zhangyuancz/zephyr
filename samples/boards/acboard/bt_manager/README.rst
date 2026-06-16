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
* exposes ``bt_manager_send()``, ``bt_manager_device_count()`` and
  ``bt_manager_forget()``.

The demo ``main.c`` brings the manager up and echoes any received PDU back to
the sender. Pair a phone with the advertised device ``ID. UNYX Pro AC999``: the
manager records the device, and the record survives resets (it is reloaded from
flash on the next boot). The next layer to add on top is the APP PDU codec.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/bt_manager
   :board: acboard_f527
   :goals: build flash
