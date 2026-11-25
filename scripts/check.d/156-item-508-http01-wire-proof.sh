#!/usr/bin/env bash
# ITEM-508 increment 29: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal Makefile and shell text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The HTTP-01 wire proof.
#
# Increment 28 proved the listener's decisions on the host; ITEM-508 point 8 is
# about the wire, so this increment proves the production listener answers a
# real HTTP/1.1 client.  A fixture boot arms exactly one token - through the
# production arming call, with one fixture epoch that the kernel loop hands to
# the listener's `poll` as well - and this slice pins that nothing else about
# the production path changes: the fixture is one object, the production build
# never defines its macro, the harness forwards to guest port 80, and the proof
# asserts the positive answer, HEAD, every named refusal, and the counters.
for item508_inc29_file in \
    kernel/src/appliance_acme_http01_fixture_qemu.c \
    kernel/src/appliance_production_acme_http01.c \
    kernel/include/appliance_production_acme_http01.h \
    tests/appliance-acme-http01-host.c \
    scripts/test-qemu-acme-http01.sh \
    kernel/src/kernel.c \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc29_file ]] || {
        echo "ITEM-508 inc29: отсутствует $item508_inc29_file" >&2
        exit 1
    }
done

# 1. The fixture family is the production kernel plus one arming hook: the
#    production object is filtered out of the link and only the fixture object
#    carries the macro, so no other translation unit can see it.
for item508_inc29_makefile_pin in \
    'ACME_HTTP01_CFLAGS := $(PRODUCTION_CFLAGS) \' \
    '-DHFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST' \
    'build/obj/appliance_acme_http01_fixture_qemu.o \' \
    'build/obj/appliance_acme_fixture_clock_qemu.o,$(OBJECTS)) \' \
    '$(ACME_HTTP01_OBJ_DIR)/appliance_acme_http01_fixture_qemu.o' \
    '$(ACME_HTTP01_KERNEL): $(ACME_HTTP01_OBJECTS)' \
    '$(ACME_HTTP01_ISO): deps $(ACME_HTTP01_KERNEL)' \
    'test-acme-http01-qemu: $(ACME_HTTP01_ISO) iso' \
    'HFOC_ACME_HTTP01_QEMU_TEST_PORT='"'"'18447'"'"' \' \
    './scripts/test-qemu-acme-http01.sh'; do
    grep -Fq -- "$item508_inc29_makefile_pin" Makefile || {
        echo "ITEM-508 inc29: Makefile не содержит «$item508_inc29_makefile_pin»" \
            >&2
        exit 1
    }
done
# The proof target runs the script directly, so the script must carry the
# executable bit in the tree.  Run 37412945427 failed the whole acceptance phase
# with exit 126 after the build because the mode was 100644; nothing noticed it
# before the QEMU phase, so the tree pins the mode here.
[[ -x scripts/test-qemu-acme-http01.sh ]] || {
    echo "ITEM-508 inc29: scripts/test-qemu-acme-http01.sh не исполняем" >&2
    exit 1
}
# The macro is named by exactly one build family: a second family carrying it
# would compile the arming hook into a kernel that is not this proof.
item508_inc29_macro_uses=$(grep -Fc -- '-DHFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST' Makefile)
[[ $item508_inc29_macro_uses -eq 1 ]] || {
    echo "ITEM-508 inc29: макрос фикстуры объявлен $item508_inc29_macro_uses раз" >&2
    exit 1
}
# The proof runs in the parallel QEMU phase, so `make test` really executes it.
awk '
    /^QEMU_PARALLEL_PHASE_1[[:space:]]*:?=/ { inside = 1; next }
    inside && /^[[:space:]]*$/ { inside = 0 }
    inside && /^[A-Za-z_]+[[:space:]]*:?=/ { inside = 0 }
    inside { print }
' Makefile | grep -Fq 'test-acme-http01-qemu' || {
    echo "ITEM-508 inc29: проба не входит в параллельную QEMU-фазу" >&2
    exit 1
}
# The fixture record must stay out of the shipped kernel and image.
for item508_inc29_negative_pin in \
    "if strings \$(KERNEL) | grep -Fq '[ACME-HTTP01-FIXTURE]'" \
    "if strings \$(IMAGE) | grep -Fq '[ACME-HTTP01-FIXTURE]'"; do
    grep -Fq -- "$item508_inc29_negative_pin" Makefile || {
        echo "ITEM-508 inc29: цель не отвергает «$item508_inc29_negative_pin»" >&2
        exit 1
    }
done

# 2. The kernel gives the fixture family one clock: the listener's `poll` and
#    the arming hook receive the same fixture epoch, so the arming instant and
#    every request decision share one window.  The production branch keeps the
#    validated production clock, unchanged.  A fixture that armed with the
#    raw production clock armed nothing at all: with no SNTP peer that clock is
#    zero, and the decision module refuses a zero clock by name (`BAD_STATE`,
#    measured on run 37415921700).  ITEM-508 increment 31 moved the epoch
#    itself into the shared fixture clock unit, so the value is named once for
#    both ACME fixture families; this slice still pins that the wire proof's
#    branch reads it and that nothing in the branch names a second clock.
grep -Fq 'appliance_acme_http01_fixture_qemu_tick(' kernel/src/kernel.c || {
    echo "ITEM-508 inc29: kernel.c не вызывает фикстурный тик" >&2
    exit 1
}
python3 - <<'PY' || exit 1
import pathlib
import re

text = pathlib.Path("kernel/src/kernel.c").read_text()
# The fixture branch is found by its widened guard (increment 31 gives both
# fixture families one clock) and the poll and tick it holds; the extern
# declaration earlier in the file carries no poll and is skipped.
guard = "#if defined(HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST) ||"
block = None
for match in re.finditer(re.escape(guard), text):
    position = match.start()
    else_at = text.find("#else", position)
    end = text.find("#endif", else_at if else_at != -1 else position)
    if else_at == -1 or end == -1:
        continue
    candidate = text[position:else_at]
    if "appliance_production_acme_http01_poll(" in candidate and \
            "appliance_acme_http01_fixture_qemu_tick(" in candidate:
        block = (candidate, text[else_at:end])
        break
if block is None:
    raise SystemExit("ITEM-508 inc29: фикстурная ветвь с poll и tick не найдена")
fixture, production = block
if fixture.count("appliance_acme_fixture_epoch_seconds()") != 1:
    raise SystemExit("ITEM-508 inc29: часы фикстуры названы не ровно один раз")
for call in ("appliance_production_acme_http01_poll(",
             "appliance_acme_http01_fixture_qemu_tick("):
    index = fixture.find(call)
    if index == -1:
        raise SystemExit("ITEM-508 inc29: в фикстурной ветви нет " + call)
    arguments = fixture[index:fixture.find(";", index)]
    if "fixture_now_seconds" not in arguments:
        raise SystemExit("ITEM-508 inc29: " + call + " получает не часы фикстуры")
if re.search(
        r"appliance_production_acme_http01_poll\(\s*&production_acme_http01,\s*"
        r"appliance_production_network_time_epoch_seconds\(\s*service_now_ms\)\);",
        production) is None:
    raise SystemExit("ITEM-508 inc29: production-ветвь потеряла проверенные часы")
PY
# The ordinary boot still arms nothing: the production kernel.c has one
# arming site and it is the fixture one.
if grep -Fq 'appliance_production_acme_http01_arm(' kernel/src/kernel.c; then
    echo "ITEM-508 inc29: kernel.c вооружает токен вне фикстуры" >&2
    exit 1
fi

# 3. The fixture decides nothing: it arms through the production call, only
#    once the production listener is listening, with one named nonzero fixture
#    epoch, and every outcome is a bounded serial sequence record.
for item508_inc29_fixture_pin in \
    'appliance_production_acme_http01_listening(http01)' \
    'appliance_production_acme_http01_arm(' \
    'APPLIANCE_ACME_HTTP01_FIXTURE_TOKEN "hfoc-fixture-token"' \
    'APPLIANCE_ACME_HTTP01_FIXTURE_KEY_AUTHORIZATION \' \
    '"hfoc-fixture-token.hfoc-fixture-thumbprint"' \
    '"[ACME-HTTP01-FIXTURE] v=1 armed=1 token="' \
    '"[ACME-HTTP01-FIXTURE] v=1 armed=0 status="' \
    'appliance_acme_challenge_status_string(status)' \
    'serial_sequence_begin()' \
    'serial_sequence_end(serial_flags)'; do
    grep -Fq -- "$item508_inc29_fixture_pin" \
        kernel/src/appliance_acme_http01_fixture_qemu.c || {
        echo "ITEM-508 inc29: фикстура не содержит «$item508_inc29_fixture_pin»" \
            >&2
        exit 1
    }
done
# The zero clock is exactly the defect this pin exists for, and increment 31
# moved the one nonzero epoch into the shared fixture clock unit: this fixture
# must name no epoch of its own at all.
if grep -Fq 'FIXTURE_EPOCH' \
    kernel/src/appliance_acme_http01_fixture_qemu.c; then
    echo "ITEM-508 inc29: фикстура снова называет свою эпоху" >&2
    exit 1
fi
# The fixture may not build its own challenge or open its own socket.
for item508_inc29_fixture_forbidden in \
    'appliance_acme_challenge_arm_token(' \
    'tcp_new(' \
    'tcp_bind(' \
    'hfoc_lwip_tcp_init('; do
    if grep -Fq -- "$item508_inc29_fixture_forbidden" \
        kernel/src/appliance_acme_http01_fixture_qemu.c; then
        echo "ITEM-508 inc29: фикстура дублирует «$item508_inc29_fixture_forbidden»" \
            >&2
        exit 1
    fi
done

# 4. The harness speaks HTTP/1.1 to guest port 80 and requires the positive
#    answer, HEAD, every named refusal and the counters.
for item508_inc29_script_pin in \
    'GUEST_PORT=${HFOC_ACME_HTTP01_QEMU_TEST_GUEST_PORT:-80}' \
    'hostfwd=tcp:127.0.0.1:$PORT-:$GUEST_PORT' \
    'wait_for_armed 300' \
    'wait_for_served 60' \
    '[ACME-HTTP01-FIXTURE] v=1 armed=0' \
    "grep -Fq 'armed=1'" \
    "grep -Fq 'served=2'" \
    'key_authorization = b"hfoc-fixture-token.hfoc-fixture-thumbprint"' \
    'target = "/.well-known/acme-challenge/" + token' \
    'expect("GET", "/", 404, 0, b"")' \
    'expect("GET", "/.well-known/acme-challenge/hfoc-fixture-wrong", 404, 0, b"")' \
    'expect("GET", target + "?x=1", 404, 0, b"")' \
    'expect("POST", target, 405, 0, b"", ("Content-Length: 0",))' \
    'expect("HEAD", target, 200, len(key_authorization), b"")' \
    'expect("GET", target, 200, len(key_authorization), key_authorization)' \
    'HFOC-QEMU-MARKER-TIMEOUT' \
    '*** HFOC KERNEL PANIC ***'; do
    grep -Fq -- "$item508_inc29_script_pin" scripts/test-qemu-acme-http01.sh || {
        echo "ITEM-508 inc29: прогон не содержит «$item508_inc29_script_pin»" \
            >&2
        exit 1
    }
done
# The harness must not judge the answer by anything but the armed bytes: a
# substring match would pass a longer body that merely starts right.
if grep -Fq 'in body' scripts/test-qemu-acme-http01.sh; then
    echo "ITEM-508 inc29: прогон сравнивает тело подстрокой" >&2
    exit 1
fi

# 5. The listener takes exactly one request line from a received segment.  A
#    real HTTP/1.1 client sends the request line and its headers in one
#    segment; the first wire-proof run accumulated the whole segment and the
#    parser refused every request as a malformed `400` before the armed answer
#    was reached.  The boundary is one named function, the connection uses it
#    before the decision, and the whole segment is never appended as a line.
for item508_inc29_line_pin in \
    'size_t appliance_production_acme_http01_line_bytes(' \
    'return index + 1U;' \
    'line_bytes = appliance_production_acme_http01_line_bytes(' \
    'connection->line_length += (uint32_t)line_bytes;' \
    'if (overflow || connection->line_length == sizeof(connection->line)) {'; do
    grep -Fq -- "$item508_inc29_line_pin" \
        kernel/src/appliance_production_acme_http01.c || {
        echo "ITEM-508 inc29: слушатель не содержит «$item508_inc29_line_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'size_t appliance_production_acme_http01_line_bytes(' \
    kernel/include/appliance_production_acme_http01.h || {
    echo "ITEM-508 inc29: заголовок не объявляет границу строки" >&2
    exit 1
}
if grep -Fq 'connection->line_length += (uint32_t)received;' \
    kernel/src/appliance_production_acme_http01.c; then
    echo "ITEM-508 inc29: слушатель снова копит весь сегмент в строку" >&2
    exit 1
fi
python3 - <<'PY' || exit 1
import pathlib

source = pathlib.Path(
    "kernel/src/appliance_production_acme_http01.c").read_text()
boundary = source.find(
    "line_bytes = appliance_production_acme_http01_line_bytes(")
serve = source.find("appliance_production_acme_http01_serve_line(connection);")
if boundary == -1 or serve == -1 or boundary > serve:
    raise SystemExit(
        "ITEM-508 inc29: граница строки не применяется до решения")
PY
# The host proof carries the boundary and the negative control that made the
# defect visible on the wire.
for item508_inc29_line_probe in \
    'only the request line is taken from a segment with headers' \
    'a line with no ending stays incomplete' \
    'the raw segment with headers is refused as malformed' \
    'the extracted request line parses to the exact target'; do
    grep -Fq -- "$item508_inc29_line_probe" tests/appliance-acme-http01-host.c || {
        echo "ITEM-508 inc29: проба не содержит «$item508_inc29_line_probe»" \
            >&2
        exit 1
    }
done

# 6. The documents name the increment.
for item508_inc29_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 29' "$item508_inc29_doc" || {
        echo "ITEM-508 inc29: $item508_inc29_doc не называет инкремент" >&2
        exit 1
    }
done
