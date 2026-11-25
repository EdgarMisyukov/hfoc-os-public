#!/usr/bin/env bash
# ITEM-508 increment 15: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The offline ACME directory exchange: a fixture kernel dials a TLS-1.3 stub
# through the real lwIP and mbedTLS binding of increment 13.
#
# Increment 15 shipped the exchange before its pin; this slice is that pin.  It
# keeps the proof honest in the places a later edit could quietly hollow out:
#
#   * the fixture family is a real family of the production kernel: it links
#     production objects, excludes the production anchor object (ISRG Root X1
#     is RSA and the frozen profile carries no RSA), and the production
#     kernel and ISO must not carry the fixture marker before or after the run;
#   * both halves of the proof carry the same fixed port, and the stub speaks
#     exactly TLS 1.3 with its bytes from the one fixture header;
#   * the binding's second verification read accepts `UINT32_MAX` only beside
#     `0`: a later edit must not turn every non-zero verdict into a success;
#   * the driver's records are single lines and name the binding status and the
#     mbedTLS error of a refusal, so "handshake failed" is distinguishable from
#     "the stub was never reached".
for item508_inc15_file in \
    Makefile \
    kernel/include/appliance_acme_transport_lwip.h \
    kernel/src/appliance_acme_transport_lwip.c \
    kernel/src/appliance_acme_directory_anchor_fixture.c \
    kernel/src/appliance_acme_directory_qemu.c \
    scripts/hfoc-acme-directory-stub.py \
    scripts/test-acme-directory-stub-host.sh \
    scripts/test-qemu-acme-directory.sh \
    tests/appliance-acme-directory-fixtures.h \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc15_file ]] || {
        echo "ITEM-508 inc15: отсутствует $item508_inc15_file" >&2
        exit 1
    }
done

# 1. The fixture family is a family of the production kernel, and only of it:
#    the production anchor object is filtered out of the link, because the two
#    definitions of the anchor symbols must not meet.
for item508_inc15_make_name in \
    'ACME_DIRECTORY_OBJ_DIR' \
    'ACME_DIRECTORY_CFLAGS' \
    'ACME_DIRECTORY_OBJECTS' \
    'ACME_DIRECTORY_KERNEL' \
    'ACME_DIRECTORY_ISO' \
    'ACME_DIRECTORY_DEPS'; do
    grep -Fq -- "$item508_inc15_make_name" Makefile || {
        echo "ITEM-508 inc15: Makefile не объявляет $item508_inc15_make_name" >&2
        exit 1
    }
done
grep -Fq -- '-DHFOC_APPLIANCE_ACME_DIRECTORY_QEMU_TEST' Makefile || {
    echo "ITEM-508 inc15: семейство не определяет макрос пробы" >&2
    exit 1
}
item508_inc15_objects=$(
    awk '/^ACME_DIRECTORY_OBJECTS[[:space:]]*:=/ { inside = 1 }
         inside { print }
         inside && $0 !~ /\\$/ { exit }' Makefile
)
# shellcheck disable=SC2016  # the compared text is literal, not expanded
[[ $item508_inc15_objects == *'$(filter-out'* ]] || {
    echo "ITEM-508 inc15: объекты семейства не исключают production-якорь" >&2
    exit 1
}
for item508_inc15_excluded in \
    'build/obj/appliance_acme_trust_anchor.o' \
    'build/obj/kernel.o'; do
    [[ $item508_inc15_objects == *"$item508_inc15_excluded"* ]] || {
        echo "ITEM-508 inc15: из компоновки не исключён $item508_inc15_excluded" >&2
        exit 1
    }
done

# 2. The production kernel and ISO never carry the fixture marker, and the
#    check runs on both sides of the harness: before it (a stale production
#    artifact is caught) and after it (the harness cannot have leaked one).
item508_inc15_target=$(
    awk '/^test-acme-directory-qemu:/ { inside = 1 }
         inside { print }
         inside && /^$/ { exit }' Makefile
)
[[ -n $item508_inc15_target ]] || {
    echo "ITEM-508 inc15: нет цели test-acme-directory-qemu" >&2
    exit 1
}
for item508_inc15_assert in \
    "strings \$(ACME_DIRECTORY_KERNEL) | grep -Fq '[ACME-DIRECTORY] proof init'" \
    "strings \$(ACME_DIRECTORY_ISO) | grep -Fq '[ACME-DIRECTORY] proof init'" \
    "if strings \$(KERNEL) | grep -Fq '[ACME-DIRECTORY]'" \
    "if strings \$(IMAGE) | grep -Fq '[ACME-DIRECTORY]'" \
    "HFOC_ACME_DIRECTORY_QEMU_TEST_PORT='18445'" \
    './scripts/test-qemu-acme-directory.sh'; do
    [[ $item508_inc15_target == *"$item508_inc15_assert"* ]] || {
        echo "ITEM-508 inc15: цель не содержит «$item508_inc15_assert»" >&2
        exit 1
    }
done
item508_inc15_kernel_asserts=$(
    grep -c 'FAIL: production kernel contains the directory proof marker' \
        <<<"$item508_inc15_target"
)
item508_inc15_iso_asserts=$(
    grep -c 'FAIL: production ISO contains the directory proof marker' \
        <<<"$item508_inc15_target"
)
[[ $item508_inc15_kernel_asserts -eq 2 && $item508_inc15_iso_asserts -eq 2 ]] || {
    echo "ITEM-508 inc15: проверка production-маркера стоит не с обеих сторон прогона" >&2
    exit 1
}

# 3. The proof really runs in the acceptance suite: the target is a member of
#    the parallel QEMU phase the CI executes.
item508_inc15_phase=$(
    awk '/^QEMU_PARALLEL_PHASE_1[[:space:]]*:=/ { inside = 1 }
         inside { print }
         inside && $0 !~ /\\$/ { exit }' Makefile
)
[[ $item508_inc15_phase == *test-acme-directory-qemu* ]] || {
    echo "ITEM-508 inc15: проба не входит в QEMU_PARALLEL_PHASE_1" >&2
    exit 1
}

# 4. One fixed port on both sides of the proof, and the stub speaks exactly
#    the one TLS version the frozen guest profile carries.
for item508_inc15_port in \
    '#define APPLIANCE_ACME_DIRECTORY_QEMU_PORT 18445U' \
    '"https://10.0.2.2:18445/directory"'; do
    grep -Fq -- "$item508_inc15_port" kernel/src/appliance_acme_directory_qemu.c || {
        echo "ITEM-508 inc15: драйвер не несёт «$item508_inc15_port»" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the compared text is literal, not expanded
grep -Fq 'PORT=${HFOC_ACME_DIRECTORY_QEMU_TEST_PORT:-18445}' \
    scripts/test-qemu-acme-directory.sh || {
    echo "ITEM-508 inc15: прогон не по умолчанию не несёт общий порт" >&2
    exit 1
}
for item508_inc15_stub in \
    'DIRECTORY_PATH = "/directory"' \
    'os.path.join(ROOT, "tests", "appliance-acme-directory-fixtures.h")' \
    'context.minimum_version = ssl.TLSVersion.TLSv1_3' \
    'context.maximum_version = ssl.TLSVersion.TLSv1_3' \
    'os.chmod(key_path, 0o600)'; do
    grep -Fq -- "$item508_inc15_stub" scripts/hfoc-acme-directory-stub.py || {
        echo "ITEM-508 inc15: заглушка не содержит «$item508_inc15_stub»" >&2
        exit 1
    }
done
# The bytes have one source: the fixture header, included by the guest anchor.
grep -Fq '#include "appliance-acme-directory-fixtures.h"' \
    kernel/src/appliance_acme_directory_anchor_fixture.c || {
    echo "ITEM-508 inc15: фикстурный якорь не берёт байты из одного заголовка" >&2
    exit 1
}
grep -Fq 'directory_fixture_ca' kernel/src/appliance_acme_directory_anchor_fixture.c || {
    echo "ITEM-508 inc15: фикстурный якорь не называет CA-массив" >&2
    exit 1
}

# 5. The second verification read: only zero and the "no session to inspect"
#    value are accepted as a verdict; every recorded failure flag still fails.
grep -Fq 'verify_result != 0U && verify_result != UINT32_MAX' \
    kernel/src/appliance_acme_transport_lwip.c || {
    echo "ITEM-508 inc15: повторное чтение вердикта ослаблено" >&2
    exit 1
}
grep -Fq 'appliance_acme_transport_lwip_last_tls_error' \
    kernel/include/appliance_acme_transport_lwip.h || {
    echo "ITEM-508 inc15: геттер ошибки TLS не опубликован" >&2
    exit 1
}
grep -Fq 'appliance_acme_transport_lwip_last_tls_error' \
    kernel/src/appliance_acme_transport_lwip.c || {
    echo "ITEM-508 inc15: геттер ошибки TLS не реализован" >&2
    exit 1
}

# 6. The driver publishes two single-line records; the failure record names
#    both the binding status and the mbedTLS error.
grep -Fq '#ifdef HFOC_APPLIANCE_ACME_DIRECTORY_QEMU_TEST' \
    kernel/src/appliance_acme_directory_qemu.c || {
    echo "ITEM-508 inc15: драйвер не ограждён макросом пробы" >&2
    exit 1
}
grep -Fq 'appliance_acme_directory_qemu_translation_unit_guard' \
    kernel/src/appliance_acme_directory_qemu.c || {
    echo "ITEM-508 inc15: пустой перевод драйвера не защищён от предупреждения" >&2
    exit 1
}
item508_inc15_begin=$(
    grep -c 'serial_sequence_begin()' kernel/src/appliance_acme_directory_qemu.c
)
item508_inc15_end=$(
    grep -c 'serial_sequence_end(' kernel/src/appliance_acme_directory_qemu.c
)
[[ $item508_inc15_begin -eq 2 && $item508_inc15_end -eq 2 ]] || {
    echo "ITEM-508 inc15: записи драйвера не обрамлены парой последовательности" >&2
    exit 1
}
grep -Fq ' tls=' kernel/src/appliance_acme_directory_qemu.c || {
    echo "ITEM-508 inc15: запись отказа не называет ошибку TLS" >&2
    exit 1
}
grep -Fq 'appliance_acme_directory_qemu_test_tick' kernel/src/kernel.c || {
    echo "ITEM-508 inc15: главный цикл не двигает драйвер" >&2
    exit 1
}
grep -Fq '[ACME-DIRECTORY] exchange ok' scripts/test-qemu-acme-directory.sh || {
    echo "ITEM-508 inc15: прогон не требует записи об успешном обмене" >&2
    exit 1
}
grep -Fq '[ACME-DIRECTORY] failed:' scripts/test-qemu-acme-directory.sh || {
    echo "ITEM-508 inc15: прогон не падает на записи отказа" >&2
    exit 1
}

# 7. The stub itself is proven on the host before any guest boots, in both
#    host aggregates.
grep -Fq 'test-appliance-acme-directory-stub-host' Makefile || {
    echo "ITEM-508 inc15: хост-проба заглушки не подключена к Makefile" >&2
    exit 1
}
item508_inc15_host_targets=$(
    awk '/[$][(]MAKE[)] test-appliance-acme-directory-stub-host/ { count += 1 } \
         END { print count + 0 }' Makefile
)
[[ $item508_inc15_host_targets -eq 2 ]] || {
    echo "ITEM-508 inc15: хост-проба заглушки не подключена к обоим наборам" >&2
    exit 1
}

# 8. The documents name the increment and its honest bound.
grep -Fq 'ITEM-508 increment 15' docs/ROADMAP.md || {
    echo "ITEM-508 inc15: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 15' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc15: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
