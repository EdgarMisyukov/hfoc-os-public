#!/usr/bin/env bash
# ITEM-611: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.
#
# The owner reported a very slow local password check.  The item first measures
# the production path, then removes only the proved extra work:
#
#   * the host-seed seed path used the naive PBKDF2 HMAC that rebuilt and hashed
#     both pads on every round, while the account KDF already prepared them once;
#   * the selected slot was opened a second time although the scan had already
#     authenticated it;
#   * the production login publishes a bounded [LOGIN-TRACE] with the account,
#     seed, publish and entropy stages, the KDF/operation counters and the build,
#     and never a password/seed/token/hash-derived value.
#
# The KDF parameters, vectors, old sealed-slot compatibility, failure stages and
# zeroization are unchanged.

for item611_file in \
    kernel/include/appliance_host_seed.h \
    kernel/src/appliance_host_seed.c \
    kernel/include/appliance_state_crypto.h \
    kernel/src/appliance_state_crypto.c \
    kernel/include/appliance_production_admin_enrollment.h \
    kernel/src/kernel.c \
    tests/state-encryption-host.c \
    tests/appliance-host-seed-host.c \
    tests/appliance-host-seed-old-slot.h \
    scripts/test-qemu-host-provisioned-entropy.sh \
    docs/hfoc-login-latency.md; do
    [[ -f $item611_file ]] || {
        echo "ITEM-611: отсутствует $item611_file" >&2
        exit 1
    }
done

# 1. The accounting seam carries counters and cycles only, never secret bytes.
for item611_accounting in \
    'struct appliance_host_seed_accounting {' \
    'appliance_host_seed_clock_fn' \
    'void appliance_host_seed_accounting_begin(' \
    'void appliance_host_seed_accounting_read(' \
    'uint32_t slot_reads;' \
    'uint32_t slot_writes;' \
    'uint32_t opens;' \
    'uint32_t seals;' \
    'uint32_t readbacks;' \
    'uint32_t kdf_calls;' \
    'uint32_t kdf_iterations;'; do
    grep -Fq -- "$item611_accounting" \
        kernel/include/appliance_host_seed.h || {
        echo "ITEM-611: учёт не содержит:" "$item611_accounting" >&2
        exit 1
    }
done
for item611_source in \
    '++accounting.opens;' \
    '++accounting.seals;' \
    '++accounting.slot_reads;' \
    '++accounting.slot_writes;' \
    '++accounting.readbacks;' \
    'accounting.kdf_iterations += iterations;' \
    'verified = candidates[selected];' \
    '*epoch_out = sequences[selected];'; do
    grep -Fq -- "$item611_source" kernel/src/appliance_host_seed.c || {
        echo "ITEM-611: seed-путь не содержит:" "$item611_source" >&2
        exit 1
    }
done
grep -Fq 'appliance_host_seed_accounting_begin(timer_cycles)' \
    kernel/src/kernel.c || {
    echo "ITEM-611: unlock не открывает учёт seed-пути" >&2
    exit 1
}

# 2. The state crypto PBKDF2 prepares the HMAC key once and zeroes it.
for item611_crypto in \
    'struct state_crypto_hmac_key {' \
    'state_crypto_hmac_key_prepare(' \
    'sha256_absorb_block(&prepared->inner)' \
    'sha256_absorb_block(&prepared->outer)' \
    'state_crypto_hmac_prepared(' \
    'state_crypto_hmac_key_clear(&prepared);'; do
    grep -Fq -- "$item611_crypto" kernel/src/appliance_state_crypto.c || {
        echo "ITEM-611: crypto-путь не содержит:" "$item611_crypto" >&2
        exit 1
    }
done
if grep -Pzo 'state_crypto_hmac\(\n                passphrase, passphrase_length, block_input' \
    kernel/src/appliance_state_crypto.c >/dev/null; then
    echo "ITEM-611: PBKDF2 всё ещё пересобирает HMAC на каждом раунде" >&2
    exit 1
fi
grep -Fq 'APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT 200000U' \
    kernel/include/appliance_admin_credential.h || {
    echo "ITEM-611: число итераций учётной записи изменилось" >&2
    exit 1
}
grep -Fq 'APPLIANCE_STATE_CRYPTO_PBKDF2_DEFAULT_ITERATIONS 20000U' \
    kernel/include/appliance_state_crypto.h || {
    echo "ITEM-611: число итераций seed-слота изменилось" >&2
    exit 1
}

# 3. The old sealed slot and the compression budget are the oracle.
grep -Fq 'old_slot0' tests/appliance-host-seed-old-slot.h || {
    echo "ITEM-611: нет сохранённого старого sealed slot" >&2
    exit 1
}
grep -Fq 'test_old_sealed_slot_compatibility' \
    tests/appliance-host-seed-host.c || {
    echo "ITEM-611: старая совместимость не проверяется" >&2
    exit 1
}
grep -Fq 'sha256_host_test_reset_compressions();' \
    tests/state-encryption-host.c || {
    echo "ITEM-611: бюджет компрессий не измеряется" >&2
    exit 1
}
grep -Fq 'PBKDF2 prepares one HMAC key per derivation' \
    tests/state-encryption-host.c || {
    echo "ITEM-611: бюджет подготовленного ключа не утверждается" >&2
    exit 1
}
grep -Fq 'tests/state-encryption-host.c' Makefile || {
    echo "ITEM-611: host-проба не в наборе" >&2
    exit 1
}
grep -Fq -- '-DHFOC_SHA256_HOST_TEST' Makefile || {
    echo "ITEM-611: счётчик компрессий не собран" >&2
    exit 1
}

# 4. The login trace is bounded, named and free of secret material.
for item611_trace in \
    '[LOGIN-TRACE]' \
    'account_iterations' \
    'seed_kdf_iterations' \
    'seed_opens' \
    'seed_seals' \
    'seed_read_us' \
    'seed_open_us' \
    'seed_seal_us' \
    'seed_write_us' \
    'seed_readback_us' \
    'publish_us' \
    'entropy_us' \
    'total_us'; do
    grep -Fq -- "$item611_trace" kernel/src/kernel_login_trace.c || {
        echo "ITEM-611: trace не содержит:" "$item611_trace" >&2
        exit 1
    }
done
grep -Fq 'require_login_trace()' \
    scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-611: QEMU-проба не требует trace" >&2
    exit 1
}
grep -Fq 'successful trace lost a required seed stage' \
    scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-611: QEMU-проба не требует стадии seed" >&2
    exit 1
}
grep -Fq 'trace did not report the stored account' \
    scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-611: QEMU-проба не требует итерации учётной записи" >&2
    exit 1
}
