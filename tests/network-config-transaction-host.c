#include "network_config_transaction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETWORK_CONFIG_TRANSACTION_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const char valid_config[] =
    "schema_version=1\n"
    "ipv4_address=192.0.2.10\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=192.0.2.1\n"
    "ipv4_dns=192.0.2.53\n";

static const char replacement_config[] =
    "schema_version=1\n"
    "ipv4_address=192.0.2.11\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=192.0.2.1\n"
    "ipv4_dns=192.0.2.54\n";

static const char third_config[] =
    "schema_version=1\n"
    "ipv4_address=192.0.2.12\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=192.0.2.1\n"
    "ipv4_dns=192.0.2.53\n";

static const char malformed_config[] =
    "schema_version=1\n"
    "ipv4_address=192.0.2.10\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=192.0.2.1\n";

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static bool config_equal(const struct network_ipv4_config *left,
                         const struct network_ipv4_config *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static bool parse_config(const char *input,
                         struct network_ipv4_config *config)
{
    return network_ipv4_config_parse(
               input, strlen(input), config) == NETWORK_IPV4_OK;
}

static bool initialize_transaction(
    struct network_config_transaction *transaction)
{
    struct network_ipv4_config initial;

    memset(transaction, 0, sizeof(*transaction));
    return parse_config(valid_config, &initial) &&
           network_config_transaction_init(
               transaction, &initial) ==
               NETWORK_CONFIG_TRANSACTION_OK;
}

static void test_status_strings(void)
{
    static const struct {
        enum network_config_transaction_status status;
        const char *string;
    } statuses[] = {
        {NETWORK_CONFIG_TRANSACTION_OK,
         "NETWORK_CONFIG_TRANSACTION_OK"},
        {NETWORK_CONFIG_TRANSACTION_NULL,
         "NETWORK_CONFIG_TRANSACTION_NULL"},
        {NETWORK_CONFIG_TRANSACTION_BAD_STATE,
         "NETWORK_CONFIG_TRANSACTION_BAD_STATE"},
        {NETWORK_CONFIG_TRANSACTION_NO_PENDING,
         "NETWORK_CONFIG_TRANSACTION_NO_PENDING"},
        {NETWORK_CONFIG_TRANSACTION_PARSE_FAILED,
         "NETWORK_CONFIG_TRANSACTION_PARSE_FAILED"},
        {NETWORK_CONFIG_TRANSACTION_INVALID_CONFIG,
         "NETWORK_CONFIG_TRANSACTION_INVALID_CONFIG"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = network_config_transaction_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "network config transaction status string is exact");
    }
    CHECK(network_config_transaction_status_string(
              (enum network_config_transaction_status)6U) == NULL,
          "network config transaction undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network config transaction status handling performs no allocation");
}

static void test_validation_and_null_arguments(void)
{
    struct network_config_transaction transaction;
    struct network_ipv4_config initial;
    struct network_ipv4_config invalid;

    memset(&transaction, 0, sizeof(transaction));
    memset(&initial, 0, sizeof(initial));
    memset(&invalid, 0, sizeof(invalid));
    initial.schema_version = NETWORK_IPV4_CONFIG_VERSION;

    CHECK(network_ipv4_config_validate(NULL) == NETWORK_IPV4_NULL,
          "IPv4 config validation rejects null config");
    CHECK(network_ipv4_config_validate(&initial) ==
              NETWORK_IPV4_INVALID_ADDRESS,
          "IPv4 config validation rejects zero address");
    CHECK(parse_config(valid_config, &initial) &&
              network_ipv4_config_validate(&initial) == NETWORK_IPV4_OK,
          "IPv4 config validation accepts exact valid config");

    invalid = initial;
    invalid.schema_version = 2U;
    CHECK(network_ipv4_config_validate(&invalid) == NETWORK_IPV4_BAD_VERSION,
          "IPv4 config validation rejects bad version");
    invalid = initial;
    invalid.prefix_length = 33U;
    CHECK(network_ipv4_config_validate(&invalid) ==
              NETWORK_IPV4_INVALID_PREFIX,
          "IPv4 config validation rejects bad prefix");
    invalid = initial;
    memset(invalid.gateway.octets, 0, sizeof(invalid.gateway.octets));
    CHECK(network_ipv4_config_validate(&invalid) ==
              NETWORK_IPV4_INVALID_ROUTE,
          "IPv4 config validation rejects zero gateway");
    invalid = initial;
    memset(invalid.dns.octets, 0, sizeof(invalid.dns.octets));
    CHECK(network_ipv4_config_validate(&invalid) ==
              NETWORK_IPV4_INVALID_DNS,
          "IPv4 config validation rejects zero DNS");

    CHECK(network_config_transaction_init(
              NULL, &initial) == NETWORK_CONFIG_TRANSACTION_NULL,
          "transaction init rejects null transaction");
    CHECK(network_config_transaction_init(
              &transaction, NULL) == NETWORK_CONFIG_TRANSACTION_NULL,
          "transaction init rejects null initial config");
    CHECK(network_config_transaction_stage(
              NULL, valid_config, sizeof(valid_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_NULL,
          "transaction stage rejects null transaction");
    CHECK(network_config_transaction_stage(
              &transaction, NULL, 1U) == NETWORK_CONFIG_TRANSACTION_NULL,
          "transaction stage rejects null input");
    CHECK(network_config_transaction_commit(
              NULL) == NETWORK_CONFIG_TRANSACTION_NULL,
          "transaction commit rejects null transaction");
    CHECK(network_config_transaction_rollback(
              NULL) == NETWORK_CONFIG_TRANSACTION_NULL,
          "transaction rollback rejects null transaction");
    CHECK(network_config_transaction_active(NULL) == NULL,
          "transaction active rejects null transaction");
    CHECK(!network_config_transaction_has_pending(NULL),
          "transaction pending query rejects null transaction");

    memset(&transaction, 0, sizeof(transaction));
    CHECK(network_config_transaction_commit(
              &transaction) == NETWORK_CONFIG_TRANSACTION_BAD_STATE,
          "commit before init fails closed");
    CHECK(network_config_transaction_rollback(
              &transaction) == NETWORK_CONFIG_TRANSACTION_BAD_STATE,
          "rollback before init fails closed");
    CHECK(network_config_transaction_stage(
              &transaction, valid_config,
              sizeof(valid_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_BAD_STATE,
          "stage before init fails closed");
    CHECK(network_config_transaction_init(
              &transaction, &invalid) ==
              NETWORK_CONFIG_TRANSACTION_INVALID_CONFIG &&
              !network_config_transaction_has_pending(&transaction) &&
              network_config_transaction_active(&transaction) == NULL,
          "invalid initial config is rejected without publication");
    CHECK(initialize_transaction(&transaction),
          "valid initial config publishes active transaction");
    CHECK(network_config_transaction_init(
              &transaction, &initial) ==
              NETWORK_CONFIG_TRANSACTION_BAD_STATE,
          "transaction rejects reinitialization");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "transaction validation performs no allocation");
}

static void test_stage_failure_and_active_isolation(void)
{
    struct network_config_transaction transaction;
    struct network_ipv4_config active_before;
    struct network_ipv4_config replacement;
    char too_long[NETWORK_IPV4_INPUT_MAX + 1U];

    memset(too_long, 'a', sizeof(too_long));
    CHECK(initialize_transaction(&transaction),
          "stage-failure fixture initializes active transaction");
    active_before = *network_config_transaction_active(&transaction);
    CHECK(parse_config(replacement_config, &replacement),
          "stage-failure fixture parses replacement");

    CHECK(network_config_transaction_stage(
              &transaction, malformed_config,
              sizeof(malformed_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_PARSE_FAILED &&
              !network_config_transaction_has_pending(&transaction) &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &active_before),
          "malformed stage fails without pending or active mutation");
    CHECK(network_config_transaction_stage(
              &transaction, too_long, sizeof(too_long)) ==
              NETWORK_CONFIG_TRANSACTION_PARSE_FAILED &&
              !network_config_transaction_has_pending(&transaction) &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &active_before),
          "overlong stage fails without pending or active mutation");

    CHECK(network_config_transaction_stage(
              &transaction, replacement_config,
              sizeof(replacement_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_OK &&
              network_config_transaction_has_pending(&transaction) &&
              config_equal(&transaction.pending, &replacement) &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &active_before),
          "valid stage creates pending without active mutation");

    CHECK(network_config_transaction_stage(
              &transaction, malformed_config,
              sizeof(malformed_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_PARSE_FAILED &&
              !network_config_transaction_has_pending(&transaction) &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &active_before),
          "later malformed stage clears stale pending and preserves active");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "stage handling performs no allocation");
}

static void test_commit_and_rollback(void)
{
    struct network_config_transaction transaction;
    struct network_ipv4_config active_before;
    struct network_ipv4_config replacement;

    CHECK(initialize_transaction(&transaction),
          "commit fixture initializes active transaction");
    active_before = *network_config_transaction_active(&transaction);
    CHECK(parse_config(replacement_config, &replacement),
          "commit fixture parses replacement");

    CHECK(network_config_transaction_commit(
              &transaction) == NETWORK_CONFIG_TRANSACTION_NO_PENDING &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &active_before),
          "commit without pending fails without active mutation");

    CHECK(network_config_transaction_stage(
              &transaction, replacement_config,
              sizeof(replacement_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_OK &&
              network_config_transaction_rollback(
                  &transaction) == NETWORK_CONFIG_TRANSACTION_OK &&
              !network_config_transaction_has_pending(&transaction) &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &active_before),
          "rollback clears pending and preserves active");

    CHECK(network_config_transaction_stage(
              &transaction, replacement_config,
              sizeof(replacement_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_OK &&
              network_config_transaction_commit(
                  &transaction) == NETWORK_CONFIG_TRANSACTION_OK &&
              !network_config_transaction_has_pending(&transaction) &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &replacement),
          "successful commit publishes exact pending config");
    CHECK(network_config_transaction_commit(
              &transaction) == NETWORK_CONFIG_TRANSACTION_NO_PENDING &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &replacement),
          "second commit without pending leaves active unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "commit and rollback perform no allocation");
}

static void test_invalid_pending_commit(void)
{
    struct network_config_transaction transaction;
    struct network_ipv4_config active_before;

    CHECK(initialize_transaction(&transaction),
          "invalid-commit fixture initializes active transaction");
    active_before = *network_config_transaction_active(&transaction);
    CHECK(network_config_transaction_stage(
              &transaction, replacement_config,
              sizeof(replacement_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_OK,
          "invalid-commit fixture stages candidate");
    transaction.pending.prefix_length = 33U;
    CHECK(network_config_transaction_commit(
              &transaction) == NETWORK_CONFIG_TRANSACTION_INVALID_CONFIG &&
              !network_config_transaction_has_pending(&transaction) &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &active_before),
          "invalid pending commit clears pending and preserves active");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid commit performs no allocation");
}

static void test_repeated_transactions(void)
{
    struct network_config_transaction transaction;
    struct network_ipv4_config replacement;
    struct network_ipv4_config third;

    CHECK(initialize_transaction(&transaction),
          "repeated-transaction fixture initializes active transaction");
    CHECK(parse_config(replacement_config, &replacement) &&
              parse_config(third_config, &third),
          "repeated-transaction fixture parses candidates");
    CHECK(network_config_transaction_stage(
              &transaction, replacement_config,
              sizeof(replacement_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_OK &&
              network_config_transaction_commit(
                  &transaction) == NETWORK_CONFIG_TRANSACTION_OK &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &replacement),
          "transaction remains usable after first commit");
    CHECK(network_config_transaction_stage(
              &transaction, third_config,
              sizeof(third_config) - 1U) ==
              NETWORK_CONFIG_TRANSACTION_OK &&
              network_config_transaction_commit(
                  &transaction) == NETWORK_CONFIG_TRANSACTION_OK &&
              config_equal(
                  network_config_transaction_active(&transaction),
                  &third),
          "repeated stage and commit publish the newest valid candidate");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "repeated transactions perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_validation_and_null_arguments();
    test_stage_failure_and_active_isolation();
    test_commit_and_rollback();
    test_invalid_pending_commit();
    test_repeated_transactions();

    if (failures != 0) {
        fprintf(stderr,
                "Network configuration transaction host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Network configuration transaction host tests: ALL OK");
    return 0;
}

#else
#error "This host test must be built with HFOC_NETWORK_CONFIG_TRANSACTION_HOST_TEST"
#endif
