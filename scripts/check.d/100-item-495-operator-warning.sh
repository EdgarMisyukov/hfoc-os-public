#!/usr/bin/env bash
# ITEM-495: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The operator entry confirms on the screen: the disk is named, the warning is
# read, and the console token is gone.
#
# ITEM-490 put the operator in front of the disks; this item puts the decision
# where the person is.  This slice pins the three rules and the one removal:
#
#   * the screen names the disk the way the machine did.  The model the ATA
#     IDENTIFY reported is recorded beside the identity - never folded into it,
#     because every disk of a kind shares a model and no two share a serial -
#     and it is printed on the row and on the warning page;
#   * nothing is written before the operator accepts the warning.  Choosing a
#     row draws the page and records `writes=0`; only ENTER on the page
#     confirms; a new choice withdraws the previous acceptance, so a confirmed
#     disk is never a disk the operator stopped looking at;
#   * the console no longer confirms an install.  `INSTALL CONFIRM <token>`, its
#     handler, its refusal and its record source are gone, so the only way to
#     authorize a write is the declaration (unattended) or the screen (operator);
#   * the unattended entry is unchanged: the declaration still authorizes it and
#     its confirmation record still reads `source=media-token`.

for item495_file in \
    kernel/include/installer_hardware_discovery.h \
    kernel/src/installer_hardware_discovery.c \
    kernel/include/installer_storage.h \
    kernel/src/installer_storage.c \
    kernel/include/installer_storage_screen.h \
    kernel/src/installer_storage_screen.c \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c \
    kernel/include/production_install_media.h \
    kernel/src/production_install_media.c \
    kernel/src/console.c \
    tests/installer-storage-host.c \
    tests/installer-hardware-discovery-host.c \
    scripts/test-qemu-installer-disk-screen.sh \
    docs/hfoc-production-install-flow.md; do
    test -f "$item495_file" || {
        echo "ITEM-495: отсутствует файл: $item495_file" >&2
        exit 1
    }
done

# 1. The name the hardware reported reaches the screen and stays out of the
#    identity the plan writes by.
grep -Fq 'char model[INSTALLER_DISCOVERY_MODEL_LENGTH];' \
    kernel/include/installer_hardware_discovery.h || {
    echo "ITEM-495: отчёт обнаружения не несёт модель диска" >&2
    exit 1
}
grep -Fq 'installer_discovery_set_disk_model' \
    kernel/include/installer_hardware_discovery.h \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-495: модель диска не записывается в отчёт" >&2
    exit 1
}
grep -Fq 'installer_discovery_set_disk_model(' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-495: поток установки не передаёт модель диска в отчёт" >&2
    exit 1
}
grep -Fq 'installer_storage_copy_text(entry->model' \
    kernel/src/installer_storage.c || {
    echo "ITEM-495: модель диска не доходит до модели экрана" >&2
    exit 1
}
grep -Fq 'disk->model' kernel/src/installer_storage.c || {
    echo "ITEM-495: экран не печатает модель диска" >&2
    exit 1
}

# 2. The warning page is the confirmation, and it says what it destroys.
#    ITEM-509 increment 18: the sentence moves to the one catalog, so the pin
#    follows it - the key in the renderer and the English value verbatim in the
#    catalog.  The rendered page itself is still pinned by the QEMU marker.
grep -Fq '"installer.storage.warning_title"' \
    kernel/src/installer_storage.c || {
    echo "ITEM-495: страница предупреждения не читает каталог" >&2
    exit 1
}
grep -Fq '"WARNING: EVERYTHING ON THIS DISK WILL BE DELETED"' \
    kernel/src/appliance_language.c || {
    echo "ITEM-495: каталог потерял предупреждение" >&2
    exit 1
}
grep -Fq '"installer.storage.not_written_yet"' \
    kernel/src/installer_storage.c || {
    echo "ITEM-495: предупреждение не читает каталог" >&2
    exit 1
}
grep -Fq '"  Nothing has been written yet."' \
    kernel/src/appliance_language.c || {
    echo "ITEM-495: каталог потерял строку «ещё ничего не записано»" >&2
    exit 1
}
grep -Fq 'installer_storage_warning_lines' \
    kernel/include/installer_storage.h kernel/src/installer_storage.c || {
    echo "ITEM-495: страница предупреждения не отрисована" >&2
    exit 1
}
grep -Fq 'installer_storage_confirm' \
    kernel/include/installer_storage.h kernel/src/installer_storage.c || {
    echo "ITEM-495: подтверждение предупреждения неявное" >&2
    exit 1
}
grep -Fq 'INSTALLER_STORAGE_DECISION_CONFIRMED' \
    kernel/include/installer_storage.h kernel/src/installer_storage_screen.c \
    kernel/src/kernel.c || {
    echo "ITEM-495: решение оператора не доходит до вызывающего" >&2
    exit 1
}
grep -Fq 'production_install_flow_confirm_by_operator' \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c kernel/src/kernel.c || {
    echo "ITEM-495: подтверждение с экрана не применяет установку" >&2
    exit 1
}
grep -Fq '"operator-screen"' kernel/src/production_install_media.c || {
    echo "ITEM-495: запись подтверждения не называет источник" >&2
    exit 1
}
grep -Fq '"media-token"' kernel/src/production_install_media.c || {
    echo "ITEM-495: безлюдное подтверждение потеряло свой источник" >&2
    exit 1
}

# 3. The console token is gone - handler, refusal, source and record - so there
#    is no second, less legible way to authorize a destructive write.  The scan
#    covers the kernel (the product code) and the proof's own gesture: the
#    documents name the removal on purpose, and the checks name it here.
if grep -Fq 'cmd_install' kernel/src/console.c ||
    grep -rFq 'production_install_flow_console_confirm' kernel/ ||
    grep -rFq 'PRODUCTION_INSTALL_MEDIA_REFUSAL_CONSOLE_TOKEN_MISMATCH' kernel/ ||
    grep -rFq 'source=console-token' kernel/ ||
    grep -Fq 'command="INSTALL CONFIRM' kernel/src/production_install_media.c ||
    grep -Fq 'install confirm' scripts/test-qemu-installer-disk-screen.sh; then
    echo "ITEM-495: консольное подтверждение установки осталось в дереве" >&2
    exit 1
fi

# 4. The proof drives the page and the console no longer types anything.
grep -Fq 'WARNING: EVERYTHING ON THIS DISK WILL BE DELETED' \
    scripts/test-qemu-installer-disk-screen.sh || {
    echo "ITEM-495: проба не ждёт страницу предупреждения" >&2
    exit 1
}
grep -Fq 'source=operator-screen' scripts/test-qemu-installer-disk-screen.sh || {
    echo "ITEM-495: проба не проверяет подтверждение с экрана" >&2
    exit 1
}
if grep -Fq 'install confirm' scripts/test-qemu-installer-disk-screen.sh; then
    echo "ITEM-495: проба всё ещё набирает консольную команду" >&2
    exit 1
fi

# 5. The documents tell the operator what he will see.
grep -Fq 'WARNING' docs/hfoc-production-install-flow.md || {
    echo "ITEM-495: документ потока установки не описывает предупреждение" >&2
    exit 1
}
