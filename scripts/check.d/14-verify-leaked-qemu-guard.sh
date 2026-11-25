#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Verify the leaked-QEMU guard in the CI workflow: the step must carry the
# complete test-script pattern, scan the job's own cgroup, and fail closed when
# that cgroup cannot be resolved or read. Only the guard's own step block is
# inspected, so a comment or an unrelated step cannot satisfy the contract.
verify_ci_leaked_qemu_guard() {
    local ci_file=$1
    local step_text
    local pattern_line
    local pattern_count
    local pattern_text
    local pattern
    local fragment

    step_text=$(awk '
        /^[[:space:]]*- name:[[:space:]]*Check for leaked QEMU processes[[:space:]]*$/ { inside = 1; next }
        inside && /^[[:space:]]*- name:/ { exit }
        inside { print }
    ' "$ci_file")
    if [[ -z "$step_text" ]]; then
        echo "В $ci_file отсутствует шаг Check for leaked QEMU processes" >&2
        return 1
    fi

    pattern_line=$(printf '%s\n' "$step_text" |
        grep -n "^[[:space:]]*pattern='" || true)
    pattern_count=$(printf '%s\n' "$pattern_line" | sed '/^$/d' | wc -l)
    if [[ "$pattern_count" -ne 1 ]]; then
        echo "Ожидалась ровно одна строка pattern=' в шаге leaked-QEMU, найдено: $pattern_count" >&2
        return 1
    fi
    pattern_text=$(printf '%s\n' "$pattern_line" | cut -d: -f2-)
    for pattern in '[q]emu-system-x86_64' '[t]est-qemu-irqs\.sh' \
                   '[t]est-qemu-panic\.sh' '[t]est-qemu-pagefault\.sh' \
                   '[t]est-qemu-doublefault\.sh' '[t]est-qemu-nmi\.sh' \
                   '[t]est-qemu-acpi\.sh' '[t]est-qemu-vmm\.sh' \
                   '[t]est-qemu-lapic\.sh' '[t]est-qemu-ioapic-probe\.sh' \
                   '[t]est-qemu-ioapic-irq0\.sh' \
                   '[t]est-qemu-ioapic-irq1\.sh' \
                   '[t]est-qemu-pic-irq0-masking\.sh' \
                   '[t]est-qemu-pic-irq0-masking-fallback\.sh' \
                   '[t]est-qemu-lapic-timer-calibration\.sh' \
                   '[t]est-qemu-lapic-timer-runtime\.sh' \
                   '[t]est-qemu-keyboard-line-editing\.sh' \
                   '[t]est-qemu-process-scheduler\.sh' \
                   '[t]est-qemu-user-iretq\.sh' \
                   '[t]est-qemu-syscall\.sh'; do
        if ! printf '%s\n' "$pattern_text" | grep -Fq -- "$pattern"; then
            echo "В pgrep-строке $ci_file отсутствует шаблон: $pattern" >&2
            return 1
        fi
    done

    # The scan itself, not just the pattern literal: the guard must read the
    # job's own cgroup and match each process command line against the pattern.
    # shellcheck disable=SC2016
    for fragment in \
        '/proc/self/cgroup' \
        'cgroup.procs' \
        'while read -r pid; do' \
        'grep -Eq "$pattern" <<<"$cmd"'; do
        if ! printf '%s\n' "$step_text" | grep -Fq -- "$fragment"; then
            echo "leaked-QEMU guard в $ci_file не сканирует процессы: нет $fragment" >&2
            return 1
        fi
    done

    # Fail-closed: an unresolvable or unreadable cgroup must fail the job
    # instead of silently reporting zero leaked processes.
    # shellcheck disable=SC2016
    for fragment in \
        '[[ -z "$cg" ]]' \
        '[[ ! -r "$procs" ]]'; do
        if ! printf '%s\n' "$step_text" | grep -Fq -- "$fragment"; then
            echo "leaked-QEMU guard в $ci_file не закрыт fail-closed: нет $fragment" >&2
            return 1
        fi
    done
}

verify_ci_leaked_qemu_guard .github/workflows/ci.yml || exit 1

# A whole-file grep could be satisfied by a comment. Validate only the guard's
# own step block, and prove both directions with out-of-repo fixtures: the
# complete step passes, while a step without the scan or without the fail-closed
# guards is rejected.
ci_guard_fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-ci-guard.XXXXXX")
trap 'rm -rf "$ci_guard_fixture_dir"' EXIT
ci_guard_pattern=$(grep -m1 "^[[:space:]]*pattern='" .github/workflows/ci.yml)
ci_guard_pattern=${ci_guard_pattern#*pattern=}

cat >"$ci_guard_fixture_dir/comment-only.yml" <<'EOF'
# Check for leaked QEMU processes
# pattern='[q]emu-system-x86_64|[t]est-qemu-irqs\.sh'
EOF
if verify_ci_leaked_qemu_guard "$ci_guard_fixture_dir/comment-only.yml" 2>/dev/null; then
    echo "leaked-QEMU comment-only fixture incorrectly passed" >&2
    exit 1
fi

{
    printf '%s\n' '      - name: Check for leaked QEMU processes' '        run: |'
    printf '          pattern=%s\n' "$ci_guard_pattern"
    cat <<'EOF'
          cg=$(awk -F: '/^0::/ {print $3}' /proc/self/cgroup)
          procs=/sys/fs/cgroup"$cg"/cgroup.procs
          if [[ -z "$cg" ]]; then
            exit 1
          fi
          if [[ ! -r "$procs" ]]; then
            exit 1
          fi
          leaked=0
          while read -r pid; do
            cmd=$(tr '\0' ' ' < /proc/"$pid"/cmdline 2>/dev/null || true)
            if grep -Eq "$pattern" <<<"$cmd"; then
              leaked=1
            fi
          done < "$procs"
EOF
} >"$ci_guard_fixture_dir/complete.yml"
if ! verify_ci_leaked_qemu_guard "$ci_guard_fixture_dir/complete.yml"; then
    echo "leaked-QEMU complete fixture failed" >&2
    exit 1
fi

{
    printf '%s\n' '      - name: Check for leaked QEMU processes' '        run: |'
    printf '          pattern=%s\n' "$ci_guard_pattern"
    cat <<'EOF'
          cg=$(awk -F: '/^0::/ {print $3}' /proc/self/cgroup)
          procs=/sys/fs/cgroup"$cg"/cgroup.procs
          leaked=0
          while read -r pid; do
            cmd=$(tr '\0' ' ' < /proc/"$pid"/cmdline 2>/dev/null || true)
            if grep -Eq "$pattern" <<<"$cmd"; then
              leaked=1
            fi
          done < "$procs"
EOF
} >"$ci_guard_fixture_dir/fail-open.yml"
if verify_ci_leaked_qemu_guard "$ci_guard_fixture_dir/fail-open.yml" 2>/dev/null; then
    echo "leaked-QEMU fail-open fixture incorrectly passed" >&2
    exit 1
fi

ci_guard_pattern_missing=$(printf '%s' "$ci_guard_pattern" |
    sed 's/\[t\]est-qemu-syscall\\\.sh//')
{
    printf '%s\n' '      - name: Check for leaked QEMU processes' '        run: |'
    printf '          pattern=%s\n' "$ci_guard_pattern_missing"
    cat <<'EOF'
          cg=$(awk -F: '/^0::/ {print $3}' /proc/self/cgroup)
          procs=/sys/fs/cgroup"$cg"/cgroup.procs
          if [[ -z "$cg" ]]; then
            exit 1
          fi
          if [[ ! -r "$procs" ]]; then
            exit 1
          fi
          while read -r pid; do
            if grep -Eq "$pattern" <<<"$cmd"; then
              leaked=1
            fi
          done < "$procs"
EOF
} >"$ci_guard_fixture_dir/missing-script.yml"
if verify_ci_leaked_qemu_guard "$ci_guard_fixture_dir/missing-script.yml" 2>/dev/null; then
    echo "leaked-QEMU missing-script fixture incorrectly passed" >&2
    exit 1
fi

rm -rf "$ci_guard_fixture_dir"
trap - EXIT

# NMI IST2 — fall first, then implement.
grep -Fq 'test-nmi' Makefile || { echo "Отсутствует test-nmi в Makefile" >&2; exit 1; }
bash -n scripts/test-qemu-nmi.sh || { echo "Синтаксис test-qemu-nmi.sh некорректен" >&2; exit 1; }
grep -Eq '#define[[:space:]]+IST_STACK_SIZE[[:space:]]+16384' \
    kernel/include/cpu.h || {
    echo "Отсутствует per-CPU IST_STACK_SIZE 16384" >&2; exit 1; }
grep -Fq 'ist2_stack[IST_STACK_SIZE]' kernel/include/cpu.h || {
    echo "Отсутствует per-CPU ist2_stack" >&2; exit 1; }
grep -Fq 'const void *tss_ist2_stack(void);' kernel/include/arch/x86_64/tss.h || {
    echo "Отсутствует объявление tss_ist2_stack" >&2; exit 1; }
grep -Fq 'size_t tss_ist2_stack_size(void);' kernel/include/arch/x86_64/tss.h || {
    echo "Отсутствует объявление tss_ist2_stack_size" >&2; exit 1; }
grep -Fq '[TSS ] IST2 stack:' kernel/src/arch/x86_64/tss.c || {
    echo "Отсутствует COM1 IST2 stack range" >&2; exit 1; }
grep -Fq 'cpu->tss.ist2 = ist2_top;' kernel/src/arch/x86_64/tss.c || {
    echo "IST2 top не записывается в TSS" >&2; exit 1; }
if grep -Fq 'cpu->tss.ist2 = 0U;' kernel/src/arch/x86_64/tss.c; then
    echo "IST2 ошибочно обнуляется" >&2; exit 1
fi

# ACPI — fall first, then implement.
grep -Fq 'LIMINE_RSDP_REQUEST_ID' kernel/include/limine_min.h || {
    echo "Отсутствует LIMINE_RSDP_REQUEST_ID" >&2; exit 1; }
grep -Fq 'rsdp_request' kernel/src/kernel.c || {
    echo "Отсутствует rsdp_request в kernel.c" >&2; exit 1; }

grep -Fq 'acpi_parse' kernel/src/acpi.c || {
    echo "Отсутствует acpi_parse в acpi.c" >&2; exit 1; }
for forbidden in memcpy memcmp '<string.h>'; do
    if grep -Fq "$forbidden" kernel/src/acpi.c; then
        echo "kernel/src/acpi.c содержит $forbidden" >&2; exit 1
    fi
done

# MADT — fall first, then implement.
grep -Fq '"APIC"' kernel/src/acpi.c || {
    echo "Отсутствует сигнатура \"APIC\" в acpi.c" >&2; exit 1; }
grep -Fq 'test-acpi-host' Makefile || {
    echo "Отсутствует test-acpi-host в Makefile" >&2; exit 1; }
grep -Fq 'DHFOC_ACPI_HOST_TEST' Makefile || {
    echo "Отсутствует -DHFOC_ACPI_HOST_TEST в test-acpi-host" >&2; exit 1; }
grep -Fq 'acpi_status_string' kernel/src/acpi.c || {
    echo "Отсутствует acpi_status_string в acpi.c" >&2; exit 1; }

grep -Fq '#include "acpi.h"' kernel/src/kernel.c || {
    echo "Отсутствует include acpi.h в kernel.c" >&2; exit 1; }
grep -Fq 'acpi_init(' kernel/src/kernel.c || {
    echo "Отсутствует вызов acpi_init в kernel.c" >&2; exit 1; }
grep -Fq '[ACPI]' kernel/src/acpi.c || {
    echo "Отсутствует COM1-диагностика ACPI в acpi.c" >&2; exit 1; }
nm build/hfoc-kernel.elf | grep -F ' acpi_init' > /dev/null || {
    echo "Отсутствует acpi_init в ELF" >&2; exit 1; }
if nm build/hfoc-kernel.elf | grep -F 'acpi_test_phys_range_to_virt' > /dev/null; then
    echo "acpi_test_phys_range_to_virt попал в kernel ELF" >&2; exit 1
fi

grep -Fq 'test-acpi' Makefile || {
    echo "Отсутствует test-acpi в Makefile" >&2; exit 1; }
bash -n scripts/test-qemu-acpi.sh 2>/dev/null || {
    echo "Синтаксис test-qemu-acpi.sh некорректен или файл отсутствует" >&2; exit 1; }
grep -Fq 'acpi_madt' kernel/src/acpi.c || {
    echo "Отсутствует acpi_madt в acpi.c" >&2; exit 1; }
grep -Fq 'test-vmm' Makefile || {
    echo "Отсутствует test-vmm в Makefile" >&2; exit 1; }
bash -n scripts/test-qemu-vmm.sh || {
    echo "Синтаксис test-qemu-vmm.sh некорректен" >&2; exit 1; }


# VMM — console integration and APIC-free invariant.
grep -Fq '#include "vmm.h"' kernel/src/console.c || {
    echo "Отсутствует include vmm.h в console.c" >&2; exit 1; }
grep -Fq 'vmmmap' kernel/src/console.c || {
    echo "Отсутствует команда vmmmap в console.c" >&2; exit 1; }
grep -Fq 'vmm_map_mmio_page' kernel/src/console.c || {
    echo "vmmmap не вызывает vmm_map_mmio_page" >&2; exit 1; }
grep -Fq 'vmm_hhdm_offset' kernel/src/kernel.c || {
    echo "Отсутствует vmm_hhdm_offset в kernel.c" >&2; exit 1; }

# VMM must not access APIC/MSR.
! grep -Eq 'rdmsr|wrmsr' kernel/src/vmm.c || {
    echo "vmm.c содержит rdmsr/wrmsr" >&2; exit 1; }
! grep -Fq 'IA32_APIC_BASE' kernel/src/vmm.c || {
    echo "vmm.c содержит IA32_APIC_BASE" >&2; exit 1; }
! grep -Fq 'lapic_' kernel/src/vmm.c || {
    echo "vmm.c содержит lapic_" >&2; exit 1; }

nm build/hfoc-kernel.elf | grep -F ' vmm_map_mmio_page' > /dev/null || {
    echo "Отсутствует vmm_map_mmio_page в ELF" >&2; exit 1; }
nm build/hfoc-kernel.elf | grep -F ' vmm_status_string' > /dev/null || {
    echo "Отсутствует vmm_status_string в ELF" >&2; exit 1; }
! nm build/hfoc-kernel.elf | grep -F 'mock_' > /dev/null || {
    echo "mock_ символы в kernel ELF" >&2; exit 1; }
! nm build/hfoc-kernel.elf | grep -F 'vmm_test_' > /dev/null || {
    echo "vmm_test_ символы в kernel ELF" >&2; exit 1; }

# VMM address-space API and boot integration.
grep -Fq 'struct address_space {' kernel/include/vmm.h || {
    echo "vmm.h не содержит struct address_space" >&2; exit 1; }
for vmm_as_api in \
    'enum vmm_status vmm_init(uint64_t hhdm_offset);' \
    'enum vmm_status vmm_create_address_space(' \
    'enum vmm_status vmm_map_page_in(' \
    'enum vmm_status vmm_unmap_page_in(' \
    'enum vmm_status vmm_activate_address_space(' \
    'const struct address_space *vmm_kernel_address_space(void);' \
    'const struct address_space *vmm_current_address_space(void);'; do
    grep -Fq "$vmm_as_api" kernel/include/vmm.h || {
        echo "vmm.h не содержит API: $vmm_as_api" >&2
        exit 1
    }
done
grep -Fq 'VMM_BAD_ADDRESS_SPACE = 7' kernel/include/vmm.h || {
    echo "vmm.h не содержит VMM_BAD_ADDRESS_SPACE" >&2; exit 1; }
grep -Fq 'VMM_NOT_MAPPED = 8' kernel/include/vmm.h || {
    echo "vmm.h не содержит VMM_NOT_MAPPED" >&2; exit 1; }
grep -Fq 'VMM_BAD_RANGE = 9' kernel/include/vmm.h || {
    echo "vmm.h не содержит VMM_BAD_RANGE" >&2; exit 1; }
grep -Fq 'VMM_BAD_FLAGS = 10' kernel/include/vmm.h || {
    echo "vmm.h не содержит VMM_BAD_FLAGS" >&2; exit 1; }
grep -Fq '#define VMM_USER_IMAGE_BASE  0x00400000ULL' \
    kernel/include/vmm.h || {
    echo "vmm.h не содержит VMM_USER_IMAGE_BASE" >&2; exit 1; }
grep -Fq '#define VMM_USER_STACK_BASE  0x00007FFFF0000000ULL' \
    kernel/include/vmm.h || {
    echo "vmm.h не содержит VMM_USER_STACK_BASE" >&2; exit 1; }
grep -Fq '#define VMM_USER_STACK_TOP   0x0000800000000000ULL' \
    kernel/include/vmm.h || {
    echo "vmm.h не содержит VMM_USER_STACK_TOP" >&2; exit 1; }
grep -Fq 'enum vmm_status vmm_validate_user_range(' \
    kernel/include/vmm.h || {
    echo "vmm.h не содержит vmm_validate_user_range" >&2; exit 1; }
grep -Fq 'enum vmm_status vmm_map_user_pages_in(' \
    kernel/include/vmm.h || {
    echo "vmm.h не содержит vmm_map_user_pages_in" >&2; exit 1; }
for vmm_as_symbol in vmm_init vmm_create_address_space vmm_map_page_in \
                     vmm_unmap_page_in vmm_activate_address_space \
                     vmm_kernel_address_space vmm_current_address_space; do
    nm build/hfoc-kernel.elf | grep -F " $vmm_as_symbol" >/dev/null || {
        echo "В ELF отсутствует символ: $vmm_as_symbol" >&2
        exit 1
    }
done
for vmm_user_symbol in vmm_validate_user_range vmm_map_user_pages_in; do
    nm build/hfoc-kernel.elf | grep -F " $vmm_user_symbol" >/dev/null || {
        echo "В ELF отсутствует символ: $vmm_user_symbol" >&2
        exit 1
    }
done
if nm build/hfoc-kernel.elf | grep -F 'vmm_user_test_' >/dev/null; then
    echo "vmm_user_test_ символы в kernel ELF" >&2
    exit 1
fi
grep -Fq '#include "vmm.h"' kernel/src/kernel.c || {
    echo "kernel.c не включает vmm.h" >&2; exit 1; }
grep -Fq 'vmm_init(hhdm_request.response->offset)' \
    kernel/src/kernel.c || {
    echo "kernel.c не вызывает vmm_init после PMM" >&2; exit 1; }
vmm_init_line=$(grep -n 'vmm_init(' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
pmm_init_line=$(grep -n 'pmm_init(' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
[[ -n "$vmm_init_line" && -n "$pmm_init_line" &&
   "$vmm_init_line" -gt "$pmm_init_line" ]] || {
    echo "vmm_init должен вызываться после pmm_init" >&2
    exit 1
}
grep -Fq 'test-vmm-as' Makefile || {
    echo "Makefile не содержит test-vmm-as" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-vmm-as' Makefile || {
    echo "test: target не содержит test-vmm-as" >&2; exit 1; }
vmm_as_target_block=$(sed -n '/^test-vmm-as:/,/^[^[:space:]]/p' Makefile)
grep -Fq "QEMU_MACHINE='q35'" <<<"$vmm_as_target_block" || {
    echo "test-vmm-as должен использовать QEMU_MACHINE='q35'" >&2
    exit 1
}
grep -Fq "QEMU_SMP='1'" <<<"$vmm_as_target_block" || {
    echo "test-vmm-as должен использовать QEMU_SMP='1'" >&2
    exit 1
}
if grep -Eq 'QEMU_CPU=.*(max|-apic|-no-apic)|-cpu[[:space:]]+(max|-apic|-no-apic)' \
    <<<"$vmm_as_target_block"; then
    echo "test-vmm-as должен не задавать QEMU_CPU" >&2
    exit 1
fi
grep -Fq 'HFOC_VMM_AS_QEMU_TEST' Makefile || {
    echo "Makefile не определяет HFOC_VMM_AS_QEMU_TEST" >&2; exit 1; }
grep -Fq '#ifdef HFOC_VMM_AS_QEMU_TEST' kernel/src/kernel.c || {
    echo "kernel.c не содержит macro-gated VMM-AS proof hook" >&2
    exit 1
}
grep -Fq '[VMM-AS] address space map/activate/restore: OK' \
    kernel/src/kernel.c || {
    echo "kernel.c не содержит VMM-AS success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
    '[VMM-AS] address space map/activate/restore: OK'; then
    echo "normal kernel ELF содержит VMM-AS test marker" >&2
    exit 1
fi
bash -n scripts/test-qemu-vmm-as.sh || {
    echo "Синтаксис scripts/test-qemu-vmm-as.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-vmm-as.sh || {
    echo "test-qemu-vmm-as.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU VMM address-space test: OK' \
    scripts/test-qemu-vmm-as.sh || {
    echo "test-qemu-vmm-as.sh не содержит success marker" >&2
    exit 1
}
for vmm_as_strict_symbol in mktemp timeout send_monitor_quit stop_qemu cleanup \
                            no-reboot no-shutdown; do
    grep -Fq "$vmm_as_strict_symbol" scripts/test-qemu-vmm-as.sh || {
        echo "test-qemu-vmm-as.sh не содержит $vmm_as_strict_symbol" >&2
        exit 1
    }
done

# VMM — must be in `make test` and correct CPU profiles.
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-vmm-host' Makefile || {
    echo "test: target не содержит test-vmm-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-vmm-user-host' Makefile || {
    echo "test: target не содержит test-vmm-user-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-vmm' Makefile || {
    echo "test: target не содержит test-vmm" >&2; exit 1; }
grep -A2 '^test-acpi:' Makefile | grep -Fq "QEMU_CPU='max'" || {
    echo "test-acpi должен использовать QEMU_CPU='max'" >&2; exit 1; }
grep -A2 '^test-vmm:' Makefile | grep -Fq "QEMU_CPU='max,-apic'" || {
    echo "test-vmm должен использовать QEMU_CPU='max,-apic'" >&2; exit 1; }

# LAPIC — VMM integration invariants and test profiles.
grep -Fq 'test-lapic-host' Makefile || {
    echo "Отсутствует test-lapic-host в Makefile" >&2; exit 1; }


grep -Fq 'DHFOC_LAPIC_HOST_TEST' Makefile || {
    echo "Отсутствует -DHFOC_LAPIC_HOST_TEST в test-lapic-host" >&2; exit 1; }
bash -n scripts/test-qemu-lapic.sh || {
    echo "Синтаксис test-qemu-lapic.sh некорректен" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq 'INTERRUPTS_LOG=$TEST_DIR/interrupts.log' scripts/test-qemu-lapic.sh || {
    echo "test-qemu-lapic.sh не создаёт INTERRUPTS_LOG" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq -- '-d int -D "$INTERRUPTS_LOG"' scripts/test-qemu-lapic.sh || {
    echo "test-qemu-lapic.sh не включает QEMU interrupt trace" >&2; exit 1; }
grep -Fq 'Servicing hardware INT=0x42' scripts/test-qemu-lapic.sh || {
    echo "test-qemu-lapic.sh не требует Servicing hardware INT=0x42" >&2; exit 1; }
grep -Fq '[LAPIC] LINT0 configured: delivery=ExtINT mask=0; legacy PIC fallback ready' \
    scripts/test-qemu-lapic.sh || {
    echo "test-qemu-lapic.sh не требует LINT0 configured marker" >&2; exit 1; }
lapic_qemu_target_count=$(grep -c '^test-lapic:' Makefile || true)
[[ "$lapic_qemu_target_count" -eq 1 ]] || {
    echo "Ожидался ровно один test-lapic target, найдено: $lapic_qemu_target_count" >&2; exit 1; }
grep -Fq 'lapic_test_vmm_map_mmio_page' kernel/src/lapic.c || {
    echo "Отсутствует lapic_test_vmm_map_mmio_page seam" >&2; exit 1; }
grep -Fq 'uint8_t lapic_bsp_apic_id(void);' kernel/include/lapic.h || {
    echo "Отсутствует объявление lapic_bsp_apic_id" >&2; exit 1; }
grep -Fq 'lapic_status_string' kernel/src/lapic.c || {
    echo "Отсутствует lapic_status_string в lapic.c" >&2; exit 1; }
grep -Fq 'LAPIC_MMIO_MAP_FAILED' kernel/include/lapic.h || {
    echo "Отсутствует LAPIC_MMIO_MAP_FAILED в lapic.h" >&2; exit 1; }
grep -Fq 'LAPIC_LINT0_FAILED' kernel/include/lapic.h || {
    echo "Отсутствует LAPIC_LINT0_FAILED в lapic.h" >&2; exit 1; }
grep -Fq 'LAPIC_DISABLE_FAILED' kernel/include/lapic.h || {
    echo "Отсутствует LAPIC_DISABLE_FAILED в lapic.h" >&2; exit 1; }
grep -Fq 'lapic_timer_on_pit_tick();' kernel/src/arch/x86_64/timer.c || {
    echo "Отсутствует lapic_timer_on_pit_tick в timer.c" >&2; exit 1; }
grep -Fq 'lapic_timer_start_calibration(HFOC_TIMER_HZ);' kernel/src/kernel.c || {
    echo "Отсутствует lapic_timer_start_calibration в kernel.c" >&2; exit 1; }
grep -Fq 'LAPIC_TIMER_CALIBRATION_SAMPLES 10U' kernel/src/lapic_timer.c || {
    echo "Отсутствует ten-sample LAPIC timer constant" >&2; exit 1; }
grep -Fq '(1U << 16U)' kernel/src/lapic.c || {
    echo "Отсутствует LAPIC timer LVT mask bit write" >&2; exit 1; }
grep -Fq 'test-lapic-timer-host' Makefile || {
    echo "Отсутствует test-lapic-timer-host в Makefile" >&2; exit 1; }
grep -Fq 'test-lapic-timer-calibration' Makefile || {
    echo "Отсутствует test-lapic-timer-calibration в Makefile" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lapic-timer-host' Makefile || {
    echo "test: target не содержит test-lapic-timer-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lapic-timer-calibration' Makefile || {
    echo "test: target не содержит test-lapic-timer-calibration" >&2; exit 1; }
bash -n scripts/test-qemu-lapic-timer-calibration.sh || {
    echo "Синтаксис test-qemu-lapic-timer-calibration.sh некорректен" >&2; exit 1; }
if grep -Eq 'idt_set_gate\(0x42U|case 0x42U' \
    kernel/src/lapic_timer.c kernel/src/arch/x86_64/interrupts.c; then
    echo "LAPIC timer runtime/calibration uses an invalid direct IDT entry" >&2
    exit 1
fi
if grep -nE '\b(pic_|ioapic_)' kernel/src/lapic_timer.c; then
    echo "lapic_timer.c ссылается на PIC/IOAPIC subsystem" >&2
    exit 1
fi
grep -Fq 'return "LINT0 failed"' kernel/src/lapic.c || {
    echo "Отсутствует status string LINT0 failed в lapic.c" >&2; exit 1; }
grep -Fq 'return "disable failed"' kernel/src/lapic.c || {
    echo "Отсутствует status string disable failed в lapic.c" >&2; exit 1; }
grep -Fq '0x350U' kernel/src/lapic.c || {
    echo "lapic.c не содержит LINT0 offset 0x350U" >&2; exit 1; }
grep -Fq 'mmio_write32(0x350U, original_lint0)' kernel/src/lapic.c || {
    echo "lapic.c не восстанавливает original_lint0 в fail-closed path" >&2; exit 1; }
grep -Fq 'disable_value = original_msr & ~(1ULL << 11U)' kernel/src/lapic.c || {
    echo "lapic.c не вычисляет disable_value через original_msr" >&2; exit 1; }
grep -Fq 'wrmsr(0x1B, disable_value)' kernel/src/lapic.c || {
    echo "lapic.c не записывает disable_value в IA32_APIC_BASE" >&2; exit 1; }
grep -Fq 'const uint64_t disable_readback = rdmsr(0x1B)' kernel/src/lapic.c || {
    echo "lapic.c не читает IA32_APIC_BASE после disable write" >&2; exit 1; }
grep -Fq 'legacy PIC availability unverified' kernel/src/lapic.c || {
    echo "lapic.c не содержит exact legacy PIC availability unverified" >&2; exit 1; }

lint0_write_line=$(grep -n 'mmio_write32(0x350U,' kernel/src/lapic.c | head -n1 | cut -d: -f1 || true)
svr_write_line=$(grep -n 'mmio_write32(0x0F0U,' kernel/src/lapic.c | head -n1 | cut -d: -f1 || true)
initialized_line=$(grep -n 'initialized = true;' kernel/src/lapic.c | head -n1 | cut -d: -f1 || true)
[[ -n "$lint0_write_line" && -n "$svr_write_line" && -n "$initialized_line" &&
   "$lint0_write_line" -lt "$svr_write_line" &&
   "$svr_write_line" -lt "$initialized_line" ]] || {
    echo "LINT0 write должен быть до SVR write и publication" >&2; exit 1; }

restore_lint0_line=$(grep -n 'mmio_write32(0x350U, original_lint0)' kernel/src/lapic.c | head -n1 | cut -d: -f1 || true)
disable_value_line=$(grep -n 'disable_value = original_msr' kernel/src/lapic.c | head -n1 | cut -d: -f1 || true)
wrmsr_disable_line=$(grep -n 'wrmsr(0x1B, disable_value)' kernel/src/lapic.c | head -n1 | cut -d: -f1 || true)
rdmsr_disable_line=$(grep -n 'const uint64_t disable_readback = rdmsr(0x1B)' kernel/src/lapic.c | head -n1 | cut -d: -f1 || true)
[[ -n "$restore_lint0_line" && -n "$disable_value_line" &&
   -n "$wrmsr_disable_line" && -n "$rdmsr_disable_line" &&
   "$restore_lint0_line" -lt "$disable_value_line" &&
   "$disable_value_line" -lt "$wrmsr_disable_line" &&
   "$wrmsr_disable_line" -lt "$rdmsr_disable_line" ]] || {
    echo "Fail-closed path должен идти restore LINT0 -> disable MSR -> read-back" >&2; exit 1; }

if grep -nE 'mmio_(read|write)32\([[:space:]]*0x360' kernel/src/lapic.c; then
    echo "lapic.c обращается к LINT1 (0x360)" >&2
    exit 1
fi

if grep -nE '\b(pic_|ioapic_|timer_|keyboard_)|interrupt_dispatch\b' kernel/src/lapic.c; then
    echo "lapic.c ссылается на out-of-scope subsystem" >&2
    exit 1
fi

if nm -g build/hfoc-kernel.elf | grep -E 'lapic_test_|host_lapic_test_'; then
    echo "LAPIC test-only symbols попали в kernel ELF" >&2
    exit 1
fi

grep -Fq '0x800U + (offset >> 4U)' kernel/src/lapic.c || {
    echo "lapic.c не содержит x2APIC MSR map formula" >&2
    exit 1
}
grep -Fq '#define IA32_APIC_BASE_EN    (1ULL << 11U)' \
    kernel/src/lapic.c || {
    echo "lapic.c не содержит IA32_APIC_BASE EN bit constant" >&2
    exit 1
}
grep -Fq '#define IA32_APIC_BASE_EXTD  (1ULL << 10U)' \
    kernel/src/lapic.c || {
    echo "lapic.c не содержит IA32_APIC_BASE EXTD bit constant" >&2
    exit 1
}
grep -Fq '~(IA32_APIC_BASE_EN | IA32_APIC_BASE_EXTD)' \
    kernel/src/lapic.c || {
    echo "lapic.c rollback path не очищает EN и EXTD" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf | grep -E 'host_cpuid_has_x2apic|host_x2apic_'; then
    echo "x2APIC host mock symbol попал в kernel ELF" >&2
    exit 1
fi
if grep -Fq 'lapic_x2apic_transition();' kernel/src/kernel.c; then
    echo "kernel.c вызывает lapic_x2apic_transition в normal boot" >&2
    exit 1
fi
grep -Fq '#ifdef HFOC_X2APIC_TEST' kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит macro-gated x2APIC test seam" >&2
    exit 1
}
grep -Fq 'lapic_x2apic_transition();' kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не вызывает lapic_x2apic_transition" >&2
    exit 1
}
if nm build/hfoc-kernel.elf | grep -F ' lapic_x2apic_test_hook'; then
    echo "lapic_x2apic_test_hook попал в kernel ELF" >&2
    exit 1
fi

grep -Fq 'vmm_map_mmio_page' kernel/src/lapic.c || {
    echo "lapic.c не вызывает vmm_map_mmio_page" >&2; exit 1; }
! grep -Fq 'map_lapic_2m_page' kernel/src/lapic.c || {
    echo "lapic.c содержит map_lapic_2m_page" >&2; exit 1; }
! grep -Fq 'is_mmio_range_mapped' kernel/src/lapic.c || {
    echo "lapic.c содержит is_mmio_range_mapped" >&2; exit 1; }
! grep -Eq 'cr3|pml4|pdpt|pd\[' kernel/src/lapic.c || {
    echo "lapic.c содержит ручной page-table walk" >&2; exit 1; }
if grep -Fq 'return LAPIC_RANGE_NOT_MAPPED' kernel/src/lapic.c; then
    echo "lapic_init возвращает LAPIC_RANGE_NOT_MAPPED" >&2; exit 1
fi
if nm build/hfoc-kernel.elf | grep -F 'lapic_test_vmm_map_mmio_page' > /dev/null; then
    echo "lapic_test_vmm_map_mmio_page попал в kernel ELF" >&2; exit 1
fi
nm -g build/hfoc-kernel.elf | grep -F ' lapic_bsp_apic_id' >/dev/null || {
    echo "В ELF отсутствует lapic_bsp_apic_id" >&2
    exit 1
}

# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lapic-host' Makefile || {
    echo "test: target не содержит test-lapic-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lapic' Makefile || {
    echo "test: target не содержит test-lapic" >&2; exit 1; }
if grep -A2 '^test-lapic:' Makefile | grep -Fq 'QEMU_CPU='; then
    echo "test-lapic должен не задавать QEMU_CPU" >&2; exit 1
fi
grep -A2 '^test-lapic:' Makefile | grep -Fq "QEMU_SMP='1'" || {
    echo "test-lapic должен использовать QEMU_SMP='1'" >&2; exit 1; }
