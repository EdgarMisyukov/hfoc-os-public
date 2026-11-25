#!/usr/bin/env bash
# ITEM-465/ITEM-506: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The forgotten password can be replaced from the local console.
#
# The owner's 2026-09-20 console photograph: `admin-provision <SECRET>`
# answered DENIED while the status line showed a real record, and there was no
# reset path, so one forgotten secret locked the owner out of the panel for
# good.  ITEM-465 added the two-line gesture; ITEM-506 moved it under the one
# account command family and removed the old names.  The claims pinned here:
#
#   * the console accepts `CREDENTIALS RESET` then `CREDENTIALS RESET CONFIRM`;
#     the first line only arms, the second performs, a confirmation with
#     nothing armed is refused, and any other submitted line disarms it;
#   * the classifier is pure (`kernel/include/console_credentials_command.h`)
#     and the console calls it once per submitted line, so the rule is proven
#     on the host and the text that boots is not a second implementation;
#   * the reset rewrites the persisted `secret.admin.credential` record to the
#     factory placeholder, so `CREDENTIALS` sets the account again, and resets
#     the production request gate, which closes every issued session;
#   * the reset exists only on the local console: no management-plane source
#     names it, so it cannot be reached over the network;
#   * the host proofs cover the classifier, the gesture, the re-provisioning
#     after it, the session it closes and the refusal without confirmation;
#     the documents name the gesture as the way back.
for console_reset_file in \
    kernel/include/console_credentials_command.h \
    kernel/include/appliance_production_admin_enrollment.h \
    kernel/src/appliance_production_admin_enrollment.c \
    kernel/src/console.c \
    tests/console-host.c \
    tests/appliance-production-admin-enrollment-host.c \
    docs/hfoc-production-admin-enrollment.md \
    docs/hfoc-management-web-ui.md; do
    [[ -f $console_reset_file ]] || {
        echo "ITEM-506: отсутствует $console_reset_file" >&2
        exit 1
    }
done

# The gesture, and the pure rule that classifies it.
for console_reset_word in \
    '#define CONSOLE_CREDENTIALS_COMMAND_RESET_REQUEST_WORD "credentials reset"' \
    '#define CONSOLE_CREDENTIALS_COMMAND_RESET_CONFIRM_WORD "credentials reset confirm"' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_ARM' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_EXECUTE' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_UNCONFIRMED' \
    'console_credentials_command_classify(' \
    '*reset_pending = true;' \
    '*reset_pending = false;'; do
    grep -Fq -- "$console_reset_word" \
        kernel/include/console_credentials_command.h || {
        echo "ITEM-506: классификатор сброса не содержит:" \
            "$console_reset_word" >&2
        exit 1
    }
done

# The console classifies every line before the command chain, arms, refuses an
# unarmed confirmation, and performs the reset through the account binding.
for console_reset_console in \
    'console_credentials_command_classify(' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_ARM' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_EXECUTE' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_UNCONFIRMED' \
    'static void console_admin_credentials_reset(void) {' \
    'admin_credentials_binding->reset(' \
    '"[ADMIN-ENROLLMENT] reset; state=UNPROVISIONED"' \
    '"[ADMIN-ENROLLMENT] reset requires confirmation; type "' \
    '"[ADMIN-ENROLLMENT] reset refused: no CREDENTIALS RESET was "' \
    'static bool credentials_reset_pending;'; do
    grep -Fq -- "$console_reset_console" kernel/src/console.c || {
        echo "ITEM-506: console не содержит:" "$console_reset_console" >&2
        exit 1
    }
done

# The reset itself: rewrite the record to the factory placeholder, then reset
# the gate so every issued session is closed.  The header declares it.
grep -Fq 'appliance_production_admin_enrollment_reset(' \
    kernel/include/appliance_production_admin_enrollment.h || {
    echo "ITEM-506: enrollment.h не объявляет сброс" >&2
    exit 1
}
for console_reset_impl in \
    'appliance_production_admin_enrollment_reset(' \
    'admin_enrollment_factory_fingerprint(factory)' \
    'appliance_persistent_secret_config_commit_admin(' \
    'appliance_production_auth_reset(enrollment->auth)' \
    'enrollment->locked_until_tick = 0U;' \
    'enrollment->provisioned = false;'; do
    grep -Fq -- "$console_reset_impl" \
        kernel/src/appliance_production_admin_enrollment.c || {
        echo "ITEM-506: сброс не содержит:" "$console_reset_impl" >&2
        exit 1
    }
done

# The host proofs: the confirmation rule and the gesture in the console suite,
# and the reset, re-provision and closed session in the enrollment suite.
for console_reset_host in \
    '#include "console_credentials_command.h"' \
    'static void test_credentials_command_classify(void)' \
    'the reset request line arms the gesture' \
    'the confirmation line executes and clears the arm' \
    'a confirmation with no request is refused, not honoured' \
    'any other command disarms a pending request' \
    'the first reset line only arms the gesture' \
    'the confirmation performs the reset'; do
    grep -Fq -- "$console_reset_host" tests/console-host.c || {
        echo "ITEM-506: console-host не требует:" "$console_reset_host" >&2
        exit 1
    }
done
for console_reset_host in \
    'test_reset_restores_provisionable' \
    'the local reset succeeds' \
    'the reset publishes the owner UNPROVISIONED' \
    'the reset closes the gate' \
    'the reset restored the factory placeholder' \
    'the session issued before the reset no longer authorizes' \
    'a new secret enrolls after the reset'; do
    grep -Fq -- "$console_reset_host" \
        tests/appliance-production-admin-enrollment-host.c || {
        echo "ITEM-506: enrollment-host не требует:" "$console_reset_host" >&2
        exit 1
    }
done

# The reset is local: no management-plane source names it.  The console module
# is the only implementation, and it is not reached by the HTTP dispatch.
if grep -rFq 'CREDENTIALS RESET' kernel/src/appliance_management*.c \
        kernel/src/appliance_production_management*.c 2>/dev/null; then
    echo "ITEM-506: сброс просочился в management plane" >&2
    exit 1
fi

# The recovery is documented where an operator looks.
for console_reset_doc in \
    'CREDENTIALS RESET' \
    'CREDENTIALS RESET CONFIRM' \
    'forgotten' ; do
    grep -Fq -- "$console_reset_doc" docs/hfoc-production-admin-enrollment.md || {
        echo "docs/hfoc-production-admin-enrollment.md не фиксирует:" \
            "$console_reset_doc" >&2
        exit 1
    }
done
grep -Fq 'CREDENTIALS RESET' docs/hfoc-management-web-ui.md || {
    echo "docs/hfoc-management-web-ui.md не фиксирует сброс" >&2
    exit 1
}
grep -Fq 'ITEM-465' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-465" >&2
    exit 1
}
grep -Fq 'ITEM-465' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-465" >&2
    exit 1
}
