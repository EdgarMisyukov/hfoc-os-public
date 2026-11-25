#!/usr/bin/env bash
# ITEM-657: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# A staged certificate-name import, delete or default answered 303 back to the
# Certificates page, but the serving path's pending list omitted
# CERTIFICATE_NAME, so that page could never show the two-phase token.  This
# slice pins the one shared list, its user, the Certificates-page rendering and
# the QEMU browser regression.
# shellcheck disable=SC2016

item657_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item657_root" || exit 1

for item657_file in \
    kernel/include/appliance_management_authorization.h \
    kernel/src/appliance_management_authorization.c \
    kernel/src/appliance_production_serving.c \
    tests/appliance-management-panel-host.c \
    scripts/test-qemu-production-service-https.sh; do
    [[ -f $item657_file ]] || {
        echo "ITEM-657: отсутствует $item657_file" >&2
        exit 1
    }
done

grep -Fq 'appliance_management_authorization_mutation_has_panel_confirmation' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-657: нет общего списка подтверждения" >&2
    exit 1
}
grep -Fq 'appliance_management_authorization_mutation_has_panel_confirmation(' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-657: общий список не реализован" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME:' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-657: CERTIFICATE_NAME не в списке подтверждения" >&2
    exit 1
}
grep -Fq 'appliance_management_authorization_mutation_has_panel_confirmation(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-657: обслуживание не использует общий список" >&2
    exit 1
}
if grep -Fq 'pending ==' kernel/src/appliance_production_serving.c; then
    echo "ITEM-657: обслуживание всё ещё перечисляет мутации отдельно" >&2
    exit 1
fi
grep -Fq 'ITEM-657: the certificate-name mutation shows its panel token' \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-657: host-проба не требует токен" >&2
    exit 1
}
grep -Fq 'the staged certificate-name mutation published no panel token' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-657: QEMU-проба не требует токен" >&2
    exit 1
}
