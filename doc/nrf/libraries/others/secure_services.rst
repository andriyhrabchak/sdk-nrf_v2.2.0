.. _lib_secure_services:

Secure Services
###############

.. contents::
   :local:
   :depth: 2

Secure Services are functions implemented in the firmware in Secure Processing Environment (SPE), but made available to be called from firmware in Non-Secure Processing Environment (NSPE).

Calling functions in this API requires that the service is enabled in the :ref:`secure_partition_manager`.
See :kconfig:option:`CONFIG_SPM_SECURE_SERVICES` in the :ref:`secure_partition_manager`'s menuconfig.
Some services are enabled by default.

.. Remove parts with regards to debugging and programming when NRF91-313 is resolved

By default :kconfig:option:`CONFIG_SPM_BLOCK_NON_SECURE_RESET` is disabled. This is to make sure that your debugger will be able to issue a system reset during the development stage and that devices which do not have pin-reset routed can do a re-flashing routine correctly. This option should be turned off when you are putting a product into production to increase the security of your device.

API documentation
*****************

| Header file: :file:`include/secure_services.h`
| Source file: :file:`subsys/spm/secure_services.c` (compiled into the :ref:`secure_partition_manager`)

.. doxygengroup:: secure_services
   :project: nrf
   :members:
