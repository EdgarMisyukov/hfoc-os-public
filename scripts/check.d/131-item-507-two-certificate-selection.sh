#!/usr/bin/env bash
# ITEM-507 increment 6: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# Two declared names are served two different certificates by the production
# SNI selection, on a live listener, and the declaration survives a reboot.
#
# Increment 5 gave the operator the page; this increment answers the item's own
# evidence requirement, which the unvalidated fixture clock had kept out of
# reach: the operator import path refuses a pair with BAD_CLOCK there (the QEMU
# fixture reaches no SNTP server, because the SNTP port is 123 and the test host
# runs unprivileged).  The proof therefore builds a fixture kernel that writes
# two test-only pairs through the same durable writer a panel commit uses.
# The claims pinned here:
#
#   * the fixture generates two EC P-256 pairs whose subjectAltName is the
#     fixture-only name `alpha.example`/`beta.example`, declares
#     `default=REFUSE`, writes them with `appliance_certificate_name_store_save`
#     and reads the record back through the loader path, and never renders key
#     material;
#   * the boot calls the fixture after binding the administrator enrollment and
#     before initialising the serving path, so the listener reads the committed
#     record; a later boot writes nothing and the same two certificates come
#     back;
#   * the fixture kernel is the production configuration plus the one fixture
#     define, only the kernel entry point and the fixture object carry it, and
#     the fixture ISO carries the boot-chain manifest and the license payload
#     the production ISO carries;
#   * the proof presents two declared names, the same name in another case, an
#     undeclared name and no name at all, and requires the certificates the
#     fixture published together with one selection record per handshake;
#   * the proof runs in the QEMU suite of CI and its script declares the
#     marker-timeout class when it gives up on a guest marker.
for item507_inc6_file in \
    kernel/src/appliance_certificate_name_fixture_qemu.c \
    kernel/src/kernel.c \
    kernel/include/appliance_certificate_name_store.h \
    scripts/test-qemu-production-sni-selection.sh \
    docs/hfoc-management-tls-lifecycle.md \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item507_inc6_file ]] || {
        echo "ITEM-507 inc6: отсутствует $item507_inc6_file" >&2
        exit 1
    }
done

# The fixture: two fixture-only names, the durable writer, the fail-closed
# declaration, and no key material in the record it publishes.
for item507_inc6_fixture in \
    'HFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST' \
    'APPLIANCE_CERTIFICATE_NAME_FIXTURE_FIRST_NAME "alpha.example"' \
    'APPLIANCE_CERTIFICATE_NAME_FIXTURE_SECOND_NAME "beta.example"' \
    'appliance_certificate_name_store_save' \
    'APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE' \
    'CERTIFICATE-FIXTURE' \
    'certificate_sha256='; do
    grep -Fq -- "$item507_inc6_fixture" \
        kernel/src/appliance_certificate_name_fixture_qemu.c || {
        echo "ITEM-507 inc6: фикстура не публикует $item507_inc6_fixture" >&2
        exit 1
    }
done
if grep -Fq 'PRIVATE KEY' kernel/src/appliance_certificate_name_fixture_qemu.c; then
    echo "ITEM-507 inc6: фикстура рендерит материал ключа" >&2
    exit 1
fi

# The hook: guarded, and between the enrollment binding and the serving init,
# so the listener reads the record the fixture committed.
grep -Fq 'appliance_certificate_name_fixture_seed(' kernel/src/kernel.c || {
    echo "ITEM-507 inc6: загрузка не вызывает фикстуру двух имён" >&2
    exit 1
}
python3 - <<'PY' || exit 1
import pathlib
import sys

text = pathlib.Path("kernel/src/kernel.c").read_text()
# The call is the last textual occurrence: the extern declaration comes first.
seed = text.rfind("appliance_certificate_name_fixture_seed(")
enrollment = text.find("console_bind_admin_enrollment(&production_admin_enrollment);")
serving = text.find("(void)appliance_production_serving_init(")
if min(seed, enrollment, serving) < 0:
    sys.exit("the fixture hook or the statements around it are missing")
guard = text.rfind(
    "#ifdef HFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST", 0, seed)
if not (enrollment <= guard < seed):
    sys.exit("the fixture call is not behind its own define in the boot path")
if not (enrollment < seed < serving):
    sys.exit("the fixture call is not between the enrollment and serving")
PY

# The build: a fixture kernel and its own ISO, the target that runs the proof,
# and both suite lists that make CI build and run it.
for item507_inc6_make in \
    'SNI_SELECTION_KERNEL' \
    'SNI_SELECTION_ISO' \
    'HFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST' \
    'test-production-sni-selection-qemu' \
    './scripts/test-qemu-production-sni-selection.sh'; do
    grep -Fq -- "$item507_inc6_make" Makefile || {
        echo "ITEM-507 inc6: Makefile не содержит $item507_inc6_make" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the Makefile variable is the literal checked
awk '/^QEMU_TEST_IMAGES :=/,/^$/' Makefile |
    grep -Fq '$(SNI_SELECTION_ISO)' || {
    echo "ITEM-507 inc6: QEMU_TEST_IMAGES не собирает фикстурный ISO" >&2
    exit 1
}
awk '/^QEMU_PARALLEL_PHASE_1 :=/,/^$/' Makefile |
    grep -Fq 'test-production-sni-selection-qemu' || {
    echo "ITEM-507 inc6: QEMU_PARALLEL_PHASE_1 не запускает доказательство" >&2
    exit 1
}

# The proof: executable, declares the timing class it can stop on, requires the
# two handshakes, the refusal, the no-SNI identity and the durable record, and
# never lets the fixture credential into the serial log.
[[ -x scripts/test-qemu-production-sni-selection.sh ]] || {
    echo "ITEM-507 inc6: доказательство не исполняемо" >&2
    exit 1
}
for item507_inc6_proof in \
    'HFOC-QEMU-MARKER-TIMEOUT marker=' \
    'result=EXACT cert=0' \
    'result=EXACT cert=1' \
    'name=unknown.example result=REFUSED cert=none reason=NOT_FOUND' \
    'name=- result=DEFAULT cert=default' \
    'seeded=NO persisted=NOT_WRITTEN source=COMMITTED count=2' \
    'Hfoc-507-fixture-secret' \
    'refusal_record_present' \
    'published no record; retrying' \
    'mode == "refuse"'; do
    grep -Fq -- "$item507_inc6_proof" \
        scripts/test-qemu-production-sni-selection.sh || {
        echo "ITEM-507 inc6: доказательство не требует $item507_inc6_proof" >&2
        exit 1
    }
done
# The affirmative-handshake retry waits with its own clock, and the clock lives
# in the client block - the heredoc written to sni-selection-client.py.  A
# retry whose module is not imported raises NameError on the first connection
# the guest is not ready for, i.e. it turns the flake it exists to absorb into
# a hard failure, so the import is part of the proof, not an incidental.
awk '/^cat >"\$TEST_DIR\/sni-selection-client.py" <<.CLIENTEOF.$/,/^CLIENTEOF$/' \
    scripts/test-qemu-production-sni-selection.sh | grep -Fq 'import time' || {
    echo "ITEM-507 inc6: повтор рукопожатия не импортирует свои часы" >&2
    exit 1
}
grep -Fq '[t]est-qemu-production-sni-selection\.sh' \
    .github/workflows/ci.yml || {
    echo "ITEM-507 inc6: CI не следит за утечкой процесса доказательства" >&2
    exit 1
}

# The documents: the increment is recorded where the item's lifecycle and the
# architecture are read.
grep -Fq 'ITEM-507 increment 6' docs/ROADMAP.md || {
    echo "ITEM-507 inc6: ROADMAP не записывает инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-507, инкремент 6' docs/ARCHITECTURE.md || {
    echo "ITEM-507 inc6: ARCHITECTURE не записывает инкремент" >&2
    exit 1
}
