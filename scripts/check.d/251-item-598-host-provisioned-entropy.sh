#!/usr/bin/env bash
# ITEM-598: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The normal no-RDRAND/RDSEED path is a host-provisioned seed sealed under the
# ordinary administrator password.  The parser, two-slot durable state,
# installer declaration and QEMU proof must remain one chain.
for host_seed_file in \
    kernel/include/appliance_host_seed.h \
    kernel/src/appliance_host_seed.c \
    scripts/hfoc-host-seed-provision.py \
    scripts/test-qemu-host-provisioned-entropy.sh \
    tests/appliance-host-seed-host.c \
    tests/test-host-seed-provision.sh; do
    [[ -f $host_seed_file ]] || {
        echo "ITEM-598: missing $host_seed_file" >&2
        exit 1
    }
done
test -x scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-598: QEMU proof is not executable" >&2
    exit 1
}
test -x tests/test-host-seed-provision.sh || {
    echo "ITEM-598: provisioning proof is not executable" >&2
    exit 1
}
bash -n scripts/hfoc-install-media-stage.sh
bash -n scripts/test-qemu-host-provisioned-entropy.sh
bash -n tests/test-host-seed-provision.sh
python3 -m py_compile scripts/hfoc-host-seed-provision.py

# The only initial unpredictability is the ready host kernel CSPRNG.  Target,
# build id and time are context and never entropy.
for provision_contract in \
    'seed = os.getrandom(SEED_LENGTH)' \
    'flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL' \
    'refusing to write a personalised seed inside the repository' \
    'record[88:120] = hashlib.sha256(record[:88]).digest()' \
    'header_sha256='; do
    grep -Fq -- "$provision_contract" \
        scripts/hfoc-host-seed-provision.py || {
        echo "ITEM-598: provisioner is missing: $provision_contract" >&2
        exit 1
    }
done
if grep -Eq '\brandom\.(random|randint|getrandbits|choice)\b' \
    scripts/hfoc-host-seed-provision.py; then
    echo "ITEM-598: provisioner has a non-CSPRNG fallback" >&2
    exit 1
fi

# The installed record is two-slot, password-authenticated and only returned
# after a durable write/readback of the next sequence.
for store_contract in \
    'APPLIANCE_HOST_SEED_STATE_SLOT_COUNT 2U' \
    'appliance_host_seed_store_provision' \
    'appliance_host_seed_store_unlock' \
    'appliance_host_seed_store_rekey' \
    'appliance_host_seed_media_match' \
    'appliance_state_crypto_pbkdf2_sha256' \
    'appliance_state_crypto_ctr' \
    'appliance_state_crypto_hmac_sha256' \
    'APPLIANCE_HOST_SEED_WRONG_PASSWORD'; do
    grep -Fq -- "$store_contract" \
        kernel/include/appliance_host_seed.h \
        kernel/src/appliance_host_seed.c || {
        echo "ITEM-598: sealed host seed store is missing: $store_contract" >&2
        exit 1
    }
done
grep -Fq 'production_install_flow_provision_host_seed' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-598: install flow does not provision the host seed" >&2
    exit 1
}
grep -Fq 'production_install_flow_set_existing_operator_credential' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-598: update does not verify the existing account" >&2
    exit 1
}
grep -Fq 'console_bind_entropy_login' kernel/src/kernel.c || {
    echo "ITEM-598: ordinary boot does not bind the seed unlock" >&2
    exit 1
}
grep -Fq 'console_bind_admin_credential_rekey' kernel/src/kernel.c || {
    echo "ITEM-598: console password change does not rekey the seed" >&2
    exit 1
}
grep -Fq 'appliance_production_admin_enrollment_verify_credential_pair' \
    kernel/src/kernel.c || {
    echo "ITEM-598: entropy login does not use no-session account proof" >&2
    exit 1
}
grep -Fq 'appliance_production_admin_enrollment_publish_verification' \
    kernel/src/kernel.c || {
    echo "ITEM-598: entropy login does not gate publication on seed success" >&2
    exit 1
}
if awk '
    /static struct console_entropy_login_result kernel_production_entropy_unlock/ {
        inside = 1
    }
    inside && /appliance_production_admin_enrollment_authenticate_credential/ {
        found = 1
    }
    inside && /^}/ { exit }
    END { exit found ? 0 : 1 }
' kernel/src/kernel.c; then
    echo "ITEM-598: entropy login issues a bearer session before seed unlock" >&2
    exit 1
fi
grep -Fq '[RNG] host unlock stage=' kernel/src/console.c || {
    echo "ITEM-598: entropy login does not publish the failed stage" >&2
    exit 1
}
grep -Fq 'if (admin_credentials_active || entropy_seed_active)' \
    kernel/src/console.c || {
    echo "ITEM-598: local-menu offer can paint over hidden input" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_account_rekey_host_seed' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-598: panel password change does not rekey the seed" >&2
    exit 1
}

# HOST_PROVISIONED seeds one bounded pool.  Rotation must fail closed rather
# than feed derived bytes back as newly credited entropy.
grep -Fq 'APPLIANCE_PRODUCTION_ENTROPY_SOURCE_HOST_PROVISIONED' \
    kernel/src/appliance_production_entropy.c || {
    echo "ITEM-598: entropy owner has no host-provisioned source" >&2
    exit 1
}
if ! awk '
    /static bool appliance_production_entropy_rotate/ { inside = 1 }
    inside && /SOURCE_HOST_PROVISIONED/ { source = 1 }
    inside && source && /return false;/ { refused = 1 }
    inside && /^}/ { exit }
    END { exit refused ? 0 : 1 }
' kernel/src/appliance_production_entropy.c; then
    echo "ITEM-598: host-provisioned pool rotation does not fail closed" >&2
    exit 1
fi
if grep -Eq 'host_seed_retained|host_epoch' \
    kernel/src/appliance_production_entropy.c; then
    echo "ITEM-598: entropy owner retains the seed for derived rotation" >&2
    exit 1
fi

# Media staging requires a private target/build-bound record, and the
# declaration carries that requirement into the installer.
for media_contract in \
    'HFOC_INSTALL_MEDIA_HOST_SEED' \
    'HFOC-INSTALL-HOST-SEED=required' \
    'module_path: boot():/boot/hfoc-seed.bin' \
    'A host-seeded medium must declare whole-disk or update layout'; do
    grep -Fq -- "$media_contract" scripts/hfoc-install-media-stage.sh || {
        echo "ITEM-598: media staging is missing: $media_contract" >&2
        exit 1
    }
done
grep -Fq 'HFOC-INSTALL-HOST-SEED=' \
    kernel/include/production_install_media.h || {
    echo "ITEM-598: media declaration has no host seed directive" >&2
    exit 1
}

# The formal QEMU proof uses no hardware RNG, no recovery hex and no artifact
# path, and is part of the canonical serial QEMU queue.
grep -Fq -- '-cpu qemu64' scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-598: QEMU proof does not pin the no-hardware-RNG CPU" >&2
    exit 1
}
grep -Fq 'reason=no_hardware_entropy_source' \
    scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-598: QEMU proof does not require the no-hardware-RNG reason" >&2
    exit 1
}
for qemu_ready_marker in \
    'source=HOST_PROVISIONED' \
    'state=READY credited=32'; do
    grep -Fq "$qemu_ready_marker" \
        scripts/test-qemu-host-provisioned-entropy.sh || {
        echo "ITEM-598: QEMU proof is missing $qemu_ready_marker" >&2
        exit 1
    }
done
grep -Fq 'expect_slots "3 2"' \
    scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-598: QEMU proof does not read the first-boot epoch slots" >&2
    exit 1
}
grep -Fq 'expect_slots "3 4"' \
    scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-598: QEMU proof does not read the password-change epoch slots" >&2
    exit 1
}
grep -Fq 'type_line(monitor, "credentials")' \
    scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-598: QEMU proof does not open the password change" >&2
    exit 1
}
grep -Fq '[ADMIN-ENROLLMENT] credentials changed login=' \
    scripts/test-qemu-host-provisioned-entropy.sh || {
    echo "ITEM-598: QEMU proof does not change and re-seal the password" >&2
    exit 1
}
if grep -Fq 'type_line(monitor, "entropy seed"' \
        scripts/test-qemu-host-provisioned-entropy.sh ||
    grep -Fq "type_line(monitor, 'entropy seed'" \
        scripts/test-qemu-host-provisioned-entropy.sh; then
    echo "ITEM-598: QEMU proof types the recovery hex command" >&2
    exit 1
fi
if grep -Fq 'HFOC_QEMU_SUITE_ARTIFACT_DIR' \
    scripts/test-qemu-host-provisioned-entropy.sh; then
    echo "ITEM-598: personalised medium can reach the CI artifact tree" >&2
    exit 1
fi
# shellcheck disable=SC2016  # the proof's literal shell text is the contract
if ! grep -Fq 'rm -rf "$TEST_DIR"' \
        scripts/test-qemu-host-provisioned-entropy.sh ||
    ! grep -Fq 'trap cleanup EXIT' \
        scripts/test-qemu-host-provisioned-entropy.sh; then
    echo "ITEM-598: QEMU proof does not delete its personalized medium" >&2
    exit 1
fi
grep -Fq 'test-host-provisioned-entropy-qemu' Makefile || {
    echo "ITEM-598: focused QEMU target is missing" >&2
    exit 1
}
if ! awk '
    /^QEMU_SERIAL_TARGETS :=/ { inside = 1 }
    inside && /test-host-provisioned-entropy-qemu/ { found = 1 }
    inside && !/\\$/ { exit }
    END { exit found ? 0 : 1 }
' Makefile; then
    echo "ITEM-598: focused QEMU proof is not in the canonical suite" >&2
    exit 1
fi
grep -Fq 'test-appliance-host-seed-host' Makefile || {
    echo "ITEM-598: sealed-store host proof is not registered" >&2
    exit 1
}
grep -Fq 'test-host-seed-provision-host' Makefile || {
    echo "ITEM-598: provisioning host proof is not registered" >&2
    exit 1
}

# The manuals describe the actual password-unlocked lifecycle and its limits.
for host_seed_doc in \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/hfoc-production-install-flow.md \
    docs/superpowers/plans/2026-11-13-item-598-host-provisioned-entropy.md; do
    grep -Fq 'HOST_PROVISIONED' "$host_seed_doc" || {
        echo "ITEM-598: $host_seed_doc does not name HOST_PROVISIONED" >&2
        exit 1
    }
done
for lifecycle_word in normal login/password durable epoch destroy overwrite; do
    grep -Fq "$lifecycle_word" \
        docs/superpowers/plans/2026-11-13-item-598-host-provisioned-entropy.md || {
        echo "ITEM-598: design does not state: $lifecycle_word" >&2
        exit 1
    }
done
grep -Fq 'ITEM-598' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-598: active queue does not record the item" >&2
    exit 1
}
