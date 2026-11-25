#ifndef HFOC_APPLIANCE_CONFIG_VALIDATION_H
#define HFOC_APPLIANCE_CONFIG_VALIDATION_H

#include "appliance_language.h"
#include "config_journal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_CONFIG_VALIDATION_FORMAT_VERSION 1U
#define APPLIANCE_CONFIG_VALIDATION_FORMAT_PREFIX \
    "v1:system.hostname="
#define APPLIANCE_CONFIG_VALIDATION_KEY "system.hostname"
#define APPLIANCE_CONFIG_VALIDATION_DEFAULT_HOSTNAME "hfoc-appliance"
#define APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX 63U
/*
 * ITEM-509 increment 2: the language is the second key of the same
 * `v1:system.` record.  The record of the previous format is one hostname
 * line; the language line is appended only when the operator chose a language
 * other than the default `en`, so an appliance that never chose one writes
 * exactly the bytes the previous format wrote and an older build still reads
 * its hostname.
 */
#define APPLIANCE_CONFIG_VALIDATION_LANGUAGE_PREFIX \
    "v1:system.language="
#define APPLIANCE_CONFIG_VALIDATION_LANGUAGE_KEY "system.language"
#define APPLIANCE_CONFIG_VALIDATION_LANGUAGE_MAX 2U
/*
 * ITEM-516: the third key of the same record, the management gate's
 * after-boot policy.  `open` is written only by an operator who proved the
 * administrator secret on the local console; `closed` is the default and is
 * never written, so a record that never enabled the policy keeps exactly the
 * bytes the previous format wrote.  The boot reads the key: `open` publishes
 * the stored credential without a local login (source=POLICY), an absent or
 * `closed` key keeps the gate waiting for a local secret.  Enabling is a
 * durable, deliberate decision; disabling is always allowed.
 */
#define APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_PREFIX \
    "v1:security.panel.after-boot="
#define APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_KEY \
    "security.panel.after-boot"
#define APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_OPEN "open"
#define APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_CLOSED "closed"
#define APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_MAX 6U
#define APPLIANCE_CONFIG_VALIDATION_RECORD_MAX CONFIG_JOURNAL_DATA_MAX

enum appliance_config_validation_status {
    APPLIANCE_CONFIG_VALIDATION_OK = 0,
    APPLIANCE_CONFIG_VALIDATION_NULL = 1,
    APPLIANCE_CONFIG_VALIDATION_BAD_STATE = 2,
    APPLIANCE_CONFIG_VALIDATION_BAD_RECORD = 3,
    APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION = 4,
    APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY = 5,
    APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME = 6,
    APPLIANCE_CONFIG_VALIDATION_BUFFER_TOO_SMALL = 7,
    APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE = 8,
    /* ITEM-516: the after-boot key carries a value outside `open|closed`. */
    APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT = 9,
};

enum appliance_config_validation_source {
    APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE = 0,
    APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED = 1,
    APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING = 2,
    APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID = 3,
};

struct appliance_config_validation_state {
    enum appliance_config_validation_source source;
    char hostname[APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX + 1U];
    uint8_t hostname_length;
    enum appliance_language language;
    /*
     * ITEM-516: the after-boot policy of the same record.  A record that
     * predates the key publishes the default `closed` with `present` false, so
     * a reader can tell "the record says closed" from "the record does not
     * carry the key" - the two mean the same policy but only one is a
     * committed decision.
     */
    bool panel_after_boot_open;
    bool panel_after_boot_present;
    bool initialized;
};

enum appliance_config_validation_status
appliance_config_validation_validate_record(
    const void *record,
    size_t record_length,
    struct appliance_config_validation_state *state);
enum appliance_config_validation_status
appliance_config_validation_validate_hostname(
    const char *hostname,
    size_t hostname_length);
enum appliance_config_validation_status
appliance_config_validation_publish_hostname(
    struct appliance_config_validation_state *state,
    const char *hostname,
    size_t hostname_length,
    enum appliance_config_validation_source source);
enum appliance_config_validation_status
appliance_config_validation_publish_default(
    struct appliance_config_validation_state *state,
    enum appliance_config_validation_source source);
enum appliance_config_validation_status
appliance_config_validation_publish_language(
    struct appliance_config_validation_state *state,
    enum appliance_language language,
    enum appliance_config_validation_source source);
enum appliance_config_validation_source
appliance_config_validation_source(
    const struct appliance_config_validation_state *state);
enum appliance_config_validation_status
appliance_config_validation_hostname(
    const struct appliance_config_validation_state *state,
    char *hostname,
    size_t capacity,
    size_t *hostname_length);
/*
 * The effective interface language of a published state: the language named by
 * the record, or `en` when the record predates the language key.  A state that
 * was never published names no language.
 */
enum appliance_config_validation_status
appliance_config_validation_language(
    const struct appliance_config_validation_state *state,
    enum appliance_language *language);
/*
 * ITEM-516: the after-boot policy of the same record.  `publish_*` refuses a
 * source that is not COMMITTED, like the other keys; the getter answers the
 * effective policy - `closed` for a state whose record predates the key - and
 * `appliance_config_validation_panel_after_boot_value()` names it exactly as
 * the record writes it.
 */
enum appliance_config_validation_status
appliance_config_validation_validate_panel_after_boot(
    const char *value,
    size_t value_length,
    bool *open_out);
enum appliance_config_validation_status
appliance_config_validation_publish_panel_after_boot(
    struct appliance_config_validation_state *state,
    bool open,
    enum appliance_config_validation_source source);
enum appliance_config_validation_status
appliance_config_validation_panel_after_boot(
    const struct appliance_config_validation_state *state,
    bool *open_out);
const char *
appliance_config_validation_panel_after_boot_value(bool open);
const char *appliance_config_validation_status_string(
    enum appliance_config_validation_status status);

#endif
