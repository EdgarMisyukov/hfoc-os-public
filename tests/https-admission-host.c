#include "https_admission.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_HTTPS_ADMISSION_HOST_TEST

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

static struct https_admission_policy make_policy(void)
{
    struct https_admission_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = HTTPS_ADMISSION_VERSION;
    policy.address[0] = HTTPS_ADMISSION_QEMU_ADDRESS_0;
    policy.address[1] = HTTPS_ADMISSION_QEMU_ADDRESS_1;
    policy.address[2] = HTTPS_ADMISSION_QEMU_ADDRESS_2;
    policy.address[3] = HTTPS_ADMISSION_QEMU_ADDRESS_3;
    policy.port = HTTPS_ADMISSION_QEMU_PORT;
    return policy;
}

static struct https_admission_generations make_generations(
    uint32_t network,
    uint32_t tls,
    uint32_t service)
{
    struct https_admission_generations generations;

    memset(&generations, 0, sizeof(generations));
    generations.version = HTTPS_ADMISSION_VERSION;
    generations.network_generation = network;
    generations.tls_policy_generation = tls;
    generations.service_generation = service;
    return generations;
}

static struct https_admission_generations valid_generations(void)
{
    return make_generations(1U, 2U, 3U);
}

static bool transaction_unchanged(
    const struct https_admission_transaction *transaction,
    const struct https_admission_transaction *original)
{
    return memcmp(transaction, original, sizeof(*transaction)) == 0;
}

static bool transaction_published_with(
    const struct https_admission_transaction *transaction,
    const struct https_admission_policy *policy,
    const struct https_admission_generations *generations)
{
    const struct https_admission_binding *active =
        https_admission_transaction_active(transaction);

    return active != NULL &&
           https_admission_transaction_published(transaction) &&
           active->published &&
           memcmp(&active->policy, policy, sizeof(*policy)) == 0 &&
           memcmp(&active->generations, generations,
                  sizeof(*generations)) == 0;
}

static void test_status_strings(void)
{
    static const struct {
        enum https_admission_status status;
        const char *string;
    } statuses[] = {
        {HTTPS_ADMISSION_OK, "HTTPS_ADMISSION_OK"},
        {HTTPS_ADMISSION_NULL, "HTTPS_ADMISSION_NULL"},
        {HTTPS_ADMISSION_BAD_STATE, "HTTPS_ADMISSION_BAD_STATE"},
        {HTTPS_ADMISSION_BAD_VERSION, "HTTPS_ADMISSION_BAD_VERSION"},
        {HTTPS_ADMISSION_BAD_ADDRESS, "HTTPS_ADMISSION_BAD_ADDRESS"},
        {HTTPS_ADMISSION_BAD_PORT, "HTTPS_ADMISSION_BAD_PORT"},
        {HTTPS_ADMISSION_BAD_GENERATION,
         "HTTPS_ADMISSION_BAD_GENERATION"},
        {HTTPS_ADMISSION_ALREADY_PUBLISHED,
         "HTTPS_ADMISSION_ALREADY_PUBLISHED"},
        {HTTPS_ADMISSION_UNPUBLISHED,
         "HTTPS_ADMISSION_UNPUBLISHED"},
        {HTTPS_ADMISSION_NO_PENDING, "HTTPS_ADMISSION_NO_PENDING"},
        {HTTPS_ADMISSION_CHANGED, "HTTPS_ADMISSION_CHANGED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = https_admission_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "HTTPS admission status string is exact");
    }
    CHECK(https_admission_status_string(
              (enum https_admission_status)11U) == NULL,
          "HTTPS admission undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS admission status handling performs no allocation");
}

static void test_policy_validation(void)
{
    struct https_admission_policy policy = make_policy();
    struct https_admission_policy bad;
    uint32_t index;

    CHECK(https_admission_policy_validate(NULL) ==
              HTTPS_ADMISSION_NULL,
          "HTTPS admission policy rejects null policy");
    CHECK(https_admission_policy_validate(&policy) ==
              HTTPS_ADMISSION_OK,
          "HTTPS admission policy accepts exact QEMU profile");

    bad = policy;
    bad.version = HTTPS_ADMISSION_VERSION + 1U;
    CHECK(https_admission_policy_validate(&bad) ==
              HTTPS_ADMISSION_BAD_VERSION,
          "HTTPS admission policy rejects unknown version");

    for (index = 0U;
         index < HTTPS_ADMISSION_QEMU_ADDRESS_LENGTH;
         ++index) {
        bad = policy;
        bad.address[index] =
            (uint8_t)(bad.address[index] + 1U);
        CHECK(https_admission_policy_validate(&bad) ==
                  HTTPS_ADMISSION_BAD_ADDRESS,
              "HTTPS admission policy rejects changed address byte");
    }

    bad = policy;
    bad.port = HTTPS_ADMISSION_QEMU_PORT + 1U;
    CHECK(https_admission_policy_validate(&bad) ==
              HTTPS_ADMISSION_BAD_PORT,
          "HTTPS admission policy rejects changed port");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS admission policy validation performs no allocation");
}

static void test_generation_validation(void)
{
    struct https_admission_generations generations =
        valid_generations();
    struct https_admission_generations bad;

    CHECK(https_admission_generations_validate(NULL) ==
              HTTPS_ADMISSION_NULL,
          "HTTPS admission generations reject null set");
    CHECK(https_admission_generations_validate(&generations) ==
              HTTPS_ADMISSION_OK,
          "HTTPS admission generations accept valid bounded set");

    bad = generations;
    bad.version = HTTPS_ADMISSION_VERSION + 1U;
    CHECK(https_admission_generations_validate(&bad) ==
              HTTPS_ADMISSION_BAD_VERSION,
          "HTTPS admission generations reject unknown version");

    bad = generations;
    bad.network_generation = HTTPS_ADMISSION_GENERATION_MIN - 1U;
    CHECK(https_admission_generations_validate(&bad) ==
              HTTPS_ADMISSION_BAD_GENERATION,
          "HTTPS admission rejects zero network generation");

    bad = generations;
    bad.tls_policy_generation = HTTPS_ADMISSION_GENERATION_MIN - 1U;
    CHECK(https_admission_generations_validate(&bad) ==
              HTTPS_ADMISSION_BAD_GENERATION,
          "HTTPS admission rejects zero TLS policy generation");

    bad = generations;
    bad.service_generation = HTTPS_ADMISSION_GENERATION_MIN - 1U;
    CHECK(https_admission_generations_validate(&bad) ==
              HTTPS_ADMISSION_BAD_GENERATION,
          "HTTPS admission rejects zero service generation");

    bad = generations;
    bad.network_generation = HTTPS_ADMISSION_GENERATION_MAX + 1U;
    CHECK(https_admission_generations_validate(&bad) ==
              HTTPS_ADMISSION_BAD_GENERATION,
          "HTTPS admission rejects over-limit network generation");

    bad = generations;
    bad.tls_policy_generation =
        HTTPS_ADMISSION_GENERATION_MAX + 1U;
    CHECK(https_admission_generations_validate(&bad) ==
              HTTPS_ADMISSION_BAD_GENERATION,
          "HTTPS admission rejects over-limit TLS policy generation");

    bad = generations;
    bad.service_generation = HTTPS_ADMISSION_GENERATION_MAX + 1U;
    CHECK(https_admission_generations_validate(&bad) ==
              HTTPS_ADMISSION_BAD_GENERATION,
          "HTTPS admission rejects over-limit service generation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS admission generation validation performs no allocation");
}

static void test_binding_published(void)
{
    struct https_admission_binding binding;
    struct https_admission_policy policy = make_policy();
    struct https_admission_generations generations =
        valid_generations();

    memset(&binding, 0, sizeof(binding));
    CHECK(!https_admission_binding_published(NULL),
          "HTTPS admission binding rejects null binding");
    CHECK(!https_admission_binding_published(&binding),
          "HTTPS admission binding rejects unpublished zero state");

    binding.published = true;
    binding.version = HTTPS_ADMISSION_VERSION;
    binding.policy = policy;
    binding.generations = generations;
    CHECK(https_admission_binding_published(&binding),
          "HTTPS admission binding accepts exact published state");

    binding.version = HTTPS_ADMISSION_VERSION + 1U;
    CHECK(!https_admission_binding_published(&binding),
          "HTTPS admission binding rejects unknown version");
    binding.version = HTTPS_ADMISSION_VERSION;
    binding.policy.port = HTTPS_ADMISSION_QEMU_PORT + 1U;
    CHECK(!https_admission_binding_published(&binding),
          "HTTPS admission binding rejects invalid policy");
    binding.policy = policy;
    binding.generations.network_generation = 0U;
    CHECK(!https_admission_binding_published(&binding),
          "HTTPS admission binding rejects invalid generation set");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS admission binding checks perform no allocation");
}

static void check_init_failure(
    struct https_admission_transaction *transaction,
    const struct https_admission_policy *policy,
    const struct https_admission_generations *generations,
    enum https_admission_status expected,
    const char *message)
{
    struct https_admission_transaction original = *transaction;

    CHECK(https_admission_transaction_init(
              transaction, policy, generations) == expected &&
              transaction_unchanged(transaction, &original) &&
              !https_admission_transaction_published(transaction),
          message);
}

static void test_transaction_init(void)
{
    struct https_admission_transaction transaction;
    struct https_admission_transaction original;
    struct https_admission_policy policy = make_policy();
    struct https_admission_policy bad_policy = policy;
    struct https_admission_generations generations =
        valid_generations();
    struct https_admission_generations bad_generations =
        generations;

    CHECK(https_admission_transaction_init(
              NULL, &policy, &generations) == HTTPS_ADMISSION_NULL,
          "HTTPS admission init rejects null transaction");
    CHECK(https_admission_transaction_init(
              &transaction, NULL, &generations) == HTTPS_ADMISSION_NULL,
          "HTTPS admission init rejects null policy");
    CHECK(https_admission_transaction_init(
              &transaction, &policy, NULL) == HTTPS_ADMISSION_NULL,
          "HTTPS admission init rejects null generations");

    memset(&transaction, 0, sizeof(transaction));
    transaction.version = 0xA5A5A5A5U;
    original = transaction;
    CHECK(https_admission_transaction_init(
              &transaction, &policy, &generations) ==
              HTTPS_ADMISSION_BAD_STATE &&
              transaction_unchanged(&transaction, &original),
          "HTTPS admission init rejects nonzero version");

    memset(&transaction, 0, sizeof(transaction));
    bad_policy.address[0] += 1U;
    check_init_failure(
        &transaction, &bad_policy, &generations,
        HTTPS_ADMISSION_BAD_ADDRESS,
        "invalid policy init leaves transaction unchanged");
    bad_policy = policy;
    bad_policy.port = HTTPS_ADMISSION_QEMU_PORT + 1U;
    check_init_failure(
        &transaction, &bad_policy, &generations,
        HTTPS_ADMISSION_BAD_PORT,
        "invalid port init leaves transaction unchanged");

    memset(&transaction, 0, sizeof(transaction));
    bad_generations.network_generation = 0U;
    check_init_failure(
        &transaction, &policy, &bad_generations,
        HTTPS_ADMISSION_BAD_GENERATION,
        "invalid network generation init leaves transaction unchanged");
    bad_generations = generations;
    bad_generations.tls_policy_generation =
        HTTPS_ADMISSION_GENERATION_MAX + 1U;
    check_init_failure(
        &transaction, &policy, &bad_generations,
        HTTPS_ADMISSION_BAD_GENERATION,
        "invalid TLS policy generation init leaves transaction unchanged");
    bad_generations = generations;
    bad_generations.service_generation = 0U;
    check_init_failure(
        &transaction, &policy, &bad_generations,
        HTTPS_ADMISSION_BAD_GENERATION,
        "invalid service generation init leaves transaction unchanged");

    memset(&transaction, 0, sizeof(transaction));
    CHECK(https_admission_transaction_init(
              &transaction, &policy, &generations) ==
              HTTPS_ADMISSION_OK &&
              transaction_published_with(
                  &transaction, &policy, &generations) &&
              !https_admission_transaction_has_pending(&transaction),
          "valid init atomically publishes exact binding");
    CHECK(https_admission_transaction_init(
              &transaction, &policy, &generations) ==
              HTTPS_ADMISSION_ALREADY_PUBLISHED,
          "HTTPS admission init rejects already published transaction");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS admission init performs no allocation");
}

static void check_stage_failure(
    struct https_admission_transaction *transaction,
    const struct https_admission_policy *policy,
    const struct https_admission_generations *generations,
    enum https_admission_status expected,
    const char *message)
{
    struct https_admission_transaction original = *transaction;

    CHECK(https_admission_transaction_stage(
              transaction, policy, generations) == expected &&
              !https_admission_transaction_has_pending(transaction) &&
              transaction_unchanged(transaction, &original),
          message);
}

static void test_stage_and_rollback(void)
{
    struct https_admission_transaction transaction;
    struct https_admission_transaction active_before;
    struct https_admission_policy policy = make_policy();
    struct https_admission_generations generations =
        valid_generations();
    struct https_admission_policy bad_policy = policy;
    struct https_admission_generations bad_generations =
        generations;
    struct https_admission_generations replacement =
        make_generations(4U, 5U, 6U);

    memset(&transaction, 0, sizeof(transaction));
    CHECK(https_admission_transaction_stage(
              &transaction, &policy, &generations) ==
              HTTPS_ADMISSION_UNPUBLISHED,
          "HTTPS admission stage rejects unpublished transaction");
    CHECK(https_admission_transaction_stage(
              NULL, &policy, &generations) == HTTPS_ADMISSION_NULL,
          "HTTPS admission stage rejects null transaction");
    CHECK(https_admission_transaction_init(
              &transaction, &policy, &generations) ==
              HTTPS_ADMISSION_OK,
          "stage fixture publishes active transaction");
    active_before = transaction;

    bad_policy.address[2] += 1U;
    check_stage_failure(
        &transaction, &bad_policy, &generations,
        HTTPS_ADMISSION_BAD_ADDRESS,
        "invalid policy stage leaves no pending and preserves active");
    bad_policy = policy;
    bad_generations.network_generation = 0U;
    check_stage_failure(
        &transaction, &policy, &bad_generations,
        HTTPS_ADMISSION_BAD_GENERATION,
        "invalid network generation stage leaves no pending");
    bad_generations = generations;
    bad_generations.tls_policy_generation = 0U;
    check_stage_failure(
        &transaction, &policy, &bad_generations,
        HTTPS_ADMISSION_BAD_GENERATION,
        "invalid TLS policy generation stage leaves no pending");
    bad_generations = generations;
    bad_generations.service_generation = 0U;
    check_stage_failure(
        &transaction, &policy, &bad_generations,
        HTTPS_ADMISSION_BAD_GENERATION,
        "invalid service generation stage leaves no pending");

    CHECK(https_admission_transaction_stage(
              &transaction, &policy, &replacement) ==
              HTTPS_ADMISSION_OK &&
              https_admission_transaction_has_pending(&transaction) &&
              transaction_unchanged(
                  &transaction, &active_before) == false &&
              memcmp(&transaction.active, &active_before.active,
                     sizeof(transaction.active)) == 0,
          "valid stage stores pending without active mutation");
    CHECK(https_admission_transaction_rollback(
              &transaction) == HTTPS_ADMISSION_OK &&
              !https_admission_transaction_has_pending(&transaction) &&
              memcmp(&transaction.active, &active_before.active,
                     sizeof(transaction.active)) == 0,
          "rollback clears pending and preserves active");
    CHECK(https_admission_transaction_rollback(
              &transaction) == HTTPS_ADMISSION_NO_PENDING,
          "rollback without pending fails closed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS admission stage and rollback perform no allocation");
}

static void test_commit_atomicity(void)
{
    struct https_admission_transaction transaction;
    struct https_admission_transaction active_before;
    struct https_admission_policy policy = make_policy();
    struct https_admission_generations generations =
        valid_generations();
    struct https_admission_generations replacement =
        make_generations(4U, 5U, 6U);

    memset(&transaction, 0, sizeof(transaction));
    CHECK(https_admission_transaction_commit(
              &transaction) == HTTPS_ADMISSION_UNPUBLISHED,
          "HTTPS admission commit rejects unpublished transaction");
    CHECK(https_admission_transaction_commit(
              NULL) == HTTPS_ADMISSION_NULL,
          "HTTPS admission commit rejects null transaction");
    CHECK(https_admission_transaction_init(
              &transaction, &policy, &generations) ==
              HTTPS_ADMISSION_OK,
          "commit fixture publishes active transaction");
    active_before = transaction;
    CHECK(https_admission_transaction_commit(
              &transaction) == HTTPS_ADMISSION_NO_PENDING &&
              memcmp(&transaction.active, &active_before.active,
                     sizeof(transaction.active)) == 0,
          "commit without pending preserves active");

    CHECK(https_admission_transaction_stage(
              &transaction, &policy, &replacement) ==
              HTTPS_ADMISSION_OK,
          "commit fixture stages replacement");
    transaction.pending.generations.network_generation = 0U;
    CHECK(https_admission_transaction_commit(
              &transaction) == HTTPS_ADMISSION_BAD_GENERATION &&
              !https_admission_transaction_has_pending(&transaction) &&
              memcmp(&transaction.active, &active_before.active,
                     sizeof(transaction.active)) == 0,
          "tampered pending commit clears pending and preserves active");

    CHECK(https_admission_transaction_stage(
              &transaction, &policy, &replacement) ==
              HTTPS_ADMISSION_OK &&
              https_admission_transaction_commit(
                  &transaction) == HTTPS_ADMISSION_OK &&
              !https_admission_transaction_has_pending(&transaction) &&
              transaction_published_with(
                  &transaction, &policy, &replacement),
          "valid commit atomically publishes exact replacement");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS admission commit performs no allocation");
}

static void test_validate_state(void)
{
    struct https_admission_transaction transaction;
    struct https_admission_policy policy = make_policy();
    struct https_admission_policy observed_policy = policy;
    struct https_admission_generations generations =
        valid_generations();
    struct https_admission_generations observed_generations =
        generations;

    memset(&transaction, 0, sizeof(transaction));
    CHECK(https_admission_transaction_validate_state(
              NULL, &policy, &generations) == HTTPS_ADMISSION_NULL,
          "HTTPS admission validate state rejects null transaction");
    CHECK(https_admission_transaction_validate_state(
              &transaction, NULL, &generations) == HTTPS_ADMISSION_NULL,
          "HTTPS admission validate state rejects null observed policy");
    CHECK(https_admission_transaction_validate_state(
              &transaction, &policy, NULL) == HTTPS_ADMISSION_NULL,
          "HTTPS admission validate state rejects null observed generations");
    CHECK(https_admission_transaction_validate_state(
              &transaction, &policy, &generations) ==
              HTTPS_ADMISSION_UNPUBLISHED,
          "HTTPS admission validate state rejects unpublished transaction");

    CHECK(https_admission_transaction_init(
              &transaction, &policy, &generations) ==
              HTTPS_ADMISSION_OK &&
              https_admission_transaction_validate_state(
                  &transaction, &policy, &generations) ==
                  HTTPS_ADMISSION_OK,
          "HTTPS admission validate state accepts exact observed state");

    observed_policy.address[0] += 1U;
    CHECK(https_admission_transaction_validate_state(
              &transaction, &observed_policy, &generations) ==
              HTTPS_ADMISSION_CHANGED,
          "changed observed policy is rejected");
    observed_policy = policy;
    observed_generations.network_generation += 1U;
    CHECK(https_admission_transaction_validate_state(
              &transaction, &policy, &observed_generations) ==
              HTTPS_ADMISSION_CHANGED,
          "changed network generation is rejected");
    observed_generations = generations;
    observed_generations.tls_policy_generation += 1U;
    CHECK(https_admission_transaction_validate_state(
              &transaction, &policy, &observed_generations) ==
              HTTPS_ADMISSION_CHANGED,
          "changed TLS policy generation is rejected");
    observed_generations = generations;
    observed_generations.service_generation += 1U;
    CHECK(https_admission_transaction_validate_state(
              &transaction, &policy, &observed_generations) ==
              HTTPS_ADMISSION_CHANGED,
          "changed service generation is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS admission state validation performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_policy_validation();
    test_generation_validation();
    test_binding_published();
    test_transaction_init();
    test_stage_and_rollback();
    test_commit_atomicity();
    test_validate_state();

    if (failures != 0) {
        fprintf(stderr, "HTTPS admission host test: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("HTTPS admission host test: ALL OK\n");
    return 0;
}

#else

int main(void)
{
    return 0;
}

#endif
