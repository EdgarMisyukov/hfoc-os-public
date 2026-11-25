#!/usr/bin/env bash
# ITEM-503: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The operator's own certificate (BYO) is imported through the appliance, is
# validated before anything is stored, and is persisted as record v2.
#
# The appliance used to serve only its own self-signed certificate: a browser
# warned, and the only way to remove the warning was to trust a certificate
# the appliance had generated for itself.  ITEM-503 makes the operator's
# certificate the primary path:
#
#   * the Security panel page carries a plain form (`POST /panel/security`)
#     and the same mutation is reachable at `POST /api/v1/system/certificate`
#     for a script; both resolve to mutation `CERTIFICATE_IMPORT` and command
#     id 12 (admin + configuration-write, exactly like every other mutation);
#   * the body is `application/x-www-form-urlencoded` with `certificate`,
#     optional `chain` and `key`; the bounded PEM decoder reads one block at a
#     time, so a chain bundle is decoded, never guessed at;
#   * the import gate validates the pair (`mbedtls_pk_check_pair`), the EC
#     P-256 family, the validity window against a validated clock, the SAN
#     against the appliance's declared name, and every member of a supplied
#     chain; a refusal is named and nothing is stored;
#   * the two-phase mutation plane executes the import: the first authorized
#     request stages a candidate and issues a single-use token, the exact
#     token commits, an explicit cancel or an expired window rolls the
#     candidate back, and an unbound executor is refused as unsupported;
#   * the accepted bytes are persisted as certificate record v2 (magic,
#     version 2, provenance, leaf, chain, key) in the CERTIFICATE store; a v1
#     record keeps being read, and the next boot loads the operator
#     certificate and its chain into the TLS configuration;
#   * the listener's configuration is built once per boot, so the import
#     applies on the next boot and the serving record says so
#     (`cert_imports=<n> cert_applies=reboot`).

for item503_file in \
    kernel/include/appliance_certificate_upload.h \
    kernel/src/appliance_certificate_upload.c \
    kernel/include/appliance_certificate_import.h \
    kernel/src/appliance_certificate_import.c \
    kernel/src/pem_decode.c \
    kernel/include/appliance_production_certificate.h \
    kernel/src/appliance_production_certificate.c \
    tests/appliance-certificate-fixtures.h \
    tests/appliance-certificate-upload-host.c; do
    test -f "$item503_file" || {
        echo "ITEM-503: отсутствует файл: $item503_file" >&2
        exit 1
    }
done

# ------------------------------------------------------- the upload module ---
for item503_upload in \
    appliance_certificate_upload_init \
    appliance_certificate_upload_set_clock \
    appliance_certificate_upload_stage \
    appliance_certificate_upload_commit \
    appliance_certificate_upload_rollback \
    APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_PEM_MAX \
    APPLIANCE_CERTIFICATE_UPLOAD_STORE_UNAVAILABLE; do
    grep -Fq "$item503_upload" kernel/include/appliance_certificate_upload.h || {
        echo "ITEM-503: модуль загрузки не публикует: $item503_upload" >&2
        exit 1
    }
done
for item503_refusal in \
    APPLIANCE_CERTIFICATE_UPLOAD_BAD_PAIR \
    APPLIANCE_CERTIFICATE_UPLOAD_BAD_CHAIN \
    APPLIANCE_CERTIFICATE_UPLOAD_CHAIN_MISMATCH \
    APPLIANCE_CERTIFICATE_UPLOAD_NAME_MISMATCH \
    APPLIANCE_CERTIFICATE_UPLOAD_BAD_CLOCK \
    APPLIANCE_CERTIFICATE_UPLOAD_MALFORMED \
    APPLIANCE_CERTIFICATE_UPLOAD_TOO_LARGE; do
    grep -Fq "$item503_refusal" kernel/src/appliance_certificate_upload.c || {
        echo "ITEM-503: отказ не назван: $item503_refusal" >&2
        exit 1
    }
done
grep -Fq 'appliance_certificate_upload_decode(' \
    kernel/src/appliance_certificate_upload.c || {
    echo "ITEM-503: форма не декодируется" >&2
    exit 1
}
grep -Fq 'APPLIANCE_CERTIFICATE_UPLOAD_EC_KEY_LABEL' \
    kernel/src/appliance_certificate_upload.c || {
    echo "ITEM-503: SEC1-ключ не принимается" >&2
    exit 1
}
grep -Fq 'appliance_certificate_import_validate(' \
    kernel/src/appliance_certificate_upload.c || {
    echo "ITEM-503: кандидат не проходит гейт импорта" >&2
    exit 1
}

# -------------------------------------------------------- record v2 chain ----
grep -Fq '#define APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BLOCKS 12U' \
    kernel/include/appliance_production_certificate.h || {
    echo "ITEM-503: запись не выросла до двенадцати блоков" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_VERSION_2 2U' \
    kernel/include/appliance_production_certificate.h || {
    echo "ITEM-503: версия записи 2 не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_CERTIFICATE_CHAIN_DER_MAX' \
    kernel/include/appliance_production_certificate.h || {
    echo "ITEM-503: граница цепочки не объявлена" >&2
    exit 1
}
for item503_record in \
    appliance_production_certificate_adopt \
    'chain_length' \
    'APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_OPERATOR' \
    'issuer'; do
    grep -Fq "$item503_record" \
        kernel/include/appliance_production_certificate.h || {
        echo "ITEM-503: запись v2 не публикует: $item503_record" >&2
        exit 1
    }
done
grep -Fq 'appliance_production_certificate_load_record(' \
    kernel/src/appliance_production_certificate.c || {
    echo "ITEM-503: запись не читается обратно" >&2
    exit 1
}
grep -Fq 'version == APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_VERSION' \
    kernel/src/appliance_production_certificate.c || {
    echo "ITEM-503: запись v1 не читается" >&2
    exit 1
}
grep -Fq 'mbedtls_x509_crt_parse_der(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-503: цепочка не попадает в TLS-конфигурацию" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_der_sequence_length(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-503: цепочка не разбирается по внешней длине" >&2
    exit 1
}

# --------------------------------------------------- the mutation target -----
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT = 6,' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-503: вид мутации сертификата не объявлен" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_CERTIFICATE_MUTATION 12U' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-503: команда сертификата не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_CERTIFICATE_PATH' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-503: API-цель сертификата не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SECURITY_PATH' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-503: цель панели не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT;' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-503: цель не маршрутизируется в мутацию" >&2
    exit 1
}
grep -Fq 'HTTP_POLICY_SECURITY_TARGET' kernel/include/http_parser.h \
    kernel/src/http_parser.c || {
    echo "ITEM-503: POST на /panel/security не разрешён" >&2
    exit 1
}
grep -Fq 'HTTP_POLICY_CERTIFICATE_TARGET' kernel/include/http_parser.h \
    kernel/src/http_parser.c || {
    echo "ITEM-503: POST на API-цель не разрешён" >&2
    exit 1
}

# ------------------------------------------------- the two-phase wiring ------
grep -Fq 'appliance_production_mutation_plane_bind_certificate(' \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_mutation_plane.c \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-503: исполнитель сертификата не привязывается" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_certificate_executor' \
    kernel/include/appliance_production_mutation_plane.h || {
    echo "ITEM-503: у плоскости нет исполнителя сертификата" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_certificate_stage' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-503: serving не привязывает стадию импорта" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_handle_certificate(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-503: маршрут импорта не обслуживается" >&2
    exit 1
}
grep -Fq 'cert_imports=' kernel/src/appliance_production_serving.c || {
    echo "ITEM-503: запись serving не называет импорты" >&2
    exit 1
}
grep -Fq 'cert_applies=' kernel/src/appliance_production_serving.c || {
    echo "ITEM-503: запись serving не называет применение" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_certificate_imports()' \
    kernel/src/kernel.c || {
    echo "ITEM-503: счётчик импортов не публикуется durable" >&2
    exit 1
}
grep -Fq 'persisted_operator' kernel/src/appliance_production_serving.c || {
    echo "ITEM-503: сертификат оператора не назван" >&2
    exit 1
}

# ------------------------------------------------------------ the panel ------
grep -Fq 'action=\"/panel/security\"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-503: панель не публикует форму импорта" >&2
    exit 1
}
for item503_field in 'name=\"certificate\"' 'name=\"chain\"' \
    'name=\"key\"' 'name=\"token\"'; do
    grep -Fq "$item503_field" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-503: форма не содержит поля: $item503_field" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SECURITY' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-503: страница Security не публикует форму" >&2
    exit 1
}

# ------------------------------------------------------------- the proofs ----
grep -Fq 'test-appliance-certificate-upload-host' Makefile || {
    echo "ITEM-503: Makefile не содержит пробу импорта" >&2
    exit 1
}
# shellcheck disable=SC2016  # the literal Makefile text is the contract
grep -Fq '$(MAKE) test-appliance-certificate-upload-host' Makefile || {
    echo "ITEM-503: host-набор не запускает пробу импорта" >&2
    exit 1
}
for item503_proof in \
    'a valid certificate and key stage a candidate' \
    'a chained certificate stages with its chain' \
    'a mismatched key is refused by name' \
    'an unvalidated clock refuses the import' \
    'a body over the framing bound is refused, not truncated' \
    'the confirmed candidate writes record v2' \
    'the next boot loads the operator record' \
    'the loaded record serves the operator certificate byte for byte' \
    'the loaded record carries the operator chain byte for byte' \
    'a cancelled candidate never replaces the stored certificate'; do
    grep -Fq "$item503_proof" \
        tests/appliance-certificate-upload-host.c || {
        echo "ITEM-503: host-проба не утверждает: $item503_proof" >&2
        exit 1
    }
done
for item503_plane_proof in \
    'certificate executor: an unbound executor is refused' \
    'certificate executor: an authorized import stages' \
    'certificate executor: a wrong token applies nothing' \
    'certificate executor: the confirmed import applies' \
    'certificate executor: the cancel rolls the candidate back' \
    'certificate executor: an unbound network mutation is refused'; do
    grep -Fq "$item503_plane_proof" \
        tests/appliance-production-mutation-plane-host.c || {
        echo "ITEM-503: проба плоскости не утверждает: $item503_plane_proof" >&2
        exit 1
    }
done
grep -Fq 'the BYO certificate import form is rendered' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-503: QEMU-проба не проверяет форму" >&2
    exit 1
}
grep -Fq 'an invalid import is refused by name' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-503: QEMU-проба не проверяет именованный отказ" >&2
    exit 1
}
# The fixture's lwIP pool holds six TCP PCBs and the phase already opens
# eleven connections, so the QEMU proof deliberately spends two: the Security
# page and the refused import.  The anonymous denial is proven by the host
# plane proof; a third connection is refused before the accept.
grep -Fq 'refused before the accept' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-503: QEMU-проба не объясняет бюджет соединений" >&2
    exit 1
}

# -------------------------------------------------------------- documents -----
grep -Fq 'ITEM-503' docs/ROADMAP.md || {
    echo "ITEM-503: ROADMAP не называет пункт" >&2
    exit 1
}
grep -Fq 'ITEM-503' docs/hfoc-management-tls-lifecycle.md || {
    echo "ITEM-503: документ TLS не описывает импорт" >&2
    exit 1
}
grep -Fq 'ITEM-503' docs/hfoc-management-web-ui.md || {
    echo "ITEM-503: документ панели не описывает форму" >&2
    exit 1
}
grep -Fq 'ITEM-503' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-503: очередь не называет пункт" >&2
    exit 1
}
grep -Fq 'ITEM-504' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-503: очередь не называет следующий пункт" >&2
    exit 1
}
