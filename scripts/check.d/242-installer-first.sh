#!/usr/bin/env bash
# CI-SPEEDUP-5: the installer proof starts first because it is the tail.
#           Sourced in pinned order by scripts/check.sh; run ShellCheck on its
#           own.

# The acceptance journal named `test-production-installer-qemu` as the last
# target to finish: it started at +667 s of the QEMU suite's window and ran
# 595 s, while everything else had finished by +1044 s.  Two changes remove
# that tail.  The claims pinned here:
#
#   * the installer media are file targets with prerequisites, not phony
#     recipes, so a run that prepared them reuses them; `deps` is an
#     order-only prerequisite so the fetch script cannot make them permanently
#     out of date;
#   * the phase's image step (`qemu-test-images`) prepares the installer media
#     before the queue starts;
#   * the standalone run still builds them: `test-production-installer-qemu`
#     keeps the media aliases as prerequisites and keeps its own marker and
#     medium checks;
#   * the installer is the first entry of `QEMU_PARALLEL_PHASE_1`, and nothing
#     else in that order changed;
#   * the documents record the measurement.
# shellcheck disable=SC2016  # Makefile spellings, not this script's values
makefile=Makefile

grep -Fq 'PRODUCTION_INSTALLER_MEDIA_PREREQUISITES :=' "$makefile" || {
    echo "CI-SPEEDUP-5: the installer media have no shared prerequisite list" >&2
    exit 1
}
grep -Fq '$(wildcard $(LIMINE)/limine)' "$makefile" || {
    echo "CI-SPEEDUP-5: the fetched limine binary is not a normal input" >&2
    exit 1
}
grep -Fq '$(PRODUCTION_INSTALLER_AUTHORISED_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps' \
    "$makefile" || {
    echo "CI-SPEEDUP-5: the authorised medium is not a cached file target" >&2
    exit 1
}
grep -Fq '$(PRODUCTION_INSTALLER_CONSOLE_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps' \
    "$makefile" || {
    echo "CI-SPEEDUP-5: the console medium is not a cached file target" >&2
    exit 1
}
grep -Fq 'production-installer-authorised-media: $(PRODUCTION_INSTALLER_AUTHORISED_ISO)' \
    "$makefile" || {
    echo "CI-SPEEDUP-5: the standalone media alias is gone" >&2
    exit 1
}
grep -Fq "test-production-installer-qemu: \$(PRODUCTION_INSTALLER_ISO) iso" \
    "$makefile" || {
    echo "CI-SPEEDUP-5: the installer target no longer asks for the base medium" >&2
    exit 1
}
grep -Fq 'production-installer-console-media' "$makefile" || {
    echo "CI-SPEEDUP-5: the installer target lost a medium prerequisite" >&2
    exit 1
}
grep -Fq 'Production-installer marker checks: before QEMU proof' "$makefile" || {
    echo "CI-SPEEDUP-5: the medium marker checks are gone" >&2
    exit 1
}
grep -Fq '$(PRODUCTION_INSTALLER_QEMU_IMAGES)' "$makefile" || {
    echo "CI-SPEEDUP-5: the image step does not prepare the installer media" >&2
    exit 1
}
installer_line=$(grep -n '^QEMU_PARALLEL_PHASE_1 :=' "$makefile" | cut -d: -f1)
first_target=$(awk -v start="$installer_line" '
    NR > start && /^[[:space:]]*test-/ {
        gsub(/^[[:space:]]+/, "")
        gsub(/[[:space:]\\]+$/, "")
        print
        exit
    }' \
    "$makefile")
if [[ $first_target != 'test-production-installer-qemu' ]]; then
    echo "CI-SPEEDUP-5: the installer is not first in QEMU_PARALLEL_PHASE_1:" \
        "$first_target" >&2
    exit 1
fi
grep -Fq 'CI-SPEEDUP-5' docs/ROADMAP.md || {
    echo "CI-SPEEDUP-5: ROADMAP does not record the installer order" >&2
    exit 1
}
