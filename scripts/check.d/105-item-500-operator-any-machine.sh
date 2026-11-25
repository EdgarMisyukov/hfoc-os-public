#!/usr/bin/env bash
# ITEM-500: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# One medium, any machine: the operator entry asks, the unattended entry refuses.
#
# The owner built a medium declared for the acceptance machine, booted it on a
# laptop and the install did not start: the plan searched the machine for the
# declared serial, did not find it, and refused before the storage screen could
# open - so the machine booted from the stick and nothing said why.
#
# What this slice pins:
#
#   * the declared disk stays the *unattended* authorization: that entry still
#     refuses `TARGET_NOT_FOUND`, and nothing here widens it;
#   * the console entry keeps `ACTION_CONSOLE` with nothing selected when this
#     machine does not carry the declared disk, so the operator's screen opens;
#   * the run says what it could not honour - `declared=NOT_ON_THIS_MACHINE
#     disk=NONE identity=NONE writes=0` - instead of publishing an invented
#     target, and the screen prints the caller's one-line reason above the
#     disks;
#   * an operator entry that refuses before the screen can open paints the
#     reason on the machine's own screen and waits, instead of booting from the
#     medium in silence; leaving the screen on purpose stays a decision with its
#     own record and no page;
#   * the host proofs and the QEMU proof read the same strings, so the screens
#     and the records cannot drift apart from what is proven.

for item500_file in \
    kernel/include/production_install_media.h \
    kernel/src/production_install_media.c \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c \
    kernel/include/installer_storage.h \
    kernel/src/installer_storage.c \
    kernel/include/installer_storage_screen.h \
    kernel/src/installer_storage_screen.c \
    kernel/src/kernel.c \
    tests/production-install-media-host.c \
    tests/installer-storage-host.c \
    scripts/test-qemu-installer-disk-screen.sh \
    docs/hfoc-installation-guide.md \
    docs/hfoc-production-install-flow.md \
    docs/hfoc-installer-storage.md; do
    test -f "$item500_file" || {
        echo "ITEM-500: отсутствует файл: $item500_file" >&2
        exit 1
    }
done

# ------------------------------------ the unattended authorization is intact --
grep -Fq 'plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_NOT_FOUND;' \
    kernel/src/production_install_media.c || {
    echo "ITEM-500: безлюдная запись потеряла отказ TARGET_NOT_FOUND" >&2
    exit 1
}
grep -Fq 'the unattended entry still refuses a machine that does not ' \
    tests/production-install-media-host.c || {
    echo "ITEM-500: host-проба не требует отказа безлюдной записи" >&2
    exit 1
}
grep -Fq 'PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE' \
    kernel/src/production_install_media.c || {
    echo "ITEM-500: план не различает операторскую и безлюдную запись" >&2
    exit 1
}

# ------------------------------------------- the operator entry asks instead --
grep -Fq 'production_install_media_render_awaiting_choice' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-500: поток не публикует запись ожидания выбора" >&2
    exit 1
}
grep -Fq 'declared=NOT_ON_THIS_MACHINE disk=NONE identity=NONE' \
    kernel/src/production_install_media.c || {
    echo "ITEM-500: запись ожидания не называет объявленный диск и отсутствие выбора" >&2
    exit 1
}
# ITEM-509 increment 15: the screen line moved into the one catalog; the
# renderer names the keys and the catalog carries the English words, so the
# English screen (and every QEMU pin below) stays byte for byte.
grep -Fq '"installer.media.not_on_machine_suffix"' \
    kernel/src/production_install_media.c || {
    echo "ITEM-500: строка экрана не называет ключ каталога" >&2
    exit 1
}
grep -Fq 'is not on this machine - choose a disk' \
    kernel/src/appliance_language.c || {
    echo "ITEM-500: строка экрана не называет причину выбора" >&2
    exit 1
}
grep -Fq 'production_install_flow_choice_note' kernel/src/kernel.c || {
    echo "ITEM-500: экран дисков не получает причину выбора" >&2
    exit 1
}
grep -Fq 'installer_storage_session_choose_note' kernel/src/kernel.c || {
    echo "ITEM-500: экран дисков не печатает причину выбора" >&2
    exit 1
}
grep -Fq 'installer_storage_set_note' kernel/include/installer_storage.h \
    kernel/src/installer_storage.c || {
    echo "ITEM-500: модель экрана не принимает причину выбора" >&2
    exit 1
}
grep -Fq 'model->note' kernel/src/installer_storage.c || {
    echo "ITEM-500: строка причины не попадает на экран" >&2
    exit 1
}

# ------------------------------------------- a refusal is visible on screen ---
grep -Fq 'production_install_flow_operator_refusal' kernel/src/kernel.c || {
    echo "ITEM-500: загрузочный цикл не спрашивает причину отказа" >&2
    exit 1
}
grep -Fq 'kernel_installer_refusal_screen' kernel/src/kernel.c || {
    echo "ITEM-500: страница отказа операторской записи отсутствует" >&2
    exit 1
}
grep -Fq 'installer_storage_refusal_show' \
    kernel/include/installer_storage_screen.h \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-500: страница отказа не объявлена или не написана" >&2
    exit 1
}
grep -Fq 'CANCELLED_BY_OPERATOR' kernel/src/production_install_flow.c || {
    echo "ITEM-500: уход оператора с экрана потерял собственный отказ" >&2
    exit 1
}

# ------------------------------------------------------------- the proofs -----
for item500_host_test in \
    'the operator entry asks the person instead of refusing' \
    'declared=NOT_ON_THIS_MACHINE disk=NONE identity=NONE' \
    'the operator entry'"'"'s pending record names the declared disk it does '; do
    grep -Fq "$item500_host_test" tests/production-install-media-host.c || {
        echo "ITEM-500: host-проба не утверждает: $item500_host_test" >&2
        exit 1
    }
done
grep -Fq 'the caller'"'"'s reason is printed above the disks it explains' \
    tests/installer-storage-host.c || {
    echo "ITEM-500: host-проба экрана не проверяет строку причины" >&2
    exit 1
}
for item500_qemu in \
    'Declared disk ' \
    'is not on this machine - choose a disk' \
    'declared=NOT_ON_THIS_MACHINE' \
    'run_refusal_case' \
    'FOREIGN_A' \
    'Installation refused'; do
    grep -Fq "$item500_qemu" scripts/test-qemu-installer-disk-screen.sh || {
        echo "ITEM-500: QEMU-проба не проверяет: $item500_qemu" >&2
        exit 1
    }
done
# ITEM-517: the refusal case used to be the legacy-BIOS one and refused
# `FIRMWARE_UNSUPPORTED`; legacy BIOS installs now, so the honest refusal for
# the no-disk fixture is the plan's own `NO_DISK`.  The pin moves to the new
# reason instead of dropping the case that proves the page and the record.
grep -Fq 'run_refusal_case refusal NO_DISK' \
    scripts/test-qemu-installer-disk-screen.sh || {
    echo "ITEM-500: проба отказа потеряла именованный отказ NO_DISK" >&2
    exit 1
}

# ---------------------------------------------------------- the documents -----
grep -Fq 'One medium, any machine' docs/hfoc-installation-guide.md || {
    echo "ITEM-500: документ установки не описывает универсальную медиа" >&2
    exit 1
}
grep -Fq 'declared=NOT_ON_THIS_MACHINE' docs/hfoc-production-install-flow.md || {
    echo "ITEM-500: документ потока не записывает запись ожидания" >&2
    exit 1
}
grep -Fq 'installer_storage_refusal_show' docs/hfoc-installer-storage.md || {
    echo "ITEM-500: документ экрана не записывает страницу отказа" >&2
    exit 1
}
grep -Fq 'ITEM-500' docs/ROADMAP.md || {
    echo "ITEM-500: роадмап не называет пункт" >&2
    exit 1
}
grep -Fq 'ITEM-500' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-500: очередь не называет пункт" >&2
    exit 1
}
