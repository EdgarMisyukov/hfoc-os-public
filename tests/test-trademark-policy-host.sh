#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

POLICY=release/trademark-and-certified-build-policy.txt
DOC=docs/hfoc-trademark-and-certified-build-policy.md

for path in "$POLICY" "$DOC" scripts/verify-trademark-policy.sh; do
    [[ -f "$path" ]] || {
        echo "Missing trademark policy file: $path" >&2
        exit 1
    }
done

# The document and its machine-readable form must point at each other.
grep -Fq "$POLICY" "$DOC" || {
    echo "policy document does not name the machine-readable artefact" >&2
    exit 1
}
grep -Fq 'scripts/verify-trademark-policy.sh' "$DOC" || {
    echo "policy document does not name its verifier" >&2
    exit 1
}

# Positive control: the verifier accepts the real artefact, reading its
# default path so the wiring of that path is proven too.
bash scripts/verify-trademark-policy.sh
bash scripts/verify-trademark-policy.sh "$POLICY"

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

fixture_count=0

# make_fixture NAME copies the real artefact and prints the copy path. The
# caller takes the copy through a command substitution, so the counter is
# advanced in expect_reject, which runs in this shell.
make_fixture() {
    local name=$1
    local dst="$TEST_DIR/$name.txt"
    cp "$POLICY" "$dst"
    printf '%s' "$dst"
}

expect_reject() {
    local fixture=$1
    fixture_count=$((fixture_count + 1))
    if bash scripts/verify-trademark-policy.sh "$fixture" >/dev/null 2>&1; then
        echo "Trademark fixture unexpectedly accepted: $fixture" >&2
        exit 1
    fi
}

# A missing section: the community build rules disappear from the artefact.
fixture=$(make_fixture missing-community-build-section)
sed -i '/^community_build_/d' "$fixture"
expect_reject "$fixture"

# A missing section: the marks disappear from the artefact.
fixture=$(make_fixture missing-marks-section)
sed -i '/^marks_/d' "$fixture"
expect_reject "$fixture"

# A missing section: the legal statements disappear from the artefact.
fixture=$(make_fixture missing-legal-section)
sed -i '/^legal_/d' "$fixture"
expect_reject "$fixture"

# An unknown section: a claim that belongs to no declared section.
fixture=$(make_fixture unknown-section)
printf 'trademark_extra_permission=none\n' >>"$fixture"
expect_reject "$fixture"

# An unknown key inside a declared section.
fixture=$(make_fixture unknown-key)
printf 'marks_extra_name=HFOC Plus\n' >>"$fixture"
expect_reject "$fixture"

# Weakened claim: a community build may claim to be certified.
fixture=$(make_fixture community-certified-claim)
sed -i 's/^community_build_may_claim_certified_build=false$/community_build_may_claim_certified_build=true/' \
    "$fixture"
expect_reject "$fixture"

# Weakened claim: a community build may claim vendor support.
fixture=$(make_fixture community-support-claim)
sed -i 's/^community_build_may_claim_vendor_support=false$/community_build_may_claim_vendor_support=true/' \
    "$fixture"
expect_reject "$fixture"

# Weakened claim: a community build may use the marks freely.
fixture=$(make_fixture community-mark-use)
sed -i 's/^community_build_mark_use=nominative_fair_use_only$/community_build_mark_use=unrestricted/' \
    "$fixture"
expect_reject "$fixture"

# Weakened claim: redistribution no longer needs a written agreement.
fixture=$(make_fixture redistribution-without-agreement)
sed -i 's/^redistribution_requires_written_agreement=true$/redistribution_requires_written_agreement=false/' \
    "$fixture"
expect_reject "$fixture"

# Fabricated claim: the repository publishes a certified build.
fixture=$(make_fixture certified-build-published)
sed -i 's/^policy_certified_build_published=false$/policy_certified_build_published=true/' \
    "$fixture"
expect_reject "$fixture"

# Fabricated claim: the policy is legal advice.
fixture=$(make_fixture legal-advice-claimed)
sed -i 's/^legal_advice=false$/legal_advice=true/' "$fixture"
expect_reject "$fixture"

# A certified-build label loses the SBOM field.
fixture=$(make_fixture label-without-sbom)
sed -i '/^certified_label_sbom_digest=/d' "$fixture"
expect_reject "$fixture"

# A certified-build label loses the source revision it was built from.
fixture=$(make_fixture label-without-revision)
sed -i '/^certified_label_source_revision=/d' "$fixture"
expect_reject "$fixture"

# The binding instrument of an OEM disappears.
fixture=$(make_fixture missing-binding-instrument)
sed -i '/^legal_binding_instrument=/d' "$fixture"
expect_reject "$fixture"

# The policy document the artefact cites does not exist.
fixture=$(make_fixture dangling-policy-document)
sed -i 's|^policy_document=.*$|policy_document=docs/hfoc-missing-policy.md|' \
    "$fixture"
expect_reject "$fixture"

# Structural refusals: duplicate key, empty value and a malformed line.
fixture=$(make_fixture duplicate-key)
printf 'legal_advice=false\n' >>"$fixture"
expect_reject "$fixture"

fixture=$(make_fixture empty-value)
sed -i 's/^policy_target_release_id=.*$/policy_target_release_id=/' "$fixture"
expect_reject "$fixture"

fixture=$(make_fixture malformed-line)
printf 'community_build_may_claim_certified_build\n' >>"$fixture"
expect_reject "$fixture"

(( fixture_count == 18 )) || {
    echo "expected 18 weakened fixtures, built $fixture_count" >&2
    exit 1
}

# The real artefact still passes after every weakened fixture was refused.
bash scripts/verify-trademark-policy.sh "$POLICY" >/dev/null

echo 'Trademark policy host test: OK'
