#!/usr/bin/env bash
# ITEM-506 increment 1: contracted slice of the HFOC OS check suite.  Sourced
# in pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The owner wants one account - a login and a password - that is set at
# installation, changed from the console and from the panel, and accepted by
# both entries.  This increment puts the model and its persistent record in
# place; the console, the installer and the panel are wired in the next
# increments, so this slice pins exactly the foundation:
#
#   * `appliance_admin_credential` owns the one policy (login 1..32 of
#     `A-Za-z0-9._-`, password 12..128 printable ASCII, no trimming, the
#     bounded weak list) and the PBKDF2-HMAC-SHA256 derivation with a 16-byte
#     salt and the iteration count stored next to the verifier;
#   * the canonical record value is
#     `pbkdf2-sha256$<iterations>$<salt-hex>$<verifier-hex>$<login>` and the
#     persistent secret store writes and reads exactly that value under the
#     old `v1:secret.admin.credential=` key;
#   * a legacy 64-character SHA-256 fingerprint is still read as itself but is
#     refused as a credential with a named status, and migration is an
#     explicit re-provisioning, never a silent reinterpretation;
#   * the secret-persistence catalogue names the new handling and checks only
#     the shared shape, leaving the exact grammar with the model;
#   * the focused host proof pins the policy, independently computed PBKDF2
#     vectors, the record fields, the absence of plaintext and the fail-closed
#     corruption path.

for item506_model_file in \
    kernel/include/appliance_admin_credential.h \
    kernel/src/appliance_admin_credential.c \
    kernel/include/appliance_persistent_secret_config.h \
    kernel/src/appliance_persistent_secret_config.c \
    kernel/src/appliance_secret_persistence.c \
    tests/appliance-admin-credential-host.c; do
    test -f "$item506_model_file" || {
        echo "ITEM-506: отсутствует файл: $item506_model_file" >&2
        exit 1
    }
done

# ----------------------------------------------------------------- the policy --
for item506_policy in \
    '#define APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MIN 1U' \
    '#define APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX 32U' \
    '#define APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MIN 12U' \
    '#define APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX 128U' \
    '#define APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH 16U' \
    '#define APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH 32U' \
    '#define APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT 200000U' \
    '#define APPLIANCE_ADMIN_CREDENTIAL_FAILURE_LIMIT 5U' \
    '#define APPLIANCE_ADMIN_CREDENTIAL_RECORD_LABEL "pbkdf2-sha256"'; do
    grep -Fq "$item506_policy" kernel/include/appliance_admin_credential.h || {
        echo "ITEM-506: политика не закреплена: $item506_policy" >&2
        exit 1
    }
done
grep -Fq 'appliance_admin_credential_validate_login' \
    kernel/include/appliance_admin_credential.h || {
    echo "ITEM-506: правило логина не объявлено" >&2
    exit 1
}
grep -Fq 'appliance_admin_credential_validate_password' \
    kernel/include/appliance_admin_credential.h || {
    echo "ITEM-506: правило пароля не объявлено" >&2
    exit 1
}
grep -Fq 'appliance_admin_credential_password_weak' \
    kernel/src/appliance_admin_credential.c || {
    echo "ITEM-506: встроенный список слабых не назван" >&2
    exit 1
}

# --------------------------------------------------------------- the KDF ------
# PBKDF2-HMAC-SHA256 on the in-tree SHA-256, not a single hash: the label, the
# iteration loop and the HMAC construction are all present.
grep -Fq '#include "sha256.h"' kernel/src/appliance_admin_credential.c || {
    echo "ITEM-506: KDF не построен на внутридеревном SHA-256" >&2
    exit 1
}
grep -Fq 'appliance_admin_credential_hmac_sha256' \
    kernel/src/appliance_admin_credential.c || {
    echo "ITEM-506: HMAC-плечо KDF не найдено" >&2
    exit 1
}
grep -Fq 'round < iterations' kernel/src/appliance_admin_credential.c || {
    echo "ITEM-506: цикл итераций PBKDF2 не найден" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN 10000U' \
    kernel/include/appliance_admin_credential.h || {
    echo "ITEM-506: нижняя граница итераций не закреплена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MAX 2000000U' \
    kernel/include/appliance_admin_credential.h || {
    echo "ITEM-506: верхняя граница итераций не закреплена" >&2
    exit 1
}
grep -Fq 'bytes_equal_constant_time' \
    kernel/src/appliance_admin_credential.c || {
    echo "ITEM-506: сравнение секретов не постоянного времени" >&2
    exit 1
}
grep -Fq 'volatile uint8_t *bytes = buffer;' \
    kernel/src/appliance_admin_credential.c || {
    echo "ITEM-506: обнуление буферов не найдено" >&2
    exit 1
}

# ------------------------------------------------------------- the record ------
grep -Fq 'appliance_admin_credential_record_encode' \
    kernel/include/appliance_admin_credential.h \
    kernel/src/appliance_admin_credential.c || {
    echo "ITEM-506: кодировщик record не объявлен" >&2
    exit 1
}
grep -Fq 'appliance_admin_credential_record_decode' \
    kernel/include/appliance_admin_credential.h \
    kernel/src/appliance_admin_credential.c || {
    echo "ITEM-506: разбор record не объявлен" >&2
    exit 1
}
grep -Fq 'appliance_admin_credential_record_is_legacy' \
    kernel/include/appliance_admin_credential.h \
    kernel/src/appliance_admin_credential.c || {
    echo "ITEM-506: legacy-форма не распознаётся явно" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ADMIN_CREDENTIAL_LEGACY_RECORD' \
    kernel/include/appliance_admin_credential.h || {
    echo "ITEM-506: отказ legacy-record не назван" >&2
    exit 1
}

# -------------------------------------------------------------- the store ------
grep -Fq 'appliance_persistent_secret_config_commit_admin_credential' \
    kernel/include/appliance_persistent_secret_config.h \
    kernel/src/appliance_persistent_secret_config.c || {
    echo "ITEM-506: запись учётной записи не объявлена" >&2
    exit 1
}
grep -Fq 'appliance_persistent_secret_config_admin_credential' \
    kernel/include/appliance_persistent_secret_config.h \
    kernel/src/appliance_persistent_secret_config.c || {
    echo "ITEM-506: чтение учётной записи не объявлено" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_PBKDF2_CREDENTIAL' \
    kernel/include/appliance_persistent_secret_config.h || {
    echo "ITEM-506: вид головы журнала не назван" >&2
    exit 1
}
for item506_refusal in \
    'APPLIANCE_PERSISTENT_SECRET_CONFIG_ABSENT' \
    'APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD'; do
    grep -Fq "$item506_refusal" \
        kernel/include/appliance_persistent_secret_config.h \
        kernel/src/appliance_persistent_secret_config.c || {
        echo "ITEM-506: именованный отказ не найден: $item506_refusal" >&2
        exit 1
    }
done
# The legacy writer and reader stay for the factory placeholder and for the
# old record this increment refuses as a credential.
grep -Fq 'appliance_persistent_secret_config_commit_admin(' \
    kernel/include/appliance_persistent_secret_config.h || {
    echo "ITEM-506: прежний писатель отпечатка удалён" >&2
    exit 1
}
grep -Fq 'appliance_persistent_secret_config_admin(' \
    kernel/include/appliance_persistent_secret_config.h || {
    echo "ITEM-506: прежний читатель отпечатка удалён" >&2
    exit 1
}

# ----------------------------------------------------------- the catalogue -----
grep -Fq 'APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER' \
    kernel/include/appliance_secret_persistence.h \
    kernel/src/appliance_secret_persistence.c || {
    echo "ITEM-506: каталог не называет PBKDF2-verifier" >&2
    exit 1
}
grep -Fq 'administrative credential PBKDF2-HMAC-SHA256 verifier with salt and iteration count' \
    kernel/src/appliance_secret_persistence.c || {
    echo "ITEM-506: описание записи каталога не переписано" >&2
    exit 1
}

# --------------------------------------------------------------- the tests -----
for item506_claim in \
    'the 10,000-iteration vector matches the independent computation' \
    'the 200,000-iteration default vector matches' \
    'the stored verifier is the PBKDF2 output, not the password' \
    'the canonical value never carries the password' \
    'the raw store never contains the plaintext password' \
    'the read-back record names its own salt and iteration count' \
    'the legacy record is refused as a credential by name' \
    'the migrated head is the structured credential' \
    'a corrupted record yields no credential, not a partial one' \
    'an iteration field that would wrap uint32_t is refused' \
    'a refused decode leaves no partially filled credential behind' \
    'a refused derivation zeroes a pre-filled verifier' \
    'the committed head zeroes the login padding the caller carried' \
    'the committed head matches the reloaded credential byte for byte'; do
    grep -Fq "$item506_claim" tests/appliance-admin-credential-host.c || {
        echo "ITEM-506: host-проба не утверждает: $item506_claim" >&2
        exit 1
    }
done
grep -Fq 'admin credential accepts the structured PBKDF2 value' \
    tests/appliance-secret-persistence-host.c || {
    echo "ITEM-506: проба каталога не знает новую форму значения" >&2
    exit 1
}
grep -Fq 'the catalogue accepts the shared shape and leaves the exact grammar to the credential module' \
    tests/appliance-secret-persistence-host.c || {
    echo "ITEM-506: граница каталога и модели не закреплена" >&2
    exit 1
}
grep -Fq 'test-appliance-admin-credential-host' Makefile || {
    echo "ITEM-506: цель host-пробы не зарегистрирована" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-admin-credential-host' Makefile || {
    echo "ITEM-506: host-проба не входит в test-host-suite" >&2
    exit 1
}

# ------------------------------------------------------------- the documents ---
QUEUE_DOC=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
# ITEM-510 made itself the current item and moved ITEM-506 into the history
# section; the pin follows the section it is about, unchanged in strength.
grep -A 3 '^## Item ITEM-506$' "$QUEUE_DOC" | grep -Fq 'ITEM-506' || {
    echo "ITEM-506: очередь не называет текущий пункт" >&2
    exit 1
}
grep -Fq 'Инкремент 1 (влит) — модель учётной записи' "$QUEUE_DOC" || {
    echo "ITEM-506: очередь не описывает первый инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-506 increment 1' docs/ARCHITECTURE.md || {
    echo "ITEM-506: ARCHITECTURE не описывает модель учётной записи" >&2
    exit 1
}
grep -Fq 'ITEM-506 increment 1' docs/ROADMAP.md || {
    echo "ITEM-506: ROADMAP не называет первый инкремент" >&2
    exit 1
}
