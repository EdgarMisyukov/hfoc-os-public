#!/usr/bin/env bash
# ITEM-508 increment 24: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The ACME account key is the persisted appliance identity key.
#
# An ACME account is named by the public key its requests are signed with, so a
# key that changed on every boot would present a new account each boot and no
# order or renewal could ever belong to the appliance.  The P-256 identity key
# the certificate owner already persists in the CERTIFICATE store is the one
# key that survives a reboot, so `appliance_acme_account_bind()` is the single
# binding point: it hands the prepared `key_der` to the JWS module (which owns
# the private context) and publishes the public RFC 7638 thumbprint, never a
# key byte.  This slice pins the module, its named refusals, the record, the
# proof that the thumbprint survives a reload, and the documents.
for item508_inc24_file in \
    kernel/include/appliance_acme_account.h \
    kernel/src/appliance_acme_account.c \
    tests/hfoc-mbedtls-upload-host-config.h \
    tests/appliance-certificate-upload-host.c \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc24_file ]] || {
        echo "ITEM-508 inc24: отсутствует $item508_inc24_file" >&2
        exit 1
    }
done

# 1. The module publishes the binding, the record and every named refusal.
for item508_inc24_header_pin in \
    '#define APPLIANCE_ACME_ACCOUNT_VERSION 1U' \
    '#define APPLIANCE_ACME_ACCOUNT_RECORD_MAX 96U' \
    'APPLIANCE_ACME_ACCOUNT_IDENTITY_UNAVAILABLE' \
    'APPLIANCE_ACME_ACCOUNT_KEY_UNUSABLE' \
    'APPLIANCE_ACME_ACCOUNT_KEY_REFUSED' \
    'appliance_acme_account_bind(' \
    'appliance_acme_account_record(' \
    'the ACME account key is the persisted appliance' ; do
    grep -Fq -- "$item508_inc24_header_pin" \
        kernel/include/appliance_acme_account.h || {
        echo "ITEM-508 inc24: заголовок не содержит «$item508_inc24_header_pin»" \
            >&2
        exit 1
    }
done
for item508_inc24_source_pin in \
    '#if defined(HFOC_PRODUCTION_TLS)' \
    'appliance_acme_key_import(key, identity->key_der,' \
    'identity->available' \
    'identity->key_length == 0U' \
    'APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX' \
    'appliance_acme_key_thumbprint(key)' \
    '"thumbprint="' \
    '"unbound"'; do
    grep -Fq -- "$item508_inc24_source_pin" \
        kernel/src/appliance_acme_account.c || {
        echo "ITEM-508 inc24: модуль не содержит «$item508_inc24_source_pin»" \
            >&2
        exit 1
    }
done
# The record never carries key bytes: the header takes no key buffer, it names
# the prepared identity and publishes the thumbprint only.
if grep -Eq 'key_der[[:space:]]*[,;)]' \
        kernel/include/appliance_acme_account.h; then
    echo "ITEM-508 inc24: заголовок выдаёт приватный материал" >&2
    exit 1
fi

# 2. The proof target links the binding and its JWS dependency, and the host
#    mbedTLS profile carries the writer the JWS module needs to link.
for item508_inc24_makefile_pin in \
    'kernel/src/appliance_acme_account.c' \
    'kernel/src/appliance_acme_jws.c' \
    'third_party/mbedtls/library/x509write_csr.c' \
    'kernel/include/appliance_acme_account.h'; do
    grep -Fq -- "$item508_inc24_makefile_pin" Makefile || {
        echo "ITEM-508 inc24: Makefile не содержит «$item508_inc24_makefile_pin»" \
            >&2
        exit 1
    }
done
grep -Fq '#define MBEDTLS_X509_CSR_WRITE_C' \
    tests/hfoc-mbedtls-upload-host-config.h || {
    echo "ITEM-508 inc24: host-профиль не включает писатель CSR" >&2
    exit 1
}

# 3. The host proof drives the reboot, a second key and every refusal.
for item508_inc24_case in \
    'the account key binds from the persisted identity' \
    'the account thumbprint has the RFC 7638 length' \
    'the bound record names the public thumbprint and nothing else' \
    'the account key binds again from the loaded identity' \
    'the account thumbprint survives the reboot byte for byte' \
    'a different key names a different account, never the old one' \
    'an unavailable identity is refused by name' \
    'a key outside the record bound is refused before parsing' \
    'a DER that is not an EC P-256 key is refused' \
    'a refused bind publishes no account record'; do
    grep -Fq -- "$item508_inc24_case" \
        tests/appliance-certificate-upload-host.c || {
        echo "ITEM-508 inc24: проба не проверяет «$item508_inc24_case»" >&2
        exit 1
    }
done

# 4. The documents name the increment and its boundary.
grep -Fq 'ITEM-508 increment 24' docs/ROADMAP.md || {
    echo "ITEM-508 inc24: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 24' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc24: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
