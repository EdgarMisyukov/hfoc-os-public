#ifndef HFOC_APPLIANCE_DNS_RESOLVER_H
#define HFOC_APPLIANCE_DNS_RESOLVER_H

#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_DNS_RESOLVER_VERSION 1U
#define APPLIANCE_DNS_RESOLVER_MAX_ATTEMPTS 3U
#define APPLIANCE_DNS_RESOLVER_HOSTNAME_MAX 127U
#define APPLIANCE_DNS_RESOLVER_OUTPUT_MAX 256U

enum appliance_dns_resolver_state {
    APPLIANCE_DNS_RESOLVER_STATE_NONE = 0,
    APPLIANCE_DNS_RESOLVER_STATE_IDLE = 1,
    APPLIANCE_DNS_RESOLVER_STATE_READY = 2,
    APPLIANCE_DNS_RESOLVER_STATE_QUERYING = 3,
    APPLIANCE_DNS_RESOLVER_STATE_RESOLVED = 4,
    APPLIANCE_DNS_RESOLVER_STATE_FAILED = 5,
};

enum appliance_dns_resolver_status {
    APPLIANCE_DNS_RESOLVER_OK = 0,
    APPLIANCE_DNS_RESOLVER_NULL = 1,
    APPLIANCE_DNS_RESOLVER_BAD_STATE = 2,
    APPLIANCE_DNS_RESOLVER_BAD_VERSION = 3,
    APPLIANCE_DNS_RESOLVER_BAD_CONFIG = 4,
    APPLIANCE_DNS_RESOLVER_BAD_HOSTNAME = 5,
    APPLIANCE_DNS_RESOLVER_BAD_ANSWER = 6,
    APPLIANCE_DNS_RESOLVER_STOPPED = 7,
    APPLIANCE_DNS_RESOLVER_EXHAUSTED = 8,
    APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL = 9,
};

struct appliance_dns_resolver {
    uint32_t version;
    enum appliance_dns_resolver_state state;
    char interface[16U];
    uint8_t interface_length;
    struct network_ipv4_address server;
    char hostname[APPLIANCE_DNS_RESOLVER_HOSTNAME_MAX + 1U];
    size_t hostname_length;
    struct network_ipv4_address answer;
    bool answer_published;
    uint32_t attempt_count;
    uint32_t max_attempts;
    bool initialized;
};

enum appliance_dns_resolver_status
appliance_dns_resolver_init(
    struct appliance_dns_resolver *resolver,
    const struct network_ipv4_config *active_config,
    const char *interface,
    size_t interface_length);
enum appliance_dns_resolver_status
appliance_dns_resolver_query(
    struct appliance_dns_resolver *resolver,
    const char *hostname,
    size_t hostname_length);
enum appliance_dns_resolver_status
appliance_dns_resolver_response(
    struct appliance_dns_resolver *resolver,
    const struct network_ipv4_address *answer);
enum appliance_dns_resolver_status
appliance_dns_resolver_timeout(
    struct appliance_dns_resolver *resolver);
enum appliance_dns_resolver_status
appliance_dns_resolver_server_failure(
    struct appliance_dns_resolver *resolver);
enum appliance_dns_resolver_status
appliance_dns_resolver_clear(
    struct appliance_dns_resolver *resolver);
enum appliance_dns_resolver_status
appliance_dns_resolver_reset(
    struct appliance_dns_resolver *resolver);
enum appliance_dns_resolver_status
appliance_dns_resolver_render(
    const struct appliance_dns_resolver *resolver,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_dns_resolver_state
appliance_dns_resolver_state(
    const struct appliance_dns_resolver *resolver);
bool appliance_dns_resolver_answer_published(
    const struct appliance_dns_resolver *resolver);
const struct network_ipv4_address *
appliance_dns_resolver_answer(
    const struct appliance_dns_resolver *resolver);
const char *appliance_dns_resolver_state_string(
    enum appliance_dns_resolver_state state);
const char *appliance_dns_resolver_status_string(
    enum appliance_dns_resolver_status status);

#endif
