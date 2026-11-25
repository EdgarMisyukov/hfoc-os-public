#include "integration_boundary.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_BOUNDARY_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void test_contract_identity(void)
{
    CHECK(INTEGRATION_BOUNDARY_VERSION == 1U,
          "integration boundary version is exactly 1");
    CHECK(INTEGRATION_BOUNDARY_SCHEMA_ID_LENGTH ==
              strlen(INTEGRATION_BOUNDARY_SCHEMA_ID) &&
              memcmp(INTEGRATION_BOUNDARY_SCHEMA_ID,
                     "HFOC-ACCT-INT-1",
                     INTEGRATION_BOUNDARY_SCHEMA_ID_LENGTH) == 0,
          "integration boundary schema ID is exact");
    CHECK(INTEGRATION_BOUNDARY_SCHEMA_VERSION_MIN == 1U &&
              INTEGRATION_BOUNDARY_SCHEMA_VERSION_MAX == 1U,
          "integration boundary schema range is exactly version 1");
}

static void test_owner_split(void)
{
    CHECK(INTEGRATION_BOUNDARY_OWNER_APPLIANCE == 1U &&
              INTEGRATION_BOUNDARY_OWNER_ACCOUNTING == 2U &&
              INTEGRATION_BOUNDARY_OWNER_SHARED_BOUNDARY == 3U,
          "integration boundary owner IDs are exact and distinct");
}

static void test_handoff_and_transport(void)
{
    CHECK(INTEGRATION_BOUNDARY_AUDIT_HANDOFF_VERSION == 1U,
          "integration audit hand-off version is exactly 1");
    CHECK(INTEGRATION_BOUNDARY_TRANSPORT_VERSION == 1U,
          "integration transport version is exactly 1");
}

static void test_unsupported_operations(void)
{
    CHECK(INTEGRATION_BOUNDARY_UNSUPPORTED_NONE == 0U,
          "integration boundary unsupported none is zero");
    CHECK(INTEGRATION_BOUNDARY_UNSUPPORTED_ARBITRARY_SHELL == (1U << 0U) &&
              INTEGRATION_BOUNDARY_UNSUPPORTED_ACCOUNTING_LEDGER ==
                  (1U << 1U) &&
              INTEGRATION_BOUNDARY_UNSUPPORTED_ACCOUNTING_BALANCE ==
                  (1U << 2U) &&
              INTEGRATION_BOUNDARY_UNSUPPORTED_DIRECT_CONFIG_WRITE ==
                  (1U << 3U) &&
              INTEGRATION_BOUNDARY_UNSUPPORTED_DIRECT_AUDIT_MUTATION ==
                  (1U << 4U) &&
              INTEGRATION_BOUNDARY_UNSUPPORTED_LOCAL_ADMIN_OVERRIDE ==
                  (1U << 5U),
          "integration boundary unsupported operation bits are exact");
    CHECK((INTEGRATION_BOUNDARY_UNSUPPORTED_ARBITRARY_SHELL &
               INTEGRATION_BOUNDARY_UNSUPPORTED_ACCOUNTING_LEDGER) == 0U &&
              (INTEGRATION_BOUNDARY_UNSUPPORTED_ACCOUNTING_LEDGER &
                   INTEGRATION_BOUNDARY_UNSUPPORTED_ACCOUNTING_BALANCE) == 0U &&
              (INTEGRATION_BOUNDARY_UNSUPPORTED_DIRECT_CONFIG_WRITE &
                   INTEGRATION_BOUNDARY_UNSUPPORTED_DIRECT_AUDIT_MUTATION) ==
                  0U,
          "integration boundary unsupported operation bits do not overlap");
}

int main(void)
{
    test_contract_identity();
    test_owner_split();
    test_handoff_and_transport();
    test_unsupported_operations();

    if (failures != 0) {
        fprintf(stderr, "Integration boundary host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration boundary host tests: ALL OK\n");
    return 0;
}

#endif
