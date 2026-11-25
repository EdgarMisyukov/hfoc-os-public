#!/usr/bin/env bash
# ITEM-614: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.
#
# ITEM-611 measured the production login and published [LOGIN-TRACE] on serial
# only.  `diag save` composes a fresh snapshot and never copies the serial
# stream, so the owner's next hardware export would still have no latency
# record.  This slice pins the one bounded trace snapshot, its one renderer,
# and the three paths that must carry the same completed record: serial/journal,
# the shared status report, and a real snapshot/extracted file.

for item614_file in \
    kernel/include/kernel_login_trace.h \
    kernel/src/kernel_login_trace.c \
    tests/diag-export-full-host.c \
    scripts/test-qemu-host-provisioned-entropy.sh \
    scripts/test-qemu-production-install-flow.sh \
    docs/hfoc-login-latency.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item614_file ]] || {
        echo "ITEM-614: отсутствует $item614_file" >&2
        exit 1
    }
done

# 1. The finished attempt is bounded and safe: no secret-bearing fields.
for item614_field in \
    'struct kernel_login_trace {' \
    'bool ok;' \
    'uint32_t stage;' \
    'uint32_t account_iterations;' \
    'uint64_t account_cycles;' \
    'uint64_t seed_cycles;' \
    'uint64_t publish_cycles;' \
    'uint64_t entropy_cycles;' \
    'uint64_t total_cycles;' \
    'struct appliance_host_seed_accounting seed;' \
    'kernel_login_trace_begin(' \
    'kernel_login_trace_commit(' \
    'kernel_login_trace_status_report('; do
    grep -Fq -- "$item614_field" kernel/include/kernel_login_trace.h || {
        echo "ITEM-614: снимок trace не содержит:" "$item614_field" >&2
        exit 1
    }
done
if grep -Eiq 'password|passphrase|private|verifier|token|seed_bytes|secret' \
    kernel/src/kernel_login_trace.c; then
    echo "ITEM-614: trace-модуль называет секретное поле" >&2
    exit 1
fi

# 2. One renderer carries the same fields to serial, journal and status.
for item614_render in \
    'available=' \
    'state=NOT_ATTEMPTED' \
    'state=IN_PROGRESS' \
    'attempt' \
    'ok' \
    'stage' \
    'account_us' \
    'account_iterations' \
    'seed_us' \
    'seed_reads' \
    'seed_writes' \
    'seed_opens' \
    'seed_seals' \
    'seed_readbacks' \
    'seed_kdf_calls' \
    'seed_kdf_iterations' \
    'seed_read_us' \
    'seed_open_us' \
    'seed_seal_us' \
    'seed_write_us' \
    'seed_readback_us' \
    'publish_us' \
    'entropy_us' \
    'total_us'; do
    grep -Fq -- "$item614_render" kernel/src/kernel_login_trace.c || {
        echo "ITEM-614: общий renderer не публикует:" "$item614_render" >&2
        exit 1
    }
done
grep -Fq 'KERNEL_LOGIN_TRACE_LINE_MAX' kernel/include/kernel_login_trace.h || {
    echo "ITEM-614: граница строки trace не объявлена" >&2
    exit 1
}

# 3. The production login opens/commits the attempt and the shared report
#    publishes the same stored record; serial also mirrors to the journal.
for item614_hook in \
    'kernel_login_trace_begin();' \
    'kernel_login_trace_commit(&trace);' \
    'kernel_login_trace_status_report(sink, context);' \
    'console_journal_begin();' \
    'console_journal_end();'; do
    grep -Fq -- "$item614_hook" kernel/src/kernel.c || {
        echo "ITEM-614: production-путь не содержит:" "$item614_hook" >&2
        exit 1
    }
done
if ! grep -Fq 'kernel_login_trace_emit_state(' kernel/src/kernel.c; then
    echo "ITEM-614: serial/journal не используют общий renderer" >&2
    exit 1
fi

# 4. The real snapshot/export proof drives the stored trace and the reader.
for item614_proof in \
    'kernel_login_trace_begin();' \
    'kernel_login_trace_commit(' \
    'kernel_login_trace_status_report(' \
    '[LOGIN-TRACE]' \
    'available=0' \
    'state=NOT_ATTEMPTED' \
    'available=1' \
    'total_us='; do
    grep -Fq -- "$item614_proof" tests/diag-export-full-host.c || {
        echo "ITEM-614: diag-export host proof не содержит:" \
            "$item614_proof" >&2
        exit 1
    }
done
for item614_qemu in \
    'LOGIN-TRACE' \
    'DIAG SNAPSHOT records=' \
    'available=1' \
    'total_us='; do
    grep -Fq -- "$item614_qemu" \
        scripts/test-qemu-host-provisioned-entropy.sh || {
        echo "ITEM-614: login QEMU proof не требует:" "$item614_qemu" >&2
        exit 1
    }
done
grep -Fq "'[LOGIN-TRACE]'" \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-614: installed diag-save proof не требует [LOGIN-TRACE]" >&2
    exit 1
}

# 5. The host proof is part of the canonical suite and builds the new module.
grep -Fq 'kernel/src/kernel_login_trace.c' Makefile || {
    echo "ITEM-614: модуль trace не собран в host proof" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is the Makefile's own words
grep -Fq '$(MAKE) test-diag-export-full-host' Makefile || {
    echo "ITEM-614: host proof не зарегистрирован в suite" >&2
    exit 1
}
