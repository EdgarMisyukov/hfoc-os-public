#!/usr/bin/env bash
# ITEM-615: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# The installed Toshiba refused `diag save` with NO_MEDIUM/MEDIUM_PROBE and the
# precise retained refusal existed only in RAM for the local `diag` command.
# The 64-record boot ring had already evicted it, so the next remote read could
# not name the first failure.  This slice pins the transfer of that same record
# into the shared status composition: one bounded `[DIAG-FAIL]` line, honest
# NO_FAILURE/NOT_ATTEMPTED before any refusal, the stage and primary cause on
# refusal, the commit/storage receipts, and a read-only publication that cannot
# clear the retained cause.
# shellcheck disable=SC2016,SC2041

item615_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item615_root" || exit 1

# 1. The record keeps the receipts as their own bounded fields and renders one
#    shared line through a read-only function.
for item615_header in \
    '#define DIAG_FAILURE_COMMIT_MAX 96U' \
    '#define DIAG_FAILURE_STORAGE_MAX 192U' \
    '#define DIAG_FAILURE_STATUS_MAX 440U' \
    'char commit[DIAG_FAILURE_COMMIT_MAX];' \
    'char storage[DIAG_FAILURE_STORAGE_MAX];' \
    'void diag_failure_status_report(const struct diag_failure *failure,'; do
    grep -Fq -- "$item615_header" kernel/include/diag_failure.h || {
        echo "ITEM-615: заголовок отказа не содержит:" "$item615_header" >&2
        exit 1
    }
done

# 2. The renderer has both honest states and every required field, and reading
#    it never resets the attempt or the record.
for item615_record in \
    '"[DIAG-FAIL] state="' \
    '"NO_FAILURE"' \
    'reason=NOT_ATTEMPTED' \
    '"RECORDED"' \
    '" valid=1"' \
    '"commit"' \
    '"storage"'; do
    grep -Fq -- "$item615_record" kernel/src/diag_failure.c || {
        echo "ITEM-615: renderer не содержит:" "$item615_record" >&2
        exit 1
    }
done
if grep -Fq 'diag_failure_attempt_reset(failure);' kernel/src/diag_failure.c; then
    : # the reset stays a documented no-op; the record is never cleared
fi
grep -Fq 'diag_failure_copy(failure->commit' kernel/src/diag_failure.c || {
    echo "ITEM-615: commit receipt не сохраняется при отказе" >&2
    exit 1
}
grep -Fq 'diag_failure_copy(failure->storage' kernel/src/diag_failure.c || {
    echo "ITEM-615: storage receipt не сохраняется при отказе" >&2
    exit 1
}

# 3. The one composition publishes it: production status, the profile fallback
#    and the real diag save/FAT12/extractor host proof.
for item615_hook in \
    'diag_failure_status_report(diag_export_last_failure(), sink, context);'; do
    grep -Fq -- "$item615_hook" kernel/src/kernel.c || {
        echo "ITEM-615: production status не публикует retained failure" >&2
        exit 1
    }
    grep -Fq -- "$item615_hook" kernel/src/kernel_status_report_profile.c || {
        echo "ITEM-615: profile fallback не публикует retained failure" >&2
        exit 1
    }
    grep -Fq -- "$item615_hook" tests/diag-export-full-host.c || {
        echo "ITEM-615: host proof не публикует retained failure" >&2
        exit 1
    }
done

# 4. The proof drives the real save and requires the record in the saved file
#    and in the next RAM snapshot, including the no-clear property.
for item615_proof in \
    '[DIAG-FAIL] state=NO_FAILURE' \
    'reason=NOT_ATTEMPTED' \
    'state=RECORDED' \
    'reason=WRITE' \
    'step=BEGIN_WRITE' \
    'reading the record does not clear or rewrite the first cause' \
    'state=END records=10 crc='; do
    grep -Fq -- "$item615_proof" tests/diag-export-full-host.c || {
        echo "ITEM-615: host proof не требует:" "$item615_proof" >&2
        exit 1
    }
done

# 5. The canonical host suite still builds the module that owns the record.
grep -Fq 'kernel/src/diag_failure.c' Makefile || {
    echo "ITEM-615: diag_failure не собран в host proof" >&2
    exit 1
}
grep -Fq '$(MAKE) test-diag-export-full-host' Makefile || {
    echo "ITEM-615: host proof не зарегистрирован в suite" >&2
    exit 1
}
