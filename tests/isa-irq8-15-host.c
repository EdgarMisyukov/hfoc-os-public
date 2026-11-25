#include "isa_irq8_15.h"

#include "ioapic.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static enum ioapic_status route_status[32];
static bool routed[32];
static bool mask_called[32];
static bool unmask_called[32];
static uint8_t route_vector[32];
static unsigned int route_count;
static int failures;

enum ioapic_status host_ioapic_route_isa_irq(uint8_t irq, uint8_t vector) {
    if (irq < 32U) {
        route_vector[irq] = vector;
    }
    ++route_count;
    return route_status[irq];
}

bool host_ioapic_is_isa_irq_routed(uint8_t irq) {
    return irq < 32U && routed[irq];
}

bool host_pic_mask(uint8_t irq) {
    if (irq < 32U) {
        mask_called[irq] = true;
    }
    return true;
}

bool host_pic_unmask(uint8_t irq) {
    if (irq < 32U) {
        unmask_called[irq] = true;
    }
    return true;
}

bool lapic_is_enabled(void) {
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

static void reset_host(void) {
    memset(route_status, 0, sizeof(route_status));
    memset(routed, 0, sizeof(routed));
    memset(mask_called, 0, sizeof(mask_called));
    memset(unmask_called, 0, sizeof(unmask_called));
    memset(route_vector, 0, sizeof(route_vector));
    route_count = 0U;
    for (uint8_t irq = ISA_IRQ8_15_FIRST; irq <= ISA_IRQ8_15_LAST; ++irq) {
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

static const char *irq_name(uint8_t irq) {
    switch (irq) {
    case 8U: return "IRQ8";
    case 9U: return "IRQ9";
    case 10U: return "IRQ10";
    case 11U: return "IRQ11";
    case 12U: return "IRQ12";
    case 13U: return "IRQ13";
    case 14U: return "IRQ14";
    case 15U: return "IRQ15";
    default: return "unknown";
    }
}

static void test_all_routes_success(void) {
    reset_host();
    isa_irq8_15_route_all();

    check(route_count == 8U, "all eight IRQ8-15 routes are requested");
    for (uint8_t irq = ISA_IRQ8_15_FIRST; irq <= ISA_IRQ8_15_LAST; ++irq) {
        char message[80];
        (void)snprintf(message, sizeof(message),
                       "%s route uses exact vector", irq_name(irq));
        check(route_vector[irq] == (uint8_t)(0x41U + irq), message);
        (void)snprintf(message, sizeof(message),
                       "%s PIC mask follows verified route", irq_name(irq));
        check(mask_called[irq], message);
        (void)snprintf(message, sizeof(message),
                       "%s does not unmask PIC on success", irq_name(irq));
        check(!unmask_called[irq], message);
    }
    for (uint8_t irq = 0U; irq < 8U; ++irq) {
        check(!mask_called[irq] && !unmask_called[irq] &&
                  route_vector[irq] == 0U,
              "IRQ0-7 are never touched");
    }
    check(!mask_called[2U] && !unmask_called[2U] &&
              route_vector[2U] == 0U,
          "IRQ2 is never touched");
    for (uint8_t irq = 16U; irq < 32U; ++irq) {
        check(!mask_called[irq] && !unmask_called[irq] &&
                  route_vector[irq] == 0U,
              "IRQ16+ are never touched");
    }
}

static void test_route_failure_falls_back(void) {
    reset_host();
    route_status[10U] = IOAPIC_ROUTE_FAILED;
    routed[10U] = false;

    isa_irq8_15_route_all();

    check(unmask_called[10U], "failed IRQ10 unmask preserves PIC fallback");
    for (uint8_t irq = ISA_IRQ8_15_FIRST; irq <= ISA_IRQ8_15_LAST; ++irq) {
        if (irq == 10U) {
            continue;
        }
        char message[80];
        (void)snprintf(message, sizeof(message),
                       "successful %s routes still mask PIC", irq_name(irq));
        check(mask_called[irq], message);
        (void)snprintf(message, sizeof(message),
                       "successful %s does not unmask PIC", irq_name(irq));
        check(!unmask_called[irq], message);
    }
}

static void test_status_ok_but_not_routed_falls_back(void) {
    reset_host();
    routed[12U] = false;

    isa_irq8_15_route_all();

    check(unmask_called[12U],
          "IOAPIC_OK without published route keeps PIC fallback");
    for (uint8_t irq = ISA_IRQ8_15_FIRST; irq <= ISA_IRQ8_15_LAST; ++irq) {
        if (irq == 12U) {
            continue;
        }
        char message[80];
        (void)snprintf(message, sizeof(message),
                       "other route %s remains masked", irq_name(irq));
        check(mask_called[irq], message);
    }
}

int main(void) {
    test_all_routes_success();
    test_route_failure_falls_back();
    test_status_ok_but_not_routed_falls_back();

    if (failures != 0) {
        fprintf(stderr, "ISA IRQ8-15 route policy host tests: %d failure(s)\n",
                failures);
        return 1;
    }
    puts("ISA IRQ8-15 route policy host tests: ALL OK");
    return 0;
}
