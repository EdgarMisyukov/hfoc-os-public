#!/usr/bin/env bash
# ITEM-449: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The traffic to the declared server goes through the machine.
#
# The appliance has a stable address (ITEM-446/447) and a declared management
# scope (ITEM-399); what it did not have is a declared forwarding rule.  This
# item is the declaration half of that rule: the operator declares one TCP
# forward - a literal IPv4 target and port, the local port the appliance
# listens on, and the local-network clients allowed to use it - on the install
# medium and in the installed boot entry, and the appliance publishes what it
# read, or refuses it, before anything is opened.
#
# The claims pinned here are the ones that make that safe:
#
#   * one grammar, one validator, one admission decision, all in
#     `kernel/src/forward_config.c` - a malformed declaration is refused with
#     the status that names it, never truncated and never guessed;
#   * the client list *is* the access rule: fail-closed, exact addresses only,
#     and an absent or disabled declaration admits nobody and says why;
#   * the declaration is refused before the first write on the medium (the
#     stage script and the generator) and refused again at boot with its own
#     record, so a medium that would install an unhonourable rule is never
#     sealed;
#   * the serving port (443) is not a listen port a forward may claim, at the
#     module, in the generator and at boot;
#   * the rule travels into the installed boot entry exactly as the management
#     scope and the network do, and the kernel publishes
#     `[PRODUCTION-FORWARD] state=DECLARED ...` or the refusal;
#   * the declaration opens nothing by itself: the relay is the next step of
#     this item, and its absence is stated in the design note rather than
#     papered over.
for forward_declaration_file in \
    kernel/include/forward_config.h \
    kernel/src/forward_config.c \
    kernel/include/production_install_media.h \
    kernel/src/production_install_media.c \
    kernel/src/kernel.c \
    kernel/src/appliance_boot_log.c \
    tests/forward-config-host.c \
    tests/production-install-media-host.c \
    scripts/hfoc-installed-config.py \
    scripts/hfoc-install-media-stage.sh \
    docs/hfoc-production-forward-declaration.md; do
    [[ -f $forward_declaration_file ]] || {
        echo "ITEM-449: отсутствует $forward_declaration_file" >&2
        exit 1
    }
done

# The one owner publishes the grammar, the validation and the access rule.
for forward_declaration_symbol in \
    forward_config_from_parts \
    forward_config_validate \
    forward_config_admit \
    forward_config_render \
    forward_config_enabled \
    forward_config_status_string \
    FORWARD_CONFIG_CLIENT_MAX \
    FORWARD_CONFIG_RESERVED_LISTEN_PORT \
    FORWARD_CONFIG_ADMITTED \
    FORWARD_CONFIG_DENIED_NO_DECLARATION \
    FORWARD_CONFIG_DENIED_DISABLED \
    FORWARD_CONFIG_DENIED_NOT_ALLOWED; do
    grep -Fq "$forward_declaration_symbol" \
        kernel/include/forward_config.h \
        kernel/src/forward_config.c || {
        echo "ITEM-449: модуль не публикует $forward_declaration_symbol" >&2
        exit 1
    }
done
# No second parser and no device in the declaration owner.
if grep -nE '\b(inet_aton|inet_pton|inet_addr|strtol|strtoul|sscanf|atoi)\b' \
        kernel/src/forward_config.c; then
    echo "ITEM-449: объявление разбирается вторым парсером" >&2
    exit 1
fi
if grep -nE '\b(console_|serial_|block_device_|tcp_|netif_|pbuf_|lwip_)\w*' \
        kernel/src/forward_config.c; then
    echo "ITEM-449: модуль объявления пишет на устройство или в стек" >&2
    exit 1
fi
# The declaration is refused before a port could be claimed.
grep -Fq 'FORWARD_CONFIG_RESERVED_LISTEN_PORT 443U' \
    kernel/include/forward_config.h || {
    echo "ITEM-449: объявление не защищает порт управления" >&2
    exit 1
}

# The medium and the installed entry carry the declaration.
for forward_declaration_directive in \
    'HFOC-INSTALL-FORWARD=' \
    'HFOC-INSTALL-FORWARD-TARGET=' \
    'HFOC-INSTALL-FORWARD-TARGET-PORT=' \
    'HFOC-INSTALL-FORWARD-LISTEN-PORT=' \
    'HFOC-INSTALL-FORWARD-CLIENTS=' \
    'hfoc.forward=on' \
    'hfoc.forward=off' \
    'hfoc.forward.target=' \
    'hfoc.forward.target-port=' \
    'hfoc.forward.listen-port=' \
    'hfoc.forward.clients='; do
    grep -Fq "$forward_declaration_directive" \
        kernel/include/production_install_media.h || {
        echo "ITEM-449: объявление не публикует $forward_declaration_directive" >&2
        exit 1
    }
done
grep -Fq 'production_install_media_forward_boot' \
    kernel/src/kernel.c kernel/include/production_install_media.h || {
    echo "ITEM-449: загрузка не разбирает объявление проброса" >&2
    exit 1
}
grep -Fq 'production_install_media_forward_cmdline' \
    kernel/src/production_install_media.c || {
    echo "ITEM-449: установленная запись не несёт объявление проброса" >&2
    exit 1
}
# The refusal and the declared rule both reach the console, mirrored into the
# boot log, and the log treats them as moments worth flushing.
grep -Fq '[PRODUCTION-FORWARD-DECLARATION] state=REFUSED reason=' \
    kernel/src/kernel.c || {
    echo "ITEM-449: отказ объявления не публикуется" >&2
    exit 1
}
grep -Fq '[PRODUCTION-FORWARD] state=DECLARED ' kernel/src/kernel.c || {
    echo "ITEM-449: принятое объявление не публикуется" >&2
    exit 1
}
grep -Fq '[PRODUCTION-FORWARD] state=UNAVAILABLE reason=' \
    kernel/src/kernel.c || {
    echo "ITEM-449: отсутствие объявления не публикуется" >&2
    exit 1
}
grep -Fq '"[PRODUCTION-FORWARD]"' kernel/src/appliance_boot_log.c || {
    echo "ITEM-449: запись проброса не переживает перезагрузку" >&2
    exit 1
}
# The build refuses before the first write: an unknown mode, incomplete values,
# an off with values, a target that is not a host, a repeated client and a
# listen port that would claim the serving port.
forward_declaration_template=boot/hfoc-installed.conf
for forward_declaration_case in \
    '--forward tunnel' \
    '--forward on --forward-target 10.0.2.3 --forward-target-port 8443 --forward-listen-port 9443' \
    '--forward off --forward-target 10.0.2.3' \
    '--forward on --forward-target 127.0.0.1 --forward-target-port 8443 --forward-listen-port 9443 --forward-clients 10.0.2.9' \
    '--forward on --forward-target 10.0.2.3 --forward-target-port 0 --forward-listen-port 9443 --forward-clients 10.0.2.9' \
    '--forward on --forward-target 10.0.2.3 --forward-target-port 8443 --forward-listen-port 443 --forward-clients 10.0.2.9' \
    '--forward on --forward-target 10.0.2.3 --forward-target-port 8443 --forward-listen-port 9443 --forward-clients 10.0.2.9,10.0.2.9' \
    '--forward on --forward-target 10.0.2.3 --forward-target-port 8443 --forward-listen-port 9443 --forward-clients 10.0.2.9,10.0.2.8,10.0.2.7,10.0.2.6,10.0.2.5' \
    '--forward on --forward-target 10.0.2.3 --forward-target-port 8443 --forward-listen-port 9443 --forward-clients 10.0.2.0/24'; do
    # shellcheck disable=SC2086  # the case is a fixed list of arguments
    if python3 scripts/hfoc-installed-config.py \
            --template "$forward_declaration_template" \
            --management local-only $forward_declaration_case \
            --output /dev/null >/dev/null 2>&1; then
        echo "ITEM-449: сборка приняла негодное объявление: $forward_declaration_case" >&2
        exit 1
    fi
done
# And the valid declaration is accepted and carries the whole rule.
forward_declaration_tmp=$(mktemp)
if ! python3 scripts/hfoc-installed-config.py \
        --template "$forward_declaration_template" \
        --management local-only \
        --forward on --forward-target 10.0.2.3 \
        --forward-target-port 8443 --forward-listen-port 9443 \
        --forward-clients 10.0.2.9,10.0.2.8 \
        --output "$forward_declaration_tmp" >/dev/null; then
    echo "ITEM-449: сборка отвергла годное объявление" >&2
    rm -f "$forward_declaration_tmp"
    exit 1
fi
if ! forward_declaration_output=$(cat "$forward_declaration_tmp"); then
    echo "ITEM-449: установленная запись не читается" >&2
    rm -f "$forward_declaration_tmp"
    exit 1
fi
rm -f "$forward_declaration_tmp"
grep -Fq 'hfoc.management=local-only hfoc.forward=on hfoc.forward.target=10.0.2.3 hfoc.forward.target-port=8443 hfoc.forward.listen-port=9443 hfoc.forward.clients=10.0.2.9,10.0.2.8' \
    <<<"$forward_declaration_output" || {
    echo "ITEM-449: установленная запись не несёт объявление целиком" >&2
    exit 1
}
# The stage script refuses the same shapes before it seals anything.
for forward_declaration_stage_check in \
    'A forward value needs HFOC_INSTALL_MEDIA_FORWARD' \
    'An off forward cannot carry a target, ports or clients' \
    'An enabled forward needs a target, both ports and clients' \
    'A forward cannot claim the management serving port 443' \
    'Invalid forward mode'; do
    grep -Fq "$forward_declaration_stage_check" \
        scripts/hfoc-install-media-stage.sh || {
        echo "ITEM-449: стадия не отвергает: $forward_declaration_stage_check" >&2
        exit 1
    }
done
grep -Fq 'HFOC-INSTALL-FORWARD-CLIENTS=' \
    scripts/hfoc-install-media-stage.sh || {
    echo "ITEM-449: стадия не записывает список клиентов в токен" >&2
    exit 1
}

# The host proof drives the real module and states every refusal.
for forward_declaration_claim in \
    'an absent declaration is not an enabled forward' \
    'the record names no rule instead of inventing one' \
    'a source is denied when nothing was declared' \
    'an explicit off is a declaration, not a malformed one' \
    'a source is denied by an explicit off' \
    'a disabled declaration refuses values instead of keeping them' \
    'a value without any declaration is refused' \
    'a complete declaration is accepted' \
    'both declared clients are kept' \
    'a declared client is admitted' \
    'a source that is not declared is denied by name' \
    'an enabled declaration with no client is refused' \
    'the target %s is refused as an address' \
    'loopback is not a local-network client' \
    'port zero is refused' \
    'a port above 65535 is refused' \
    'a port that would wrap is refused instead of truncated' \
    'the management serving port is refused as a listen port' \
    'a fifth client is refused instead of dropped' \
    'a duplicated client is refused' \
    'a prefix is not a client: only exact addresses are allowed' \
    'a buffer that cannot hold the rule is refused, not truncated' \
    'an invalid rule admits nobody and says why'; do
    grep -Fq "$forward_declaration_claim" tests/forward-config-host.c || {
        echo "ITEM-449: проба не утверждает: $forward_declaration_claim" >&2
        exit 1
    }
done
for forward_declaration_media_claim in \
    'an enabled forward declaration parses' \
    'an explicit off parses as a declaration, not as a malformed one' \
    'a forward without its client list refuses before any write' \
    'a forward that claims the serving port refuses before any write' \
    'the forward declaration renders the installed directives' \
    'the installed command line carries the forward too' \
    'the installed declaration parses back into a validated forward' \
    'a boot declaration that claims the serving port is refused' \
    'the declaration record names the declared forward'; do
    grep -Fq "$forward_declaration_media_claim" \
        tests/production-install-media-host.c || {
        echo "ITEM-449: проба medium не утверждает: $forward_declaration_media_claim" >&2
        exit 1
    }
done
grep -Fq 'test-forward-config-host' Makefile || {
    echo "ITEM-449: проба модуля не зарегистрирована" >&2
    exit 1
}
# The installed disk proves the declaration reached the appliance, and a disk
# without one states the absence.
grep -Fq '[PRODUCTION-FORWARD] state=DECLARED target=' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-449: wire-проба не читает объявление с установленного диска" >&2
    exit 1
}
grep -Fq '[PRODUCTION-FORWARD] state=UNAVAILABLE reason=NO_DECLARATION' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-449: wire-проба не читает отсутствие объявления" >&2
    exit 1
}
# The sealed medium is checked where the install proof builds it.
grep -Fq 'production-installer-forward-media' Makefile || {
    echo "ITEM-449: medium с объявлением проброса не собирается" >&2
    exit 1
}
# The design note states what the declaration opens and what stays open.
for forward_declaration_note in \
    'access rule' \
    'no IP_FORWARD' \
    'relay'; do
    grep -Fqi "$forward_declaration_note" \
        docs/hfoc-production-forward-declaration.md || {
        echo "ITEM-449: заметка не описывает: $forward_declaration_note" >&2
        exit 1
    }
done
