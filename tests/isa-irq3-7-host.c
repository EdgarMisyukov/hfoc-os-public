#include "isa_irq3_7.h"

#include "ioapic.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static enum ioapic_status route_status[16];
static bool routed[16];
static bool mask_called[16];
static bool unmask_called[16];
static uint8_t route_vector[16];
static unsigned int route_count;
static int failures;

enum ioapic_status host_ioapic_route_isa_irq(uint8_t irq, uint8_t vector) {
    if (irq < 16U) {
        route_vector[irq] = vector;
    }
    ++route_count;
    return route_status[irq];
}

bool host_ioapic_is_isa_irq_routed(uint8_t irq) {
    return irq < 16U && routed[irq];
}

bool host_pic_mask(uint8_t irq) {
    if (irq < 16U) {
        mask_called[irq] = true;
    }
    return true;
}

bool host_pic_unmask(uint8_t irq) {
    if (irq < 16U) {
        unmask_called[irq] = true;
    }
    return true;
}

uint64_t serial_sequence_begin(void) {
    return 0U;
}

void serial_sequence_end(uint64_t flags) {
    (void)flags;
}

void serial_write(const char *text) {
    (void)text;
}

void serial_write_u64(uint64_t value) {
    (void)value;
}

void serial_write_hex(uint64_t value) {
    (void)value;
}

bool lapic_is_enabled(void) {
    return true;
}

static void reset_host(void) {
    memset(route_status, 0, sizeof(route_status));
    memset(routed, 0, sizeof(routed));
    memset(mask_called, 0, sizeof(mask_called));
    memset(unmask_called, 0, sizeof(unmask_called));
    memset(route_vector, 0, sizeof(route_vector));
    route_count = 0U;
    for (uint8_t irq = ISA_IRQ3_7_FIRST; irq <= ISA_IRQ3_7_LAST; ++irq) {
        route_status[irq] = IOAPIC_OK;
        routed[irq] = true;
    }
}

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    } else {
        printf("PASS: %s\n", message);
    }
}

static void test_all_routes_success(void) {
    reset_host();
    isa_irq3_7_route_all();

    check(route_count == 5U, "all five IRQ3-7 routes are requested");
    for (uint8_t irq = ISA_IRQ3_7_FIRST; irq <= ISA_IRQ3_7_LAST; ++irq) {
        const char *irq_name = irq == 3U ? "IRQ3" :
                               irq == 4U ? "IRQ4" :
                               irq == 5U ? "IRQ5" :
                               irq == 6U ? "IRQ6" : "IRQ7";
        char message[80];
        (void)snprintf(message, sizeof(message),
                       "%s route uses exact vector", irq_name);
        check(route_vector[irq] == (uint8_t)(0x41U + irq), message);
        (void)snprintf(message, sizeof(message),
                       "%s PIC mask follows verified route", irq_name);
        check(mask_called[irq], message);
        (void)snprintf(message, sizeof(message),
                       "%s does not unmask PIC on success", irq_name);
        check(!unmask_called[irq], message);
    }
    check(!mask_called[2U] && !unmask_called[2U],
          "IRQ2 is never touched");
    for (uint8_t irq = 8U; irq < 16U; ++irq) {
        check(!mask_called[irq] && !unmask_called[irq],
              "IRQ8-15 are never touched");
    }
}

static void test_route_failure_falls_back(void) {
    reset_host();
    route_status[5U] = IOAPIC_ROUTE_FAILED;
    routed[5U] = false;

    isa_irq3_7_route_all();

    check(unmask_called[5U], "failed IRQ5 unmask preserves PIC fallback");
    check(mask_called[3U] && mask_called[4U] &&
              mask_called[6U] && mask_called[7U],
          "successful IRQ3/4/6/7 routes still mask PIC");
    check(!unmask_called[3U] && !unmask_called[4U] &&
              !unmask_called[6U] && !unmask_called[7U],
          "successful routes do not unmask PIC");
}

static void test_status_ok_but_not_routed_falls_back(void) {
    reset_host();
    routed[3U] = false;

    isa_irq3_7_route_all();

    check(unmask_called[3U],
          "IOAPIC_OK without published route keeps PIC fallback");
    check(mask_called[4U] && mask_called[5U] &&
              mask_called[6U] && mask_called[7U],
          "other routes remain unaffected");
}

int main(void) {
    test_all_routes_success();
    test_route_failure_falls_back();
    test_status_ok_but_not_routed_falls_back();

    if (failures != 0) {
        fprintf(stderr, "ISA IRQ3-7 route policy host tests: %d failure(s)\n",
                failures);
        return 1;
    }
    puts("ISA IRQ3-7 route policy host tests: ALL OK");
    return 0;
}
