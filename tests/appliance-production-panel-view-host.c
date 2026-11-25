#include "appliance_production_panel_view.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(APPLIANCE_PRODUCTION_PANEL_VIEW_HOST_TEST)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const uint8_t fixture_mac[APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_LENGTH] = {
    0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U,
};

static void fill_owners(
    struct appliance_production_panel_view_owners *owners)
{
    memset(owners, 0, sizeof(*owners));
    owners->version = APPLIANCE_PRODUCTION_PANEL_VIEW_VERSION;
    owners->hostname = "hfoc-appliance";
    owners->mac_address = fixture_mac;
    owners->service_name = "https";
    owners->serving_state = "LISTENING";
    owners->serving_reason = "none";
    owners->recent_log =
        "[PRODUCTION-SERVING] state=LISTENING reason=none listener=live";
}

static void test_null_arguments(void)
{
    struct appliance_production_panel_view_storage storage;
    struct appliance_production_panel_view_owners owners;

    fill_owners(&owners);
    CHECK(appliance_production_panel_view_compose(NULL, &storage) ==
              APPLIANCE_PRODUCTION_PANEL_VIEW_NULL,
          "panel view refuses a NULL owner set");
    CHECK(appliance_production_panel_view_compose(&owners, NULL) ==
              APPLIANCE_PRODUCTION_PANEL_VIEW_NULL,
          "panel view refuses NULL storage");
    CHECK(appliance_production_panel_view_compose(&owners, &storage) ==
              APPLIANCE_PRODUCTION_PANEL_VIEW_OK,
          "panel view composes from a full owner set");
}

static void test_bad_version(void)
{
    struct appliance_production_panel_view_storage storage;
    struct appliance_production_panel_view_owners owners;

    fill_owners(&owners);
    owners.version = APPLIANCE_PRODUCTION_PANEL_VIEW_VERSION + 1U;
    CHECK(appliance_production_panel_view_compose(&owners, &storage) ==
              APPLIANCE_PRODUCTION_PANEL_VIEW_BAD_VERSION,
          "panel view refuses an unknown owner version");
    CHECK(storage.view.hostname == NULL &&
              storage.view.mac_address == NULL &&
              storage.view.service_name == NULL &&
              storage.view.detail == NULL &&
              storage.view.log_line == NULL,
          "a refused owner set publishes every field as unavailable");
}

static void test_full_view(void)
{
    struct appliance_production_panel_view_storage storage;
    struct appliance_production_panel_view_owners owners;

    fill_owners(&owners);
    CHECK(appliance_production_panel_view_compose(&owners, &storage) ==
              APPLIANCE_PRODUCTION_PANEL_VIEW_OK,
          "panel view composes");
    CHECK(storage.view.hostname == storage.hostname &&
              strcmp(storage.hostname, "hfoc-appliance") == 0,
          "the hostname is published from the owner");
    CHECK(storage.view.mac_address == storage.mac_address &&
              strcmp(storage.mac_address, "52:54:00:12:34:56") == 0,
          "the MAC is rendered in the panel's own form");
    CHECK(storage.view.service_name == storage.service_name &&
              strcmp(storage.service_name, "https") == 0,
          "the service name is published from the owner");
    CHECK(storage.view.detail == storage.detail &&
              strcmp(storage.detail, "state=LISTENING reason=none") == 0,
          "the detail names the serving state and its reason");
    CHECK(storage.view.log_line == storage.log_line &&
              strcmp(storage.log_line,
                     "[PRODUCTION-SERVING] state=LISTENING reason=none "
                     "listener=live") == 0,
          "the recent log line is published from the owner");
}

static void test_missing_owners_are_unavailable(void)
{
    struct appliance_production_panel_view_storage storage;
    struct appliance_production_panel_view_owners owners;

    memset(&owners, 0, sizeof(owners));
    owners.version = APPLIANCE_PRODUCTION_PANEL_VIEW_VERSION;
    CHECK(appliance_production_panel_view_compose(&owners, &storage) ==
              APPLIANCE_PRODUCTION_PANEL_VIEW_OK,
          "a panel view composes with no owners at all");
    CHECK(storage.view.hostname == NULL &&
              storage.view.mac_address == NULL &&
              storage.view.service_name == NULL &&
              storage.view.detail == NULL &&
              storage.view.log_line == NULL,
          "no owner publishes no field, and every field is unavailable");
}

static void test_empty_values_are_unavailable(void)
{
    struct appliance_production_panel_view_storage storage;
    struct appliance_production_panel_view_owners owners;

    fill_owners(&owners);
    owners.hostname = "";
    owners.service_name = "";
    owners.recent_log = "";
    owners.serving_reason = "";
    CHECK(appliance_production_panel_view_compose(&owners, &storage) ==
              APPLIANCE_PRODUCTION_PANEL_VIEW_OK,
          "empty owner values compose");
    CHECK(storage.view.hostname == NULL &&
              storage.view.service_name == NULL &&
              storage.view.log_line == NULL,
          "an empty owner value is unavailable, not an empty field");
    CHECK(storage.view.detail != NULL &&
              strcmp(storage.detail, "state=LISTENING") == 0,
          "a missing reason leaves the state alone in the detail");
}

static void test_oversized_values_are_refused(void)
{
    struct appliance_production_panel_view_storage storage;
    struct appliance_production_panel_view_owners owners;
    char oversized_hostname[APPLIANCE_PRODUCTION_PANEL_VIEW_HOSTNAME_MAX + 8U];
    char oversized_log[APPLIANCE_PRODUCTION_PANEL_VIEW_LOG_MAX + 8U];
    char oversized_detail[APPLIANCE_PRODUCTION_PANEL_VIEW_DETAIL_MAX + 8U];

    memset(oversized_hostname, 'h', sizeof(oversized_hostname) - 1U);
    oversized_hostname[sizeof(oversized_hostname) - 1U] = '\0';
    memset(oversized_log, 'l', sizeof(oversized_log) - 1U);
    oversized_log[sizeof(oversized_log) - 1U] = '\0';
    memset(oversized_detail, 'd', sizeof(oversized_detail) - 1U);
    oversized_detail[sizeof(oversized_detail) - 1U] = '\0';

    fill_owners(&owners);
    owners.hostname = oversized_hostname;
    owners.recent_log = oversized_log;
    owners.serving_state = oversized_detail;
    CHECK(appliance_production_panel_view_compose(&owners, &storage) ==
              APPLIANCE_PRODUCTION_PANEL_VIEW_OK,
          "oversized values still compose the view");
    CHECK(storage.view.hostname == NULL,
          "a hostname that does not fit is unavailable, not cut");
    CHECK(storage.view.log_line == NULL,
          "a log line that does not fit is unavailable, not cut");
    CHECK(storage.view.detail == NULL,
          "a detail line that does not fit is unavailable, not cut");
    CHECK(storage.view.mac_address != NULL &&
              storage.view.service_name != NULL,
          "the fields that fit are still published");
}

static void test_short_owner_values(void)
{
    struct appliance_production_panel_view_storage storage;
    struct appliance_production_panel_view_owners owners;

    fill_owners(&owners);
    owners.hostname = "n";
    owners.service_name = "s";
    owners.recent_log = "r";
    CHECK(appliance_production_panel_view_compose(&owners, &storage) ==
              APPLIANCE_PRODUCTION_PANEL_VIEW_OK,
          "short owner values compose");
    CHECK(strcmp(storage.hostname, "n") == 0 &&
              strcmp(storage.service_name, "s") == 0 &&
              strcmp(storage.log_line, "r") == 0,
          "a value that fits is published whole");
}

int main(void)
{
    test_null_arguments();
    test_bad_version();
    test_full_view();
    test_missing_owners_are_unavailable();
    test_empty_values_are_unavailable();
    test_oversized_values_are_refused();
    test_short_owner_values();

    if (failures != 0) {
        fprintf(stderr, "Appliance production panel view host test: %d "
                        "failure(s)\n", failures);
        return 1;
    }
    printf("Appliance production panel view host test: ALL OK\n");
    return 0;
}

#else

int main(void)
{
    return 0;
}

#endif
