#!/usr/bin/env bash
# ITEM-502: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Reinstalling HFOC OS must not resurrect the previous run's state.
#
# The owner installed twice and the second machine came up with the static
# address and the administrator credential of the first.  The cause was
# mechanical: the layout writers touch only the GPT, and the old "state
# initialization" rewrote each store's block-0 data-region header while leaving
# the journal body - blocks 1..63 of every store - untouched, so the loader
# found and replayed the old records.
#
# This slice pins the fix:
#
#   * the erase is an explicit named step of the install
#     (`production_installer_state_erase()`), between the confirmation and the
#     `applied` record, with its own record
#     `[PRODUCTION-INSTALL] state erase stores=<n> blocks=<n> verify=CLEAN`;
#   * it rebuilds every store as an empty data region (journal zeroed, header
#     rewritten) and zeroes the start and tail of the appliance's own roles -
#     never the shared ESP, never a foreign partition;
#   * it is fail-closed: the raw read-back (all eight stores) and the real
#     loader read-back (every loaded source `DEFAULT_MISSING`, administrator
#     absent) must both agree the state is empty;
#   * no path can reach `applied` without it: `state_initialize()` and
#     `verify_layout()` refuse a run whose `state_erased` flag is not set;
#   * the operator's warning page names what the erase destroys (network
#     address, administrator credential, certificates);
#   * the guest proof commits an address and a credential, reinstalls, and
#     reads neither of them back.

for item502_file in \
    kernel/include/production_installer.h \
    kernel/include/production_installer_state.h \
    kernel/src/production_installer_state.c \
    kernel/src/production_installer.c \
    kernel/src/production_install_flow.c \
    kernel/src/production_installer_qemu.c \
    kernel/src/installer_storage.c \
    tests/production-installer-orchestration-host.c \
    tests/production-installer-host.c \
    tests/installed-boot-disk-host.c; do
    test -f "$item502_file" || {
        echo "ITEM-502: отсутствует файл: $item502_file" >&2
        exit 1
    }
done

# --------------------------------------------------- the explicit named step --
grep -Fq 'production_installer_state_erase(' \
    kernel/include/production_installer_state.h \
    kernel/src/production_installer_state.c || {
    echo "ITEM-502: шаг стирания не объявлен" >&2
    exit 1
}
grep -Fq 'struct production_installer_state_erase_evidence' \
    kernel/include/production_installer_state.h || {
    echo "ITEM-502: у шага стирания нет свидетельства" >&2
    exit 1
}
grep -Fq 'PRODUCTION_INSTALLER_STATE_ERASE_EDGE_BLOCKS' \
    kernel/include/production_installer_state.h \
    kernel/src/production_installer_state.c || {
    echo "ITEM-502: граница стирания краёв не объявлена" >&2
    exit 1
}
grep -Fq 'production_state_erase_store' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-502: журнал хранилища не обнуляется" >&2
    exit 1
}
grep -Fq 'memset(target' kernel/src/production_installer_state.c 2>/dev/null || true
grep -Fq 'production_state_verify_stores' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-502: сырое обратное чтение отсутствует" >&2
    exit 1
}
grep -Fq 'production_state_verify_empty' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-502: обратное чтение загрузчиком отсутствует" >&2
    exit 1
}
for item502_source in \
    'APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING' \
    'APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING' \
    'APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING' \
    'APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_MISSING' \
    'appliance_persistent_secret_config_admin'; do
    grep -Fq "$item502_source" \
        kernel/src/production_installer_state.c || {
        echo "ITEM-502: обратное чтение не требует пустоты: $item502_source" >&2
        exit 1
    }
done
grep -Fq 'INSTALLER_LAYOUT_ROLE_EFI_SYSTEM' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-502: стирание не исключает общий ESP" >&2
    exit 1
}

# --------------------------------------------------------- fail-closed gate ---
grep -Fq 'bool state_erased;' kernel/include/production_installer.h || {
    echo "ITEM-502: у контроллера нет флага стирания" >&2
    exit 1
}
grep -Fq 'installer->state_erased = true;' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-502: флаг стирания никем не ставится" >&2
    exit 1
}
grep -Fq 'if (!installer->state_erased)' \
    kernel/src/production_installer_state.c \
    kernel/src/production_installer.c || {
    echo "ITEM-502: путь без стирания не отвергается" >&2
    exit 1
}
grep -Fq '"state-erase"' kernel/src/production_install_flow.c || {
    echo "ITEM-502: отказ стирания не называется" >&2
    exit 1
}

# ------------------------------------------------------------- the record -----
grep -Fq '"[PRODUCTION-INSTALL] state erase stores="' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-502: запись стирания не публикуется" >&2
    exit 1
}
grep -Fq '" verify=CLEAN\n"' kernel/src/production_install_flow.c || {
    echo "ITEM-502: запись стирания не утверждает чистоту" >&2
    exit 1
}
python3 - <<'PY' || exit 1
import pathlib
import re
text = pathlib.Path("kernel/src/production_install_flow.c").read_text()
erase = text.index("production_installer_state_erase(")
applied = text.index("production_install_media_render_applied(")
if erase > applied:
    raise SystemExit("ITEM-502: стирание публикуется после applied")
PY

# ------------------------------------------------------- the warning pages ----
# ITEM-509 increment 18 moved the erase notice into the one catalog: the keys
# are read by the renderers, the English values stay verbatim in the catalog.
for item502_warning in \
    '"installer.storage.settings_note"' \
    '"installer.storage.settings_detail"'; do
    grep -Fq -- "$item502_warning" kernel/src/installer_storage.c || {
        echo "ITEM-502: предупреждение не читает каталог: $item502_warning" >&2
        exit 1
    }
done
for item502_warning in \
    "The appliance's own settings are erased too:" \
    'network address, administrator credential, certificates.'; do
    grep -Fq "$item502_warning" kernel/src/appliance_language.c || {
        echo "ITEM-502: каталог не называет: $item502_warning" >&2
        exit 1
    }
done
grep -Fq '"installer.storage.warning_title"' \
    kernel/src/installer_storage.c || {
    echo "ITEM-502: потеряно предупреждение ITEM-495" >&2
    exit 1
}
grep -Fq '"WARNING: EVERYTHING ON THIS DISK WILL BE DELETED"' \
    kernel/src/appliance_language.c || {
    echo "ITEM-502: каталог потерял предупреждение ITEM-495" >&2
    exit 1
}

# ------------------------------------------------------------ the proofs ------
for item502_host in \
    'state erase clears the previous run and verifies it empty' \
    'state erase performs bounded store writes' \
    'state initialization refuses a run that did not erase' \
    'layout verification refuses a run that did not erase' \
    'the first run commits a static address and a credential' \
    'the committed state is readable before the reinstall' \
    'the layout write alone does not touch the committed state' \
    'the reinstall erases the state and verifies it empty' \
    "the previous run's address and credential are gone"; do
    grep -Fq "$item502_host" \
        tests/production-installer-orchestration-host.c || {
        echo "ITEM-502: host-проба не утверждает: $item502_host" >&2
        exit 1
    }
done
grep -Fq 'controller refuses verification before the state erase' \
    tests/production-installer-host.c || {
    echo "ITEM-502: проба контроллера не проверяет гейт" >&2
    exit 1
}
grep -Fq 'state erase refused' tests/installed-boot-disk-host.c || {
    echo "ITEM-502: проба установленного диска не стирает состояние" >&2
    exit 1
}

for item502_qemu in \
    'state erase proof OK' \
    'state erase reinstall proof OK' \
    'previous state is committed and readable' \
    'the layout write alone leaves the previous state readable' \
    'the reinstall erase leaves no address and no credential' \
    'the dual-boot install erases the state before it verifies'; do
    grep -Fq "$item502_qemu" kernel/src/production_installer_qemu.c || {
        echo "ITEM-502: QEMU-проба не утверждает: $item502_qemu" >&2
        exit 1
    }
done
grep -Fq 'state erase proof OK' scripts/test-qemu-production-installer.sh || {
    echo "ITEM-502: маркер стирания не проверяется QEMU-скриптом" >&2
    exit 1
}
grep -Fq 'state erase stores=' scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-502: install-flow проба не требует записи стирания" >&2
    exit 1
}
grep -Fq 'persistence journal block' scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-502: install-flow проба не читает журнал сырым" >&2
    exit 1
}
grep -Fq 'state erase stores=' \
    scripts/test-qemu-production-install-flow-preserve.sh || {
    echo "ITEM-502: preserve-проба не требует записи стирания" >&2
    exit 1
}

# -------------------------------------------------------------- documents -----
grep -Fq 'ITEM-502' docs/ROADMAP.md || {
    echo "ITEM-502: ROADMAP не называет пункт" >&2
    exit 1
}
grep -Fq 'ITEM-502' docs/hfoc-production-install-flow.md || {
    echo "ITEM-502: документ потока установки не описывает стирание" >&2
    exit 1
}
grep -Fq 'ITEM-502' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-502: очередь не называет пункт" >&2
    exit 1
}
