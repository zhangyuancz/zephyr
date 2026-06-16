.. zephyr:code-sample:: acboard_flash_test
   :name: ACBoard internal flash test

ACBoard internal flash test
###########################

This sample exercises the GD32F527 on-chip flash (FMC v4 driver,
:kconfig:option:`CONFIG_GD32_NV_FLASH_V4`) on ACBoard-F527.

The board overlay defines a 128KB scratch partition ``storage_partition`` at
flash offset ``0x380000`` (sector 36, in Bank1), well above the application
image so the test is non-destructive to running code.

The sample:

#. opens the partition through the flash map API,
#. reports the sector index and size at the partition start,
#. erases the first sector and verifies it reads back as ``0xff``,
#. writes a 64-byte pattern and reads it back to confirm the data matches.

On success it prints ``PASS: write/read-back verified``.

Build and flash
***************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/acboard/flash_test
   :board: acboard_f527
   :goals: build flash
