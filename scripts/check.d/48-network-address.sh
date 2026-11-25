#!/usr/bin/env bash
# ITEM-416: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The address a machine holds is the one it was given.
#
# The acceptance host reported `10.0.2.15` - the address of QEMU's user-mode
# network - while its own DHCP server was handing out `192.0.2.0/24` leases,
# its listener published itself on that address, and the panel was unreachable
# although the network worked.  Two product paths put it there: the bind filled
# the fixture address in for a caller that passed no configuration (the hardware
# path passes none), and releasing an address installed the same fixture address
# as the state a released lease leaves behind.  The acquisition also gave up
# after three one-second attempts, which is two seconds of patience for a switch
# port that has to leave spanning-tree blocking.
#
# This slice pins all four: a caller with no configuration gets no address, an
# address that is released is cleared rather than replaced, the client keeps
# asking for the whole budget and says which attempt it is on, and the proofs
# that read those decisions exist.
if grep -Fq 'network_ipv4_profile_qemu_fill' \
    kernel/src/appliance_production_network.c; then
    echo "продуктовый bind всё ещё подставляет адрес фикстуры" >&2
    exit 1
fi
if ! grep -Fq 'network_ipv4_profile_init_unaddressed' \
        kernel/src/appliance_production_network.c ||
    ! grep -Fq 'network_ipv4_profile_init_unaddressed' \
        kernel/include/network_ipv4_profile.h ||
    ! grep -Fq 'profile->unaddressed' kernel/src/network_ipv4_profile.c; then
    echo "профиль не умеет публиковаться без адреса" >&2
    exit 1
fi
if ! grep -Fq 'hfoc_lwip_netif_clear_address' kernel/include/hfoc_lwip_netif.h ||
    ! grep -Fq 'hfoc_lwip_netif_clear_address' kernel/src/hfoc_lwip_netif.c ||
    ! grep -Fq 'hfoc_lwip_netif_clear_address' \
        kernel/src/appliance_production_network_wire_x86.c; then
    echo "освобождение адреса не очищает его" >&2
    exit 1
fi
grep -Fq 'APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS 40U' \
    kernel/include/appliance_dhcp_client.h || {
    echo "бюджет приобретения адреса не расширен" >&2
    exit 1
}
if ! grep -Fq 'appliance_network_wire_dhcp_attempts' \
        kernel/src/appliance_network_wire.c ||
    ! grep -Fq '" attempt="' kernel/src/kernel.c; then
    echo "запись DHCP не называет, какая это попытка" >&2
    exit 1
fi
# shellcheck disable=SC2016  # the single-quoted text is the proof's own words
for address_proof in \
    'and the machine answers it for nobody: it holds no address' \
    'the released configuration is the zero configuration' \
    'a discovery timeout inside the budget keeps asking' \
    'the budget is what ends the acquisition'; do
    grep -Fq -- "$address_proof" tests/appliance-production-network-host.c \
        tests/lwip-netif-host.c tests/appliance-dhcp-client-host.c || {
        echo "хостовое доказательство не проверяет: $address_proof" >&2
        exit 1
    }
done

# ITEM-416: live and unaddressed is a state, not a failure.
#
# The QEMU acceptance phase caught the half-fix: the fixture address was gone,
# but the serving plane still initialised its listener against the interface's
# configuration.  With no address that configuration is all zeros, the listener
# refused it, the state became FAILED - and the poll never retries a hard
# failure - so the panel never came up on a machine that had simply not been
# given an address yet (`test-production-dhcp-dns-qemu`: "the management TLS
# lifecycle did not become ACTIVE after the SNTP clock was validated").
grep -Fq 'hfoc_lwip_netif_addressed' kernel/include/hfoc_lwip_netif.h || {
    echo "нет предиката «интерфейс жив, но без адреса»" >&2
    exit 1
}
grep -Fq 'hfoc_lwip_netif_addressed' kernel/src/appliance_production_serving.c || {
    echo "плоскость обслуживания не ждёт адрес" >&2
    exit 1
}
grep -Fq 'address_not_assigned' kernel/src/appliance_production_serving.c || {
    echo "запись не называет ожидание адреса" >&2
    exit 1
}
grep -Fq 'hfoc_lwip_netif_addressed(&netif)' tests/lwip-netif-host.c || {
    echo "host-тест не проверяет предикат адреса" >&2
    exit 1
}
