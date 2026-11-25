#!/usr/bin/env bash
# ITEM-447: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The address is set from the machine's own menu.
#
# ITEM-446 made the address a declaration on the install medium: the installed
# appliance comes up on it and stores it in the persistent network record.
# Changing it still needed a new medium.  This item is the menu half: the
# person at the machine picks Network, answers four bounded questions, and the
# appliance commits the answer into the same record the boot reads, so the next
# boot states the new address - and a committed record wins over the medium.
#
# The claims pinned here are the ones that make that safe:
#
#   * the answer is four questions (mode, address, gateway, DNS) and nothing is
#     committed until all four were answered and the one IPv4 owner accepted
#     them;
#   * an empty answer cancels at every step and writes nothing;
#   * a mode that is not a mode, and a value that does not validate, are
#     refused with the owner's own reason - never guessed, never truncated;
#   * the console stores no record module: it commits through the two-word
#     binding the boot path publishes, which reaches the same setter the
#     install declaration uses;
#   * the record says whether the commit was stored, so a machine without a
#     state volume cannot claim persistence.
for menu_address_file in \
    kernel/include/local_network_setup.h \
    kernel/src/local_network_setup.c \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/local-network-setup-host.c \
    tests/console-host.c \
    scripts/test-qemu-production-install-flow.sh; do
    [[ -f $menu_address_file ]] || {
        echo "ITEM-447: отсутствует $menu_address_file" >&2
        exit 1
    }
done

# The questions, the record and the two-word binding exist.
for menu_address_symbol in \
    local_network_setup_begin \
    local_network_setup_feed \
    local_network_setup_prompt \
    local_network_setup_record \
    local_network_setup_refusal \
    LOCAL_NETWORK_SETUP_STEP_MODE \
    LOCAL_NETWORK_SETUP_STEP_ADDRESS \
    LOCAL_NETWORK_SETUP_STEP_GATEWAY \
    LOCAL_NETWORK_SETUP_STEP_DNS \
    LOCAL_NETWORK_SETUP_ACTION_COMMIT_DHCP \
    LOCAL_NETWORK_SETUP_ACTION_COMMIT_STATIC \
    LOCAL_NETWORK_SETUP_ACTION_CANCEL \
    LOCAL_NETWORK_SETUP_ACTION_REFUSED; do
    grep -Fq "$menu_address_symbol" \
        kernel/include/local_network_setup.h \
        kernel/src/local_network_setup.c || {
        echo "ITEM-447: модуль не публикует $menu_address_symbol" >&2
        exit 1
    }
done
grep -Fq 'struct console_network_binding' kernel/include/console.h || {
    echo "ITEM-447: консоль не публикует привязку записи" >&2
    exit 1
}
grep -Fq 'console_bind_network_config' \
    kernel/include/console.h kernel/src/console.c kernel/src/kernel.c || {
    echo "ITEM-447: привязка записи не объявлена, не определена или не связана" >&2
    exit 1
}
# The commit goes through the same owner the boot applies.
grep -Fq 'appliance_persistent_network_config_set_static' kernel/src/kernel.c || {
    echo "ITEM-447: меню не сохраняет static в тот же стор" >&2
    exit 1
}
grep -Fq 'appliance_persistent_network_config_set_dhcp' kernel/src/kernel.c || {
    echo "ITEM-447: меню не сохраняет dhcp в тот же стор" >&2
    exit 1
}
# One validator: the IPv4 owner, and no second parser of the slash form.
grep -Fq 'network_ipv4_config_from_parts' \
    kernel/src/local_network_setup.c kernel/src/ipv4_config.c || {
    echo "ITEM-447: ответ меню не проверяется общим IPv4-владельцем" >&2
    exit 1
}
if grep -nE '\b(inet_aton|inet_pton|strtol|sscanf)\b' \
        kernel/src/local_network_setup.c; then
    echo "ITEM-447: модуль разбирает адрес вторым парсером" >&2
    exit 1
fi
# The menu entry starts the questions only when a record owner is bound.
grep -Fq 'network_binding_published' kernel/src/console.c || {
    echo "ITEM-447: пункт меню не различает связанный и несвязанный стор" >&2
    exit 1
}
grep -Fq 'console_network_setup_start' kernel/src/console.c || {
    echo "ITEM-447: пункт Network не начинает вопросы" >&2
    exit 1
}
# The module holds no device, no record and no console.
if grep -nE '\b(console_|serial_|block_device_|appliance_persistent_network_config_)\w*' \
        kernel/src/local_network_setup.c; then
    echo "ITEM-447: модуль вопросов пишет на устройство или в консоль" >&2
    exit 1
fi

# The host proof drives the real module and states every refusal.
for menu_address_claim in \
    'the answer starts on the mode question' \
    'an empty mode answer cancels the whole answer' \
    'an unknown mode is refused with its reason' \
    'the completed answer commits the validated configuration' \
    'the DHCP answer commits discovery without more questions' \
    'an invalid static answer is refused with the IPv4 reason' \
    'an over-long address is refused instead of truncated' \
    'the record names the committed answer and when it applies' \
    'a record that could not be stored says so' \
    'an unbound record owner is published, not hidden'; do
    grep -Fq "$menu_address_claim" tests/local-network-setup-host.c || {
        echo "ITEM-447: проба не утверждает: $menu_address_claim" >&2
        exit 1
    }
done
# The console proof drives the menu entry, the four answers and the refusals.
for menu_address_console_claim in \
    'the Network entry is the one the menu ran' \
    'the menu hands the keyboard back to the questions' \
    'the completed answer is committed through the bound owner' \
    'the committed answer is published with its address' \
    'an unknown mode is refused and nothing is committed' \
    'an invalid prefix is refused and nothing is committed' \
    'an empty answer cancels without committing' \
    'without a record owner the entry does not start the questions' \
    'a record the owner refused says persisted=FAILED'; do
    grep -Fq "$menu_address_console_claim" tests/console-host.c || {
        echo "ITEM-447: консольная проба не утверждает: $menu_address_console_claim" >&2
        exit 1
    }
done
# The wire proof sets the address from the menu and reads it back after a reset.
grep -Fq 'network-menu' scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-447: QEMU-проба не настраивает адрес из меню" >&2
    exit 1
}
grep -Fq 'menu answer committed' scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-447: QEMU-проба не утверждает запись из меню" >&2
    exit 1
}
grep -Fq 'network-menu-persisted' scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-447: QEMU-проба не перезагружается после записи" >&2
    exit 1
}
# The proof is registered, and the documentation records the item.
grep -Fq 'test-local-network-setup-host' Makefile \
    tests/test-host-suite-manifest.sh || {
    echo "ITEM-447: проба модуля не зарегистрирована в host suite" >&2
    exit 1
}
grep -Fq 'ITEM-447' docs/hfoc-appliance-boot-console.md || {
    echo "ITEM-447: документ консоли не описывает адрес из меню" >&2
    exit 1
}
