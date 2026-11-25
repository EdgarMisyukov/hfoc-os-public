#!/usr/bin/env bash
# ITEM-446: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The address is declared on the install medium and the installed appliance
# comes up on it.
#
# Until this item the installed system always discovered an address: the media
# declaration carried the confirmation, the target, the management scope and
# the layout, and nothing about the network.  The owner's requirement is that
# the operator declares the address once, on the medium, and the installed
# appliance states it instead of discovering one - and that a declaration which
# cannot be honoured is refused before the first write, never guessed at.
#
# The claims pinned here are the ones that make that safe:
#
#   * the declaration is four bounded directives (`HFOC-INSTALL-NETWORK=`,
#     `ADDRESS=`, `GATEWAY=`, `DNS=`) and a mode the operator misspelled is
#     refused, not read as "default";
#   * a static declaration is only accepted complete, and it is validated by
#     the one IPv4 owner the tree already has - not by a second parser;
#   * the plan refuses an unhonourable declaration (NETWORK_INCOMPLETE /
#     NETWORK_MALFORMED) before it can select a target, so the run writes
#     nothing;
#   * the installed boot entry states the declaration and the kernel applies it
#     from that command line, publishing the address it actually put in force;
#   * a committed persistent record wins over the declaration, so a setting
#     changed later is not silently overwritten by the install-time value.
for install_network_file in \
    scripts/hfoc-install-media-stage.sh \
    scripts/hfoc-installed-config.py \
    kernel/include/production_install_media.h \
    kernel/src/production_install_media.c \
    kernel/src/production_install_flow.c \
    kernel/src/kernel.c \
    tests/production-install-media-host.c \
    scripts/test-qemu-production-install-flow.sh; do
    [[ -f $install_network_file ]] || {
        echo "ITEM-446: отсутствует $install_network_file" >&2
        exit 1
    }
done

# The four directives, spelled exactly once each, on the medium and in the
# installed entry.
for install_network_directive in \
    'HFOC-INSTALL-NETWORK=' \
    'HFOC-INSTALL-ADDRESS=' \
    'HFOC-INSTALL-GATEWAY=' \
    'HFOC-INSTALL-DNS='; do
    grep -Fq "$install_network_directive" \
        kernel/include/production_install_media.h || {
        echo "ITEM-446: объявление не публикует $install_network_directive" >&2
        exit 1
    }
    grep -Fq "$install_network_directive" \
        scripts/hfoc-install-media-stage.sh || {
        echo "ITEM-446: медиа не пишет $install_network_directive" >&2
        exit 1
    }
done
for install_network_cmdline in \
    'hfoc.network=dhcp' \
    'hfoc.network=static' \
    'hfoc.address=' \
    'hfoc.gateway=' \
    'hfoc.dns='; do
    grep -Fq "$install_network_cmdline" \
        kernel/include/production_install_media.h || {
        echo "ITEM-446: установленная запись не публикует $install_network_cmdline" >&2
        exit 1
    }
done

# One validator: the IPv4 owner the tree already has, reached through the
# builder that splits `a.b.c.d/prefix` (ITEM-447 moved that builder into the
# owner itself, where the parser and the validator live); no second parser of
# the same text anywhere.
grep -Fq 'network_ipv4_config_from_parts(' \
    kernel/src/production_install_media.c \
    kernel/src/ipv4_config.c || {
    echo "ITEM-446: объявление не разбирается общим IPv4-владельцем" >&2
    exit 1
}
grep -Fq 'network_ipv4_config_validate(' \
    kernel/src/ipv4_config.c || {
    echo "ITEM-446: объявление не проверяется общим IPv4-владельцем" >&2
    exit 1
}
if grep -nE '\b(inet_aton|inet_pton|strtol|sscanf)\b' \
        kernel/src/production_install_media.c; then
    echo "ITEM-446: объявление разбирает адрес вторым парсером" >&2
    exit 1
fi

# The refusals exist and the plan reaches them before it can select a target.
for install_network_refusal in \
    PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_INCOMPLETE \
    PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_MALFORMED; do
    grep -Fq "$install_network_refusal" \
        kernel/include/production_install_media.h \
        kernel/src/production_install_media.c || {
        echo "ITEM-446: отказ $install_network_refusal не объявлен" >&2
        exit 1
    }
    grep -Fq "$install_network_refusal" \
        tests/production-install-media-host.c || {
        echo "ITEM-446: проба не проверяет $install_network_refusal" >&2
        exit 1
    }
done

# The installed entry states the declaration, and the flow composes it in the
# one place so the sealed configuration and the generated menu cannot disagree.
grep -Fq 'production_install_media_installed_cmdline' \
    kernel/include/production_install_media.h \
    kernel/src/production_install_media.c \
    kernel/src/production_install_flow.c || {
    echo "ITEM-446: установленная запись не собирается одним владельцем" >&2
    exit 1
}
grep -Fq 'production_install_media_network_boot' \
    kernel/src/kernel.c || {
    echo "ITEM-446: загрузка не разбирает объявление установленной записи" >&2
    exit 1
}
# The boot applies what it validated, and a committed record wins.
grep -Fq 'APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED' \
    kernel/src/kernel.c || {
    echo "ITEM-446: загрузка не уважает записанную настройку" >&2
    exit 1
}
grep -Fq 'appliance_persistent_network_config_set_static' \
    kernel/src/kernel.c || {
    echo "ITEM-446: загрузка не сохраняет объявленный адрес" >&2
    exit 1
}
# The addressing record publishes the address the interface actually has.
grep -Fq 'appliance_network_wire_active_config' kernel/src/kernel.c || {
    echo "ITEM-446: запись режима не читает действующий адрес" >&2
    exit 1
}
grep -Fq 'address=' kernel/src/kernel.c || {
    echo "ITEM-446: запись режима не публикует адрес" >&2
    exit 1
}
# The declaration record names the network before the first write.
grep -Fq 'network=' kernel/src/production_install_media.c || {
    echo "ITEM-446: запись объявления не называет сеть" >&2
    exit 1
}

# The build refuses a declaration the kernel would refuse: a misspelled mode, a
# static mode without its values, DHCP with a static value, a bad prefix and a
# gateway outside the declared subnet.
install_network_template=boot/hfoc-installed.conf
for install_network_case in \
    '--network ethernet' \
    '--network static --address 10.0.2.15/24 --gateway 10.0.2.2' \
    '--network static --address 10.0.2.15/24 --gateway 10.0.2.2 --dns 10.0.2.3 --network dhcp' \
    '--network dhcp --address 10.0.2.15/24 --gateway 10.0.2.2 --dns 10.0.2.3' \
    '--network static --address 10.0.2.15/33 --gateway 10.0.2.2 --dns 10.0.2.3' \
    '--network static --address 10.0.2.15/24 --gateway 10.0.3.1 --dns 10.0.2.3' \
    '--network static --address 10.0.2.15 --gateway 10.0.2.2 --dns 10.0.2.3'; do
    # shellcheck disable=SC2086  # the case is a fixed list of arguments
    if python3 scripts/hfoc-installed-config.py \
            --template "$install_network_template" \
            --management local-only $install_network_case \
            --output /dev/null >/dev/null 2>&1; then
        echo "ITEM-446: сборка приняла негодное объявление: $install_network_case" >&2
        exit 1
    fi
done
# And the valid declaration is accepted and carries both declarations.
if ! install_network_output=$(python3 scripts/hfoc-installed-config.py \
        --template "$install_network_template" \
        --management local-only \
        --network static --address 10.0.2.15/24 \
        --gateway 10.0.2.2 --dns 10.0.2.3 \
        --output /dev/stdout 2>/dev/null); then
    # --output /dev/stdout is not how the tool writes; fall back to a temp file.
    install_network_tmp=$(mktemp)
    python3 scripts/hfoc-installed-config.py \
        --template "$install_network_template" \
        --management local-only \
        --network static --address 10.0.2.15/24 \
        --gateway 10.0.2.2 --dns 10.0.2.3 \
        --output "$install_network_tmp" >/dev/null || {
        echo "ITEM-446: сборка отвергла годное объявление" >&2
        rm -f "$install_network_tmp"
        exit 1
    }
    install_network_output=$(cat "$install_network_tmp")
    rm -f "$install_network_tmp"
fi
grep -Fq 'hfoc.management=local-only hfoc.network=static hfoc.address=10.0.2.15/24 hfoc.gateway=10.0.2.2 hfoc.dns=10.0.2.3' \
    <<<"$install_network_output" || {
    echo "ITEM-446: установленная запись не несёт объявление целиком" >&2
    exit 1
}

# The host proof drives the real module and states the refusals, and the QEMU
# proof installs from a declared medium and boots the installed disk.
for install_network_claim in \
    'a static network declaration parses' \
    'an unknown network mode is refused' \
    'a complete static declaration validates' \
    'a static declaration without its DNS is refused' \
    'DHCP with a static value is refused' \
    'an address without a network mode is refused' \
    'an address without a prefix is refused' \
    'a prefix outside 1..32 is refused' \
    'a gateway outside the declared subnet is refused' \
    'a malformed DNS is refused' \
    'a gateway outside the subnet refuses before any write' \
    'an incomplete static declaration refuses the run' \
    'the installed declaration parses back into a validated config' \
    'the installed command line carries both declarations' \
    'the declaration record names the declared network'; do
    grep -Fq "$install_network_claim" \
        tests/production-install-media-host.c || {
        echo "ITEM-446: проба не утверждает: $install_network_claim" >&2
        exit 1
    }
done
grep -Fq 'static network' scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-446: QEMU-проба не устанавливает с объявленной сетью" >&2
    exit 1
}
grep -Fq 'a static declaration still ran a DHCP discover' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-446: QEMU-проба не отвергает DHCP при static" >&2
    exit 1
}
grep -Fq 'production-installer-network-media' Makefile || {
    echo "ITEM-446: сборка не публикует медиа с объявленной сетью" >&2
    exit 1
}
# The proof drives the real module, and the documentation records the item.
grep -Fq 'kernel/src/ipv4_config.c' Makefile || {
    echo "ITEM-446: проба не линкует общий IPv4-владелец" >&2
    exit 1
}
grep -Fq 'ITEM-446' docs/hfoc-production-install-flow.md || {
    echo "ITEM-446: документ установки не описывает объявленный адрес" >&2
    exit 1
}
