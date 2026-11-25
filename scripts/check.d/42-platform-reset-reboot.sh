#!/usr/bin/env bash
# ITEM-411: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# A completed install reboots the machine. The flow used to end at `applied`
# and the machine carried on with the ordinary boot of the installation medium,
# so the operator saw the appliance from the stick and had to power-cycle by
# hand; the reset now happens at the end of the success path only, through a
# documented order of mechanisms whose selection is proven on the host and whose
# result is published as a record.
for platform_reset_file in \
    kernel/include/platform_reset.h \
    kernel/src/platform_reset.c \
    kernel/include/acpi.h \
    kernel/src/acpi.c \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c \
    tests/platform-reset-host.c \
    tests/acpi-madt.c \
    scripts/test-qemu-production-install-flow.sh \
    docs/hfoc-production-install-flow.md \
    docs/hfoc-hardware-acceptance.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    test -f "$platform_reset_file" || {
        echo "Отсутствует ITEM-411 file: $platform_reset_file" >&2
        exit 1
    }
done

# The mechanism-selection proof is wired into the canonical host suite.
grep -Fq 'test-platform-reset-host' Makefile || {
    echo "Makefile не содержит test-platform-reset-host" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the Makefile says
grep -Fq '$(MAKE) test-platform-reset-host' Makefile || {
    echo "test-host-suite не вызывает test-platform-reset-host" >&2
    exit 1
}
grep -Fq 'DHFOC_PLATFORM_RESET_HOST_TEST' Makefile || {
    echo "Отсутствует -DHFOC_PLATFORM_RESET_HOST_TEST в test-platform-reset-host" >&2
    exit 1
}

# The order and the bounds are declared once, in the header the proof reads.
for platform_reset_contract in \
    'PLATFORM_RESET_MECHANISM_ACPI' \
    'PLATFORM_RESET_MECHANISM_PCI' \
    'PLATFORM_RESET_MECHANISM_I8042' \
    'PLATFORM_RESET_MECHANISM_FAULT' \
    'PLATFORM_RESET_PCI_PORT UINT16_C(0xCF9)' \
    'PLATFORM_RESET_PCI_HARD_RESET UINT8_C(0x02)' \
    'PLATFORM_RESET_PCI_CPU_RESET UINT8_C(0x04)' \
    'PLATFORM_RESET_I8042_PORT UINT16_C(0x64)' \
    'PLATFORM_RESET_I8042_COMMAND UINT8_C(0xFE)' \
    'PLATFORM_RESET_OPERATOR_DELAY_MS' \
    'platform_reset_plan_build' \
    'platform_reset_plan_primary' \
    'platform_reset_execute'; do
    grep -Fq "$platform_reset_contract" kernel/include/platform_reset.h || {
        echo "platform reset не фиксирует контракт: $platform_reset_contract" >&2
        exit 1
    }
done

# The mechanism is published, and a reset never returns: the record vocabulary
# is what a hardware run quotes to state which mechanism worked.
for platform_reset_record in \
    '[PLATFORM-RESET] plan reset=' \
    '[PLATFORM-RESET] attempt=' \
    '"ACPI"' \
    '"PCI"' \
    '"I8042"' \
    '"FAULT"'; do
    grep -Fq "$platform_reset_record" kernel/src/platform_reset.c || {
        echo "platform reset не публикует: $platform_reset_record" >&2
        exit 1
    }
done

# The ACPI reset register comes from the FADT the kernel already parses, and it
# is read independently of the MADT: a machine without an acceptable MADT still
# states how it wants to be reset.
grep -Fq 'ACPI_FADT_FLAG_RESET_REG_SUP' kernel/include/acpi.h || {
    echo "acpi.h не фиксирует RESET_REG_SUP" >&2
    exit 1
}
grep -Fq 'acpi_fadt_reset' kernel/src/acpi.c || {
    echo "acpi.c не публикует регистр сброса FADT" >&2
    exit 1
}
grep -Fq '"FACP"' kernel/src/acpi.c || {
    echo "Отсутствует сигнатура \"FACP\" в acpi.c" >&2
    exit 1
}

# The reboot sits at the end of the success path, and the medium-removal
# instruction is part of the record the operator reads.
grep -Fq 'medium=REMOVE reset=' kernel/src/production_install_flow.c || {
    echo "install flow не публикует требование вынуть носитель" >&2
    exit 1
}
grep -Fq 'platform_reset_plan_current' kernel/src/production_install_flow.c || {
    echo "install flow не строит план сброса" >&2
    exit 1
}
grep -Fq 'platform_reset_execute' kernel/src/production_install_flow.c || {
    echo "install flow не выполняет сброс" >&2
    exit 1
}

# The product-flow proof observes the reboot: the guest runs with -no-reboot, so
# a guest reset makes QEMU exit by itself, and a run that must not reset has to
# still be running.
grep -Fq -- '-no-reboot' scripts/test-qemu-production-install-flow.sh || {
    echo "proof не запускает QEMU с -no-reboot" >&2
    exit 1
}
grep -Fq 'the machine did not reboot itself after the completed install' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "proof не требует перезагрузки после успешной установки" >&2
    exit 1
}
grep -Fq 'a run that must not reset the machine' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "proof не требует, чтобы отказной прогон не сбрасывал машину" >&2
    exit 1
}
grep -Fq '[PLATFORM-RESET] plan ' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "proof не проверяет записи плана сброса" >&2
    exit 1
}

# ITEM-417: the operator is told on the screen, for as long as the record says.
#
# The install flow runs before the kernel's console exists, so its records reach
# the serial stream and are held back from the display.  On 2026-09-15 the
# acceptance machine rebooted with nothing on its screen saying the install had
# finished, and the delay that was supposed to give the operator five seconds to
# remove the medium was measured at 0.318 s: the wait sampled a PIT channel that
# the installer path had never put into a counting state, and a stopped counter
# reads the same value twice.  Both halves of the promise are contracted here -
# the banner with its countdown on the machine's own display, and the wait the
# record announces - and both are proven in QEMU from the guest's own frame.
for install_notice_file in \
    kernel/include/installer_notice.h \
    kernel/src/installer_notice.c \
    scripts/test-qemu-install-notice.sh; do
    test -f "$install_notice_file" || {
        echo "Отсутствует ITEM-417 file: $install_notice_file" >&2
        exit 1
    }
done
grep -Fq 'test-install-notice-qemu' Makefile || {
    echo "Makefile не содержит test-install-notice-qemu" >&2
    exit 1
}
grep -Fq 'installer_notice_banner' kernel/src/production_install_flow.c || {
    echo "поток установки не рисует баннер" >&2
    exit 1
}
# The record says which of the two outcomes the operator got, and the flow only
# resets through the banner when it painted one.
# The record is built from its parts, so the contract is the parts it is built
# from plus the fact that the proof reads the assembled line.
for install_notice_record in \
    '"s screen="' \
    '"BANNER"' \
    'notice screen=NONE reason='; do
    grep -Fq -- "$install_notice_record" \
        kernel/src/production_install_flow.c || {
        echo "поток установки не называет исход баннера: $install_notice_record" >&2
        exit 1
    }
done
grep -Fq 'screen=(\w+)' scripts/test-qemu-install-notice.sh || {
    echo "proof не читает исход баннера из записи" >&2
    exit 1
}
# The delay owns its time base: it programs channel 0 as a rate generator
# instead of trusting whatever the firmware left behind, and it ends a wait on a
# counter that stops moving instead of parking on it forever.
for platform_reset_delay_contract in \
    'PLATFORM_RESET_PIT_MODE_RATE' \
    'platform_reset_pit_start();' \
    'PLATFORM_RESET_DELAY_STALL_MAX'; do
    grep -Fq -- "$platform_reset_delay_contract" \
        kernel/src/platform_reset.c || {
        echo "задержка сброса не владеет своей шкалой: $platform_reset_delay_contract" >&2
        exit 1
    }
done
# The proof reads the guest's frame while the machine is up, requires the
# countdown to move, and requires the wait the record announced.
for install_notice_proof in \
    'screendump' \
    'the countdown did not change while the machine waited' \
    'the record promised' \
    'HFOC-QEMU-MARKER-TIMEOUT'; do
    grep -Fq -- "$install_notice_proof" scripts/test-qemu-install-notice.sh || {
        echo "proof не проверяет обещание экрана: $install_notice_proof" >&2
        exit 1
    }
done
grep -Fq 'ITEM-417' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-417" >&2
    exit 1
}
