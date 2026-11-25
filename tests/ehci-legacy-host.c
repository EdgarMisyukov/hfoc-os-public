/*
 * ITEM-568 host proof: the BIOS -> OS handoff runs in PCI configuration space.
 *
 * EHCI 1.0 Table 2-7 says the EECP field of HCCPARAMS is "the offset in PCI
 * configuration space of the first EHCI extended capability", must be 40h or
 * greater, and section 5 puts USBLEGSUP at EECP+00h and USBLEGCTLSTS at
 * EECP+04h.  The previous handoff read `BAR0 + EECP*4` and stepped
 * `offset + Next*4` instead, which on the owner's laptop found nothing and
 * left the firmware owning the controller.
 *
 * This proof drives the production sequence over a fake configuration space
 * whose words are all it may read.  A decoy capability is planted in the MMIO
 * window at `EECP*4` (what the old code would have read) and the proof fails
 * if the walk ever reads a byte offset outside the configuration space, or
 * uses a dword-scaled pointer.
 */
#include "ehci_legacy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

#define CONFIG_BYTES 256U
#define READ_LOG 64U
#define WRITE_LOG 16U

struct fake_config {
    uint32_t words[CONFIG_BYTES / 4U];
    uint16_t reads[READ_LOG];
    uint32_t read_count;
    uint16_t writes[WRITE_LOG];
    uint32_t write_values[WRITE_LOG];
    uint32_t write_count;
    uint32_t delayed_us;
    uint32_t delay_count;
    /* Behaviour of the firmware: clear BIOS owned when the OS owned semaphore
       is written.  False models a BIOS that never lets go. */
    bool release_on_os_write;
};

static uint32_t config_get(const struct fake_config *config, uint16_t offset) {
    return config->words[offset / 4U];
}

static void config_set(struct fake_config *config, uint16_t offset,
                       uint32_t value) {
    config->words[offset / 4U] = value;
}

static uint32_t fake_read32(void *context, uint16_t offset) {
    struct fake_config *config = context;

    CHECK(offset < CONFIG_BYTES && (offset % 4U) == 0U,
          "the handoff read outside the PCI configuration space, or at a "
          "misaligned offset");
    if (config->read_count < READ_LOG) {
        config->reads[config->read_count] = offset;
    }
    config->read_count += 1U;
    return config_get(config, offset);
}

static void fake_write32(void *context, uint16_t offset, uint32_t value) {
    struct fake_config *config = context;

    CHECK(offset < CONFIG_BYTES && (offset % 4U) == 0U,
          "the handoff wrote outside the PCI configuration space, or at a "
          "misaligned offset");
    if (config->write_count < WRITE_LOG) {
        config->writes[config->write_count] = offset;
        config->write_values[config->write_count] = value;
    }
    config->write_count += 1U;
    config_set(config, offset, value);
    /* The firmware releases ownership when it sees the OS semaphore; the
       status half of USBLEGCTLSTS is read-only from the OS side. */
    if (offset != 0U && (offset % 4U) == 0U &&
        (value & EHCI_LEGACY_OS_OWNED) != 0U && config->release_on_os_write) {
        uint32_t sup = config_get(config, offset);

        sup &= ~EHCI_LEGACY_BIOS_OWNED;
        config_set(config, offset, sup);
    }
}

static void fake_delay_us(void *context, uint32_t microseconds) {
    struct fake_config *config = context;

    config->delayed_us += microseconds;
    config->delay_count += 1U;
}

static struct ehci_legacy_ops fake_ops(struct fake_config *config) {
    struct ehci_legacy_ops ops;

    ops.context = config;
    ops.read32 = fake_read32;
    ops.write32 = fake_write32;
    ops.delay_us = fake_delay_us;
    return ops;
}

/* configuration space: HCCPARAMS.EECP is a dword-carried byte offset. */
static uint32_t hccparams_for(uint32_t eecp, bool supports_64bit) {
    return (eecp << 8U) | (supports_64bit ? 0x1U : 0U);
}

static void test_the_walk_uses_configuration_space_not_mmio(void) {
    struct fake_config config;
    struct ehci_legacy_state state;
    struct ehci_legacy_ops ops;
    uint32_t mmio_decoy[(0x140U + 4U) / 4U];

    memset(&config, 0, sizeof(config));
    memset(mmio_decoy, 0, sizeof(mmio_decoy));
    /* The real capability: config offset 0x50, BIOS owns, list ends here, and
       the legacy control register has SMIs enabled. */
    config_set(&config, 0x50U,
               EHCI_LEGACY_CAPABILITY_ID | EHCI_LEGACY_BIOS_OWNED);
    config_set(&config, 0x54U, 0x00030000U | EHCI_LEGACY_CTL_ENABLES);
    /* What the old code would have read: the same shape at MMIO 0x140
       (0x50 * 4).  It must never be consulted. */
    mmio_decoy[0x140U / 4U] = EHCI_LEGACY_CAPABILITY_ID | EHCI_LEGACY_BIOS_OWNED;
    config.release_on_os_write = true;
    ops = fake_ops(&config);

    CHECK(ehci_legacy_handoff_run(&ops, hccparams_for(0x50U, true), &state) ==
              EHCI_LEGACY_TAKEN,
          "the handoff takes a controller whose legacy capability is present");
    CHECK(state.eecp == 0x50U,
          "the EECP is kept as the configuration-space offset it is");
    CHECK(state.legacy_offset == 0x50U,
          "USBLEGSUP is read at the configuration-space offset EECP");
    CHECK(state.cap_count == 1U && state.cap_offset[0] == 0x50U &&
              state.caps[0] ==
                  (EHCI_LEGACY_CAPABILITY_ID | EHCI_LEGACY_BIOS_OWNED),
          "the capability chain is kept as the words actually read");
    CHECK(config.read_count >= 1U && config.reads[0] == 0x50U,
          "the first read is at EECP, not at EECP*4");
    CHECK(mmio_decoy[0x140U / 4U] ==
              (EHCI_LEGACY_CAPABILITY_ID | EHCI_LEGACY_BIOS_OWNED),
          "the decoy in the MMIO window is left untouched");
    CHECK(config.write_count >= 1U && config.writes[0] == 0x50U,
          "the ownership write goes to USBLEGSUP in configuration space");
    CHECK((state.sup_written & EHCI_LEGACY_OS_OWNED) != 0U,
          "the OS-owned semaphore is what is written");
    CHECK((state.sup_after & EHCI_LEGACY_BIOS_OWNED) == 0U,
          "the BIOS-owned semaphore cleared before ownership was declared");
    CHECK(config.write_count >= 2U && config.writes[1] == 0x54U,
          "USBLEGCTLSTS is written at EECP+4");
    CHECK((state.ctl_before & EHCI_LEGACY_CTL_ENABLES) != 0U &&
              (state.ctl_after & EHCI_LEGACY_CTL_ENABLES) == 0U,
          "the legacy SMI enables are cleared after the handoff");
    CHECK((state.ctl_after & 0xFFFF0000U) == 0x00030000U,
          "the status half of USBLEGCTLSTS is left as it was read");
}

/* The chain can carry other capabilities first: each Next is a byte offset. */
static void test_the_chain_is_walked_by_byte_offsets(void) {
    struct fake_config config;
    struct ehci_legacy_state state;
    struct ehci_legacy_ops ops;

    memset(&config, 0, sizeof(config));
    /* 0x40: id 0x02 (not ours), next 0x50; 0x50: id 0x01, next 0. */
    config_set(&config, 0x40U, 0x00000002U | (0x50U << 8U));
    config_set(&config, 0x50U, EHCI_LEGACY_CAPABILITY_ID);
    config.release_on_os_write = true;
    ops = fake_ops(&config);

    CHECK(ehci_legacy_handoff_run(&ops, hccparams_for(0x40U, false), &state) ==
              EHCI_LEGACY_NOT_OWNED,
          "a released controller needs no ownership write");
    CHECK(state.legacy_offset == 0x50U,
          "the legacy capability is found at the byte offset the chain gives");
    CHECK(state.cap_count == 2U && state.cap_offset[0] == 0x40U &&
              state.cap_offset[1] == 0x50U,
          "both capability words and their offsets are kept");
    CHECK(config.write_count >= 1U && config.writes[0] == 0x54U,
          "a released controller still has its legacy SMIs disabled");
    CHECK((state.ctl_after & EHCI_LEGACY_CTL_ENABLES) == 0U,
          "the SMI enables are cleared even when the BIOS had let go");
}

static void test_a_missing_capability_is_named(void) {
    struct fake_config config;
    struct ehci_legacy_state state;
    struct ehci_legacy_ops ops;

    memset(&config, 0, sizeof(config));
    config_set(&config, 0x40U, 0x00000002U | (0x60U << 8U));
    config_set(&config, 0x60U, 0x00000003U);
    ops = fake_ops(&config);

    CHECK(ehci_legacy_handoff_run(&ops, hccparams_for(0x40U, false), &state) ==
              EHCI_LEGACY_NOT_FOUND,
          "a chain without the legacy capability is a named absence, not a "
          "silent success");
    CHECK(state.cap_count == 2U && state.caps[0] == 0x00006002U &&
              state.caps[1] == 0x00000003U,
          "the words that were walked are kept for the next run");
    CHECK(config.write_count == 0U,
          "nothing is written into a chain that has no legacy capability");
}

static void test_eecp_outside_the_pointer_range_is_absent(void) {
    struct fake_config config;
    struct ehci_legacy_state state;
    struct ehci_legacy_ops ops;
    static const uint32_t bad[] = {0U, 0x20U, 0xFCU};

    memset(&config, 0, sizeof(config));
    ops = fake_ops(&config);
    for (size_t index = 0U; index < sizeof(bad) / sizeof(bad[0]); ++index) {
        memset(&state, 0, sizeof(state));
        CHECK(ehci_legacy_handoff_run(&ops, hccparams_for(bad[index], false),
                                      &state) == EHCI_LEGACY_ABSENT,
              "an EECP that is zero, below 40h or beyond the space is absent");
        CHECK(config.read_count == 0U,
              "no configuration space is read when there is no pointer");
    }
}

static void test_a_bios_that_never_releases_is_a_timeout(void) {
    struct fake_config config;
    struct ehci_legacy_state state;
    struct ehci_legacy_ops ops;

    memset(&config, 0, sizeof(config));
    config_set(&config, 0x40U,
               EHCI_LEGACY_CAPABILITY_ID | EHCI_LEGACY_BIOS_OWNED);
    config.release_on_os_write = false;
    ops = fake_ops(&config);

    CHECK(ehci_legacy_handoff_run(&ops, hccparams_for(0x40U, false), &state) ==
              EHCI_LEGACY_TIMEOUT,
          "a BIOS that never lets go is a named timeout");
    CHECK(state.waited_us == EHCI_LEGACY_BUDGET_US && config.delay_count > 0U,
          "the wait is the bounded budget, not an unbounded spin");
    CHECK(state.forced &&
              (state.sup_after & EHCI_LEGACY_BIOS_OWNED) == 0U,
          "the semaphore is forced clear after the budget, and that is kept");
}

static void test_a_self_referencing_chain_terminates(void) {
    struct fake_config config;
    struct ehci_legacy_state state;
    struct ehci_legacy_ops ops;

    memset(&config, 0, sizeof(config));
    config_set(&config, 0x40U, 0x00000005U | (0x40U << 8U));
    ops = fake_ops(&config);

    CHECK(ehci_legacy_handoff_run(&ops, hccparams_for(0x40U, false), &state) ==
              EHCI_LEGACY_NOT_FOUND,
          "a chain that points at itself ends the walk");
    CHECK(state.cap_count == 1U,
          "a non-forward Next does not make the walk loop");
}

int main(void) {
    test_the_walk_uses_configuration_space_not_mmio();
    test_the_chain_is_walked_by_byte_offsets();
    test_a_missing_capability_is_named();
    test_eecp_outside_the_pointer_range_is_absent();
    test_a_bios_that_never_releases_is_a_timeout();
    test_a_self_referencing_chain_terminates();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI BIOS handoff host test: ALL OK\n");
    return 0;
}
