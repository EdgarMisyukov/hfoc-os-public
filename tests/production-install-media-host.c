/*
 * ITEM-394 host proof for the installer medium's declaration and decision.
 *
 * The QEMU proof shows the whole product install run on a real (virtual) disk;
 * this proof covers the part that decides whether a write may happen at all:
 * the boot entry's install directive, the media declaration, the one-disk
 * selection and the bounded records the flow publishes.  It is deliberately
 * exhaustive on the refusal cases, because "never touch a disk the operator
 * did not select" is only as good as these decisions.
 */
#ifdef HFOC_PRODUCTION_INSTALL_MEDIA_HOST_TEST

#include "production_install_media.h"

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

static const char media_declaration[] =
    "# ITEM-394 acceptance media declaration\n"
    "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"
    "HFOC-INSTALL-TARGET=DEADBEEF00000001\n"
    "HFOC-INSTALL-MANAGEMENT=management-interface\n";

static void test_mode_parse(void)
{
    enum production_install_media_mode mode =
        PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED;

    CHECK(production_install_media_mode_parse(
              NULL, 0U, &mode) == PRODUCTION_INSTALL_MEDIA_OK &&
              mode == PRODUCTION_INSTALL_MEDIA_MODE_NONE,
          "an absent command line selects no install");
    CHECK(production_install_media_mode_parse(
              "hfoc.install=unattended",
              strlen("hfoc.install=unattended"),
              &mode) == PRODUCTION_INSTALL_MEDIA_OK &&
              mode == PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
          "the unattended entry is recognised");
    CHECK(production_install_media_mode_parse(
              "quiet hfoc.install=console debug",
              strlen("quiet hfoc.install=console debug"),
              &mode) == PRODUCTION_INSTALL_MEDIA_OK &&
              mode == PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE,
          "the console entry is recognised among other tokens");
    CHECK(production_install_media_mode_parse(
              "hfoc.install=none", strlen("hfoc.install=none"),
              &mode) == PRODUCTION_INSTALL_MEDIA_OK &&
              mode == PRODUCTION_INSTALL_MEDIA_MODE_NONE,
          "an explicit no-install entry selects no install");
    CHECK(production_install_media_mode_parse(
              "console=ttyS0", strlen("console=ttyS0"),
              &mode) == PRODUCTION_INSTALL_MEDIA_OK &&
              mode == PRODUCTION_INSTALL_MEDIA_MODE_NONE,
          "an unrelated command line selects no install");
    CHECK(production_install_media_mode_parse(
              "hfoc.install=silent", strlen("hfoc.install=silent"),
              &mode) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an unknown install directive is refused");
    CHECK(production_install_media_mode_parse(
              "hfoc.install=unattended", strlen("hfoc.install=unattended"),
              NULL) == PRODUCTION_INSTALL_MEDIA_NULL,
          "the mode parser refuses a null output");

    CHECK(!production_install_media_management_opt_in(NULL, 0U) &&
              !production_install_media_management_opt_in(
                  "hfoc.install=unattended",
                  strlen("hfoc.install=unattended")) &&
              !production_install_media_management_opt_in(
                  "hfoc.management=local-only",
                  strlen("hfoc.management=local-only")) &&
              production_install_media_management_opt_in(
                  "quiet hfoc.management=management-interface",
                  strlen("quiet hfoc.management=management-interface")) &&
              !production_install_media_management_opt_in(
                  "hfoc.management=management-interface "
                  "hfoc.management=local-only",
                  strlen("hfoc.management=management-interface "
                         "hfoc.management=local-only")),
          "the management opt-in needs one explicit declaration");
}

static void test_declaration_parse(void)
{
    struct production_install_media_declaration declaration;

    CHECK(production_install_media_declaration_parse(
              media_declaration, strlen(media_declaration),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.present &&
              declaration.confirm_declared &&
              strcmp(declaration.confirm, "ITEM-394-UNATTENDED") == 0 &&
              declaration.target_declared &&
              strcmp(declaration.target, "DEADBEEF00000001") == 0 &&
              declaration.management_interface,
          "the acceptance declaration parses");

    CHECK(production_install_media_declaration_parse(
              NULL, 0U, &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              !declaration.present,
          "an absent media declaration is not present");

    CHECK(production_install_media_declaration_parse(
              "", 0U, &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              !declaration.present,
          "an empty media declaration is not present");

    CHECK(production_install_media_declaration_parse(
              "# no declaration on this medium\n",
              strlen("# no declaration on this medium\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              !declaration.present && !declaration.confirm_declared &&
              !declaration.target_declared,
          "a comments-only declaration carries no install declaration");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-CONFIRM=TOKEN\n", 27U,
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.present && declaration.confirm_declared &&
              !declaration.target_declared,
          "a declaration without a target parses but declares no target");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-CONFIRM=\n",
              strlen("HFOC-INSTALL-CONFIRM=\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an empty confirmation value is refused");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-TARGET=serial with space\n",
              strlen("HFOC-INSTALL-TARGET=serial with space\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "a target with a space is refused");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-UNKNOWN=1\n",
              strlen("HFOC-INSTALL-UNKNOWN=1\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an unknown directive is refused");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-MANAGEMENT=outer-interface\n",
              strlen("HFOC-INSTALL-MANAGEMENT=outer-interface\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an unknown management scope is refused");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-MANAGEMENT=local-only\n",
              strlen("HFOC-INSTALL-MANAGEMENT=local-only\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              !declaration.management_interface,
          "the default management scope stays local-only");

    /*
     * ITEM-509 increment 27 (509.7c-2): the declared language is the catalog's
     * two codes and nothing else.  An absent directive is the default `en`
     * (zero), which is what every medium built before the directive relies on;
     * a misspelled value refuses the whole declaration, never a guess.
     */
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-LANGUAGE=ru\n",
              strlen("HFOC-INSTALL-LANGUAGE=ru\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.present && declaration.language_declared &&
              declaration.language == APPLIANCE_LANGUAGE_RU,
          "the declared language is parsed as Russian");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-LANGUAGE=en\n",
              strlen("HFOC-INSTALL-LANGUAGE=en\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.language_declared &&
              declaration.language == APPLIANCE_LANGUAGE_EN,
          "the declared language is parsed as English");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-CONFIRM=TOKEN\n", 27U,
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              !declaration.language_declared &&
              declaration.language == APPLIANCE_LANGUAGE_EN,
          "an absent language directive keeps the default language");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-LANGUAGE=de\n",
              strlen("HFOC-INSTALL-LANGUAGE=de\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "a language the catalog does not name is refused");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-LANGUAGE=\n",
              strlen("HFOC-INSTALL-LANGUAGE=\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an empty language is refused");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-LANGUAGE=RU\n",
              strlen("HFOC-INSTALL-LANGUAGE=RU\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "the language code is case-sensitive like the console command");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-CONFIRM=TOKEN\n"
              "HFOC-INSTALL-HOST-SEED=required\n",
              strlen("HFOC-INSTALL-CONFIRM=TOKEN\n"
                     "HFOC-INSTALL-HOST-SEED=required\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.present && declaration.host_seed_required,
          "a personalised medium requires its host seed module");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-HOST-SEED=optional\n",
              strlen("HFOC-INSTALL-HOST-SEED=optional\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an unknown host seed requirement is refused");
    /*
     * ITEM-606: the medium declares the fingerprint the owner confirmed on the
     * trusted Debian machine.  A missing, duplicated or malformed directive is
     * refused at parse time; the flow still requires the key module itself.
     */
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-SSH-KEY="
              "SHA256:MGSoetNGMLzNYvBWHBYJACHQk3NBN9s25zLPjPbEDMM\n",
              strlen("HFOC-INSTALL-SSH-KEY="
                     "SHA256:MGSoetNGMLzNYvBWHBYJACHQk3NBN9s25zLPjPbEDMM\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.ssh_key_declared &&
              strcmp(declaration.ssh_key_fingerprint,
                     "SHA256:MGSoetNGMLzNYvBWHBYJACHQk3NBN9s25zLPjPbEDMM") ==
                  0,
          "the owner-confirmed SSH key fingerprint parses");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-SSH-KEY=SHA256:AAAA\n"
              "HFOC-INSTALL-SSH-KEY=SHA256:BBBB\n",
              strlen("HFOC-INSTALL-SSH-KEY=SHA256:AAAA\n"
                     "HFOC-INSTALL-SSH-KEY=SHA256:BBBB\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "a duplicated SSH key directive is refused");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-SSH-KEY=MGSoetNGMLzNYvBWHBYJACHQk3NBN9s25zLPjPbEDMM\n",
              strlen("HFOC-INSTALL-SSH-KEY=MGSoetNGMLzNYvBWHBYJACHQk3NBN9s25zLPjPbEDMM\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an SSH key fingerprint without the SHA256: prefix is refused");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-SSH-KEY=\n",
              strlen("HFOC-INSTALL-SSH-KEY=\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an empty SSH key fingerprint is refused");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-SSH-KEY=SHA256:has space\n",
              strlen("HFOC-INSTALL-SSH-KEY=SHA256:has space\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an SSH key fingerprint with a space is refused");
    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-CONFIRM=TOKEN\n", 27U,
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              !declaration.ssh_key_declared,
          "an absent SSH key directive declares no key");
}

/*
 * ITEM-638: the owner update medium carries the full standard declaration -
 * confirmation, target, management, update layout, the confirmed SSH key, a
 * static address and an enabled forward with four clients.  That is already
 * 580 bytes, so the old 512-byte declaration limit refused the medium before
 * the flow ever looked at the disk.  The file keeps its own finite bound; the
 * boot command line keeps a separate one and must not grow with it.
 */
static const char capacity_declaration[] =
    "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED-INSTALL\n"
    "HFOC-INSTALL-TARGET=TESTNVME000001\n"
    "HFOC-INSTALL-MANAGEMENT=management-interface\n"
    "HFOC-INSTALL-LAYOUT=update\n"
    "HFOC-INSTALL-SSH-KEY="
    "SHA256:38DjJ3jKB8fc8aJ7u6xgM7LZnCzg8IYMjwE5YWDEcbc\n"
    "HFOC-INSTALL-NETWORK=static\n"
    "HFOC-INSTALL-ADDRESS=192.0.2.55/24\n"
    "HFOC-INSTALL-GATEWAY=192.0.2.1\n"
    "HFOC-INSTALL-DNS=192.0.2.1\n"
    "HFOC-INSTALL-FORWARD=on\n"
    "HFOC-INSTALL-FORWARD-TARGET=192.0.2.7\n"
    "HFOC-INSTALL-FORWARD-TARGET-PORT=443\n"
    "HFOC-INSTALL-FORWARD-LISTEN-PORT=9443\n"
    "HFOC-INSTALL-FORWARD-CLIENTS="
    "192.0.2.7,192.0.2.8,192.0.2.9,192.0.2.10\n";

static void test_declaration_capacity(void)
{
    struct production_install_media_declaration declaration;
    enum production_install_media_mode mode;
    char at_limit[PRODUCTION_INSTALL_MEDIA_DECLARATION_MAX];
    char over_limit[PRODUCTION_INSTALL_MEDIA_DECLARATION_MAX + 1U];
    char command_line[PRODUCTION_INSTALL_MEDIA_COMMAND_LINE_MAX + 1U];

    CHECK(strlen(capacity_declaration) > 512U &&
              strlen(capacity_declaration) <=
                  PRODUCTION_INSTALL_MEDIA_DECLARATION_MAX,
          "the full standard declaration is over the old limit and finite");
    CHECK(production_install_media_declaration_parse(
              capacity_declaration, strlen(capacity_declaration),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.present && declaration.confirm_declared &&
              strcmp(declaration.confirm,
                     "ITEM-394-UNATTENDED-INSTALL") == 0 &&
              declaration.target_declared &&
              strcmp(declaration.target, "TESTNVME000001") == 0 &&
              declaration.management_interface &&
              declaration.layout_declared && declaration.layout_update &&
              declaration.ssh_key_declared &&
              strcmp(declaration.ssh_key_fingerprint,
                     "SHA256:38DjJ3jKB8fc8aJ7u6xgM7LZnCzg8IYMjwE5YWDEcbc") ==
                  0 &&
              declaration.network_declared && declaration.network_static &&
              strcmp(declaration.address, "192.0.2.55/24") == 0 &&
              strcmp(declaration.gateway, "192.0.2.1") == 0 &&
              strcmp(declaration.dns, "192.0.2.1") == 0 &&
              declaration.forward_declared && declaration.forward_enabled &&
              strcmp(declaration.forward_target, "192.0.2.7") == 0 &&
              strcmp(declaration.forward_target_port, "443") == 0 &&
              strcmp(declaration.forward_listen_port, "9443") == 0 &&
              strcmp(declaration.forward_clients,
                     "192.0.2.7,192.0.2.8,192.0.2.9,"
                     "192.0.2.10") == 0,
          "every standard field of the >512-byte declaration parses");

    memset(at_limit, '#', sizeof(at_limit));
    CHECK(production_install_media_declaration_parse(
              at_limit, sizeof(at_limit),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK,
          "a declaration at the finite limit parses");

    memset(over_limit, '#', sizeof(over_limit));
    CHECK(production_install_media_declaration_parse(
              over_limit, sizeof(over_limit),
              &declaration) == PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "a declaration one byte over the limit is refused");

    memset(command_line, ' ', sizeof(command_line));
    memcpy(command_line, "hfoc.install=unattended",
           strlen("hfoc.install=unattended"));
    CHECK(production_install_media_mode_parse(
              command_line, sizeof(command_line),
              &mode) == PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "the boot command line keeps its own smaller limit");

    CHECK(strcmp(production_install_media_refusal_string(
                     PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_TOO_LARGE),
                 "TOKEN_TOO_LARGE") == 0,
          "an oversized declaration has its own refusal name");
    {
        char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
        size_t length = 0U;

        CHECK(production_install_media_render_refusal(
                  PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_TOO_LARGE, NULL,
                  record, sizeof(record), &length) ==
                  PRODUCTION_INSTALL_MEDIA_OK &&
                  strcmp(record,
                         "[PRODUCTION-INSTALL] refused reason="
                         "TOKEN_TOO_LARGE writes=0\n") == 0,
              "the oversized refusal says why and reports zero writes");
    }
}

static void test_report_fixture(
    struct installer_discovery_report *report,
    size_t disk_count,
    bool share_identity)
{
    size_t index;

    memset(report, 0, sizeof(*report));
    report->version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report->firmware_mode = INSTALLER_FIRMWARE_UEFI;
    report->read_only = true;
    report->scan_complete = true;
    report->cpu_present = true;
    report->cpu_cores = 2U;
    report->ram_present = true;
    report->ram_bytes = UINT64_C(2147483648);
    report->disk_count = (uint32_t)disk_count;
    for (index = 0U; index < disk_count && index < 2U; ++index) {
        report->disks[index].present = true;
        report->disks[index].id = (uint32_t)(index + 1U);
        report->disks[index].controller = INSTALLER_DISK_CONTROLLER_SATA;
        report->disks[index].partition_table =
            INSTALLER_PARTITION_TABLE_NONE;
        report->disks[index].block_size = 512U;
        report->disks[index].block_count = UINT64_C(234375000);
        memcpy(report->disks[index].identity,
               share_identity || index == 0U
                   ? "DEADBEEF00000001"
                   : "DEADBEEF00000002",
               sizeof("DEADBEEF00000001"));
    }
}

static void test_plan_build(void)
{
    struct production_install_media_declaration declaration;
    struct production_install_media_plan plan;
    struct installer_discovery_report report;

    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        media_declaration, strlen(media_declaration), &declaration);
    test_report_fixture(&report, 1U, false);

    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_NONE, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report,
              &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_NONE &&
              !plan.selected,
          "the ordinary entry never installs");

    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, NULL,
              INSTALLER_FIRMWARE_UEFI, &report,
              &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED &&
              plan.refusal ==
                  PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_MISSING,
          "an install entry without a media declaration refuses");

    /*
     * ITEM-517: a legacy-BIOS boot installs the same declared disk.  The
     * firmware mode selects the boot chain at install time; it is not a
     * reason to refuse the operator's declared target.
     */
    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
              INSTALLER_FIRMWARE_BIOS_LEGACY, &report,
              &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action ==
                  PRODUCTION_INSTALL_MEDIA_ACTION_UNATTENDED &&
              plan.refusal == PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE &&
              plan.selected && plan.target.disk_id == 1U &&
              strcmp(plan.target.identity, "DEADBEEF00000001") == 0,
          "a legacy-BIOS boot selects the declared disk");

    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
              INSTALLER_FIRMWARE_UNKNOWN, &report,
              &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
              plan.refusal ==
                  PRODUCTION_INSTALL_MEDIA_REFUSAL_FIRMWARE_UNSUPPORTED,
          "an unknown firmware still refuses the install");

    {
        struct installer_discovery_report invalid;

        test_report_fixture(&invalid, 1U, false);
        invalid.scan_complete = false;
        CHECK(production_install_media_plan_build(
                  PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
                  INSTALLER_FIRMWARE_UEFI, &invalid,
                  &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
                  plan.refusal ==
                      PRODUCTION_INSTALL_MEDIA_REFUSAL_REPORT_UNAVAILABLE,
              "an unproven scan refuses instead of guessing");
    }

    {
        struct installer_discovery_report empty;

        test_report_fixture(&empty, 0U, false);
        CHECK(production_install_media_plan_build(
                  PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
                  INSTALLER_FIRMWARE_UEFI, &empty,
                  &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
                  plan.refusal ==
                      PRODUCTION_INSTALL_MEDIA_REFUSAL_NO_DISK,
              "a machine without a disk refuses");
    }

    {
        struct installer_discovery_report other;

        test_report_fixture(&other, 1U, false);
        memcpy(other.disks[0].identity, "DEADBEEF00000002",
               sizeof("DEADBEEF00000002"));
        CHECK(production_install_media_plan_build(
                  PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
                  INSTALLER_FIRMWARE_UEFI, &other,
                  &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
                  plan.refusal ==
                      PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_NOT_FOUND &&
                  !plan.selected,
              "a disk the operator did not select is never the target");
    }

    {
        struct installer_discovery_report ambiguous;

        test_report_fixture(&ambiguous, 2U, true);
        CHECK(production_install_media_plan_build(
                  PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
                  INSTALLER_FIRMWARE_UEFI, &ambiguous,
                  &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
                  plan.refusal ==
                      PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_AMBIGUOUS &&
                  !plan.selected,
              "two disks with the declared identity are ambiguous");
    }

    /*
     * ITEM-500: a medium declared for one machine installs on another when a
     * person is at the screen.  The console entry keeps the disks this machine
     * has and lets the operator name one - the declared disk being absent is
     * the reason to ask, not a reason to refuse.  The unattended entry has
     * nobody to ask and keeps the refusal the QEMU proof reads.
     */
    {
        struct installer_discovery_report foreign;

        test_report_fixture(&foreign, 1U, false);
        memcpy(foreign.disks[0].identity, "DEADBEEF00000002",
               sizeof("DEADBEEF00000002"));
        CHECK(production_install_media_plan_build(
                  PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
                  INSTALLER_FIRMWARE_UEFI, &foreign,
                  &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
                  plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED &&
                  plan.refusal ==
                      PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_NOT_FOUND,
              "the unattended entry still refuses a machine that does not "
              "carry the declared disk");
        CHECK(production_install_media_plan_build(
                  PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE, &declaration,
                  INSTALLER_FIRMWARE_UEFI, &foreign,
                  &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
                  plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_CONSOLE &&
                  plan.refusal == PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE &&
                  !plan.selected,
              "the operator entry asks the person instead of refusing");
    }

    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report,
              &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action ==
                  PRODUCTION_INSTALL_MEDIA_ACTION_UNATTENDED &&
              plan.selected && plan.target.disk_id == 1U &&
              strcmp(plan.target.identity, "DEADBEEF00000001") == 0 &&
              plan.target.controller == INSTALLER_DISK_CONTROLLER_SATA &&
              plan.target.block_count == UINT64_C(234375000),
          "the declared disk is selected with its identity");

    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report, &plan) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_CONSOLE,
          "the console entry selects the same disk and waits");

    {
        struct production_install_media_declaration no_confirm;

        (void)production_install_media_declaration_parse(
            "HFOC-INSTALL-TARGET=DEADBEEF00000001\n",
            strlen("HFOC-INSTALL-TARGET=DEADBEEF00000001\n"),
            &no_confirm);
        CHECK(production_install_media_plan_build(
                  PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &no_confirm,
                  INSTALLER_FIRMWARE_UEFI, &report,
                  &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
                  plan.refusal ==
                      PRODUCTION_INSTALL_MEDIA_REFUSAL_CONFIRM_MISSING,
              "a declaration without a confirmation refuses");
    }

    {
        struct production_install_media_declaration no_target;

        (void)production_install_media_declaration_parse(
            "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n",
            strlen("HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"),
            &no_target);
        CHECK(production_install_media_plan_build(
                  PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &no_target,
                  INSTALLER_FIRMWARE_UEFI, &report,
                  &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
                  plan.refusal ==
                      PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_MISSING,
              "a declaration without a target refuses");
    }

    CHECK(production_install_media_candidate_count(&report) == 1U &&
              production_install_media_candidate(&report, 0U) ==
                  &report.disks[0] &&
              production_install_media_candidate(&report, 1U) == NULL,
          "the candidate list is bounded and ordered");
}

static void test_confirmation_match(void)
{
    struct production_install_media_declaration declaration;

    (void)production_install_media_declaration_parse(
        media_declaration, strlen(media_declaration), &declaration);
    CHECK(production_install_media_confirm_matches(
              &declaration, "ITEM-394-UNATTENDED",
              strlen("ITEM-394-UNATTENDED")),
          "the declared confirmation token matches");
    CHECK(!production_install_media_confirm_matches(
              &declaration, "ITEM-394-SOMETHING-ELSE",
              strlen("ITEM-394-SOMETHING-ELSE")),
          "a different confirmation token is refused");
    CHECK(!production_install_media_confirm_matches(
              &declaration, NULL, 0U),
          "a missing confirmation token is refused");
    CHECK(!production_install_media_confirm_matches(
              NULL, "ITEM-394-UNATTENDED",
              strlen("ITEM-394-UNATTENDED")),
          "a media without a declaration cannot be confirmed");
}

static void test_render(void)
{
    struct production_install_media_declaration declaration;
    struct production_install_media_plan plan;
    struct installer_discovery_report report;
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    char small[16U];
    size_t length = 0U;

    (void)production_install_media_declaration_parse(
        media_declaration, strlen(media_declaration), &declaration);
    test_report_fixture(&report, 1U, false);
    (void)production_install_media_plan_build(
        PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
        INSTALLER_FIRMWARE_UEFI, &report, &plan);

    memset(record, 0, sizeof(record));
    CHECK(production_install_media_render_declaration(
              &declaration, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "[PRODUCTION-INSTALL] media token=PRESENT "
                     "confirm=ITEM-394-UNATTENDED "
                     "target=DEADBEEF00000001 "
                     "management=management-interface "
                     "mode=default network=default forward=default "
                     "entry=unattended\n") == 0,
          "the media declaration record is exact");

    /*
     * ITEM-398: the declared layout is published as `mode=` - never as
     * `layout=`, which the applied record already uses for `layout=VERIFIED` -
     * and an operator who declared `preserve` reads it back verbatim.
     */
    {
        static const char preserve_declaration[] =
            "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"
            "HFOC-INSTALL-TARGET=DEADBEEF00000001\n"
            "HFOC-INSTALL-MANAGEMENT=local-only\n"
            "HFOC-INSTALL-LAYOUT=preserve\n";
        struct production_install_media_declaration declared;

        memset(&declared, 0, sizeof(declared));
        CHECK(production_install_media_declaration_parse(
                  preserve_declaration, strlen(preserve_declaration),
                  &declared) == PRODUCTION_INSTALL_MEDIA_OK &&
                  declared.layout_declared && declared.layout_preserve,
              "the layout directive is parsed as declared preserve");
        CHECK(production_install_media_render_declaration(
                  &declared, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
                  record, sizeof(record), &length) ==
                  PRODUCTION_INSTALL_MEDIA_OK &&
                  strcmp(record,
                         "[PRODUCTION-INSTALL] media token=PRESENT "
                         "confirm=ITEM-394-UNATTENDED "
                         "target=DEADBEEF00000001 "
                         "management=local-only "
                         "mode=preserve network=default forward=default "
                         "entry=unattended\n") == 0,
              "a declared preserve layout is published as mode=preserve");
    }

    /*
     * ITEM-586: `update` is its own declaration, never inferred from a disk.
     * It is published as mode=update and keeps preserve/whole-disk false.
     */
    {
        static const char update_declaration[] =
            "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"
            "HFOC-INSTALL-TARGET=DEADBEEF00000001\n"
            "HFOC-INSTALL-MANAGEMENT=local-only\n"
            "HFOC-INSTALL-LAYOUT=update\n";
        struct production_install_media_declaration declared;

        memset(&declared, 0, sizeof(declared));
        CHECK(production_install_media_declaration_parse(
                  update_declaration, strlen(update_declaration),
                  &declared) == PRODUCTION_INSTALL_MEDIA_OK &&
                  declared.layout_declared && declared.layout_update &&
                  !declared.layout_preserve,
              "the layout directive is parsed as an explicit update");
        CHECK(production_install_media_render_declaration(
                  &declared, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
                  record, sizeof(record), &length) ==
                  PRODUCTION_INSTALL_MEDIA_OK &&
                  strcmp(record,
                         "[PRODUCTION-INSTALL] media token=PRESENT "
                         "confirm=ITEM-394-UNATTENDED "
                         "target=DEADBEEF00000001 "
                         "management=local-only "
                         "mode=update network=default forward=default "
                         "entry=unattended\n") == 0,
              "a declared update layout is published as mode=update");
    }

    /*
     * ITEM-509 increment 27 (509.7c-2): a declared language is named in the
     * media record before the first write, and a medium that declares none
     * keeps the record asserted above byte for byte.
     */
    {
        static const char language_declaration[] =
            "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"
            "HFOC-INSTALL-TARGET=DEADBEEF00000001\n"
            "HFOC-INSTALL-MANAGEMENT=local-only\n"
            "HFOC-INSTALL-LANGUAGE=ru\n";
        struct production_install_media_declaration declared;

        memset(&declared, 0, sizeof(declared));
        CHECK(production_install_media_declaration_parse(
                  language_declaration, strlen(language_declaration),
                  &declared) == PRODUCTION_INSTALL_MEDIA_OK &&
                  declared.language_declared &&
                  declared.language == APPLIANCE_LANGUAGE_RU,
              "the language directive is parsed as declared Russian");
        CHECK(production_install_media_render_declaration(
                  &declared, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
                  record, sizeof(record), &length) ==
                  PRODUCTION_INSTALL_MEDIA_OK &&
                  strcmp(record,
                         "[PRODUCTION-INSTALL] media token=PRESENT "
                         "confirm=ITEM-394-UNATTENDED "
                         "target=DEADBEEF00000001 "
                         "management=local-only "
                         "mode=default language=ru network=default "
                         "forward=default entry=unattended\n") == 0,
              "a declared language is published in the media record");
    }

    CHECK(production_install_media_render_candidate(
              &report.disks[0], "KINGSTON SA400S37480G", true,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "[PRODUCTION-INSTALL] candidate disk=1 "
                     "controller=SATA identity=DEADBEEF00000001 "
                     "model=KINGSTON SA400S37480G blocks=234375000 "
                     "size_mib=114440 selected=1\n") == 0,
          "the candidate record carries the identity and the model");

    CHECK(production_install_media_render_candidate(
              &report.disks[0], NULL, false, record, sizeof(record),
              &length) == PRODUCTION_INSTALL_MEDIA_OK &&
              strstr(record, "model=unavailable") != NULL &&
              strstr(record, " selected=0\n") != NULL,
          "a candidate without a model is published honestly");

    CHECK(production_install_media_render_confirmation(
              &declaration, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
              &plan.target, PRODUCTION_INSTALL_MEDIA_CONFIRMATION_MEDIA, 7U,
              record, sizeof(record),
              &length) == PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "[PRODUCTION-INSTALL] confirm source=media-token "
                     "entry=unattended token=ITEM-394-UNATTENDED "
                     "target=DEADBEEF00000001 disk=1 "
                     "identity=DEADBEEF00000001 "
                     "management=management-interface "
                     "when_uptime_s=7\n") == 0,
          "the confirmation record names the source and the disk");

    CHECK(production_install_media_render_confirmation(
              &declaration, PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE,
              &plan.target, PRODUCTION_INSTALL_MEDIA_CONFIRMATION_OPERATOR, 9U,
              record, sizeof(record),
              &length) == PRODUCTION_INSTALL_MEDIA_OK &&
              strstr(record, "source=operator-screen") != NULL &&
              strstr(record, "entry=console") != NULL,
          "an operator confirmation publishes its own source, and no console "
          "token appears anywhere in it");

    CHECK(production_install_media_render_refusal(
              PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_MISSING, NULL,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "[PRODUCTION-INSTALL] refused "
                     "reason=TOKEN_MISSING writes=0\n") == 0,
          "a refusal without a declaration states zero writes");

    CHECK(production_install_media_render_refusal(
              PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_NOT_FOUND,
              &declaration, record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "[PRODUCTION-INSTALL] refused "
                     "reason=TARGET_NOT_FOUND "
                     "target=DEADBEEF00000001 writes=0\n") == 0,
          "a target refusal names the target that was not found");

    CHECK(production_install_media_render_refusal(
              PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE, NULL, record,
              sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "a refusal record cannot be rendered without a reason");

    /*
     * ITEM-399: the installed configuration always declares the scope, so the
     * bind record has to tell an explicit local-only declaration - which is
     * what the installed appliance carries - from an entry that declares
     * nothing and therefore really is the kernel's default.
     */
    {
        bool management_interface = true;

        CHECK(production_install_media_management_declared(
                  "hfoc.install=unattended hfoc.management=local-only",
                  strlen("hfoc.install=unattended "
                         "hfoc.management=local-only"),
                  &management_interface) &&
                  !management_interface,
              "an explicit local-only declaration is reported as declared");
        CHECK(production_install_media_management_declared(
                  "hfoc.management=management-interface",
                  strlen("hfoc.management=management-interface"),
                  &management_interface) &&
                  management_interface,
              "the management interface declaration is reported as declared");
        CHECK(!production_install_media_management_declared(
                  "hfoc.install=unattended",
                  strlen("hfoc.install=unattended"),
                  &management_interface) &&
                  !management_interface,
              "an entry that declares no scope keeps the default");
        CHECK(!production_install_media_management_declared(
                  "hfoc.management=management-interface "
                  "hfoc.management=local-only",
                  strlen("hfoc.management=management-interface "
                         "hfoc.management=local-only"),
                  &management_interface) &&
                  !management_interface,
              "an ambiguous declaration is reported as no declaration");
        CHECK(!production_install_media_management_declared(
                  NULL, 0U, &management_interface) &&
                  !management_interface &&
                  production_install_media_management_declared(
                      "hfoc.management=local-only",
                      strlen("hfoc.management=local-only"), NULL),
              "the declaration query tolerates a missing output");
    }

    CHECK(production_install_media_render_applied(
              &declaration, &plan.target, 12U, record, sizeof(record),
              &length) == PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "[PRODUCTION-INSTALL] applied disk=1 "
                     "identity=DEADBEEF00000001 controller=SATA "
                     "management=management-interface "
                     "mode=default when_uptime_s=12 "
                     "layout=VERIFIED\n") == 0,
          "the applied record names the written disk and layout state");

    CHECK(production_install_media_render_declaration(
              &declaration, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
              small, sizeof(small), &length) ==
              PRODUCTION_INSTALL_MEDIA_OVERFLOW,
          "a record that does not fit is refused, not truncated");

    CHECK(production_install_media_render_refusal(
              PRODUCTION_INSTALL_MEDIA_REFUSAL_CONFIRM_MISSING,
              &declaration, NULL, 0U, &length) ==
              PRODUCTION_INSTALL_MEDIA_NULL,
          "the renderer refuses a null buffer");

    CHECK(production_install_media_render_pending(
              &declaration, &plan.target, record, sizeof(record),
              &length) == PRODUCTION_INSTALL_MEDIA_OK &&
              strstr(record,
                     "[PRODUCTION-INSTALL] pending source=console "
                     "entry=console token=ITEM-394-UNATTENDED "
                     "target=DEADBEEF00000001 disk=1 "
                     "identity=DEADBEEF00000001") == record &&
              strstr(record, "writes=0\n") != NULL &&
              strstr(record, "INSTALL CONFIRM") == NULL,
          "the console path publishes what it prepared, with zero writes and "
          "no console command to type");

    CHECK(production_install_media_render_controller_refusal(
              "PRODUCTION_INSTALLER_MEDIA_TOO_SMALL", NULL,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "[PRODUCTION-INSTALL] refused reason=CONTROLLER "
                     "status=PRODUCTION_INSTALLER_MEDIA_TOO_SMALL "
                     "writes=0\n") == 0,
          "a planner refusal states the controller status, not a guess");

    /*
     * ITEM-500: the record the operator entry publishes when the declared disk
     * is not on this machine, and the line the screen shows for the same
     * reason.  Both name the declared disk, and the record says out loud that
     * nothing is chosen yet.
     */
    CHECK(production_install_media_render_awaiting_choice(
              &declaration, record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "[PRODUCTION-INSTALL] pending source=console "
                     "entry=console token=ITEM-394-UNATTENDED "
                     "target=DEADBEEF00000001 "
                     "declared=NOT_ON_THIS_MACHINE disk=NONE identity=NONE "
                     "writes=0\n") == 0,
          "the operator entry's pending record names the declared disk it does "
          "not have and says no disk is chosen yet");

    CHECK(production_install_media_render_choice_note(
              &declaration, APPLIANCE_LANGUAGE_DEFAULT,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "Declared disk DEADBEEF00000001 is not on this machine - "
                     "choose a disk") == 0,
          "the screen line names the declared disk and asks for a choice");
    /*
     * ITEM-509 increment 15: the same line in Russian comes from the one
     * catalog; the declared target stays the machine value the medium carries,
     * and a language the catalog cannot answer refuses the line by name.
     */
    CHECK(production_install_media_render_choice_note(
              &declaration, APPLIANCE_LANGUAGE_RU,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "Объявленный диск DEADBEEF00000001 отсутствует на этой "
                     "машине — выберите диск") == 0,
          "the screen line names the declared disk in the selected language");
    CHECK(production_install_media_render_choice_note(
              &declaration, (enum appliance_language)7,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "a language the catalog cannot answer refuses the screen line by "
          "name");

    CHECK(production_install_media_render_awaiting_choice(
              &declaration, NULL, 0U, &length) ==
              PRODUCTION_INSTALL_MEDIA_NULL &&
              production_install_media_render_choice_note(
                  &declaration, APPLIANCE_LANGUAGE_DEFAULT,
                  NULL, 0U, &length) ==
              PRODUCTION_INSTALL_MEDIA_NULL,
          "both operator-entry renderers refuse a null buffer");

    CHECK(production_install_media_render_controller_refusal(
              "PRODUCTION_INSTALLER_MEDIA_UNVERIFIED",
              "INSTALLER_MEDIA_SIGNATURE_INVALID",
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strstr(record,
                     "status=PRODUCTION_INSTALLER_MEDIA_UNVERIFIED "
                     "detail=INSTALLER_MEDIA_SIGNATURE_INVALID "
                     "writes=0") != NULL,
          "a media refusal states the media verification status");

    CHECK(production_install_media_render_failure(
              "esp-payload", "PRODUCTION_INSTALLER_IO_ERROR", record,
              sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(record,
                     "[PRODUCTION-INSTALL] failed stage=esp-payload "
                     "status=PRODUCTION_INSTALLER_IO_ERROR\n") == 0,
          "a failure after the first write is never reported as a refusal");
}

static void test_strings(void)
{
    CHECK(strcmp(production_install_media_mode_string(
                     PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED),
                 "unattended") == 0 &&
              strcmp(production_install_media_action_string(
                         PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED),
                     "REFUSED") == 0 &&
              strcmp(production_install_media_refusal_string(
                         PRODUCTION_INSTALL_MEDIA_REFUSAL_NO_DISK),
                     "NO_DISK") == 0 &&
              strcmp(production_install_media_status_string(
                         PRODUCTION_INSTALL_MEDIA_BAD_TEXT),
                     "PRODUCTION_INSTALL_MEDIA_BAD_TEXT") == 0 &&
              strcmp(production_install_media_controller_string(
                         INSTALLER_DISK_CONTROLLER_VIRTIO_BLK),
                     "VIRTIO-BLK") == 0,
          "the published vocabulary is stable");
    CHECK(production_install_media_mode_string(
              (enum production_install_media_mode)99) != NULL &&
              production_install_media_refusal_string(
                  (enum production_install_media_refusal)99) == NULL,
          "unknown values are published honestly");
    CHECK(strcmp(production_install_media_refusal_string(
                     PRODUCTION_INSTALL_MEDIA_REFUSAL_MODE_MALFORMED),
                 "MODE_MALFORMED") == 0 &&
              /* ITEM-495: 12 was CONSOLE_TOKEN_MISMATCH and is retired with the
               * console token; the value after it keeps its own spelling. */
              strcmp(production_install_media_refusal_string(
                         PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_INCOMPLETE),
                     "NETWORK_INCOMPLETE") == 0,
          "the refusal vocabulary is stable across the retired console token");
    CHECK(strcmp(production_install_media_refusal_string(
                     PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_MISSING),
                 "HOST_SEED_MISSING") == 0 &&
              strcmp(production_install_media_refusal_string(
                         PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_INVALID),
                     "HOST_SEED_INVALID") == 0 &&
              strcmp(production_install_media_refusal_string(
                         PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_TARGET_MISMATCH),
                     "HOST_SEED_TARGET_MISMATCH") == 0 &&
              strcmp(production_install_media_refusal_string(
                         PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_BUILD_MISMATCH),
                     "HOST_SEED_BUILD_MISMATCH") == 0 &&
              strcmp(production_install_media_refusal_string(
                         PRODUCTION_INSTALL_MEDIA_REFUSAL_CREDENTIAL_REQUIRED),
                     "CREDENTIAL_REQUIRED") == 0,
          "the host seed refusals have stable names");
}

/*
 * ITEM-446: the network the operator declares on the medium, the one the
 * installed entry carries, and the refusals that keep an unhonourable
 * declaration from reaching the first write.
 */
static const char network_declaration[] =
    "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"
    "HFOC-INSTALL-TARGET=DEADBEEF00000001\n"
    "HFOC-INSTALL-MANAGEMENT=management-interface\n"
    "HFOC-INSTALL-LAYOUT=whole-disk\n"
    "HFOC-INSTALL-NETWORK=static\n"
    "HFOC-INSTALL-ADDRESS=10.0.2.15/24\n"
    "HFOC-INSTALL-GATEWAY=10.0.2.2\n"
    "HFOC-INSTALL-DNS=10.0.2.3\n";

static void test_network_declaration_parse(void)
{
    struct production_install_media_declaration declaration;

    CHECK(production_install_media_declaration_parse(
              network_declaration, strlen(network_declaration),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.present && declaration.network_declared &&
              declaration.network_static &&
              declaration.address_declared &&
              strcmp(declaration.address, "10.0.2.15/24") == 0 &&
              declaration.gateway_declared &&
              strcmp(declaration.gateway, "10.0.2.2") == 0 &&
              declaration.dns_declared &&
              strcmp(declaration.dns, "10.0.2.3") == 0,
          "a static network declaration parses");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-NETWORK=dhcp\n",
              strlen("HFOC-INSTALL-NETWORK=dhcp\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.network_declared && !declaration.network_static &&
              !declaration.address_declared,
          "a DHCP declaration parses without static values");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-NETWORK=ethernet\n",
              strlen("HFOC-INSTALL-NETWORK=ethernet\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an unknown network mode is refused");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-ADDRESS=10.0.2.15/24\n",
              strlen("HFOC-INSTALL-ADDRESS=10.0.2.15/24\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.address_declared &&
              !declaration.network_declared,
          "an address without a mode parses but declares no network");
}

static void test_network_config_validation(void)
{
    struct production_install_media_declaration declaration;
    struct network_ipv4_config config;
    bool is_static = false;

    memset(&declaration, 0, sizeof(declaration));
    declaration.network_declared = true;
    declaration.network_static = true;
    declaration.address_declared = true;
    declaration.gateway_declared = true;
    declaration.dns_declared = true;
    memcpy(declaration.address, "10.0.2.15/24", sizeof("10.0.2.15/24"));
    memcpy(declaration.gateway, "10.0.2.2", sizeof("10.0.2.2"));
    memcpy(declaration.dns, "10.0.2.3", sizeof("10.0.2.3"));

    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              is_static && config.prefix_length == 24U &&
              config.address.octets[0] == 10U &&
              config.address.octets[3] == 15U &&
              config.gateway.octets[3] == 2U &&
              config.dns.octets[3] == 3U,
          "a complete static declaration validates");

    /* An incomplete declaration is refused, never treated as DHCP. */
    declaration.dns_declared = false;
    declaration.dns[0] = '\0';
    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_BAD_TEXT &&
              !is_static,
          "a static declaration without its DNS is refused");
    declaration.dns_declared = true;
    memcpy(declaration.dns, "10.0.2.3", sizeof("10.0.2.3"));

    /* DHCP with a static value is an ambiguous declaration. */
    declaration.network_static = false;
    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "DHCP with a static value is refused");
    declaration.network_static = true;

    /* Values without a mode are refused as well. */
    declaration.network_declared = false;
    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an address without a network mode is refused");
    declaration.network_declared = true;

    /* The address must carry one prefix, and the gateway must be inside it. */
    memcpy(declaration.address, "10.0.2.15", sizeof("10.0.2.15"));
    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "an address without a prefix is refused");
    memcpy(declaration.address, "10.0.2.15/24", sizeof("10.0.2.15/24"));

    memcpy(declaration.address, "10.0.2.15/33", sizeof("10.0.2.15/33"));
    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "a prefix outside 1..32 is refused");
    memcpy(declaration.address, "10.0.2.15/24", sizeof("10.0.2.15/24"));

    memcpy(declaration.gateway, "10.0.3.1", sizeof("10.0.3.1"));
    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "a gateway outside the declared subnet is refused");
    memcpy(declaration.gateway, "10.0.2.2", sizeof("10.0.2.2"));

    memcpy(declaration.dns, "not-an-address", sizeof("not-an-address"));
    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "a malformed DNS is refused");
    memcpy(declaration.dns, "10.0.2.3", sizeof("10.0.2.3"));

    /* The absence of a declaration keeps the pre-item behaviour. */
    memset(&declaration, 0, sizeof(declaration));
    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              !is_static,
          "an undeclared network stays DHCP");
    declaration.network_declared = true;
    declaration.network_static = false;
    CHECK(production_install_media_network_config(
              &declaration, &is_static, &config) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              !is_static,
          "a DHCP declaration stays DHCP");
}

static void test_network_plan_refusal(void)
{
    struct production_install_media_declaration declaration;
    struct production_install_media_plan plan;
    struct installer_discovery_report report;

    test_report_fixture(&report, 1U, false);
    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        network_declaration, strlen(network_declaration), &declaration);
    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report, &plan) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_UNATTENDED &&
              plan.selected,
          "a valid static declaration plans the install");

    memcpy(declaration.gateway, "10.0.3.1", sizeof("10.0.3.1"));
    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report, &plan) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED &&
              plan.refusal ==
                  PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_MALFORMED &&
              !plan.selected,
          "a gateway outside the subnet refuses before any write");
    memcpy(declaration.gateway, "10.0.2.2", sizeof("10.0.2.2"));

    declaration.dns_declared = false;
    declaration.dns[0] = '\0';
    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report, &plan) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED &&
              plan.refusal ==
                  PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_INCOMPLETE,
          "an incomplete static declaration refuses the run");
}

static void test_network_boot_and_cmdline(void)
{
    static const char sntp_declaration_text[] =
        "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"
        "HFOC-INSTALL-TARGET=DEADBEEF00000001\n"
        "HFOC-INSTALL-MANAGEMENT=management-interface\n"
        "HFOC-INSTALL-NETWORK=dhcp\n"
        "HFOC-INSTALL-SNTP=192.0.2.7\n";
    struct production_install_media_declaration declaration;
    struct production_install_media_declaration sntp_declaration;
    struct production_install_media_boot_network boot_network;
    char cmdline[PRODUCTION_INSTALL_MEDIA_NETWORK_CMDLINE_MAX];
    char installed[PRODUCTION_INSTALL_MEDIA_NETWORK_CMDLINE_MAX];
    size_t length = 0U;

    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        network_declaration, strlen(network_declaration), &declaration);

    memset(cmdline, 0, sizeof(cmdline));
    CHECK(production_install_media_network_cmdline(
              &declaration, cmdline, sizeof(cmdline), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(cmdline,
                     "hfoc.network=static hfoc.address=10.0.2.15/24 "
                     "hfoc.gateway=10.0.2.2 hfoc.dns=10.0.2.3") == 0 &&
              length == strlen(cmdline),
          "the network declaration renders the installed directive");

    memset(installed, 0, sizeof(installed));
    CHECK(production_install_media_installed_cmdline(
              &declaration, installed, sizeof(installed), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(installed,
                     "hfoc.management=management-interface "
                     "hfoc.network=static hfoc.address=10.0.2.15/24 "
                     "hfoc.gateway=10.0.2.2 hfoc.dns=10.0.2.3") == 0,
          "the installed command line carries both declarations");

    /* ITEM-654: the declared time source renders into the installed entry
     * next to the network declaration. */
    declaration.sntp_declared = true;
    strcpy(declaration.sntp, "192.0.2.7");
    memset(cmdline, 0, sizeof(cmdline));
    CHECK(production_install_media_network_cmdline(
              &declaration, cmdline, sizeof(cmdline), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(cmdline,
                     "hfoc.network=static hfoc.address=10.0.2.15/24 "
                     "hfoc.gateway=10.0.2.2 hfoc.dns=10.0.2.3 "
                     "hfoc.sntp=192.0.2.7") == 0,
          "the declared SNTP source renders as one installed token");
    declaration.sntp_declared = false;
    memset(declaration.sntp, 0, sizeof(declaration.sntp));

    /* The boot parser accepts what the installed entry carries. */
    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              installed, strlen(installed), &boot_network) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              boot_network.declared && boot_network.is_static &&
              boot_network.config_valid &&
              boot_network.config.prefix_length == 24U &&
              strcmp(boot_network.address, "10.0.2.15/24") == 0,
          "the installed declaration parses back into a validated config");

    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              "hfoc.management=local-only hfoc.network=dhcp",
              strlen("hfoc.management=local-only hfoc.network=dhcp"),
              &boot_network) == PRODUCTION_INSTALL_MEDIA_OK &&
              boot_network.declared && !boot_network.is_static &&
              !boot_network.config_valid,
          "a DHCP declaration parses without a static config");

    /* ITEM-654: an explicit SNTP source travels beside the addressing mode
     * and is validated as one unicast IPv4 without refusing the network. */
    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              "hfoc.network=dhcp hfoc.sntp=192.0.2.7",
              strlen("hfoc.network=dhcp hfoc.sntp=192.0.2.7"),
              &boot_network) == PRODUCTION_INSTALL_MEDIA_OK &&
              boot_network.declared && boot_network.sntp_declared &&
              boot_network.sntp_valid &&
              boot_network.sntp_address.octets[0] == 192U &&
              boot_network.sntp_address.octets[3] == 7U,
          "the boot parser accepts an explicit SNTP source");

    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              "hfoc.network=dhcp hfoc.sntp=not-an-address",
              strlen("hfoc.network=dhcp hfoc.sntp=not-an-address"),
              &boot_network) == PRODUCTION_INSTALL_MEDIA_OK &&
              boot_network.declared && boot_network.sntp_declared &&
              !boot_network.sntp_valid,
          "a malformed SNTP source is diagnosed, not a network refusal");

    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              "hfoc.network=dhcp hfoc.sntp=224.0.0.1",
              strlen("hfoc.network=dhcp hfoc.sntp=224.0.0.1"),
              &boot_network) == PRODUCTION_INSTALL_MEDIA_OK &&
              boot_network.sntp_declared && !boot_network.sntp_valid,
          "a multicast SNTP source is refused");

    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              "hfoc.sntp=192.0.2.7 hfoc.sntp=192.0.2.8",
              strlen("hfoc.sntp=192.0.2.7 hfoc.sntp=192.0.2.8"),
              &boot_network) == PRODUCTION_INSTALL_MEDIA_OK &&
              boot_network.sntp_declared && !boot_network.sntp_valid,
          "a duplicate SNTP source is ambiguous and refused");

    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              "hfoc.management=local-only",
              strlen("hfoc.management=local-only"), &boot_network) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              !boot_network.declared,
          "a command line without a network declares none");

    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              "hfoc.network=ethernet", strlen("hfoc.network=ethernet"),
              &boot_network) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an unknown boot network mode is refused");

    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              "hfoc.network=static hfoc.address=10.0.3.15/24 "
              "hfoc.gateway=10.0.2.2 hfoc.dns=10.0.2.3",
              strlen("hfoc.network=static hfoc.address=10.0.3.15/24 "
                     "hfoc.gateway=10.0.2.2 hfoc.dns=10.0.2.3"),
              &boot_network) ==
              PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "a boot declaration with a gateway outside its subnet is refused");

    memset(&boot_network, 0, sizeof(boot_network));
    CHECK(production_install_media_network_boot(
              "hfoc.network=static hfoc.address=10.0.2.15",
              strlen("hfoc.network=static hfoc.address=10.0.2.15"),
              &boot_network) ==
              PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "a boot declaration without a prefix is refused");

    /* ITEM-654: the medium directive is parsed, validated and rendered. */
    memset(&sntp_declaration, 0, sizeof(sntp_declaration));
    CHECK(production_install_media_declaration_parse(
              sntp_declaration_text, strlen(sntp_declaration_text),
              &sntp_declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              sntp_declaration.sntp_declared &&
              strcmp(sntp_declaration.sntp, "192.0.2.7") == 0,
          "the medium SNTP directive is parsed");
    memset(cmdline, 0, sizeof(cmdline));
    CHECK(production_install_media_network_cmdline(
              &sntp_declaration, cmdline, sizeof(cmdline), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(cmdline, "hfoc.network=dhcp hfoc.sntp=192.0.2.7") == 0,
          "a DHCP declaration carries the explicit SNTP source");

    sntp_declaration.sntp[0] = 'z';
    sntp_declaration.sntp[1] = '\0';
    CHECK(production_install_media_network_cmdline(
              &sntp_declaration, cmdline, sizeof(cmdline), &length) ==
              PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "a malformed medium SNTP directive is refused before write");
}

static void test_network_declaration_record(void)
{
    struct production_install_media_declaration declaration;
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    size_t length = 0U;

    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        network_declaration, strlen(network_declaration), &declaration);
    CHECK(production_install_media_render_declaration(
              &declaration, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strstr(record, "network=static") != NULL &&
              strstr(record, "address=10.0.2.15/24") != NULL &&
              strstr(record, "gateway=10.0.2.2") != NULL &&
              strstr(record, "dns=10.0.2.3") != NULL,
          "the declaration record names the declared network");

    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        media_declaration, strlen(media_declaration), &declaration);
    CHECK(production_install_media_render_declaration(
              &declaration, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strstr(record, "network=default") != NULL,
          "an undeclared network is recorded as the default");

    CHECK(strcmp(production_install_media_refusal_string(
                     PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_INCOMPLETE),
                 "NETWORK_INCOMPLETE") == 0 &&
              strcmp(production_install_media_refusal_string(
                         PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_MALFORMED),
                     "NETWORK_MALFORMED") == 0,
          "the network refusals have their own names");
}

/*
 * ITEM-449: the forward the operator declares on the medium, the one the
 * installed entry carries, and the refusals that keep an unhonourable
 * declaration from reaching the first write.
 */
static const char forward_declaration[] =
    "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"
    "HFOC-INSTALL-TARGET=DEADBEEF00000001\n"
    "HFOC-INSTALL-MANAGEMENT=management-interface\n"
    "HFOC-INSTALL-LAYOUT=whole-disk\n"
    "HFOC-INSTALL-FORWARD=on\n"
    "HFOC-INSTALL-FORWARD-TARGET=10.0.2.3\n"
    "HFOC-INSTALL-FORWARD-TARGET-PORT=8443\n"
    "HFOC-INSTALL-FORWARD-LISTEN-PORT=9443\n"
    "HFOC-INSTALL-FORWARD-CLIENTS=10.0.2.9,10.0.2.8\n";

static void test_forward_declaration_parse(void)
{
    struct production_install_media_declaration declaration;
    struct forward_config config;

    memset(&declaration, 0, sizeof(declaration));
    CHECK(production_install_media_declaration_parse(
              forward_declaration, strlen(forward_declaration),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.present && declaration.forward_declared &&
              declaration.forward_enabled &&
              strcmp(declaration.forward_target, "10.0.2.3") == 0 &&
              strcmp(declaration.forward_target_port, "8443") == 0 &&
              strcmp(declaration.forward_listen_port, "9443") == 0 &&
              strcmp(declaration.forward_clients,
                     "10.0.2.9,10.0.2.8") == 0,
          "an enabled forward declaration parses");

    memset(&config, 0, sizeof(config));
    CHECK(production_install_media_forward_config(
              &declaration, &config) == PRODUCTION_INSTALL_MEDIA_OK &&
              forward_config_enabled(&config) &&
              config.client_count == 2U,
          "the medium declaration becomes the validated forward");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-FORWARD=off\n",
              strlen("HFOC-INSTALL-FORWARD=off\n"), &declaration) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              declaration.forward_declared &&
              !declaration.forward_enabled,
          "an explicit off parses as a declaration, not as a malformed one");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-FORWARD=sometimes\n",
              strlen("HFOC-INSTALL-FORWARD=sometimes\n"), &declaration) ==
              PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "a forward mode that is not a mode is refused");

    CHECK(production_install_media_declaration_parse(
              "HFOC-INSTALL-FORWARD-TARGET=10.0.2.3\n",
              strlen("HFOC-INSTALL-FORWARD-TARGET=10.0.2.3\n"),
              &declaration) == PRODUCTION_INSTALL_MEDIA_OK &&
              !declaration.forward_declared,
          "a target without a mode parses but declares no forward");

    memset(&config, 0, sizeof(config));
    CHECK(production_install_media_forward_config(
              &declaration, &config) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "a target without a mode is refused when it is applied");
}

static void test_forward_plan_refusal(void)
{
    struct production_install_media_declaration declaration;
    struct installer_discovery_report report;
    struct production_install_media_plan plan;
    static const char incomplete[] =
        "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"
        "HFOC-INSTALL-TARGET=DEADBEEF00000001\n"
        "HFOC-INSTALL-MANAGEMENT=local-only\n"
        "HFOC-INSTALL-LAYOUT=whole-disk\n"
        "HFOC-INSTALL-FORWARD=on\n"
        "HFOC-INSTALL-FORWARD-TARGET=10.0.2.3\n"
        "HFOC-INSTALL-FORWARD-TARGET-PORT=8443\n"
        "HFOC-INSTALL-FORWARD-LISTEN-PORT=9443\n";
    static const char malformed[] =
        "HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED\n"
        "HFOC-INSTALL-TARGET=DEADBEEF00000001\n"
        "HFOC-INSTALL-MANAGEMENT=local-only\n"
        "HFOC-INSTALL-LAYOUT=whole-disk\n"
        "HFOC-INSTALL-FORWARD=on\n"
        "HFOC-INSTALL-FORWARD-TARGET=10.0.2.3\n"
        "HFOC-INSTALL-FORWARD-TARGET-PORT=8443\n"
        "HFOC-INSTALL-FORWARD-LISTEN-PORT=443\n"
        "HFOC-INSTALL-FORWARD-CLIENTS=10.0.2.9\n";

    test_report_fixture(&report, 1U, false);
    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        incomplete, strlen(incomplete), &declaration);
    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report, &plan) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED &&
              plan.refusal ==
                  PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_INCOMPLETE &&
              !plan.selected,
          "a forward without its client list refuses before any write");

    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        malformed, strlen(malformed), &declaration);
    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report, &plan) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED &&
              plan.refusal ==
                  PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_MALFORMED &&
              !plan.selected,
          "a forward that claims the serving port refuses before any write");
}

static void test_forward_boot_and_cmdline(void)
{
    struct production_install_media_declaration declaration;
    struct production_install_media_boot_forward boot_forward;
    char cmdline[PRODUCTION_INSTALL_MEDIA_FORWARD_CMDLINE_MAX];
    char installed[PRODUCTION_INSTALL_MEDIA_FORWARD_CMDLINE_MAX +
                   PRODUCTION_INSTALL_MEDIA_NETWORK_CMDLINE_MAX + 64U];
    size_t length = 0U;

    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        forward_declaration, strlen(forward_declaration), &declaration);

    memset(cmdline, 0, sizeof(cmdline));
    CHECK(production_install_media_forward_cmdline(
              &declaration, cmdline, sizeof(cmdline), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strcmp(cmdline,
                     "hfoc.forward=on hfoc.forward.target=10.0.2.3 "
                     "hfoc.forward.target-port=8443 "
                     "hfoc.forward.listen-port=9443 "
                     "hfoc.forward.clients=10.0.2.9,10.0.2.8") == 0 &&
              length == strlen(cmdline),
          "the forward declaration renders the installed directives");

    memset(installed, 0, sizeof(installed));
    CHECK(production_install_media_installed_cmdline(
              &declaration, installed, sizeof(installed), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strstr(installed, "hfoc.management=management-interface") !=
                  NULL &&
              strstr(installed, "hfoc.forward=on") != NULL &&
              strstr(installed, "hfoc.forward.clients=10.0.2.9,10.0.2.8") !=
                  NULL,
          "the installed command line carries the forward too");

    memset(&boot_forward, 0, sizeof(boot_forward));
    CHECK(production_install_media_forward_boot(
              installed, strlen(installed), &boot_forward) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              boot_forward.declared && boot_forward.enabled &&
              boot_forward.config_valid &&
              boot_forward.config.target_port == 8443U &&
              boot_forward.config.listen_port == 9443U &&
              boot_forward.config.client_count == 2U,
          "the installed declaration parses back into a validated forward");

    memset(&boot_forward, 0, sizeof(boot_forward));
    CHECK(production_install_media_forward_boot(
              "hfoc.management=local-only hfoc.forward=off",
              strlen("hfoc.management=local-only hfoc.forward=off"),
              &boot_forward) == PRODUCTION_INSTALL_MEDIA_OK &&
              boot_forward.declared && !boot_forward.enabled &&
              !boot_forward.config_valid,
          "an explicit off parses without a config");

    memset(&boot_forward, 0, sizeof(boot_forward));
    CHECK(production_install_media_forward_boot(
              "hfoc.management=local-only",
              strlen("hfoc.management=local-only"), &boot_forward) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              !boot_forward.declared,
          "a command line without a forward declares none");

    memset(&boot_forward, 0, sizeof(boot_forward));
    CHECK(production_install_media_forward_boot(
              "hfoc.forward=tunnel", strlen("hfoc.forward=tunnel"),
              &boot_forward) == PRODUCTION_INSTALL_MEDIA_BAD_TEXT,
          "an unknown forward mode on the command line is refused");

    memset(&boot_forward, 0, sizeof(boot_forward));
    CHECK(production_install_media_forward_boot(
              "hfoc.forward=on hfoc.forward.target=10.0.2.3 "
              "hfoc.forward.target-port=8443 "
              "hfoc.forward.listen-port=443 "
              "hfoc.forward.clients=10.0.2.9",
              strlen("hfoc.forward=on hfoc.forward.target=10.0.2.3 "
                     "hfoc.forward.target-port=8443 "
                     "hfoc.forward.listen-port=443 "
                     "hfoc.forward.clients=10.0.2.9"),
              &boot_forward) == PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT,
          "a boot declaration that claims the serving port is refused");

    memset(&boot_forward, 0, sizeof(boot_forward));
    CHECK(production_install_media_forward_boot(
              "hfoc.forward=on hfoc.forward.target=10.0.2.3 "
              "hfoc.forward.target-port=8443 "
              "hfoc.forward.listen-port=9443 "
              "hfoc.forward.clients=10.0.2.9",
              strlen("hfoc.forward=on hfoc.forward.target=10.0.2.3 "
                     "hfoc.forward.target-port=8443 "
                     "hfoc.forward.listen-port=9443 "
                     "hfoc.forward.clients=10.0.2.9"),
              &boot_forward) == PRODUCTION_INSTALL_MEDIA_OK &&
              boot_forward.config_valid &&
              forward_config_admit(
                  &boot_forward.config,
                  boot_forward.config.clients[0]) ==
                  FORWARD_CONFIG_ADMITTED,
          "the boot-forward owner admits the client it declared");
}

static void test_forward_declaration_record(void)
{
    struct production_install_media_declaration declaration;
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    size_t length = 0U;

    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        forward_declaration, strlen(forward_declaration), &declaration);
    memset(record, 0, sizeof(record));
    CHECK(production_install_media_render_declaration(
              &declaration, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strstr(record, " forward=on target=10.0.2.3") != NULL &&
              strstr(record, "target-port=8443") != NULL &&
              strstr(record, "listen-port=9443") != NULL &&
              strstr(record, "clients=10.0.2.9,10.0.2.8") != NULL,
          "the declaration record names the declared forward");

    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        media_declaration, strlen(media_declaration), &declaration);
    CHECK(production_install_media_render_declaration(
              &declaration, PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED,
              record, sizeof(record), &length) ==
              PRODUCTION_INSTALL_MEDIA_OK &&
              strstr(record, "forward=default") != NULL,
          "an undeclared forward is recorded as the default");

    CHECK(strcmp(production_install_media_refusal_string(
                     PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_INCOMPLETE),
                 "FORWARD_INCOMPLETE") == 0 &&
              strcmp(production_install_media_refusal_string(
                         PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_MALFORMED),
                     "FORWARD_MALFORMED") == 0,
          "the forward refusals have their own names");
}

/*
 * ITEM-490: the operator's choice on the storage screen is the disk the plan
 * selects, whatever the medium declared - and the rest of the declaration is
 * untouched.  The screen itself is proven on the machine (the QEMU proof); the
 * decision it feeds is proven here.
 */
static void test_operator_choice_sets_the_target(void)
{
    struct production_install_media_declaration declaration;
    struct production_install_media_plan plan;
    struct installer_discovery_report report;

    memset(&declaration, 0, sizeof(declaration));
    (void)production_install_media_declaration_parse(
        media_declaration, strlen(media_declaration), &declaration);
    test_report_fixture(&report, 2U, false);

    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report,
              &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
              plan.selected && plan.target.disk_id == 1U &&
              strcmp(plan.target.identity, "DEADBEEF00000001") == 0,
          "the declared disk is the target while nobody has chosen");

    CHECK(production_install_media_declaration_set_target(
              &declaration, "DEADBEEF00000002") &&
              declaration.target_declared &&
              strcmp(declaration.target, "DEADBEEF00000002") == 0,
          "the operator's disk replaces the declared target");
    CHECK(strcmp(declaration.confirm, "ITEM-394-UNATTENDED") == 0 &&
              declaration.management_interface,
          "the rest of the declaration is exactly as the medium wrote it");
    CHECK(production_install_media_plan_build(
              PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE, &declaration,
              INSTALLER_FIRMWARE_UEFI, &report,
              &plan) == PRODUCTION_INSTALL_MEDIA_OK &&
              plan.action == PRODUCTION_INSTALL_MEDIA_ACTION_CONSOLE &&
              plan.selected && plan.target.disk_id == 2U &&
              strcmp(plan.target.identity, "DEADBEEF00000002") == 0,
          "the plan selects the disk the operator chose, not the declared one");

    CHECK(!production_install_media_declaration_set_target(&declaration, "") &&
              !production_install_media_declaration_set_target(&declaration,
                                                               NULL) &&
              strcmp(declaration.target, "DEADBEEF00000002") == 0,
          "an empty or missing identity leaves the target alone");
    CHECK(!production_install_media_declaration_set_target(
              &declaration, "not a token") &&
              strcmp(declaration.target, "DEADBEEF00000002") == 0,
          "an identity the declaration grammar cannot carry is refused");
    CHECK(!production_install_media_declaration_set_target(
              NULL, "DEADBEEF00000001"),
          "a choice without a declaration sets nothing");

    CHECK(strcmp(production_install_media_refusal_string(
                     PRODUCTION_INSTALL_MEDIA_REFUSAL_CANCELLED_BY_OPERATOR),
                 "CANCELLED_BY_OPERATOR") == 0,
          "leaving the screen without choosing has its own refusal name");
}

int main(void)
{
    test_network_declaration_parse();
    test_network_config_validation();
    test_network_plan_refusal();
    test_network_boot_and_cmdline();
    test_network_declaration_record();
    test_forward_declaration_parse();
    test_forward_plan_refusal();
    test_forward_boot_and_cmdline();
    test_forward_declaration_record();
    test_mode_parse();
    test_declaration_parse();
    test_declaration_capacity();
    test_plan_build();
    test_confirmation_match();
    test_render();
    test_strings();
    test_operator_choice_sets_the_target();

    if (failures != 0) {
        fprintf(stderr,
                "Production install media host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Production install media host tests: ALL OK");
    return 0;
}

#else
#error "tests/production-install-media-host.c requires -DHFOC_PRODUCTION_INSTALL_MEDIA_HOST_TEST"
#endif
