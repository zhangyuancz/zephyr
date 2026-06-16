.. zephyr:code-sample:: acboard_littlefs_config
   :name: ACBoard LittleFS config storage

ACBoard LittleFS config storage
###############################

This sample manages the GD32F527 Bank1 flash (2MB, ``0x08200000``) as a
LittleFS filesystem for persistent configuration, leaving Bank0 for the
application image.

The board overlay declares a ``storage_partition`` covering all of Bank1 and a
``zephyr,fstab`` LittleFS entry that auto-mounts it at ``/lfs``. Bank1 erase
blocks are 128KB; the first block is physically the ``4x16KB + 64KB`` group,
which sums to exactly 128KB, so a uniform 128KB LittleFS block aligns across the
whole bank.

The sample:

#. confirms ``/lfs`` is mounted and prints its block geometry,
#. keeps a persistent boot counter in ``/lfs/boot_count`` that increments on
   every reset,
#. writes an example device config to ``/lfs/device.cfg`` on the first boot and
   reads it back afterwards.

Reset the board and the boot counter keeps climbing, demonstrating that the
configuration survives resets and power cycles. This is the storage backend
intended for Bluetooth paired-device records and other runtime configuration.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/littlefs_config
   :board: acboard_f527
   :goals: build flash
