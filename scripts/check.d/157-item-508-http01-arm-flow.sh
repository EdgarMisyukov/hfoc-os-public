#!/usr/bin/env bash
# ITEM-508 increment 30: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The order path arms the HTTP-01 listener.
#
# Increment 28 owns the socket and the wire proof, but the listener is armed by
# a literal in that proof.  This slice pins the production connector that joins
# the two proven halves: the token is the flow's own parsed `http-01` token,
# the answer is the flow's own key authorization, and the arm is the same
# decision-module arm the wire proof drives.  Nothing in the connector decides
# a path, a method or a lifetime of its own, and no caller literal is accepted
# in place of the flow.
for item508_inc30_file in \
    kernel/include/appliance_acme_flow.h \
    kernel/src/appliance_acme_flow.c \
    kernel/include/appliance_production_acme_http01.h \
    kernel/src/appliance_production_acme_http01.c \
    kernel/src/appliance_production_acme_http01_flow.c \
    tests/appliance-acme-flow-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc30_file ]] || {
        echo "ITEM-508 inc30: отсутствует $item508_inc30_file" >&2
        exit 1
    }
done

# 1. The flow publishes the token of its parsed challenge as its own accessor;
#    the token is never re-read from a caller string.
for item508_inc30_flow_pin in \
    'enum appliance_acme_flow_status appliance_acme_flow_http01_token(' \
    'flow->authorization.has_http01' \
    'flow->authorization.http01.token_length' \
    'APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL'; do
    grep -Fq -- "$item508_inc30_flow_pin" \
        kernel/include/appliance_acme_flow.h kernel/src/appliance_acme_flow.c || {
        echo "ITEM-508 inc30: поток не содержит «$item508_inc30_flow_pin»" >&2
        exit 1
    }
done
# The accessor has a declaration in the flow header and exactly one definition
# in the flow source; a definition without a declaration, or a second copy of
# either, is not the one public token accessor this increment pins.
[[ $(grep -Fc 'appliance_acme_flow_http01_token(' \
    kernel/include/appliance_acme_flow.h) -eq 1 ]] || {
    echo "ITEM-508 inc30: заголовок потока не объявляет accessor токена" >&2
    exit 1
}
[[ $(grep -Fc 'appliance_acme_flow_http01_token(' \
    kernel/src/appliance_acme_flow.c) -eq 1 ]] || {
    echo "ITEM-508 inc30: поток не определяет accessor токена ровно один раз" >&2
    exit 1
}

# 2. The connector is declared beside the arm it delegates to and includes the
#    flow header; it links the listener to the flow, not the other way round.
for item508_inc30_header_pin in \
    '#include "appliance_acme_flow.h"' \
    'appliance_production_acme_http01_arm_flow('; do
    grep -Fq -- "$item508_inc30_header_pin" \
        kernel/include/appliance_production_acme_http01.h || {
        echo "ITEM-508 inc30: заголовок слушателя не содержит «$item508_inc30_header_pin»" \
            >&2
        exit 1
    }
done

# 3. The connector lives in its own translation unit, decides nothing itself:
#    both values come from the flow and the arm is delegated to the
#    increment-28 function.  A direct `appliance_acme_challenge_arm_token(`
#    inside the connector would be the second decision path this increment
#    exists to prevent, and the listener source must stay flow-free.
for item508_inc30_source_pin in \
    'appliance_acme_flow_http01_token(' \
    'appliance_acme_flow_key_authorization(' \
    'appliance_production_acme_http01_arm(' \
    'APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL'; do
    grep -Fq -- "$item508_inc30_source_pin" \
        kernel/src/appliance_production_acme_http01_flow.c || {
        echo "ITEM-508 inc30: коннектор не содержит «$item508_inc30_source_pin»" >&2
            exit 1
        }
done
if grep -Fq 'appliance_acme_flow_' \
    kernel/src/appliance_production_acme_http01.c; then
    echo "ITEM-508 inc30: слушатель тянет поток в свой модуль" >&2
    exit 1
fi
if grep -Fq 'appliance_acme_challenge_arm_token(' \
    kernel/src/appliance_production_acme_http01_flow.c; then
    echo "ITEM-508 inc30: коннектор вооружает в обход arm()" >&2
    exit 1
fi

# 4. The host proof drives the real flow into the real listener and serves the
#    flow's key authorization; a caller literal is not part of the proof.
for item508_inc30_test_pin in \
    'static void test_the_listener_arms_from_the_flow(void)' \
    'the token accessor returns exactly the parsed challenge token' \
    'a token that does not fit is refused without a partial copy' \
    'a flow without the http-01 challenge has no token' \
    'the order path arms the listener from the flow' \
    'the listener serves exactly the flow'"'"'s key authorization' \
    'a foreign token is refused with an empty body' \
    'arm_flow refuses null arguments' \
    'a flow without a challenge and a listener that is not up refuse' \
    'a refused arm leaves the armed answer untouched' \
    'disarm closes the challenge for the same path'; do
    grep -Fq -- "$item508_inc30_test_pin" tests/appliance-acme-flow-host.c || {
        echo "ITEM-508 inc30: проба не содержит «$item508_inc30_test_pin»" >&2
        exit 1
    }
done

# 5. The flow host proof links the real listener and its lwIP objects, so the
#    connector is proven against the production module, not a stub.
for item508_inc30_make_pin in \
    'kernel/src/appliance_production_acme_http01.c' \
    'kernel/src/appliance_production_acme_http01_flow.c' \
    'build/acme-flow-host/*.o' \
    '-DHFOC_APPLIANCE_ACME_HTTP01_HOST_TEST'; do
    grep -Fq -- "$item508_inc30_make_pin" Makefile || {
        echo "ITEM-508 inc30: Makefile не содержит «$item508_inc30_make_pin»" >&2
        exit 1
    }
done

# 6. The increment is registered in the queue document and named by the docs.
for item508_inc30_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 30' "$item508_inc30_doc" || {
        echo "ITEM-508 inc30: $item508_inc30_doc не называет инкремент" >&2
        exit 1
    }
done
