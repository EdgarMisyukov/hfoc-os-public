#!/usr/bin/env bash
# ITEM-514: contracted slice of the HFOC OS check suite. Sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-514: the install mode - the scheme screen between the disk and the write.
#
# The owner's order (2026-10-10 03:20 UTC) is the ordinary installers' model:
# the operator chooses the disk and then a named scheme, each scheme says what
# it does and how much room it needs, and a full summary of the plan is the
# last page before the first write.  The claims pinned here:
#
#   * four schemes are listed always - alongside, free space, erase, manual -
#     each with its one-line description and its stable refusal name when it
#     cannot be used; a scheme that cannot be used refuses by name and never
#     silently selects another one, and the manual row says the boundary out
#     loud (no full manual partitioning, no shrink, no LVM or RAID);
#   * the default is the medium's declaration when it made one, otherwise the
#     disk's own suggestion (alongside when a system is here, erase otherwise);
#     it is a cursor position, not a decision - the model selects nothing
#     until an operator's ENTER;
#   * the operator decides, including more destructively than the medium
#     declared: the declaration is a default, not a lock, and the records name
#     both sources (`scheme declared=… chosen=…` and the layout record's
#     `chosen=`);
#   * the destructive scheme draws the ITEM-495 disk warning page before it is
#     chosen, and a disk whose table could not be read says so instead of
#     looking empty (R03 of the 2026-10-09 audit);
#   * RELEASE_CONFLICT is judged against the mode the run will use, not against
#     the medium's declaration;
#   * the summary renders the plan the installer's own `installer_layout_contract_*`
#     planner builds - the same calls `production_installer_plan_layout()` makes
#     - so what the operator reads is the plan the writer executes, and a plan
#     the contract refuses is a named refusal, never a different scheme.
for item514_file in \
    kernel/include/installer_scheme.h \
    kernel/include/installer_storage.h \
    kernel/include/installer_storage_summary.h \
    kernel/src/installer_storage.c \
    kernel/src/installer_storage_summary.c \
    kernel/include/installer_storage_screen.h \
    kernel/src/installer_storage_screen.c \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c \
    kernel/src/kernel.c \
    kernel/src/appliance_language.c \
    tests/installer-storage-host.c \
    scripts/test-qemu-installer-disk-screen.sh \
    scripts/test-qemu-production-install-flow-preserve.sh \
    docs/hfoc-production-install-flow.md \
    docs/hfoc-installer-storage.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item514_file ]] || {
        echo "ITEM-514: отсутствует $item514_file" >&2
        exit 1
    }
done

# ------------------------------------------------- the four schemes, one list --
for item514_scheme in ALONGSIDE FREE_SPACE ERASE MANUAL; do
    grep -Fq "INSTALLER_STORAGE_SCHEME_$item514_scheme" \
        kernel/include/installer_scheme.h || {
        echo "ITEM-514: в модели нет схемы $item514_scheme" >&2
        exit 1
    }
done
grep -Fq '#define INSTALLER_STORAGE_SCHEMES_MAX 4U' \
    kernel/include/installer_scheme.h || {
    echo "ITEM-514: список схем не ограничен четырьмя" >&2
    exit 1
}
grep -Fq 'installer_storage_schemes_inventory(' \
    kernel/include/installer_storage.h kernel/src/installer_storage.c || {
    echo "ITEM-514: модель схем не собирается из отчёта" >&2
    exit 1
}
for item514_key in installer.scheme.alongside installer.scheme.free_space \
    installer.scheme.erase installer.scheme.manual; do
    grep -Fq "\"$item514_key\"" kernel/src/appliance_language.c || {
        echo "ITEM-514: каталог не несёт схему $item514_key" >&2
        exit 1
    }
done
grep -Fq '"  No full manual partitioning, no shrink, no LVM or RAID."' \
    kernel/src/appliance_language.c || {
    echo "ITEM-514: экран не называет границы ручного режима" >&2
    exit 1
}

# ------------------------------------------- a refusal is named, never silent --
for item514_refusal in NO_EXISTING_SYSTEM NOT_ENOUGH_FREE_SPACE MEDIA_TOO_SMALL \
    NO_RELEASABLE_PARTITION NO_EFI_SYSTEM_PARTITION; do
    grep -Fq "\"$item514_refusal\"" kernel/src/installer_storage.c || {
        echo "ITEM-514: отказ $item514_refusal не назван" >&2
        exit 1
    }
done
grep -Fq 'if (!model->rows[model->cursor].available) {' \
    kernel/src/installer_storage.c || {
    echo "ITEM-514: недоступная схема может быть выбрана" >&2
    exit 1
}
grep -Fq 'installer_storage_scheme_refusal_lines(' \
    kernel/include/installer_storage.h kernel/src/installer_storage.c \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-514: страница именованного отказа не отрисована" >&2
    exit 1
}

# ------------------------------------ declaration default, operator decides --
grep -Fq 'production_install_flow_declared_scheme' \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c || {
    echo "ITEM-514: поток не называет объявленную схему" >&2
    exit 1
}
grep -Fq 'production_install_flow_default_scheme' \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c kernel/src/kernel.c || {
    echo "ITEM-514: схема по умолчанию не доходит до экрана" >&2
    exit 1
}
grep -Fq 'production_install_flow_choose_scheme' \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c kernel/src/kernel.c || {
    echo "ITEM-514: выбор оператора не доходит до потока" >&2
    exit 1
}
grep -Fq ' source=operator writes=0\n' kernel/src/production_install_flow.c || {
    echo "ITEM-514: запись схемы не называет источник" >&2
    exit 1
}
grep -Fq 'production_install_flow_line_append(&line, " chosen=");' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-514: запись раскладки не называет выбранную схему" >&2
    exit 1
}
if grep -Fq 'if (production_install_flow.declaration.layout_declared &&' \
    kernel/src/production_install_flow.c; then
    echo "ITEM-514: конфликт отдачи раздела всё ещё считается по объявлению" >&2
    exit 1
fi
grep -Fq 'selected == PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK) {' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-514: конфликт отдачи раздела не считается по действующему режиму" >&2
    exit 1
}

# ------------------------------------------------ the summary is the plan -----
grep -Fq 'installer_storage_summary_build(' \
    kernel/include/installer_storage_summary.h \
    kernel/src/installer_storage_summary.c || {
    echo "ITEM-514: сводка плана не собирается" >&2
    exit 1
}
for item514_planner in installer_layout_contract_plan_clean \
    installer_layout_contract_plan_dual_boot \
    installer_layout_contract_plan_release; do
    grep -Fq "$item514_planner(" kernel/src/installer_storage_summary.c || {
        echo "ITEM-514: сводка не зовёт планировщик $item514_planner" >&2
        exit 1
    }
done
grep -Fq 'installer_layout_contract_validate_summary(' \
    tests/installer-storage-host.c || {
    echo "ITEM-514: проба не сверяет показанный план с записываемым" >&2
    exit 1
}
grep -Fq 'installer_storage_session_summary(' \
    kernel/include/installer_storage_screen.h \
    kernel/src/installer_storage_screen.c kernel/src/kernel.c || {
    echo "ITEM-514: страница сводки не подключена" >&2
    exit 1
}
grep -Fq 'installer_storage_session_scheme(' \
    kernel/include/installer_storage_screen.h \
    kernel/src/installer_storage_screen.c kernel/src/kernel.c || {
    echo "ITEM-514: экран схем не подключён" >&2
    exit 1
}

# ------------------------------------- the erase page and the unknown contents --
grep -Fq 'installer_storage_warning_lines(&model->disk, language,' \
    kernel/src/installer_storage.c || {
    echo "ITEM-514: разрушительная схема не рисует страницу предупреждения" >&2
    exit 1
}
grep -Fq 'installer_storage_scheme_erase_warning_lines(' \
    kernel/include/installer_storage.h kernel/src/installer_storage.c \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-514: страница стирания не собрана моделью" >&2
    exit 1
}
grep -Fq 'test_the_erase_page_names_unknown_contents();' \
    tests/installer-storage-host.c || {
    echo "ITEM-514: проба нечитаемой таблицы на странице стирания отсутствует" >&2
    exit 1
}
grep -Fq 'model->contents_unknown' kernel/src/installer_storage.c \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-514: нечитаемая таблица не доходит до экрана" >&2
    exit 1
}
grep -Fq '"installer.scheme.unknown_contents"' \
    kernel/src/installer_storage_screen.c kernel/src/appliance_language.c || {
    echo "ITEM-514: неизвестное содержимое диска не названо" >&2
    exit 1
}
grep -Fq 'production_install_flow_contents_unknown' \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c kernel/src/kernel.c || {
    echo "ITEM-514: поток не сообщает о нечитаемой таблице" >&2
    exit 1
}

# --------------------------------------------------------------- proofs --------
grep -Fq 'Choose the installation scheme' \
    scripts/test-qemu-installer-disk-screen.sh \
    scripts/test-qemu-production-install-flow-preserve.sh || {
    echo "ITEM-514: пробы не ждут экран схем" >&2
    exit 1
}
grep -Fq 'Confirm the installation plan' \
    scripts/test-qemu-installer-disk-screen.sh \
    scripts/test-qemu-production-install-flow-preserve.sh || {
    echo "ITEM-514: пробы не ждут сводку плана" >&2
    exit 1
}
grep -Fq 'layout mode=whole-disk reason=operator-erase declared=0 chosen=erase' \
    scripts/test-qemu-installer-disk-screen.sh || {
    echo "ITEM-514: проба стирания не называет выбранную схему" >&2
    exit 1
}
grep -Fq 'declared=0 released_index=2 chosen=manual' \
    scripts/test-qemu-production-install-flow-preserve.sh || {
    echo "ITEM-514: проба отдачи раздела не называет выбранную схему" >&2
    exit 1
}

# ------------------------------------------------------------ documents --------
grep -Fq 'ITEM-514' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md \
    docs/ROADMAP.md docs/ARCHITECTURE.md || {
    echo "ITEM-514: документы не записали пункт" >&2
    exit 1
}
grep -Fq 'installer.scheme.' docs/hfoc-installer-storage.md || {
    echo "ITEM-514: документ экрана хранилища не описывает схемы" >&2
    exit 1
}
grep -Fq 'chosen=' docs/hfoc-production-install-flow.md || {
    echo "ITEM-514: документ потока не описывает запись выбора" >&2
    exit 1
}
