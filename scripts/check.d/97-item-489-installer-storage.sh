#!/usr/bin/env bash
# ITEM-489: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The installer's storage screen: the operator names the disk, by identity.
#
# The install medium has to show an operator what the machine has and let that
# operator - not a default - name the disk an installation will later write to.
# This slice pins the rules that make that safe, and pins them where they are
# decided rather than in the prose around them:
#
#   * the model is a value machine: no block device, no ESP, no installer, no
#     flow, no AHCI/NVMe header, and no write call anywhere in it, so "this item
#     writes nothing" is a property of the code and not a promise;
#   * nothing is chosen automatically.  The inventory leaves `selected` at NONE
#     whatever it found, and a disk is selectable only when its identity and its
#     geometry were both read: a disk nobody can name is a disk nobody may write
#     to, and a refused choice leaves the selection as it was;
#   * the keys the screen advertises are decided by a pure function and proven on
#     the host - the arrow keys and the `w`/`s` the hint prints both move, and a
#     key this screen does not define does nothing;
#   * the unattended flow is unchanged: the install flow publishes the reading
#     (one bounded record, no key read, no waiting screen) and keeps selecting
#     its target through the media declaration as it did;
#   * the reading names the counts, the chosen disk by its discovery id, its
#     state, its partition table and whether a system already lives on it.

for item489_file in \
    kernel/include/installer_storage.h \
    kernel/include/installer_storage_screen.h \
    kernel/src/installer_storage.c \
    kernel/src/installer_storage_screen.c \
    tests/installer-storage-host.c \
    docs/hfoc-installer-storage.md; do
    test -f "$item489_file" || {
        echo "ITEM-489: отсутствует файл: $item489_file" >&2
        exit 1
    }
done

# ------------------------------------------------ this item writes nothing --
# The model cannot reach a device: every header that could write one is absent,
# and no write call appears in the file at all.
for item489_forbidden in \
    block_device.h block_cache.h efi_fat32_esp.h installer_layout.h \
    installer_gpt_preserve.h production_install_flow.h ahci.h nvme.h \
    virtio_pci_identity.h; do
    if grep -Fq "$item489_forbidden" kernel/src/installer_storage.c; then
        echo "ITEM-489: модель хранилища знает про $item489_forbidden и может писать" >&2
        exit 1
    fi
done
if grep -Fq 'write(' kernel/src/installer_storage.c; then
    echo "ITEM-489: в модели хранилища появился вызов записи" >&2
    exit 1
fi
# The screen writes nothing either: it draws and it publishes, and the only
# record it produces is the reading.
for item489_screen_forbidden in block_device.h installer_layout.h ahci.h \
    nvme.h; do
    if grep -Fq "$item489_screen_forbidden" \
        kernel/src/installer_storage_screen.c; then
        echo "ITEM-489: экран хранилища получил доступ к $item489_screen_forbidden" >&2
        exit 1
    fi
done

# ------------------------------------------- nothing is chosen by default --
grep -Fq 'model->selected = INSTALLER_STORAGE_NONE;' \
    kernel/src/installer_storage.c || {
    echo "ITEM-489: inventory выбирает диск за оператора" >&2
    exit 1
}
grep -Fq 'model->disks[model->cursor].state !=' \
    kernel/src/installer_storage.c || {
    echo "ITEM-489: выбор не проверяет состояние строки" >&2
    exit 1
}
grep -Fq 'INSTALLER_STORAGE_DISK_SELECTABLE) {' \
    kernel/src/installer_storage.c || {
    echo "ITEM-489: отказ выбирать нечитаемый диск пропал" >&2
    exit 1
}
# Selectable means identity *and* geometry were read: each of the four terms is
# pinned, so dropping one is a contract change rather than a silent widening.
for item489_readable in \
    'disk->present' \
    'disk->identity[0]' \
    'disk->block_size != 0U' \
    'disk->block_count != 0U' \
    'INSTALLER_STORAGE_DISK_UNREADABLE'; do
    grep -Fq "$item489_readable" kernel/src/installer_storage.c || {
        echo "ITEM-489: признак читаемости диска ослаблен: $item489_readable" >&2
        exit 1
    }
done
# A disk the machine cannot describe is never offered as the disk to install on.
grep -Fq 'installer_storage_can_continue' kernel/src/installer_storage.c || {
    echo "ITEM-489: продолжение не требует явного выбора" >&2
    exit 1
}

# --------------------------------------------------------- the keys are pure --
for item489_key in \
    'installer_storage_key_action' \
    'KEYBOARD_EVENT_UP' \
    'KEYBOARD_EVENT_DOWN' \
    'KEYBOARD_EVENT_ENTER' \
    "character == 'w'" \
    "character == 'W'" \
    "character == 's'" \
    "character == 'S'" \
    "character == 'q'" \
    "character == 'Q'" \
    'INSTALLER_STORAGE_KEY_NONE'; do
    grep -Fq "$item489_key" kernel/src/installer_storage.c || {
        echo "ITEM-489: раскладка клавиш экрана не решена в модели: $item489_key" >&2
        exit 1
    }
done
# The machine half takes the decision from the model instead of re-deciding it.
grep -Fq 'installer_storage_key_action(event.type, event.character)' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-489: экран решает клавиши сам, мимо модели" >&2
    exit 1
}
grep -Fq 'INSTALLER_STORAGE_KEY_LEAVE:' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-489: выйти с экрана нечем" >&2
    exit 1
}
grep -Fq 'installer_storage_expired(deadline)' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-489: сессия экрана не ограничена по времени" >&2
    exit 1
}
# ITEM-495: the session waits for the operator's *decision*, not for a choice
# alone.  It used to end the moment a row was chosen (`can_continue`); it now
# draws the warning page for that row and returns only when the operator
# accepted it, so the pin moved to the confirmation the page performs.  The rule
# it protects is unchanged: no path through this screen reaches a write without
# the person deciding.
grep -Fq 'installer_storage_confirm(&model)' \
    kernel/src/installer_storage_screen.c ||
    grep -Fq 'installer_storage_confirm(' kernel/src/installer_storage_screen.c || {
    echo "ITEM-489/495: сессия экрана не ждёт решения оператора" >&2
    exit 1
}
grep -Fq 'INSTALLER_STORAGE_DECISION_CONFIRMED' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-489/495: у сессии экрана нет подтверждённого исхода" >&2
    exit 1
}

# ------------------------------------------------------ the reading is bounded --
for item489_record in \
    '"[INSTALLER-STORAGE] disks="' \
    '" selectable="' \
    '" selected="' \
    '"NONE"' \
    '" state="' \
    '" table="' \
    '" existing_os="' \
    'INSTALLER_STORAGE_RECORD_BYTES'; do
    grep -Fq -- "$item489_record" kernel/src/installer_storage.c || {
        echo "ITEM-489: запись [INSTALLER-STORAGE] не называет: $item489_record" >&2
        exit 1
    }
done
# The record is assembled whole and refused when it does not fit the caller's
# buffer: half a record is worse than none.
grep -Fq 'line.length + 1U > size' kernel/src/installer_storage.c || {
    echo "ITEM-489: запись хранилища обрезается вместо отказа" >&2
    exit 1
}
# One renderer, two backends: the screen paints through the abstraction when it
# can and prints the same lines when it cannot, and always mirrors the serial
# stream.
for item489_screen in \
    'installer_storage_lines(model, language, lines)' \
    'display_framebuffer_usable()' \
    'display_text(margin, y, lines[index], colour, scale)' \
    'console_write(lines[index])' \
    'serial_sequence_begin()' \
    'local_input_pop_event(&event)' \
    'console_publish_journal(record, length)'; do
    grep -Fq -- "$item489_screen" kernel/src/installer_storage_screen.c || {
        echo "ITEM-489: экран хранилища не делает: $item489_screen" >&2
        exit 1
    }
done

# ---------------------------------------- the unattended install is unchanged --
grep -Fq '#include "installer_storage_screen.h"' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-489: поток установки не публикует чтение хранилища" >&2
    exit 1
}
grep -Fq 'installer_storage_publish(&production_install_flow.report);' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-489: публикация чтения хранилища не вызывается" >&2
    exit 1
}
# No session is opened by the unattended flow: it reads no key and waits for
# nobody.  The interactive screen is offered by the item that owns an operator's
# install session, together with the planner and the executor.
if grep -Fq 'installer_storage_session' \
    kernel/src/production_install_flow.c; then
    echo "ITEM-489: безлюдный поток установки ждёт оператора" >&2
    exit 1
fi
# The flow still selects its target through the media declaration, as before.
grep -Fq 'production_install_flow.plan.target.disk_id' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-489: выбор диска потоком установки пропал" >&2
    exit 1
}

# ---------------------------------------------------------------- the evidence --
for item489_case in \
    'test_an_empty_machine_is_honest' \
    'test_one_disk_is_still_not_chosen_for_the_operator' \
    'test_an_unreadable_disk_is_never_selectable' \
    'test_navigation_stops_at_the_ends' \
    'test_a_machine_with_an_existing_system_says_so' \
    'test_the_model_is_bounded' \
    'test_the_keys_the_screen_advertises_work'; do
    grep -Fq "$item489_case" tests/installer-storage-host.c || {
        echo "ITEM-489: host-проба хранилища не доказывает: $item489_case" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the Makefile text is literal, not expanded
grep -Fq '$(MAKE) test-installer-storage-host' Makefile || {
    echo "ITEM-489: host-проба хранилища не в suite" >&2
    exit 1
}
# The keys the hint prints are the keys the proof drives: a screen that
# advertises a key it does not answer is a screen an operator cannot use.
# ITEM-509 increment 16 moved the hint into the one catalog; the renderer names
# the key and the catalog carries the English words.
grep -Fq '"installer.storage.move_hint"' kernel/src/installer_storage.c || {
    echo "ITEM-489: подсказка клавиш на экране пропала" >&2
    exit 1
}
grep -Fq 'W/S or arrow keys move' kernel/src/appliance_language.c || {
    echo "ITEM-489: каталог не несёт подсказку клавиш экрана" >&2
    exit 1
}
grep -Fq "KEYBOARD_EVENT_CHARACTER, 'w'" tests/installer-storage-host.c || {
    echo "ITEM-489: host-проба не нажимает W/S, которые обещает подсказка" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item489_doc_word in ITEM-489 identity 'never by a name' NOT_IMPLEMENTED \
    'NOT TESTED' unattended; do
    grep -Fq -- "$item489_doc_word" docs/hfoc-installer-storage.md || {
        echo "docs/hfoc-installer-storage.md не фиксирует ITEM-489:" \
            "$item489_doc_word" >&2
        exit 1
    }
done

# ------------------------------------------------- ITEM-490: экран обязателен --
# Операторская запись медиа открывает экран дисков и ждёт человека; выбранный
# диск становится целью записи, а уход с экрана отказывает прогону с нулём
# записей.  Безлюдная запись не меняется и экрана не показывает.

# Модель умеет выбирать по идентификатору с тем же fail-closed правилом.
grep -Fq 'bool installer_storage_select_id(' kernel/src/installer_storage.c || {
    echo "ITEM-490: выбор диска по id пропал" >&2
    exit 1
}
grep -Fq 'installer_storage_select_id(&model, disk_id)' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-490: чтение выбора не применяет правило модели" >&2
    exit 1
}
# Сессия отдаёт копию выбранной строки: указатель на локальную модель был бы
# висячим к моменту записи.
grep -Fq 'installer_storage_copy_choice(&model, chosen)' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-490: сессия отдаёт указатель на свою локальную модель" >&2
    exit 1
}
grep -Fq 'INSTALLER_STORAGE_NO_DEADLINE' kernel/include/installer_storage_screen.h || {
    echo "ITEM-490: у обязательного экрана нет режима без таймаута" >&2
    exit 1
}
# Экран обслуживает ввод так же, как главный цикл: USB-отчёты иначе не доходят.
grep -Fq 'local_input_service();' kernel/src/installer_storage_screen.c || {
    echo "ITEM-490: экран не обслуживает локальный ввод" >&2
    exit 1
}
# Уход с экрана — не выбор.
grep -Fq 'return false;' kernel/src/installer_storage_screen.c || {
    echo "ITEM-490: уход с экрана не отличается от выбора" >&2
    exit 1
}

# Поток: экран показывается только на операторской записи, выбор переопределяет
# декларацию, уход отказывает с CANCELLED_BY_OPERATOR.
for item490_flow in \
    'production_install_flow_awaiting_choice' \
    'production_install_flow_choose_disk' \
    'production_install_flow_cancel_by_operator' \
    'production_install_media_declaration_set_target' \
    'PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE' \
    'media_storage = *media'; do
    grep -Fq "$item490_flow" kernel/src/production_install_flow.c || {
        echo "ITEM-490: поток не умеет: $item490_flow" >&2
        exit 1
    }
done
grep -Fq 'PRODUCTION_INSTALL_MEDIA_REFUSAL_CANCELLED_BY_OPERATOR' \
    kernel/include/production_install_media.h || {
    echo "ITEM-490: у отказа по уходу с экрана нет имени" >&2
    exit 1
}
grep -Fq 'return "CANCELLED_BY_OPERATOR";' kernel/src/production_install_media.c || {
    echo "ITEM-490: отказ CANCELLED_BY_OPERATOR не публикуется" >&2
    exit 1
}
# Копия медиа: поток живёт дольше стека загрузочной функции.
grep -Fq 'struct production_install_flow_media media_storage;' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-490: поток держит указатель на стек вызывающего" >&2
    exit 1
}

# Ядро: экран вызывается из главного цикла — там, где локальный ввод живой
# (поток установки выполняется до включения прерываний).
grep -Fq 'kernel_installer_disk_screen();' kernel/src/kernel.c || {
    echo "ITEM-490: экран выбора диска не вызывается из главного цикла" >&2
    exit 1
}
grep -Fq 'INSTALLER_STORAGE_NO_DEADLINE,' kernel/src/kernel.c || {
    echo "ITEM-490: экран в главном цикле не ждёт оператора" >&2
    exit 1
}
grep -Fq 'production_install_flow_cancel_by_operator();' kernel/src/kernel.c || {
    echo "ITEM-490: уход с экрана не отказывает прогону" >&2
    exit 1
}

# Медиа: какая запись Limine предлагается первой, решает сборка.
grep -Fq 'HFOC_INSTALL_MEDIA_ENTRY' Makefile || {
    echo "ITEM-490: порядок записей медиа не выбирается сборкой" >&2
    exit 1
}
grep -Fq 'HFOC_INSTALL_MEDIA_ENTRY' scripts/hfoc-install-media-stage.sh || {
    echo "ITEM-490: стадия медиа не читает порядок записей" >&2
    exit 1
}
grep -Fq "want='hfoc.install=console'" scripts/hfoc-install-media-stage.sh || {
    echo "ITEM-490: операторская запись не поднимается первой" >&2
    exit 1
}
grep -Fq 'production-installer-console-media' Makefile || {
    echo "ITEM-490: операторской медиа нет" >&2
    exit 1
}
grep -Fq 'HFOC_INSTALL_MEDIA_ENTRY ?= unattended' Makefile || {
    echo "ITEM-490: безлюдная запись перестала быть записью по умолчанию" >&2
    exit 1
}

# Проба: выбор оператора, отказ по уходу, неприкосновенность объявленного диска.
grep -Fq 'scripts/test-qemu-installer-disk-screen.sh' Makefile || {
    echo "ITEM-490: QEMU-проба экрана не зарегистрирована" >&2
    exit 1
}
for item490_proof in \
    'hfoc.install=console' \
    'selected=2 state=SELECTABLE' \
    'complete disk=2 identity=' \
    'CANCELLED_BY_OPERATOR writes=0' \
    'the declared disk was written' \
    'a cancelled run wrote a disk'; do
    grep -Fq "$item490_proof" scripts/test-qemu-installer-disk-screen.sh || {
        echo "ITEM-490: проба не доказывает: $item490_proof" >&2
        exit 1
    }
done
grep -Fq 'test-installer-disk-screen-qemu' Makefile || {
    echo "ITEM-490: цель QEMU-пробы экрана пропала" >&2
    exit 1
}

# Документ.
for item490_doc_word in ITEM-490 'hfoc.install=console' CANCELLED_BY_OPERATOR \
    'NOT TESTED'; do
    grep -Fq -- "$item490_doc_word" docs/hfoc-installer-storage.md || {
        echo "docs/hfoc-installer-storage.md не фиксирует ITEM-490:" \
            "$item490_doc_word" >&2
        exit 1
    }
done
