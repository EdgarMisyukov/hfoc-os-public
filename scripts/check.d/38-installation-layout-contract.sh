#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Production installation layout contract (ITEM-391). The four install roles
# have declared product sizes instead of caller-supplied ranges, the plan is
# derived from them through the existing ITEM-340/ITEM-341 planners, and the
# plan preview plus the [INSTALLER-LAYOUT] serial record publish the numbers.
for layout_contract_file in \
    kernel/include/installer_layout_contract.h \
    kernel/src/installer_layout_contract.c \
    kernel/include/production_installer_layout.h \
    kernel/src/production_installer_layout.c \
    tests/installer-layout-contract-host.c \
    scripts/test-qemu-production-installer.sh \
    docs/hfoc-installer-layout.md; do
    test -f "$layout_contract_file" || {
        echo "Отсутствует ITEM-391 file: $layout_contract_file" >&2
        exit 1
    }
done
grep -Fq 'test-installer-layout-contract-host' Makefile || {
    echo "Makefile не содержит test-installer-layout-contract-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-installer-layout-contract-host' Makefile || {
    echo "test-host-suite не вызывает test-installer-layout-contract-host" >&2
    exit 1
}
grep -Fq 'INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL' \
    kernel/include/installer_layout_contract.h || {
    echo "layout contract не фиксирует minimum-media отказ" >&2
    exit 1
}
grep -Fq 'INSTALLER_LAYOUT_CONTRACT_EXISTING_SYSTEM_OVERLAP' \
    kernel/include/installer_layout_contract.h || {
    echo "layout contract не фиксирует overlap с прежней системой" >&2
    exit 1
}
grep -Fq 'INSTALLER_LAYOUT_CONTRACT_NO_RESERVE' \
    kernel/include/installer_layout_contract.h || {
    echo "layout contract не фиксирует отказ без места прежней системе" >&2
    exit 1
}
# The frozen ITEM-340 planner keeps its capacity and its entry points: the
# contract derives requests, it does not replace the planner.  ITEM-517 raised
# the capacity from 4 to 5 for the legacy-BIOS role set (BIOS boot plus FAT
# boot volume plus the three HFOC roles); the UEFI layouts still plan exactly
# four roles, which the layout-contract host proof pins separately.
grep -Fq 'INSTALLER_LAYOUT_PARTITION_CAPACITY 5U' \
    kernel/include/installer_layout.h || {
    echo "planner partition capacity изменилась" >&2
    exit 1
}
for layout_planner_entry in \
    installer_layout_plan_whole_disk \
    installer_layout_plan_manual \
    installer_layout_execute; do
    grep -Fq "$layout_planner_entry" kernel/include/installer_layout.h || {
        echo "planner entry point отсутствует: $layout_planner_entry" >&2
        exit 1
    }
done
grep -Fq 'installer_layout_plan_whole_disk' \
    kernel/src/installer_layout_contract.c || {
    echo "layout contract не строит план через ITEM-340 planner" >&2
    exit 1
}
grep -Fq 'installer_dual_boot_plan_selected' \
    kernel/src/installer_layout_contract.c || {
    echo "layout contract не использует ITEM-341 dual-boot planner" >&2
    exit 1
}
# Production publication: the controller plans the contracted layout and the
# preview carries it; the record is written inside one serial sequence.
grep -Fq 'production_installer_plan_layout' \
    kernel/src/production_installer.c || {
    echo "production installer не строит contracted layout" >&2
    exit 1
}
grep -Fq 'installer_layout_contract_plan_clean' \
    kernel/src/production_installer.c || {
    echo "production installer не вызывает clean layout contract" >&2
    exit 1
}
grep -Fq 'installer_layout_contract_plan_dual_boot' \
    kernel/src/production_installer.c || {
    echo "production installer не вызывает dual-boot layout contract" >&2
    exit 1
}
grep -Fq 'installer_layout_contract_render_preview' \
    kernel/src/production_installer.c || {
    echo "plan preview не несёт contracted sizes" >&2
    exit 1
}
grep -Fq 'serial_sequence_begin' kernel/src/production_installer_layout.c || {
    echo "layout record не защищён serial_sequence_begin" >&2
    exit 1
}
grep -Fq 'serial_sequence_end' kernel/src/production_installer_layout.c || {
    echo "layout record не защищён serial_sequence_end" >&2
    exit 1
}
grep -Fq 'serial_write(record)' kernel/src/production_installer_layout.c || {
    echo "layout record не публикуется" >&2
    exit 1
}
grep -Fq 'production_installer_publish_layout_record' \
    kernel/src/production_installer_qemu.c || {
    echo "QEMU proof не публикует layout record" >&2
    exit 1
}
grep -Fq 'production_installer_plan_layout' \
    kernel/src/production_installer_qemu.c || {
    echo "QEMU proof не использует production entry point" >&2
    exit 1
}
grep -Fq 'layout contract proof OK' Makefile || {
    echo "Makefile не проверяет layout contract marker" >&2
    exit 1
}
grep -Fq 'layout contract proof OK' \
    scripts/test-qemu-production-installer.sh || {
    echo "QEMU proof не проверяет layout contract marker" >&2
    exit 1
}
grep -Fq '[INSTALLER-LAYOUT] plan ' \
    scripts/test-qemu-production-installer.sh || {
    echo "QEMU proof не проверяет layout record" >&2
    exit 1
}

# The declared numbers are single-sourced: the check compiles the header and
# requires the evaluated constants, the host proof's pins and the published
# numbers in the QEMU proof to agree with them.
layout_contract_dir=$(mktemp -d)
trap 'rm -rf "$layout_contract_dir"' EXIT
cat >"$layout_contract_dir/values.c" <<'EOF'
#include <stdio.h>

#include "installer_layout_contract.h"

int main(void)
{
    printf("%llu %llu %llu %llu %llu %llu %llu\n",
           (unsigned long long)
               INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA,
           (unsigned long long)
               INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA,
           (unsigned long long)
               INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA,
           (unsigned long long)
               INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA,
           (unsigned long long)INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA,
           (unsigned long long)
               INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA,
           (unsigned long long)
               INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA);
    return 0;
}
EOF
gcc -std=gnu11 -Wall -Wextra -Werror -Wpedantic -Ikernel/include \
    "$layout_contract_dir/values.c" -o "$layout_contract_dir/values" || {
    echo "layout contract header не компилируется отдельно" >&2
    exit 1
}
read -r layout_esp layout_system layout_persistence layout_recovery \
    layout_footprint layout_dual layout_minimum \
    < <("$layout_contract_dir/values")
if [[ "$layout_esp" != 67584 || "$layout_system" != 131072 ||
    "$layout_persistence" != 65536 || "$layout_recovery" != 65536 ||
    "$layout_footprint" != 329728 || "$layout_dual" != 262144 ||
    "$layout_minimum" != 331810 ]]; then
    echo "declared layout sizes дрейфуют: $layout_esp $layout_system" \
        "$layout_persistence $layout_recovery $layout_footprint" \
        "$layout_dual $layout_minimum" >&2
    exit 1
fi
layout_test_footprint=$(sed -n \
    's/^#define EXPECTED_FOOTPRINT_LBA UINT64_C(\([0-9]*\))$/\1/p' \
    tests/installer-layout-contract-host.c)
layout_test_minimum=$(sed -n \
    's/^#define EXPECTED_MINIMUM_MEDIA_LBA UINT64_C(\([0-9]*\))$/\1/p' \
    tests/installer-layout-contract-host.c)
[[ "$layout_test_footprint" == "$layout_footprint" &&
    "$layout_test_minimum" == "$layout_minimum" ]] || {
    echo "host proof pins не совпадают с заголовком: $layout_test_footprint" \
        "$layout_test_minimum" >&2
    exit 1
}
layout_media=$(sed -n \
    's/^#define INSTALLER_QEMU_MEDIA_LBA UINT64_C(\([0-9]*\))$/\1/p' \
    kernel/src/production_installer_qemu.c)
layout_free=$((layout_media - 34 - 2048 - layout_footprint))
[[ -n "$layout_media" && "$layout_free" -gt 0 ]] || {
    echo "QEMU fixture media не определена" >&2
    exit 1
}
for layout_published in \
    "media_lba=$layout_media" \
    "minimum_media_lba=$layout_minimum" \
    "footprint_lba=$layout_footprint" \
    "free_lba=$layout_free" \
    "role=hfoc-system first_lba=69632 length_lba=$layout_system"; do
    grep -Fq "$layout_published" \
        scripts/test-qemu-production-installer.sh || {
        echo "QEMU proof не публикует: $layout_published" >&2
        exit 1
    }
done

# Documented contract: the sizes, the minimum media and the wiring.
grep -Fq 'ITEM-391' docs/hfoc-installer-layout.md || {
    echo "hfoc-installer-layout.md не фиксирует ITEM-391" >&2
    exit 1
}
grep -Fq '331810' docs/hfoc-installer-layout.md || {
    echo "hfoc-installer-layout.md не фиксирует minimum media" >&2
    exit 1
}
grep -Fq '329728' docs/hfoc-installer-layout.md || {
    echo "hfoc-installer-layout.md не фиксирует declared footprint" >&2
    exit 1
}
for layout_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Fq 'ITEM-391' "$layout_doc" || {
        echo "$layout_doc не документирует ITEM-391" >&2
        exit 1
    }
    grep -Fq '331810' "$layout_doc" || {
        echo "$layout_doc не фиксирует minimum media" >&2
        exit 1
    }
done
