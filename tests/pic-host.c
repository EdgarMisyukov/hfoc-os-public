#include "arch/x86_64/pic.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define PIC1_DATA_PORT 0x21U
#define PIC2_DATA_PORT 0xa1U

bool pic_disable_host_test_rollback_verified(void);

static uint8_t host_master_mask;
static uint8_t host_slave_mask;
static unsigned int host_pic1_data_writes;
static unsigned int host_pic1_data_reads;
static unsigned int host_pic2_data_writes;
static unsigned int host_pic2_data_reads;
static bool ignore_master_writes;
static bool ignore_slave_writes;
static bool ignore_master_restore_write;
static bool ignore_slave_restore_write;
static bool fail_master_disable_readback;
static bool fail_slave_disable_readback;
static bool fail_master_restore_readback;
static bool fail_slave_restore_readback;

uint8_t host_pic_inb(uint16_t port) {
    if (port == PIC1_DATA_PORT) {
        ++host_pic1_data_reads;
        if (fail_master_disable_readback && host_pic1_data_reads == 2U) {
            return (uint8_t)(host_master_mask ^ 0xffU);
        }
        if (fail_master_restore_readback && host_pic1_data_reads == 3U) {
            return (uint8_t)(host_master_mask ^ 0xffU);
        }
        return host_master_mask;
    }
    if (port == PIC2_DATA_PORT) {
        ++host_pic2_data_reads;
        if (fail_slave_disable_readback && host_pic2_data_reads == 2U) {
            return (uint8_t)(host_slave_mask ^ 0xffU);
        }
        if (fail_slave_restore_readback && host_pic2_data_reads == 3U) {
            return (uint8_t)(host_slave_mask ^ 0xffU);
        }
        return host_slave_mask;
    }
    return 0U;
}

void host_pic_outb(uint16_t port, uint8_t value) {
    if (port == PIC1_DATA_PORT) {
        ++host_pic1_data_writes;
        if (!ignore_master_writes &&
            !(ignore_master_restore_write && host_pic1_data_writes == 2U)) {
            host_master_mask = value;
        }
    } else if (port == PIC2_DATA_PORT) {
        ++host_pic2_data_writes;
        if (!ignore_slave_writes &&
            !(ignore_slave_restore_write && host_pic2_data_writes == 2U)) {
            host_slave_mask = value;
        }
    }
}

static void reset_state(uint8_t master, uint8_t slave) {
    host_master_mask = master;
    host_slave_mask = slave;
    host_pic1_data_writes = 0U;
    host_pic1_data_reads = 0U;
    host_pic2_data_writes = 0U;
    host_pic2_data_reads = 0U;
    ignore_master_writes = false;
    ignore_slave_writes = false;
    ignore_master_restore_write = false;
    ignore_slave_restore_write = false;
    fail_master_disable_readback = false;
    fail_slave_disable_readback = false;
    fail_master_restore_readback = false;
    fail_slave_restore_readback = false;
}

static int failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    } else {
        printf("PASS: %s\n", message);
    }
}

static void test_pic_irq0_mask_save_restore(void) {
    reset_state(0xFEU, 0xFFU);

    uint8_t previous = 0U;
    check(pic_irq0_mask_save(&previous),
          "PIC IRQ0 mask save succeeds from FE fallback mask");
    check(previous == 0xFEU,
          "PIC IRQ0 mask save returns exact previous master mask");
    check(host_master_mask == 0xFFU,
          "PIC IRQ0 mask save writes full IRQ0-masked master mask");
    check(host_pic1_data_writes == 1U,
          "PIC IRQ0 mask save writes one masked master byte");
    check(host_pic1_data_reads >= 2U,
          "PIC IRQ0 mask save reads previous and masked readback");

    check(pic_master_mask_restore(previous),
          "PIC master mask restore succeeds with exact FE byte");
    check(host_master_mask == 0xFEU,
          "PIC master mask restore writes exact previous FF->FE byte");
    check(host_pic1_data_writes == 2U,
          "PIC master mask restore writes one exact restore byte");
}

static void test_pic_irq0_mask_save_readback_failure_rollback(void) {
    reset_state(0xFEU, 0xFFU);
    ignore_master_writes = true;

    uint8_t previous = 0U;
    check(!pic_irq0_mask_save(&previous),
          "PIC IRQ0 mask save fails when masked readback is not confirmed");
    check(previous == 0xFEU,
          "PIC IRQ0 mask save returns previous mask before rollback");
    check(host_master_mask == 0xFEU,
          "PIC IRQ0 mask save rollback leaves exact previous master mask");
    check(host_pic1_data_writes == 2U,
          "PIC IRQ0 mask save rollback attempts restore write");
    check(host_pic1_data_reads == 3U,
          "PIC IRQ0 mask save rollback verifies restore readback");
}

static void test_pic_disable_transaction_success(void) {
    reset_state(0xFCU, 0xFEU);

    check(pic_disable_transactional(),
          "PIC disable transaction succeeds");
    check(host_master_mask == 0xFFU && host_slave_mask == 0xFFU,
          "PIC disable transaction masks both controllers exactly");
}

static void test_pic_disable_transaction_slave_failure_rolls_back(void) {
    reset_state(0xFCU, 0xFEU);
    ignore_slave_writes = true;

    check(!pic_disable_transactional(),
          "PIC disable fails on slave readback mismatch");
    check(host_master_mask == 0xFCU && host_slave_mask == 0xFEU,
          "slave failure restores exact original mask pair");
}

static void test_pic_disable_transaction_master_failure_rolls_back(void) {
    reset_state(0xFCU, 0xFEU);
    ignore_master_writes = true;

    check(!pic_disable_transactional(),
          "PIC disable fails on master readback mismatch");
    check(host_master_mask == 0xFCU && host_slave_mask == 0xFEU,
          "master failure restores exact original mask pair");
}

static void test_pic_disable_transaction_restore_failure_no_publish(void) {
    reset_state(0xFCU, 0xFEU);
    ignore_master_writes = true;

    check(!pic_disable_transactional(),
          "restoration write failure never reports PIC disable success");
    check(host_pic1_data_writes == 2U &&
              host_pic2_data_writes == 2U,
          "failed transaction attempts both restore writes");
}

static void test_pic_disable_transaction_restore_master_write_failure(void) {
    reset_state(0xFCU, 0xFEU);
    fail_master_disable_readback = true;
    ignore_master_restore_write = true;

    check(!pic_disable_transactional(),
          "restore master write failure keeps PIC disable false");
    check(host_master_mask == 0xFFU && host_slave_mask == 0xFEU,
          "restore master write failure leaves disable mask observable");
    check(!pic_disable_host_test_rollback_verified(),
          "restore master write failure fails rollback verification");
}

static void test_pic_disable_transaction_restore_slave_write_failure(void) {
    reset_state(0xFCU, 0xFEU);
    fail_slave_disable_readback = true;
    ignore_slave_restore_write = true;

    check(!pic_disable_transactional(),
          "restore slave write failure keeps PIC disable false");
    check(host_master_mask == 0xFCU && host_slave_mask == 0xFFU,
          "restore slave write failure leaves disable mask observable");
    check(!pic_disable_host_test_rollback_verified(),
          "restore slave write failure fails rollback verification");
}

static void test_pic_disable_transaction_restore_master_readback_mismatch(void) {
    reset_state(0xFCU, 0xFEU);
    fail_master_disable_readback = true;
    fail_master_restore_readback = true;

    check(!pic_disable_transactional(),
          "restore master readback mismatch keeps PIC disable false");
    check(host_master_mask == 0xFCU && host_slave_mask == 0xFEU,
          "restore master readback mismatch still restores exact mask pair");
    check(!pic_disable_host_test_rollback_verified(),
          "restore master readback mismatch fails rollback verification");
}

static void test_pic_disable_transaction_restore_slave_readback_mismatch(void) {
    reset_state(0xFCU, 0xFEU);
    fail_slave_disable_readback = true;
    fail_slave_restore_readback = true;

    check(!pic_disable_transactional(),
          "restore slave readback mismatch keeps PIC disable false");
    check(host_master_mask == 0xFCU && host_slave_mask == 0xFEU,
          "restore slave readback mismatch still restores exact mask pair");
    check(!pic_disable_host_test_rollback_verified(),
          "restore slave readback mismatch fails rollback verification");
}

static void test_pic_irq2_is_permanently_reserved(void) {
    reset_state(0xFFU, 0xFFU);

    check(!pic_unmask(2U),
          "PIC IRQ2 direct unmask is rejected");
    check(host_master_mask == 0xFFU && host_slave_mask == 0xFFU,
          "PIC IRQ2 rejection preserves both masks");
    check(host_pic1_data_reads == 0U && host_pic1_data_writes == 0U &&
              host_pic2_data_reads == 0U && host_pic2_data_writes == 0U,
          "PIC IRQ2 rejection performs no port I/O");

    reset_state(0xFFU, 0xFFU);
    check(pic_unmask(1U), "PIC IRQ1 unmask remains available");
    check(host_master_mask == 0xFDU,
          "PIC IRQ1 unmask clears only master bit 1");

    reset_state(0xFFU, 0xFFU);
    check(pic_unmask(8U), "explicit legacy slave IRQ8 unmask remains available");
    check(host_master_mask == 0xFBU && host_slave_mask == 0xFEU,
          "legacy slave IRQ8 retains existing cascade behavior");
}

int main(void) {
    test_pic_irq0_mask_save_restore();
    test_pic_irq0_mask_save_readback_failure_rollback();
    test_pic_disable_transaction_success();
    test_pic_disable_transaction_slave_failure_rolls_back();
    test_pic_disable_transaction_master_failure_rolls_back();
    test_pic_disable_transaction_restore_failure_no_publish();
    test_pic_disable_transaction_restore_master_write_failure();
    test_pic_disable_transaction_restore_slave_write_failure();
    test_pic_disable_transaction_restore_master_readback_mismatch();
    test_pic_disable_transaction_restore_slave_readback_mismatch();
    test_pic_irq2_is_permanently_reserved();

    if (failures != 0) {
        printf("PIC host tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("PIC host tests: ALL OK");
    return 0;
}
