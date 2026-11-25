#!/usr/bin/env bash
# ITEM-508 increment 9: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The renewal decision: the window, the retry and the bounded budget.
#
# The order exists so the appliance holds a certificate it obtained itself, and
# a certificate expires.  This slice pins the policy that decides when the next
# one is asked for, and when the appliance stops asking:
#
#   * the decision runs on the expiry the increment 8 step verified - a
#     certificate that was only downloaded never moves it;
#   * renewal starts inside the thirty-day window, no earlier, and an expired
#     certificate is due at once;
#   * an unvalidated clock (zero) refuses the decision before any window, and a
#     clock that moved backwards since the last attempt is not a validated
#     clock either;
#   * an appliance without a certificate has nothing to renew: the first
#     issuance stays a different path;
#   * one attempt is in flight at a time, a failure waits out its six-hour
#     retry window, and at most five attempts are made before the decision
#     becomes `ATTEMPTS_EXHAUSTED` and the operator is required;
#   * every accepted call publishes one bounded record naming the fact, the
#     expiry, the attempt count and the next attempt instant, and no record
#     ever carries a key, a nonce, a token or a certificate byte.
for item508_inc9_file in \
    kernel/include/appliance_acme_renewal.h \
    kernel/src/appliance_acme_renewal.c \
    tests/appliance-acme-renewal-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc9_file ]] || {
        echo "ITEM-508 inc9: отсутствует $item508_inc9_file" >&2
        exit 1
    }
done

# The published policy constants, decisions and the entry points.
for item508_inc9_name in \
    'APPLIANCE_ACME_RENEWAL_WINDOW_SECONDS UINT32_C(2592000)' \
    'APPLIANCE_ACME_RENEWAL_RETRY_SECONDS UINT32_C(21600)' \
    'APPLIANCE_ACME_RENEWAL_ATTEMPT_MAX 5U' \
    'APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK' \
    'APPLIANCE_ACME_RENEWAL_DECISION_NO_CERTIFICATE' \
    'APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER' \
    'APPLIANCE_ACME_RENEWAL_DECISION_ATTEMPTS_EXHAUSTED' \
    'appliance_acme_renewal_evaluate' \
    'appliance_acme_renewal_attempt_started' \
    'appliance_acme_renewal_attempt_failed' \
    'appliance_acme_renewal_certificate_obtained' \
    'appliance_acme_renewal_attempts_reset'; do
    grep -Fq -- "$item508_inc9_name" kernel/include/appliance_acme_renewal.h || {
        echo "ITEM-508 inc9: заголовок не публикует $item508_inc9_name" >&2
        exit 1
    }
done

# The clock is refused before any window is computed.
grep -Fq 'now_epoch_seconds == 0U' kernel/src/appliance_acme_renewal.c || {
    echo "ITEM-508 inc9: решение не отвергает невыверенные часы" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK' \
    kernel/src/appliance_acme_renewal.c || {
    echo "ITEM-508 inc9: у отказа часов нет имени" >&2
    exit 1
}
# The window and the retry are the published constants, not copies.
grep -Fq 'APPLIANCE_ACME_RENEWAL_WINDOW_SECONDS' \
    kernel/src/appliance_acme_renewal.c || {
    echo "ITEM-508 inc9: окно продления не используется решением" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ACME_RENEWAL_RETRY_SECONDS' \
    kernel/src/appliance_acme_renewal.c || {
    echo "ITEM-508 inc9: окно повтора не используется решением" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ACME_RENEWAL_ATTEMPT_MAX' \
    kernel/src/appliance_acme_renewal.c || {
    echo "ITEM-508 inc9: предел попыток не используется решением" >&2
    exit 1
}
# One order at a time: an attempt in flight blocks the next start.
grep -Fq 'last_outcome == APPLIANCE_ACME_RENEWAL_OUTCOME_NONE' \
    kernel/src/appliance_acme_renewal.c || {
    echo "ITEM-508 inc9: попытка в полёте не мешает новой" >&2
    exit 1
}
# A retry instant at the top of the clock saturates instead of wrapping.
grep -Fq 'appliance_acme_renewal_add_saturating' \
    kernel/src/appliance_acme_renewal.c || {
    echo "ITEM-508 inc9: мгновение повтора может переполниться" >&2
    exit 1
}
# The published record and the four fields it is allowed to carry.
grep -Fq '"[ACME-RENEWAL] "' kernel/src/appliance_acme_renewal.c || {
    echo "ITEM-508 inc9: запись не несёт метку модуля" >&2
    exit 1
}
for item508_inc9_field in ' expires=' ' attempts=' ' next='; do
    grep -Fq -- "\"$item508_inc9_field\"" kernel/src/appliance_acme_renewal.c || {
        echo "ITEM-508 inc9: запись не называет поле$item508_inc9_field" >&2
        exit 1
    }
done

# The proof: the module is driven at fixed instants in both host aggregates.
grep -Fq 'test-appliance-acme-renewal-host' Makefile || {
    echo "ITEM-508 inc9: проба не подключена к Makefile" >&2
    exit 1
}
# Both host aggregates (test-serial and test-host-suite-serial) invoke it, in
# addition to the target definition the file check above pins.
item508_inc9_make_targets=$(awk \
    '/[$][(]MAKE[)] test-appliance-acme-renewal-host/ { count += 1 } \
     END { print count + 0 }' Makefile)
[[ $item508_inc9_make_targets -eq 2 ]] || {
    echo "ITEM-508 inc9: проба не подключена к обоим хост-наборам" >&2
    exit 1
}
for item508_inc9_case in \
    'the empty state' \
    'an unvalidated clock refuses the decision' \
    'an appliance without a certificate has nothing to renew' \
    'the first second inside the window is due' \
    'the last second of the retry window waits' \
    'the attempt budget is spent exactly at its bound' \
    'a clock that moved backwards is not a validated clock' \
    'the next attempt saturates instead of wrapping' \
    'no record mentions a key or a token'; do
    grep -Fq -- "$item508_inc9_case" tests/appliance-acme-renewal-host.c || {
        echo "ITEM-508 inc9: проба не проверяет «$item508_inc9_case»" >&2
        exit 1
    }
done
