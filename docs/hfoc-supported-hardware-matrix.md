# HFOC Supported Hardware Matrix

**Release ID:** HFOC-APPLIANCE-V1

This matrix is the canonical hardware support record. A row may be changed
from `NOT TESTED` to `PASS` only when the Hardware Acceptance Report records
hardware model, firmware/BIOS version, CPU, RAM, disk, NIC, test date, HFOC
commit/release ID, and observed result.

QEMU software proof is not hardware support evidence.

| Hardware category | Current status | Notes |
| --- | --- | --- |
| UEFI firmware boot | NOT TESTED | No physical UEFI acceptance recorded |
| Legacy BIOS boot | NOT TESTED | No physical legacy acceptance recorded |
| CPU | NOT TESTED | QEMU CPU profile only |
| RAM | NOT TESTED | QEMU memory profile only |
| APIC/LAPIC | NOT TESTED | QEMU interrupt profile only |
| Timer | NOT TESTED | QEMU timer profile only |
| Keyboard | NOT TESTED | QEMU keyboard paths only: PS/2 and, since ITEM-395, a USB HID boot-protocol keyboard behind an xHCI controller - every controller and every root port is enumerated in PCI order, low/full/high speed devices are addressed, and the scan stops only at a usable boot keyboard (ITEM-406); a machine with neither boots with `local_input=UNAVAILABLE` and `source=NONE` (ITEM-392) |
| Local console | NOT TESTED | QEMU console path only; local input comes from PS/2 or from a USB HID keyboard on any xHCI controller of the machine (ITEM-395, ITEM-406), the serial console publishes only |
| SATA storage | NOT TESTED | QEMU SATA/AHCI acceptance only |
| NVMe storage | NOT TESTED | QEMU NVMe proof only |
| PCI devices | NOT TESTED | QEMU PCI/AHCI proof only |
| Ethernet NIC | NOT TESTED | QEMU VirtIO NIC only |
| DHCP | NOT TESTED | QEMU network proof only |
| Static IPv4 | NOT TESTED | QEMU network proof only |
| Gateway/DNS | NOT TESTED | QEMU network proof only |
| HTTPS management | NOT TESTED | Real TLS QEMU fixture only |
| Management UI | NOT TESTED | No production browser acceptance |
| Clean installation | NOT TESTED | Production installer software boundary only |
| Dual-boot installation | NOT TESTED | Linux/Windows planning proof only |
| ESP/UEFI NVRAM | NOT TESTED | OVMF QEMU evidence only |
| Bootloader | NOT TESTED | No physical bootloader acceptance |
| Recovery | NOT TESTED | Host/QEMU recovery composition proof only |
| Factory reset | NOT TESTED | Host proof only |

## Required Evidence

For every PASS row, record:

- hardware model;
- firmware/BIOS version;
- CPU;
- RAM;
- disk;
- NIC;
- test date;
- HFOC commit/release ID;
- observed result.

Do not substitute `NOT TESTED` with `PASS`.
