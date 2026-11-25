#include "platform_power.h"

#include "acpi.h"
#include "platform_reset.h"

#include <string.h>

/*
 * ---------------------------------------------------------------- the plan --
 *
 * A reboot is the reset chain and nothing else.  A shutdown is one or two PM1
 * writes, and it exists only when the firmware named both the register and the
 * sleep type: an S5 write with a guessed sleep type is a write into a register
 * whose meaning nobody stated, and on a machine that is not the one the value
 * was guessed for it can mean something else entirely.
 */
enum platform_power_status platform_power_plan_build(
    enum platform_power_action action,
    const struct platform_power_environment *environment,
    struct platform_power_plan *out)
{
    if (environment == NULL || out == NULL) {
        return PLATFORM_POWER_NULL;
    }
    memset(out, 0, sizeof(*out));
    out->action = action;
    switch (action) {
    case PLATFORM_POWER_ACTION_REBOOT:
        /*
         * The reset chain always ends in a triple fault, so a machine that
         * ignores every mechanism it declares is still reset by its own CPU.
         */
        out->mechanism = PLATFORM_POWER_MECHANISM_RESET_CHAIN;
        return PLATFORM_POWER_OK;
    case PLATFORM_POWER_ACTION_SHUTDOWN:
        break;
    default:
        return PLATFORM_POWER_BAD_ACTION;
    }

    if (!environment->acpi_present || !environment->pm1a_declared ||
        !environment->dsdt_present || !environment->sleep_type_declared) {
        return PLATFORM_POWER_NO_MECHANISM;
    }
    out->mechanism = PLATFORM_POWER_MECHANISM_ACPI_S5;
    out->pm1a_control = environment->pm1a_control;
    out->pm1b_declared = environment->pm1b_declared;
    out->pm1b_control = environment->pm1b_control;
    out->sleep_type_a = environment->sleep_type_a;
    /*
     * ACPI declares one element per PM1 block; a platform with a single block
     * states one value, and the second block then speaks the same type.
     */
    out->sleep_type_b = environment->pm1b_declared
                            ? environment->sleep_type_b
                            : environment->sleep_type_a;
    out->sleep_type_offset = environment->sleep_type_offset;
    return PLATFORM_POWER_OK;
}

const char *platform_power_action_string(enum platform_power_action action) {
    switch (action) {
    case PLATFORM_POWER_ACTION_REBOOT: return "REBOOT";
    case PLATFORM_POWER_ACTION_SHUTDOWN: return "SHUTDOWN";
    case PLATFORM_POWER_ACTION_NONE: break;
    }
    return "NONE";
}

const char *platform_power_mechanism_string(
    enum platform_power_mechanism mechanism)
{
    switch (mechanism) {
    case PLATFORM_POWER_MECHANISM_RESET_CHAIN: return "RESET_CHAIN";
    case PLATFORM_POWER_MECHANISM_ACPI_S5: return "ACPI_S5";
    case PLATFORM_POWER_MECHANISM_NONE: break;
    }
    return "NONE";
}

const char *platform_power_status_string(enum platform_power_status status) {
    switch (status) {
    case PLATFORM_POWER_OK: return "PLATFORM_POWER_OK";
    case PLATFORM_POWER_NULL: return "PLATFORM_POWER_NULL";
    case PLATFORM_POWER_BAD_ACTION: return "PLATFORM_POWER_BAD_ACTION";
    case PLATFORM_POWER_NO_MECHANISM: return "PLATFORM_POWER_NO_MECHANISM";
    }
    return "PLATFORM_POWER_UNKNOWN";
}

/*
 * ------------------------------------------------------------------- \_S5 --
 *
 * The DSDT states the sleep type as `Name (_S5, Package (2) { <a>, <b> })`, and
 * this walk reads exactly that shape.  It is deliberately narrow: the object is
 * found by its four name bytes preceded by the Name opcode, the package is read
 * with its own length, and only the integer encodings AML uses for a small
 * constant are accepted.  Anything else leaves the type undeclared, so a table
 * this kernel does not understand is never half-read into a register write.
 */
#define PLATFORM_POWER_AML_NAME_OP 0x08U
#define PLATFORM_POWER_AML_PACKAGE_OP 0x12U
#define PLATFORM_POWER_AML_ZERO_OP 0x00U
#define PLATFORM_POWER_AML_ONE_OP 0x01U
#define PLATFORM_POWER_AML_ONES_OP 0xFFU
#define PLATFORM_POWER_AML_BYTE_OP 0x0AU
#define PLATFORM_POWER_AML_WORD_OP 0x0BU
#define PLATFORM_POWER_AML_DWORD_OP 0x0CU
/*
 * Bounded: the sleep type object is a package of integers, and ACPI 1.0-era
 * firmware emits four of them (`{ SLP_TYPa, SLP_TYPb, 0, 0 }`) where the
 * specification shows two.  QEMU's own DSDT is one of those tables, so the
 * walk reads up to four elements and uses the first two - the ones ACPI
 * defines as the PM1a and PM1b sleep types - and refuses a package longer than
 * that rather than guessing which pair is meant.
 */
#define PLATFORM_POWER_AML_MAX_ELEMENTS 4U
#define PLATFORM_POWER_AML_MAX_SCAN (1024U * 1024U)

static bool platform_power_aml_package_length(
    const uint8_t *bytes, uint32_t available, uint32_t *length,
    uint32_t *consumed)
{
    uint8_t lead;
    uint32_t value;
    uint32_t follow;
    uint32_t index;

    if (available == 0U) {
        return false;
    }
    lead = bytes[0U];
    follow = (uint32_t)(lead >> 6U);
    if (follow == 0U) {
        *length = (uint32_t)(lead & 0x3FU);
        *consumed = 1U;
        return true;
    }
    if (1U + follow > available) {
        return false;
    }
    value = (uint32_t)(lead & 0x0FU);
    for (index = 0U; index < follow; ++index) {
        value |= (uint32_t)bytes[1U + index] << (4U + 8U * index);
    }
    *length = value;
    *consumed = 1U + follow;
    return true;
}

static bool platform_power_aml_integer(
    const uint8_t *bytes, uint32_t available, uint16_t *value,
    uint32_t *consumed)
{
    if (available == 0U) {
        return false;
    }
    switch (bytes[0U]) {
    case PLATFORM_POWER_AML_ZERO_OP:
        *value = 0U;
        *consumed = 1U;
        return true;
    case PLATFORM_POWER_AML_ONE_OP:
        *value = 1U;
        *consumed = 1U;
        return true;
    case PLATFORM_POWER_AML_ONES_OP:
        *value = 0xFFU;
        *consumed = 1U;
        return true;
    case PLATFORM_POWER_AML_BYTE_OP:
        if (available < 2U) {
            return false;
        }
        *value = bytes[1U];
        *consumed = 2U;
        return true;
    case PLATFORM_POWER_AML_WORD_OP:
        if (available < 3U) {
            return false;
        }
        *value = (uint16_t)((uint16_t)bytes[1U] |
                            ((uint16_t)bytes[2U] << 8U));
        *consumed = 3U;
        return true;
    case PLATFORM_POWER_AML_DWORD_OP:
        if (available < 5U) {
            return false;
        }
        *value = (uint16_t)((uint16_t)bytes[1U] |
                            ((uint16_t)bytes[2U] << 8U));
        *consumed = 5U;
        return true;
    default:
        return false;
    }
}

bool platform_power_sleep_type_find(
    const void *dsdt, uint32_t dsdt_length,
    uint16_t *sleep_type_a, uint16_t *sleep_type_b,
    uint32_t *offset)
{
    static const uint8_t name[4U] = {0x5FU, 0x53U, 0x35U, 0x5FU};
    const uint8_t *bytes = (const uint8_t *)dsdt;
    uint32_t scan_length;
    uint32_t index;

    if (dsdt == NULL || dsdt_length < 8U ||
        sleep_type_a == NULL || sleep_type_b == NULL) {
        return false;
    }
    scan_length = dsdt_length < PLATFORM_POWER_AML_MAX_SCAN
                      ? dsdt_length
                      : PLATFORM_POWER_AML_MAX_SCAN;
    for (index = 1U; index + 8U < scan_length; ++index) {
        uint32_t package_length = 0U;
        uint32_t consumed = 0U;
        uint32_t position;
        uint32_t element;
        uint32_t elements;
        uint16_t values[PLATFORM_POWER_AML_MAX_ELEMENTS];
        bool complete = true;

        if (bytes[index - 1U] != PLATFORM_POWER_AML_NAME_OP ||
            bytes[index] != name[0U] || bytes[index + 1U] != name[1U] ||
            bytes[index + 2U] != name[2U] ||
            bytes[index + 3U] != name[3U]) {
            continue;
        }
        position = index + 4U;
        if (position >= scan_length ||
            bytes[position] != PLATFORM_POWER_AML_PACKAGE_OP) {
            continue;
        }
        ++position;
        if (!platform_power_aml_package_length(
                bytes + position, scan_length - position,
                &package_length, &consumed)) {
            continue;
        }
        position += consumed;
        (void)package_length;
        if (position >= scan_length) {
            continue;
        }
        elements = bytes[position++];
        if (elements == 0U ||
            elements > PLATFORM_POWER_AML_MAX_ELEMENTS) {
            continue;
        }
        for (element = 0U; element < elements; ++element) {
            if (!platform_power_aml_integer(
                    bytes + position, scan_length - position,
                    &values[element], &consumed)) {
                complete = false;
                break;
            }
            position += consumed;
        }
        if (!complete) {
            continue;
        }
        *sleep_type_a = values[0U];
        *sleep_type_b = elements > 1U ? values[1U] : values[0U];
        if (offset != NULL) {
            *offset = index - 1U;
        }
        return true;
    }
    return false;
}

#ifdef HFOC_PLATFORM_POWER_HOST_TEST
/* The host proof drives the decision and the table walk; the mechanisms below
 * need the console, the serial port and the machine itself. */
#else

#include "console.h"
#include "kstring.h"
#include "record_line.h"
#include "serial.h"

static inline void platform_power_outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void platform_power_publish(const char *text) {
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write(text);
    serial_sequence_end(serial_flags);
    /* ITEM-478: the record reaches the durable journal, not the screen; the
     * serial stream stays byte for byte what this module wrote. */
    console_publish_journal(text, strlen(text));
}

static void platform_power_publish_plan(
    const struct platform_power_plan *plan)
{
    static struct record_line line;

    record_line_begin(&line);
    record_line_append(&line, "[PLATFORM-POWER] plan action=");
    record_line_append(
        &line, platform_power_action_string(plan->action));
    record_line_append(&line, " mechanism=");
    record_line_append(
        &line, platform_power_mechanism_string(plan->mechanism));
    if (plan->mechanism == PLATFORM_POWER_MECHANISM_ACPI_S5) {
        record_line_append(&line, " pm1a=");
        record_line_hex(&line, plan->pm1a_control);
        record_line_append(&line, " pm1b=");
        if (plan->pm1b_declared) {
            record_line_hex(&line, plan->pm1b_control);
        } else {
            record_line_append(&line, "NONE");
        }
        record_line_append(&line, " slp_typ=");
        record_line_hex(&line, plan->sleep_type_a);
        record_line_append(&line, " slp_typ_b=");
        record_line_hex(&line, plan->sleep_type_b);
        record_line_append(&line, " dsdt=OK sleep_offset=");
        record_line_hex(&line, plan->sleep_type_offset);
        record_line_append(&line, " steps=");
        record_line_append(&line, plan->pm1b_declared ? "PM1A>PM1B" : "PM1A");
    } else {
        record_line_append(&line, " steps=ACPI>PCI>I8042>FAULT");
    }
    platform_power_publish(line.text);
}

static void platform_power_publish_attempt(
    const struct platform_power_plan *plan,
    size_t index, size_t count, uint16_t address, uint16_t value)
{
    static struct record_line line;

    record_line_begin(&line);
    record_line_append(&line, "[PLATFORM-POWER] attempt=");
    record_line_u64(&line, index);
    record_line_append(&line, "/");
    record_line_u64(&line, count);
    record_line_append(&line, " action=");
    record_line_append(
        &line, platform_power_action_string(plan->action));
    record_line_append(&line, " space=IO address=");
    record_line_hex(&line, address);
    record_line_append(&line, " value=");
    record_line_hex(&line, value);
    platform_power_publish(line.text);
}

static void platform_power_publish_result(
    const struct platform_power_plan *plan, const char *result,
    size_t attempts)
{
    static struct record_line line;

    record_line_begin(&line);
    record_line_append(&line, "[PLATFORM-POWER] result=");
    record_line_append(&line, result);
    record_line_append(&line, " action=");
    record_line_append(
        &line, platform_power_action_string(plan->action));
    record_line_append(&line, " attempts=");
    record_line_u64(&line, attempts);
    platform_power_publish(line.text);
}

/*
 * A refusal names the ingredient that was missing, not just the verdict: the
 * firmware's declarations are what a shutdown is built from, and an operator
 * reading `reason=NO_MECHANISM` alone cannot tell a machine without a DSDT from
 * one whose sleep type this kernel would not read.
 */
static void platform_power_publish_refused(
    enum platform_power_action action,
    const struct platform_power_environment *environment,
    const char *reason)
{
    static struct record_line line;

    record_line_begin(&line);
    record_line_append(&line, "[PLATFORM-POWER] refused action=");
    record_line_append(&line, platform_power_action_string(action));
    record_line_append(&line, " reason=");
    record_line_append(&line, reason);
    record_line_append(&line, " acpi=");
    record_line_append(
        &line, environment != NULL && environment->acpi_present ? "1" : "0");
    record_line_append(&line, " pm1a=");
    if (environment != NULL && environment->pm1a_declared) {
        record_line_hex(&line, environment->pm1a_control);
    } else {
        record_line_append(&line, "NONE");
    }
    record_line_append(&line, " pm1b=");
    if (environment != NULL && environment->pm1b_declared) {
        record_line_hex(&line, environment->pm1b_control);
    } else {
        record_line_append(&line, "NONE");
    }
    record_line_append(&line, " dsdt=");
    record_line_append(
        &line, environment != NULL && environment->dsdt_present ? "1" : "0");
    record_line_append(&line, " sleep=");
    record_line_append(
        &line,
        environment != NULL && environment->sleep_type_declared ? "1" : "0");
    /*
     * ITEM-418: what the DSDT actually contains.  A machine whose sleep type
     * this kernel cannot read is otherwise indistinguishable from one that
     * declares none, and the difference decides whether the fix is a parser or
     * a platform: the count of `_S5_` literals and the bytes that follow the
     * first one are the evidence for both.
     */
    if (environment != NULL && environment->dsdt != NULL &&
        environment->dsdt_length != 0U) {
        const uint8_t *bytes = (const uint8_t *)environment->dsdt;
        uint32_t length = environment->dsdt_length;
        uint32_t literals = 0U;
        uint32_t first = 0U;
        uint32_t index;

        for (index = 0U; index + 4U <= length; ++index) {
            if (bytes[index] == 0x5FU && bytes[index + 1U] == 0x53U &&
                bytes[index + 2U] == 0x35U && bytes[index + 3U] == 0x5FU) {
                if (literals == 0U) {
                    first = index;
                }
                ++literals;
            }
        }
        record_line_append(&line, " dsdt_bytes=");
        record_line_u64(&line, length);
        record_line_append(&line, " s5_literals=");
        record_line_u64(&line, literals);
        if (literals != 0U) {
            record_line_append(&line, " s5_at=");
            record_line_hex(&line, first);
            record_line_append(&line, " s5_next=");
            for (index = 0U; index < 8U; ++index) {
                uint32_t at = first + 4U + index;

                if (at >= length) {
                    break;
                }
                record_line_append(&line, index == 0U ? "" : ",");
                record_line_hex(&line, bytes[at]);
            }
        }
    }
    platform_power_publish(line.text);
}

void platform_power_environment_current(
    struct platform_power_environment *out)
{
    const struct acpi_fadt_power *power = acpi_fadt_power();
    uint16_t sleep_type_a = 0U;
    uint16_t sleep_type_b = 0U;
    uint32_t offset = 0U;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (power == NULL) {
        return;
    }
    out->acpi_present = true;
    out->pm1a_declared = power->pm1a_declared;
    out->pm1b_declared = power->pm1b_declared;
    out->pm1a_control = power->pm1a_control;
    out->pm1b_control = power->pm1b_control;
    if (power->dsdt != NULL && power->dsdt_length != 0U) {
        out->dsdt_present = true;
        out->dsdt = power->dsdt;
        out->dsdt_length = power->dsdt_length;
        out->sleep_type_declared = platform_power_sleep_type_find(
            power->dsdt, power->dsdt_length, &sleep_type_a,
            &sleep_type_b, &offset);
        if (out->sleep_type_declared) {
            out->sleep_type_a = sleep_type_a;
            out->sleep_type_b = sleep_type_b;
            out->sleep_type_offset = offset;
        }
    }
}

enum platform_power_status platform_power_plan_current(
    enum platform_power_action action,
    struct platform_power_plan *out)
{
    struct platform_power_environment environment;
    enum platform_power_status status;

    platform_power_environment_current(&environment);
    status = platform_power_plan_build(action, &environment, out);
    if (status != PLATFORM_POWER_OK) {
        /*
         * A refusal is published where it is decided, so the log carries it
         * even when the caller only prints a line to the screen.
         */
        platform_power_publish_refused(
            action, &environment,
            platform_power_status_string(status));
    }
    return status;
}

void platform_power_execute(const struct platform_power_plan *plan)
{
    size_t attempts = 0U;
    struct platform_reset_plan reset;

    if (plan == NULL || plan->action == PLATFORM_POWER_ACTION_NONE) {
        return;
    }
    if (plan->mechanism == PLATFORM_POWER_MECHANISM_RESET_CHAIN) {
        platform_power_publish_plan(plan);
        /*
         * ITEM-411's chain, and the reason it is the whole reboot mechanism: it
         * ends in the reset the CPU performs by itself, so a reboot that a
         * platform ignores everywhere else still happens.
         */
        if (platform_reset_plan_current(&reset) != PLATFORM_RESET_OK) {
            struct platform_power_environment refused_environment;

            platform_power_environment_current(&refused_environment);
            platform_power_publish_refused(
                plan->action, &refused_environment,
                "RESET_PLAN_UNAVAILABLE");
            return;
        }
        platform_reset_execute(&reset);
        /* `platform_reset_execute` does not return. */
        return;
    }
    if (plan->mechanism != PLATFORM_POWER_MECHANISM_ACPI_S5) {
        struct platform_power_environment refused_environment;

        platform_power_environment_current(&refused_environment);
        platform_power_publish_refused(
            plan->action, &refused_environment, "NO_MECHANISM");
        return;
    }

    platform_power_publish_plan(plan);
    platform_reset_delay_ms(PLATFORM_POWER_OPERATOR_DELAY_MS);

    attempts = plan->pm1b_declared ? 2U : 1U;
    platform_power_publish_attempt(
        plan, 1U, attempts, plan->pm1a_control,
        (uint16_t)((uint16_t)(plan->sleep_type_a <<
                              PLATFORM_POWER_SLP_TYP_SHIFT) |
                   PLATFORM_POWER_SLP_EN));
    platform_power_outw(
        plan->pm1a_control,
        (uint16_t)((uint16_t)(plan->sleep_type_a <<
                              PLATFORM_POWER_SLP_TYP_SHIFT) |
                   PLATFORM_POWER_SLP_EN));
    platform_reset_delay_ms(PLATFORM_POWER_PM1_SETTLE_MS);

    if (plan->pm1b_declared) {
        platform_power_publish_attempt(
            plan, 2U, attempts, plan->pm1b_control,
            (uint16_t)((uint16_t)(plan->sleep_type_b <<
                                  PLATFORM_POWER_SLP_TYP_SHIFT) |
                       PLATFORM_POWER_SLP_EN));
        platform_power_outw(
            plan->pm1b_control,
            (uint16_t)((uint16_t)(plan->sleep_type_b <<
                                  PLATFORM_POWER_SLP_TYP_SHIFT) |
                       PLATFORM_POWER_SLP_EN));
        platform_reset_delay_ms(PLATFORM_POWER_PM1_SETTLE_MS);
    }
    /*
     * Reaching this line means the machine is still running: an S5 write the
     * platform accepted powers it off inside the attempt, so the record states
     * what happened instead of claiming a shutdown that did not occur.
     */
    platform_power_publish_result(plan, "STILL_RUNNING", attempts);
}

#endif
