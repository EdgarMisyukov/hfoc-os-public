#!/usr/bin/env bash
# ITEM-509 increment 27 (509.7c-2): contracted slice of the HFOC OS check suite.
#           Sourced in pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 27 (509.7c-2): the last surface of the item - the source of
# the language on the installation medium.  The medium declares it as a property
# of the medium (`HFOC-INSTALL-LANGUAGE=en|ru` in `boot/hfoc-install.token`,
# written from `HFOC_INSTALL_MEDIA_LANGUAGE`), the boot parses it through the one
# catalog owner before the first screen, every installer screen and the console
# read it, and the install writes it into the installed system's state and reads
# it back through the installed system's own reader.  A medium that declares
# nothing - every medium that existed before the directive - keeps the default
# language and its exact bytes.
for item509_inc27_file in \
    kernel/include/production_install_media.h \
    kernel/src/production_install_media.c \
    kernel/include/production_installer_state.h \
    kernel/src/production_installer_state.c \
    kernel/src/production_install_flow.c \
    kernel/src/kernel.c \
    scripts/hfoc-install-media-stage.sh \
    scripts/test-qemu-production-install-flow.sh \
    tests/production-install-media-host.c \
    tests/production-installer-orchestration-host.c \
    docs/hfoc-language-surfaces.md \
    docs/hfoc-production-install-flow.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc27_file ]] || {
        echo "ITEM-509 inc27: отсутствует $item509_inc27_file" >&2
        exit 1
    }
done

# 1. The declaration grammar: the directive is the medium's property, the value
#    is the catalog's two codes through the one owner, an absent directive is
#    the default language and a misspelled one refuses the whole declaration.
grep -Fq '#define PRODUCTION_INSTALL_MEDIA_LANGUAGE_DIRECTIVE "HFOC-INSTALL-LANGUAGE="' \
    kernel/include/production_install_media.h || {
    echo "ITEM-509 inc27: директива языка не объявлена" >&2
    exit 1
}
grep -Fq 'appliance_language_parse(value, value_length,' \
    kernel/src/production_install_media.c || {
    echo "ITEM-509 inc27: значение языка разбирает не каталог" >&2
    exit 1
}
grep -Fq 'declaration->language_declared = true;' \
    kernel/src/production_install_media.c || {
    echo "ITEM-509 inc27: объявленный язык не отмечается" >&2
    exit 1
}
grep -Fq 'language_declared;' kernel/include/production_install_media.h || {
    echo "ITEM-509 inc27: поле языка не объявлено" >&2
    exit 1
}
grep -Fq 'LANGUAGE=${HFOC_INSTALL_MEDIA_LANGUAGE:-}' \
    scripts/hfoc-install-media-stage.sh || {
    echo "ITEM-509 inc27: стенд-скрипт не читает язык носителя" >&2
    exit 1
}
grep -Fq "printf 'HFOC-INSTALL-LANGUAGE=%s\\n' \"\$LANGUAGE\"" \
    scripts/hfoc-install-media-stage.sh || {
    echo "ITEM-509 inc27: директива не попадает в токен носителя" >&2
    exit 1
}
# The kernel owns the vocabulary: the stage script must not parse `en`/`ru`
# itself, or a typo would be caught at build time instead of refusing the medium.
if grep -Eq '^[[:space:]]*(en|ru)[[:space:]]*\|[[:space:]]*(en|ru)\)' \
    scripts/hfoc-install-media-stage.sh; then
    echo "ITEM-509 inc27: у языка появился второй разборщик" >&2
    exit 1
fi

# 2. The declared language reaches every installer screen and the console: one
#    parse before the first screen, one accessor, and no screen left on the
#    default language as a stand-in for the declaration.
grep -Fq 'kernel_installer_language_prepare(&media);' kernel/src/kernel.c || {
    echo "ITEM-509 inc27: язык носителя не разбирается до первого экрана" >&2
    exit 1
}
# ITEM-514 added the scheme and summary screens, so the count moved 6 -> 8:
# every installer screen, the foundation page included, reads the declaration.
if [[ $(grep -Fc 'kernel_installer_declared_language()' kernel/src/kernel.c) != 8 ]]; then
    echo "ITEM-509 inc27: не все экраны читают язык объявления" >&2
    exit 1
fi
grep -Fq 'display_show_installer_foundation(kernel_installer_declared_language());' \
    kernel/src/kernel.c || {
    echo "ITEM-509 inc27: фундаментный экран не читает язык объявления" >&2
    exit 1
}
grep -Fq 'console_publish_language(kernel_installer_declared_language(), false);' \
    kernel/src/kernel.c || {
    echo "ITEM-509 inc27: консоль установочной загрузки не публикует язык" >&2
    exit 1
}
grep -Fq 'console_bind_language(NULL);' kernel/src/kernel.c || {
    echo "ITEM-509 inc27: на носителе привязывается запись, которой нет" >&2
    exit 1
}
grep -Fq 'installer_notice_banner(' kernel/src/production_install_flow.c || {
    echo "ITEM-509 inc27: баннер установки не вызывается" >&2
    exit 1
}
grep -Fq 'production_install_flow.declaration.language,' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-509 inc27: баннер установки не читает язык объявления" >&2
    exit 1
}

# 3. The install persists the language immediately after the state exists, reads
#    it back through the installed system's own reader, publishes the record and
#    fails closed on a mismatch.  The default language writes nothing at all.
grep -Fq 'production_installer_state_commit_language(' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-509 inc27: установка не записывает язык" >&2
    exit 1
}
grep -Fq 'production_installer_state_commit_language(' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-509 inc27: запись языка не живёт у владельца состояния" >&2
    exit 1
}
grep -Fq 'appliance_persistent_config_set_language(system_config, language);' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-509 inc27: язык пишется не системной записью" >&2
    exit 1
}
grep -Fq 'appliance_persistent_config_language(system_config, &readback);' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-509 inc27: язык не читается обратно читателем системы" >&2
    exit 1
}
grep -Fq 'return readback == language ? PRODUCTION_INSTALLER_OK' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-509 inc27: несовпадение чтения не отвергает установку" >&2
    exit 1
}
grep -Fq 'if (production_install_flow.declaration.language !=' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-509 inc27: язык по умолчанию пишется в запись" >&2
    exit 1
}
grep -Fq '"[PRODUCTION-INSTALL] language="' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-509 inc27: запись о языке не публикуется" >&2
    exit 1
}
grep -Fq '" persisted=VERIFIED\n"' kernel/src/production_install_flow.c || {
    echo "ITEM-509 inc27: запись не подтверждает чтение обратно" >&2
    exit 1
}

# 4. The proofs: the host side drives the grammar, the record and the installed
#    reader; the QEMU side installs from the Russian medium and reads the record
#    from the raw persistence volume.
grep -Fq 'the declared language is parsed as Russian' \
    tests/production-install-media-host.c || {
    echo "ITEM-509 inc27: проба не разбирает объявленный язык" >&2
    exit 1
}
grep -Fq 'an absent language directive keeps the default language' \
    tests/production-install-media-host.c || {
    echo "ITEM-509 inc27: проба не проверяет отсутствие директивы" >&2
    exit 1
}
grep -Fq 'a language the catalog does not name is refused' \
    tests/production-install-media-host.c || {
    echo "ITEM-509 inc27: проба не отвергает чужой язык" >&2
    exit 1
}
grep -Fq 'a declared language is published in the media record' \
    tests/production-install-media-host.c || {
    echo "ITEM-509 inc27: проба не пинит запись объявления" >&2
    exit 1
}
grep -Fq 'the installed system'"'"'s own reader returns the declared language' \
    tests/production-installer-orchestration-host.c || {
    echo "ITEM-509 inc27: проба не читает запись установленной системой" >&2
    exit 1
}
grep -Fq 'a fresh installed state reads the default language from absence' \
    tests/production-installer-orchestration-host.c || {
    echo "ITEM-509 inc27: проба не проверяет язык по умолчанию" >&2
    exit 1
}
grep -Fq 'test_state_commits_the_declared_language();' \
    tests/production-installer-orchestration-host.c || {
    echo "ITEM-509 inc27: проба языка не запускается" >&2
    exit 1
}
grep -Fq 'run_boot "language" uefi "$LANGUAGE_IMAGE"' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-509 inc27: QEMU-проба не ставит с русского носителя" >&2
    exit 1
}
grep -Fq '"[PRODUCTION-INSTALL] language=ru persisted=VERIFIED" "" reset' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-509 inc27: QEMU-проба не ждёт запись о языке" >&2
    exit 1
}
grep -Fq 'b"v1:system.language=ru" not in region' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-509 inc27: QEMU-проба не читает запись с диска" >&2
    exit 1
}
grep -Fq 'production-installer-language-media' Makefile || {
    echo "ITEM-509 inc27: русский носитель не собирается" >&2
    exit 1
}
grep -Fq "HFOC-INSTALL-LANGUAGE=ru' \$(PRODUCTION_INSTALLER_LANGUAGE_ISO_ROOT)/boot/hfoc-install.token" \
    Makefile || {
    echo "ITEM-509 inc27: токен русского носителя не проверяется" >&2
    exit 1
}

# 5. The item's surfaces and boundaries are recorded where the next window
#    reads them.
grep -Fq '509.7c-2' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc27: поверхности языка не называют 509.7c-2" >&2
    exit 1
}
grep -Fq 'HFOC-INSTALL-LANGUAGE' docs/hfoc-production-install-flow.md || {
    echo "ITEM-509 inc27: документ потока установки не называет директиву" >&2
    exit 1
}
grep -Fq 'ITEM-509 increment 27' docs/ROADMAP.md || {
    echo "ITEM-509 inc27: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-509 increment 27' docs/ARCHITECTURE.md || {
    echo "ITEM-509 inc27: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-509 increment 27' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc27: очередь не называет инкремент" >&2
    exit 1
}
# ITEM-512 moved the queue's `## Current item` heading on to its own item, so
# this pin names the ITEM-509 section the queue keeps instead of the heading,
# which belongs to whichever item is running.
grep -Fq 'ITEM-509 increment 27' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc27: очередь не называет ITEM-509" >&2
    exit 1
}
