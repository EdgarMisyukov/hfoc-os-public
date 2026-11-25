#ifndef HFOC_APPLIANCE_PRODUCTION_STORAGE_H
#define HFOC_APPLIANCE_PRODUCTION_STORAGE_H

#include "appliance_production_boot.h"
#include "appliance_storage_report.h"
#include "pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-394: the installer medium names the one disk it may write.  The boot
 * coordinator identifies backends by an opaque number, which is enough to
 * select a state volume but not enough to guard a whole-disk write, so the
 * storage owner publishes the bounded identity strings the controller reports:
 * the ATA serial (and model) for SATA, an address-derived identifier for NVMe
 * until its Identify Controller serial is parsed.
 */
#define APPLIANCE_PRODUCTION_STORAGE_IDENTITY_LENGTH 32U
#define APPLIANCE_PRODUCTION_STORAGE_MODEL_LENGTH 41U

struct appliance_production_storage_disk {
    bool present;
    enum appliance_production_boot_backend_kind kind;
    struct block_device *device;
    char identity[APPLIANCE_PRODUCTION_STORAGE_IDENTITY_LENGTH];
    char model[APPLIANCE_PRODUCTION_STORAGE_MODEL_LENGTH];
};

void appliance_production_storage_boot_init(uint64_t hhdm_offset);
enum appliance_production_boot_probe_status
appliance_production_storage_probe(
    void *context,
    struct appliance_production_boot_backend *backends,
    size_t capacity,
    size_t *count);
size_t appliance_production_storage_disk_count(void);
bool appliance_production_storage_disk_info(
    size_t index,
    struct appliance_production_storage_disk *info);

/*
 * ITEM-423: what the probe saw, not only what it kept.
 *
 * The 2026-09-15 hardware run's menu record ended `disks=1`: the machine
 * carries two disks, and a menu that has to list every system on the machine
 * offered only the one the install was writing.  A list of one cannot say
 * whether the second controller was never matched at the PCI level, was matched
 * and stopped inside the NVMe bring-up, or was brought up and produced no
 * namespace - three different defects with three different fixes - so the
 * record carries the counts and the step.
 *
 * `pci_ahci` and `pci_nvme` are the controllers the class scan matched;
 * `ahci` and `nvme` are the disks those controllers produced; `nvme_status` is
 * the bring-up step the last NVMe controller stopped at, named with
 * nvme_status_string(), and NVME_OK when none stopped.
 *
 * ITEM-429 adds the step and the wait to that pair.  A timeout is two
 * different findings - a controller that never reported ready and a driver
 * that never waited - and `nvme_step` names which bring-up call it was
 * (`RESET`, `ENABLE`, `IDENTIFY`, ..., `READY`) while `nvme_wait_ms` is what
 * the last controller-ready wait cost in milliseconds.
 */
struct appliance_production_storage_evidence {
    size_t pci_ahci;
    size_t pci_nvme;
    size_t ahci;
    size_t nvme;
    int nvme_status;
    const char *nvme_step;
    uint32_t nvme_wait_ms;
    /*
     * ITEM-540: what the probe itself did, so an unprobed machine and a machine
     * whose search failed are not the same silence.  `probe_attempted` says the
     * search was entered; `probe_stage` names the class scan that stopped
     * (`AHCI_SCAN`, `NVME_SCAN`, `READY`); `probe_status` is the primary PCI
     * status and `probe_failure` names the device it came from - BDF,
     * vendor/device and the BAR index when a decode failed.
     */
    bool probe_attempted;
    const char *probe_stage;
    enum pci_status probe_status;
    struct pci_scan_failure probe_failure;
};

void appliance_production_storage_evidence(
    struct appliance_production_storage_evidence *out);

/*
 * ITEM-518: the same evidence as the bounded operator report - the controllers,
 * the disks they brought up, and why a disk cannot be used - for the installer's
 * refusal page and the console `storage` command.  The probe's own counters say
 * what the class scan matched; each kept disk is described with the identity,
 * geometry and the one reason it is or is not usable.  A machine whose probe
 * never ran reports `probed=false` instead of a row of zeroes that would read
 * like a machine with no storage.
 *
 * This adds no decision: nothing is selected, rejected or written here.
 */
void appliance_production_storage_report(struct appliance_storage_report *out);

#endif
