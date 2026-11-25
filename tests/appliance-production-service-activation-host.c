#include "appliance_production_service_activation.h"

#include "appliance_production_service_https.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_HOST_TEST

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

static struct appliance_production_service_https_evidence make_evidence(void)
{
    struct appliance_production_service_https_evidence evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_EVIDENCE_VERSION;
    evidence.source_generation = 1U;
    evidence.published = true;
    evidence.loaded_state_committed = true;
    evidence.manager_configured = true;
    evidence.manager_stopped = true;
    evidence.https_enabled = true;
    evidence.https_stopped = true;
    return evidence;
}

struct fixture {
    struct appliance_production_service_https composition;
    struct appliance_production_service_activation activation;
};

static bool fixture_init(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    if (appliance_production_service_https_init(&fixture->composition) !=
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return false;
    }
    return appliance_production_service_activation_init(
               &fixture->activation, &fixture->composition) ==
           APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK;
}

static bool render_contains(
    const struct appliance_production_service_activation_snapshot *snapshot,
    const char *needle)
{
    char output[APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    if (appliance_production_service_activation_render(
            snapshot, output, sizeof(output), &output_length) !=
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK) {
        return false;
    }
    return strstr(output, needle) != NULL;
}

static void test_init_null_and_unconfigured(void)
{
    struct appliance_production_service_activation activation;
    struct appliance_production_service_activation_snapshot snapshot;
    struct appliance_production_service_https_evidence evidence =
        make_evidence();

    memset(&activation, 0, sizeof(activation));
    CHECK(
        appliance_production_service_activation_init(
            NULL, NULL) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NULL,
        "activation init rejects null arguments");
    CHECK(
        appliance_production_service_activation_configure(
            &activation, &evidence) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED,
        "activation configure rejects unpublished object");
    CHECK(
        appliance_production_service_activation_snapshot(
            &activation, &snapshot) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_UNPUBLISHED,
        "activation snapshot rejects unpublished object");
}

static void test_ordering_running_and_restart(void)
{
    struct fixture fixture;
    struct appliance_production_service_https_evidence evidence =
        make_evidence();
    struct appliance_production_service_activation_snapshot snapshot;
    char output[APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    CHECK(fixture_init(&fixture), "activation fixture initializes");

    CHECK(
        appliance_production_service_activation_evaluate(
            &fixture.activation, false, false, false, 0U) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_INVALID_EVIDENCE,
        "unconfigured activation awaits evidence");
    CHECK(
        appliance_production_service_activation_state(&fixture.activation) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_EVIDENCE,
        "unconfigured activation publishes awaiting evidence");

    CHECK(
        appliance_production_service_activation_configure(
            &fixture.activation, &evidence) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK,
        "activation accepts committed service evidence");
    CHECK(
        appliance_production_service_activation_evaluate(
            &fixture.activation, false, true, true, 0U) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_NETWORK,
        "activation blocks on missing network");
    CHECK(
        appliance_production_service_activation_state(&fixture.activation) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_NETWORK,
        "activation publishes awaiting network");

    CHECK(
        appliance_production_service_activation_evaluate(
            &fixture.activation, true, true, false, 0U) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_MISSING_READINESS,
        "activation blocks on missing certificate readiness");
    CHECK(
        appliance_production_service_activation_state(&fixture.activation) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_AWAITING_READINESS,
        "activation publishes awaiting readiness");
    CHECK(
        appliance_production_service_https_state(&fixture.composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_CONFIGURED,
        "missing readiness leaves the composition configured");

    CHECK(
        appliance_production_service_activation_evaluate(
            &fixture.activation, true, true, true, 0U) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK,
        "complete evidence activates the production composition");
    CHECK(
        appliance_production_service_activation_state(&fixture.activation) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_RUNNING,
        "activation reaches running");
    CHECK(
        appliance_production_service_https_state(&fixture.composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING,
        "composition reaches running");
    CHECK(
        !appliance_production_service_activation_secrets_logged(
            &fixture.activation),
        "activation never logs secrets");

    CHECK(
        appliance_production_service_activation_snapshot(
            &fixture.activation, &snapshot) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK,
        "activation snapshot publishes");
    CHECK(
        appliance_production_service_activation_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK,
        "activation render publishes");
    CHECK(
        strstr(output,
                "production_service_activation v=1 state=RUNNING "
                "https=ENABLED network=ACTIVE entropy=READY "
                "certificate=READY secrets=NONE start=1 restart=0 "
                "degraded=0 fail=0") != NULL,
        "activation render matches the canonical record");
    CHECK(
        strstr(output, "credential") == NULL &&
            strstr(output, "private") == NULL &&
            strstr(output, "BEGIN") == NULL,
        "activation render never contains secret material");

    CHECK(
        appliance_production_service_activation_restart(
            &fixture.activation) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK,
        "activation restarts the fixed service");
    CHECK(
        appliance_production_service_activation_running(&fixture.activation),
        "activation remains running after restart");
    CHECK(
        appliance_production_service_activation_snapshot(
            &fixture.activation, &snapshot) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK &&
            render_contains(&snapshot, "restart=1"),
        "activation snapshot counts the restart");
    CHECK(
        appliance_production_service_https_state(&fixture.composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING,
        "composition is running after restart");
}

static void test_network_loss_fails_closed(void)
{
    struct fixture fixture;
    struct appliance_production_service_https_evidence evidence =
        make_evidence();
    struct appliance_production_service_activation_snapshot snapshot;

    CHECK(fixture_init(&fixture), "activation fail-closed fixture init");
    CHECK(
        appliance_production_service_activation_configure(
            &fixture.activation, &evidence) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK,
        "activation fail-closed evidence configured");
    CHECK(
        appliance_production_service_activation_evaluate(
            &fixture.activation, true, true, true, 0U) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK,
        "activation fail-closed running");

    CHECK(
        appliance_production_service_activation_poll(
            &fixture.activation, false, 300U) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_NETWORK_LOST,
        "link loss degrades the activation");
    CHECK(
        appliance_production_service_activation_state(&fixture.activation) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_DEGRADED,
        "activation publishes degraded on link loss");
    CHECK(
        appliance_production_service_https_state(&fixture.composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED,
        "composition fails closed on link loss");
    CHECK(
        appliance_production_service_activation_snapshot(
            &fixture.activation, &snapshot) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK &&
            render_contains(&snapshot, "state=DEGRADED") &&
            render_contains(&snapshot, "network=INACTIVE") &&
            render_contains(&snapshot, "degraded=1"),
        "activation degraded snapshot is honest");
}

static void test_stop_path(void)
{
    struct fixture fixture;
    struct appliance_production_service_https_evidence evidence =
        make_evidence();

    CHECK(fixture_init(&fixture), "activation stop fixture init");
    CHECK(
        appliance_production_service_activation_configure(
            &fixture.activation, &evidence) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK &&
            appliance_production_service_activation_evaluate(
                &fixture.activation, true, true, true, 0U) ==
                APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK,
        "activation stop fixture running");
    CHECK(
        appliance_production_service_activation_stop(&fixture.activation) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK,
        "activation stops cleanly");
    CHECK(
        appliance_production_service_activation_state(&fixture.activation) ==
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_STOPPED,
        "activation publishes stopped");
    CHECK(
        appliance_production_service_https_state(&fixture.composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED,
        "composition returns to stopped");
}

int main(void)
{
    test_init_null_and_unconfigured();
    test_ordering_running_and_restart();
    test_network_loss_fails_closed();
    test_stop_path();
    if (failures != 0) {
        fprintf(stderr,
                "Appliance production service activation host tests: %d FAIL\n",
                failures);
        return 1;
    }
    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr,
                "Appliance production service activation host allocation\n");
        return 1;
    }
    printf("Appliance production service activation host tests: ALL OK\n");
    return 0;
}

#endif
