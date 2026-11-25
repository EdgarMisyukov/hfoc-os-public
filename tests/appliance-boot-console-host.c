#include "appliance_boot_console.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct appliance_identity make_identity(void)
{
    struct appliance_identity identity;

    memset(&identity, 0, sizeof(identity));
    identity.version = APPLIANCE_IDENTITY_VERSION;
    memcpy(identity.machine_class, APPLIANCE_IDENTITY_MACHINE_CLASS,
           sizeof(APPLIANCE_IDENTITY_MACHINE_CLASS));
    memcpy(identity.product, APPLIANCE_IDENTITY_PRODUCT,
           sizeof(APPLIANCE_IDENTITY_PRODUCT));
    memcpy(identity.architecture, APPLIANCE_IDENTITY_ARCHITECTURE,
           sizeof(APPLIANCE_IDENTITY_ARCHITECTURE));
    memcpy(identity.platform, APPLIANCE_IDENTITY_PLATFORM,
           sizeof(APPLIANCE_IDENTITY_PLATFORM));
    memcpy(identity.hostname, APPLIANCE_IDENTITY_DEFAULT_HOSTNAME,
           sizeof(APPLIANCE_IDENTITY_DEFAULT_HOSTNAME));
    memcpy(identity.instance, APPLIANCE_IDENTITY_DEFAULT_INSTANCE,
           sizeof(APPLIANCE_IDENTITY_DEFAULT_INSTANCE));
    identity.published = true;
    return identity;
}

static struct appliance_boot_console_snapshot make_snapshot(void)
{
    struct appliance_boot_console_snapshot snapshot;
    struct appliance_identity identity = make_identity();

    memset(&snapshot, 0, sizeof(snapshot));
    if (appliance_boot_console_snapshot_init(
            &snapshot, &identity,
            APPLIANCE_CONSOLE_STATE_OK, 42U, 2U,
            UINT64_C(0x20000000)) != APPLIANCE_BOOT_CONSOLE_OK) {
        fprintf(stderr, "fixture failed to init boot snapshot\n");
        __builtin_unreachable();
    }
    return snapshot;
}

static const char *unavailable_expected(void)
{
    return "HFOC Appliance\n"
           "--------------\n"
           "Hostname:      hfoc-appliance\n"
           "CPU:           2\n"
           "Memory:        512 MiB\n"
           "Storage:       UNAVAILABLE\n"
           "Health:        OK\n"
           "Network:       UNAVAILABLE\n"
           "Services:      UNAVAILABLE\n"
           "Management:    UNAVAILABLE\n"
           "\n"
           "Local administration:\n"
           "[1] Status\n"
           "[2] Network\n"
           "[3] Diagnostics\n"
           "[4] Reboot\n"
           "[5] Shutdown\n"
           "[6] Recovery\n";
}

static void test_status_and_action_strings(void)
{
    enum appliance_boot_console_status status;
    enum appliance_boot_console_action action;

    for (status = APPLIANCE_BOOT_CONSOLE_OK;
         status <= APPLIANCE_BOOT_CONSOLE_OVERFLOW;
         ++status) {
        CHECK(appliance_boot_console_status_string(status) != NULL,
              "every boot console status has a string");
    }
    CHECK(appliance_boot_console_status_string(
              (enum appliance_boot_console_status)99U) == NULL,
          "undefined boot console status is rejected");
    for (action = APPLIANCE_BOOT_CONSOLE_ACTION_NONE;
         action <= APPLIANCE_BOOT_CONSOLE_ACTION_RECOVERY;
         ++action) {
        CHECK(appliance_boot_console_action_string(action) != NULL,
              "every boot console action has a string");
    }
    CHECK(appliance_boot_console_action_string(
              (enum appliance_boot_console_action)99U) == NULL,
          "undefined boot console action is rejected");
}

static void test_action_parsing(void)
{
    CHECK(appliance_boot_console_action("status", 6U) ==
              APPLIANCE_BOOT_CONSOLE_ACTION_STATUS,
          "status action parses");
    CHECK(appliance_boot_console_action("network", 7U) ==
              APPLIANCE_BOOT_CONSOLE_ACTION_NETWORK,
          "network action parses");
    CHECK(appliance_boot_console_action("diag", 4U) ==
              APPLIANCE_BOOT_CONSOLE_ACTION_DIAGNOSTICS,
          "diag action parses");
    CHECK(appliance_boot_console_action("diagnostics", 11U) ==
              APPLIANCE_BOOT_CONSOLE_ACTION_DIAGNOSTICS,
          "diagnostics action parses");
    CHECK(appliance_boot_console_action("reboot", 6U) ==
              APPLIANCE_BOOT_CONSOLE_ACTION_REBOOT,
          "reboot action parses");
    CHECK(appliance_boot_console_action("shutdown", 8U) ==
              APPLIANCE_BOOT_CONSOLE_ACTION_SHUTDOWN,
          "shutdown action parses");
    CHECK(appliance_boot_console_action("recovery", 8U) ==
              APPLIANCE_BOOT_CONSOLE_ACTION_RECOVERY,
          "recovery action parses");
    CHECK(appliance_boot_console_action("unknown", 7U) ==
              APPLIANCE_BOOT_CONSOLE_ACTION_NONE,
          "unknown action returns none");
}

static void test_unavailable_render(void)
{
    struct appliance_boot_console_snapshot snapshot =
        make_snapshot();
    char output[APPLIANCE_BOOT_CONSOLE_OUTPUT_MAX];
    size_t output_length = 0U;
    const char *expected = unavailable_expected();
    const size_t expected_length = strlen(expected);

    CHECK(appliance_boot_console_render(
              &snapshot, APPLIANCE_LANGUAGE_EN, output, sizeof(output),
              &output_length) == APPLIANCE_BOOT_CONSOLE_OK,
          "unavailable boot console renders");
    CHECK(output_length == expected_length &&
              memcmp(output, expected, output_length) == 0,
          "unavailable boot console output is exact");
    CHECK(appliance_boot_console_render(
              &snapshot, APPLIANCE_LANGUAGE_EN, output, expected_length,
              &output_length) == APPLIANCE_BOOT_CONSOLE_OVERFLOW,
          "boot console render requires caller capacity for NUL terminator");
}

static void test_management_ready_render(void)
{
    struct appliance_boot_console_snapshot snapshot =
        make_snapshot();
    char output[APPLIANCE_BOOT_CONSOLE_OUTPUT_MAX];
    size_t output_length = 0U;

    CHECK(appliance_boot_console_publish_network(
              &snapshot, APPLIANCE_CONSOLE_STATE_OK, 1U) ==
              APPLIANCE_BOOT_CONSOLE_OK &&
          appliance_boot_console_publish_storage(
              &snapshot, true, 1U) == APPLIANCE_BOOT_CONSOLE_OK &&
          appliance_boot_console_publish_services(
              &snapshot, APPLIANCE_CONSOLE_STATE_OK, 1U) ==
              APPLIANCE_BOOT_CONSOLE_OK &&
          appliance_boot_console_publish_management(
              &snapshot, "https://192.168.50.10/", 22U) ==
              APPLIANCE_BOOT_CONSOLE_OK,
          "ready boot console state publishes");
    CHECK(appliance_boot_console_render(
              &snapshot, APPLIANCE_LANGUAGE_EN, output, sizeof(output),
              &output_length) == APPLIANCE_BOOT_CONSOLE_OK,
          "ready boot console renders");
    CHECK(strstr(output, "Storage:       OK\n") != NULL &&
              strstr(output, "Network:       OK\n") != NULL &&
              strstr(output, "Services:      OK\n") != NULL &&
              strstr(output,
                     "Management:    https://192.168.50.10/\n") != NULL,
          "ready boot console publishes all ready domains");
}

/*
 * ITEM-509 increment 9: the boot console screen in the chosen language.  The
 * English render above is byte-for-byte the wording that stood here before;
 * this pass renders the same snapshot in Russian and requires the exact screen,
 * so a half-translated screen cannot pass.  The product name, the rule, the
 * state values and the machine words behind the entries stay English, because
 * the item keeps values and markers as they are.
 */
static const char *unavailable_expected_russian(void)
{
    return "HFOC Appliance\n"
           "--------------\n"
           "Имя узла:      hfoc-appliance\n"
           "ЦП:            2\n"
           "Память:        512 MiB\n"
           "Хранилище:     UNAVAILABLE\n"
           "Состояние:     OK\n"
           "Сеть:          UNAVAILABLE\n"
           "Службы:        UNAVAILABLE\n"
           "Управление:    UNAVAILABLE\n"
           "\n"
           "Локальное администрирование:\n"
           "[1] Статус\n"
           "[2] Сеть\n"
           "[3] Диагностика\n"
           "[4] Перезагрузка\n"
           "[5] Выключение\n"
           "[6] Восстановление\n";
}

static void test_russian_render(void)
{
    struct appliance_boot_console_snapshot snapshot = make_snapshot();
    char output[APPLIANCE_BOOT_CONSOLE_OUTPUT_MAX];
    size_t output_length = 0U;
    const char *expected = unavailable_expected_russian();
    const size_t expected_length = strlen(expected);

    CHECK(appliance_boot_console_render(
              &snapshot, APPLIANCE_LANGUAGE_RU, output, sizeof(output),
              &output_length) == APPLIANCE_BOOT_CONSOLE_OK,
          "Russian boot console renders");
    CHECK(output_length == expected_length &&
              memcmp(output, expected, output_length) == 0,
          "Russian boot console output is exact");
    CHECK(strstr(output, "Hostname:") == NULL &&
              strstr(output, "Local administration:") == NULL,
          "no English label survives the Russian screen");
}

static void test_management_url_validation(void)
{
    struct appliance_boot_console_snapshot snapshot =
        make_snapshot();

    CHECK(appliance_boot_console_publish_management(
              &snapshot, "http://192.168.50.10/", 22U) ==
              APPLIANCE_BOOT_CONSOLE_BAD_MANAGEMENT_URL,
          "boot console rejects non-https management URL");
    CHECK(appliance_boot_console_publish_management(
              &snapshot, "https://user@192.168.50.10/", 28U) ==
              APPLIANCE_BOOT_CONSOLE_BAD_MANAGEMENT_URL,
          "boot console rejects URL credentials");
    CHECK(snapshot.management_ready == false &&
              snapshot.management_url[0] == '\0',
          "failed management publication leaves state unchanged");
}

static void test_overflow_and_no_mutation(void)
{
    struct appliance_boot_console_snapshot snapshot =
        make_snapshot();
    struct appliance_boot_console_snapshot original = snapshot;
    char small[16U];
    size_t output_length = 0U;

    CHECK(appliance_boot_console_render(
              &snapshot, APPLIANCE_LANGUAGE_EN, small, sizeof(small),
              &output_length) == APPLIANCE_BOOT_CONSOLE_OVERFLOW,
          "boot console render rejects small output buffer");
    CHECK(memcmp(&snapshot, &original, sizeof(snapshot)) == 0,
          "failed boot console render leaves snapshot unchanged");
}

int main(void)
{
    test_status_and_action_strings();
    test_action_parsing();
    test_unavailable_render();
    test_management_ready_render();
    test_russian_render();
    test_management_url_validation();
    test_overflow_and_no_mutation();
    if (failures != 0) {
        fprintf(stderr, "Appliance boot console host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Appliance boot console host test: OK\n");
    return 0;
}
