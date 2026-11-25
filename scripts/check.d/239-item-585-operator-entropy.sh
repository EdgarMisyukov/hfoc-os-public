#!/usr/bin/env bash
# ITEM-585: the operator entropy fallback is explicit, hidden and bounded.
#           Sourced in pinned order by scripts/check.sh; run ShellCheck on its
#           own.

grep -Fq 'APPLIANCE_PRODUCTION_ENTROPY_SOURCE_OPERATOR' \
    kernel/include/appliance_production_entropy.h || {
    echo "ITEM-585: entropy header does not name the operator source" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES 32U' \
    kernel/include/appliance_production_entropy.h || {
    echo "ITEM-585: operator seed is not exactly 32 bytes" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_MIN_DISTINCT' \
    kernel/include/appliance_production_entropy.h || {
    echo "ITEM-585: operator seed has no diversity rule" >&2
    exit 1
}
grep -Fq 'appliance_production_entropy_seed_operator' \
    kernel/include/appliance_production_entropy.h || {
    echo "ITEM-585: operator seed entry point is not declared" >&2
    exit 1
}
grep -Fq 'appliance_production_entropy_publish_external(' \
    kernel/src/appliance_production_entropy.c || {
    echo "ITEM-585: operator seed is not mixed through the entropy chain" >&2
    exit 1
}
grep -Fq 'case APPLIANCE_PRODUCTION_ENTROPY_SOURCE_OPERATOR:' \
    kernel/src/appliance_production_entropy.c || {
    echo "ITEM-585: operator source has no published name" >&2
    exit 1
}
grep -Fq 'entropy_seed_active' kernel/src/console.c || {
    echo "ITEM-585: console has no hidden operator seed line" >&2
    exit 1
}
grep -Fq 'console_entropy_seed_enter' kernel/src/console.c || {
    echo "ITEM-585: hidden operator seed line is not submitted" >&2
    exit 1
}
grep -Fq 'console_command_is(input_line, "entropy seed")' \
    kernel/src/console.c || {
    echo "ITEM-585: console has no explicit entropy seed command" >&2
    exit 1
}
grep -Fq 'seed is entered on a hidden line' kernel/src/console.c || {
    echo "ITEM-585: a seed argument is not refused" >&2
    exit 1
}
# ITEM-586: the no-RNG/operator-seed scenario is its own scheduling unit,
# extracted from the HTTPS proof; the canonical suite still runs it exactly
# once, and the suite's overall verdict requires it.
grep -Eq 'status=OPERATOR|source=OPERATOR' \
    scripts/test-qemu-production-service-https-noentropy.sh || {
    echo "ITEM-585: QEMU negative proof does not exercise the operator seed" >&2
    exit 1
}
grep -Fq 'test-production-service-https-noentropy-qemu' Makefile || {
    echo "ITEM-586: the canonical suite does not schedule the extracted no-RNG child" >&2
    exit 1
}
grep -Fq 'ITEM-585' docs/ROADMAP.md || {
    echo "ITEM-585: ROADMAP does not record the fallback" >&2
    exit 1
}
