#!/usr/bin/env bash
# ITEM-462: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The CI diagnostics artifact must stay small and useful.
#
# The owner's GitHub alert was that the free Actions storage was exhausted:
# 418 artifacts held 4.1 GiB, the largest `hfoc-ci-diagnostics-<run_id>` at
# ~481 MiB, because the upload shipped all of `build/test-artifacts/**` (raw
# disk images and ISOs; 16 GiB on the runner) with a 90-day retention.  The
# claims pinned here:
#
#   * the collector stages text evidence only - logs, `*.txt`, `*.sha256`,
#     summaries - keeps a bounded tail of each large file, never copies a raw
#     image or an ISO, names each excluded file, and stops at a byte budget;
#   * the runner cleanup removes test-artifact directories older than the
#     retention and publishes the count and the bytes it removed;
#   * the workflow's diagnostics upload takes the staged directory, repeats the
#     `!**/*.raw`/`!**/*.img`/`!build/hfoc-os-*.iso` exclusions, keeps a
#     bounded retention, and could not ship the raw tree unqualified;
#   * the guard and the host proof that drives these rules are registered.
for ci_artifacts_file in \
    scripts/hfoc-ci-artifacts.sh \
    tests/test-ci-artifacts-host.sh \
    .github/workflows/ci.yml \
    docs/ci-self-hosted-runner.md; do
    [[ -f $ci_artifacts_file ]] || {
        echo "ITEM-462: отсутствует $ci_artifacts_file" >&2
        exit 1
    }
done

# The three modes exist and publish the markers a reader greps for.
for ci_artifacts_mode in \
    'cleanup) cmd_cleanup' \
    'collect) cmd_collect' \
    'guard) cmd_guard' \
    '[CI-ARTIFACTS] cleanup root=' \
    '[CI-ARTIFACTS] collect root=' \
    '[CI-ARTIFACTS] guard workflow='; do
    grep -Fq -- "$ci_artifacts_mode" scripts/hfoc-ci-artifacts.sh || {
        echo "ITEM-462: инструмент не содержит:" "$ci_artifacts_mode" >&2
        exit 1
    }
done

# The collector keeps text evidence, a bounded tail, a budget and the
# exclusions' own record.
# shellcheck disable=SC2016  # the tool's literal text is what the search matches
for ci_artifacts_collect in \
    'is_text_evidence()' \
    '*.log | *.txt | *.sha256 | *.md | *.json | *.csv | *.summary)' \
    'tail -c "$take" -- "$file"' \
    '--budget-bytes' \
    '--per-file-bytes' \
    'budget_exhausted' \
    'EXCLUDED.txt'; do
    grep -Fq -- "$ci_artifacts_collect" scripts/hfoc-ci-artifacts.sh || {
        echo "ITEM-462: сборщик не содержит:" "$ci_artifacts_collect" >&2
        exit 1
    }
done

# The cleanup is bounded by age and names what it removed.
# shellcheck disable=SC2016  # the tool's literal text is what the search matches
for ci_artifacts_cleanup in \
    '-mtime +"$days"' \
    'removed_dirs=' \
    'removed_bytes='; do
    grep -Fq -- "$ci_artifacts_cleanup" scripts/hfoc-ci-artifacts.sh || {
        echo "ITEM-462: уборка не содержит:" "$ci_artifacts_cleanup" >&2
        exit 1
    }
done

# The guard refuses the shapes that caused the exhaustion.
for ci_artifacts_guard in \
    'TEST_ARTIFACTS_UNQUALIFIED' \
    'EXCLUSION_MISSING:' \
    'RETENTION_TOO_LONG:' \
    'COLLECT_STEP_MISSING' \
    'CLEANUP_STEP_MISSING'; do
    grep -Fq -- "$ci_artifacts_guard" scripts/hfoc-ci-artifacts.sh || {
        echo "ITEM-462: страж не содержит:" "$ci_artifacts_guard" >&2
        exit 1
    }
done

# The workflow takes the staged directory, not the raw tree, and keeps the
# collector, the cleanup and the exclusions.
for ci_artifacts_workflow in \
    'name: Clean old test artifacts' \
    'scripts/hfoc-ci-artifacts.sh cleanup' \
    'name: Collect CI diagnostics' \
    'scripts/hfoc-ci-artifacts.sh collect' \
    'HFOC_CI_DIAGNOSTICS_BUDGET_BYTES: "33554432"' \
    'HFOC_CI_ARTIFACT_RETENTION_DAYS: "7"'; do
    grep -Fq -- "$ci_artifacts_workflow" .github/workflows/ci.yml || {
        echo "ITEM-462: workflow не содержит:" "$ci_artifacts_workflow" >&2
        exit 1
    }
done

# And the static guard itself accepts the tree as it stands, so a future edit
# that ships the raw tree fails the suite here.
bash scripts/hfoc-ci-artifacts.sh guard --workflow .github/workflows/ci.yml || {
    echo "ITEM-462: страж отверг собственный workflow" >&2
    exit 1
}

# The upload step of the diagnostics is bounded: the staged tree plus the
# named exclusions, under a retention ceiling.
ci_artifacts_upload=$(awk '
    index($0, "      - name: ") == 1 {
        inside = (index($0, "      - name: Upload diagnostics") == 1)
        next
    }
    inside { print }
' .github/workflows/ci.yml)
[[ -n $ci_artifacts_upload ]] || {
    echo "ITEM-462: нет шага Upload diagnostics" >&2
    exit 1
}
for ci_artifacts_upload_rule in \
    'build/ci-diagnostics/**' \
    '!**/*.raw' \
    '!**/*.img' \
    '!build/hfoc-os-*.iso' \
    'retention-days: 7'; do
    grep -Fq -- "$ci_artifacts_upload_rule" <<<"$ci_artifacts_upload" || {
        echo "ITEM-462: загрузка диагностики не содержит:" \
            "$ci_artifacts_upload_rule" >&2
        exit 1
    }
done
if grep -Eq '^[[:space:]]+build/test-artifacts/' <<<"$ci_artifacts_upload"; then
    echo "ITEM-462: загрузка диагностики шлёт сырой каталог" >&2
    exit 1
fi

# The host proof drives each rule and each refusal.
for ci_artifacts_host in \
    'TEST_ARTIFACTS_UNQUALIFIED' \
    'RETENTION_TOO_LONG:90' \
    'COLLECT_STEP_MISSING' \
    'CLEANUP_STEP_MISSING' \
    'the raw image was collected' \
    'budget_exhausted=1'; do
    grep -Fq -- "$ci_artifacts_host" tests/test-ci-artifacts-host.sh || {
        echo "ITEM-462: host-проба не требует:" "$ci_artifacts_host" >&2
        exit 1
    }
done

# The proof is registered in the canonical host suite.
grep -Fq 'test-ci-artifacts-host' Makefile || {
    echo "ITEM-462: Makefile не содержит test-ci-artifacts-host" >&2
    exit 1
}
if ! awk '
    /^test-host-suite-serial:/ { in_recipe = 1; next }
    in_recipe && /^[^[:space:]#]/ { exit }
    in_recipe && index($0, "$(MAKE) test-ci-artifacts-host") { found = 1 }
    END { exit found ? 0 : 1 }
' Makefile; then
    echo "ITEM-462: host-проба не входит в канонический набор" >&2
    exit 1
fi

# The item is recorded where a maintainer looks.
for ci_artifacts_doc in \
    'ITEM-462' \
    'CI-ARTIFACTS' \
    'build/ci-diagnostics'; do
    grep -Fq -- "$ci_artifacts_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-462:" \
            "$ci_artifacts_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-462' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-462" >&2
    exit 1
}
grep -Fq 'ITEM-462' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-462" >&2
    exit 1
}
