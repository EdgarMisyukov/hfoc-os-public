#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
POLICY_FILE=${1:-$ROOT/release/trademark-and-certified-build-policy.txt}

fail() { echo "Trademark policy: $*" >&2; exit 1; }

[[ -f "$POLICY_FILE" && ! -L "$POLICY_FILE" ]] || \
    fail "missing or linked policy file: $POLICY_FILE"

# Sections of the machine-readable policy. Every key of the artefact belongs to
# exactly one section, and the section is the longest declared prefix of the
# key: a key that matches no section is an unknown section and a key that
# matches two sections is ambiguous, so neither can be smuggled in.
sections=(
    policy
    marks
    permitted_use
    prohibited_use
    certified_label
    redistribution
    community_build
    legal
)

# The complete required key set. The artefact may not carry a key outside it
# and may not omit one: deleting a load-bearing key is a weakened policy, and
# the verifier refuses it instead of treating the policy as merely shorter.
required_keys=(
    policy_version
    policy_target_release_id
    policy_document
    policy_verifier
    policy_release_gate_item
    policy_release_gate_artifacts
    policy_certified_build_published
    marks_project_name
    marks_project_form
    marks_appliance_name
    marks_appliance_form
    marks_certification_name
    marks_certification_form
    marks_owner
    marks_registration_claimed
    permitted_use_project_reference
    permitted_use_community_redistribution
    permitted_use_agreement_scope
    permitted_use_boundary
    prohibited_use_mark_in_name
    prohibited_use_certification_mark_without_label
    prohibited_use_community_certification_claim
    prohibited_use_implied_endorsement
    prohibited_use_attribution_removal
    certified_label_release_id
    certified_label_source_revision
    certified_label_image_sha256
    certified_label_release_gate_report
    certified_label_sbom_digest
    certified_label_acceptance_report
    certified_label_signer_anchor
    certified_label_issued_by
    certified_label_registry
    certified_label_producer_revision
    certified_label_producer_reproduce
    certified_label_producer_publish
    certified_label_producer_sign
    certified_label_producer_record
    certified_label_verify_digest
    certified_label_verify_revision
    certified_label_verify_artifacts
    certified_label_verify_signature
    certified_label_invalid_digest
    certified_label_invalid_gate
    certified_label_invalid_modification
    certified_label_invalid_artifacts
    certified_label_invalid_no_record
    redistribution_requires_written_agreement
    redistribution_community_basis
    redistribution_commercial_basis
    redistribution_may_redistribute
    redistribution_mark_use
    redistribution_obligation_licence_payload
    redistribution_obligation_notices
    redistribution_obligation_identity
    redistribution_obligation_sbom
    redistribution_obligation_source
    redistribution_obligation_pass_through
    redistribution_obligation_support
    community_build_licence
    community_build_may_claim_certified_build
    community_build_may_claim_vendor_support
    community_build_may_claim_official_update_channel
    community_build_mark_use
    community_build_obligation_source
    community_build_obligation_notices
    community_build_prohibition_certified_claim
    community_build_prohibition_support_claim
    community_build_prohibition_mark_use
    community_build_prohibition_identity
    legal_advice
    legal_binding_instrument
    legal_agreement_precedence
    legal_review_required
)

declare -A is_required=()
for key in "${required_keys[@]}"; do
    is_required[$key]=1
done

declare -A value_of=()
declare -A seen_section=()
key_count=0

while IFS= read -r line || [[ -n "$line" ]]; do
    [[ "$line" != *$'\r'* ]] || fail 'policy file contains a CR character'
    [[ -n "$line" ]] || continue
    [[ "$line" != '#'* ]] || continue
    [[ "$line" =~ ^([a-z][a-z0-9_]*)=(.*)$ ]] || fail "malformed line: $line"
    key=${BASH_REMATCH[1]}
    value=${BASH_REMATCH[2]}
    [[ -n "$value" ]] || fail "empty value for key: $key"
    [[ "$value" != ' '* && "$value" != $'\t'* ]] || \
        fail "value of key starts with whitespace: $key"
    [[ "$value" != *[[:space:]] ]] || \
        fail "value of key ends with whitespace: $key"
    [[ -z ${value_of[$key]:-} ]] || fail "duplicate key: $key"
    value_of[$key]=$value
    key_count=$((key_count + 1))

    section=
    section_matches=0
    for candidate in "${sections[@]}"; do
        if [[ "$key" == "$candidate" || "$key" == "$candidate"_* ]]; then
            section=$candidate
            section_matches=$((section_matches + 1))
        fi
    done
    if (( section_matches == 0 )); then
        fail "unknown section for key: $key"
    fi
    if (( section_matches > 1 )); then
        fail "ambiguous section for key: $key"
    fi
    seen_section[$section]=1
done <"$POLICY_FILE"

for section in "${sections[@]}"; do
    [[ -n ${seen_section[$section]:-} ]] || fail "missing section: $section"
done

for key in "${required_keys[@]}"; do
    [[ -n ${value_of[$key]:-} ]] || fail "missing key: $key"
done

for key in "${!value_of[@]}"; do
    [[ -n ${is_required[$key]:-} ]] || fail "unknown key: $key"
done

[[ "$key_count" == "${#required_keys[@]}" ]] || \
    fail "policy file carries $key_count keys, expected ${#required_keys[@]}"

require_value() {
    local key=$1
    local expected=$2
    [[ "${value_of[$key]}" == "$expected" ]] || \
        fail "$key must be '$expected', found '${value_of[$key]}'"
}

require_existing_path() {
    local key=$1
    local path=${value_of[$key]}
    [[ "$path" != /* ]] || fail "$key must be a repository-relative path"
    [[ -f "$ROOT/$path" && ! -L "$ROOT/$path" ]] || \
        fail "$key points at a missing file: $path"
}

# Load-bearing claims, each pinned to its required value: a weakened policy
# changes a value and is refused here instead of being accepted as a variant.
require_value policy_version 1
require_value policy_target_release_id HFOC-APPLIANCE-V1
require_value policy_verifier scripts/verify-trademark-policy.sh
require_value policy_release_gate_item ITEM-380
require_value policy_certified_build_published false
require_value marks_registration_claimed false
require_value redistribution_requires_written_agreement true
require_value redistribution_mark_use agreement_only
require_value community_build_licence AGPL-3.0-only
require_value community_build_may_claim_certified_build false
require_value community_build_may_claim_vendor_support false
require_value community_build_may_claim_official_update_channel false
require_value community_build_mark_use nominative_fair_use_only
require_value legal_advice false
require_value legal_review_required true

require_existing_path policy_document
require_existing_path legal_binding_instrument

[[ "${value_of[legal_agreement_precedence]}" == *binding* ]] || \
    fail 'legal_agreement_precedence must name the binding instrument'

[[ "${value_of[policy_release_gate_artifacts]}" == *SBOM* ]] || \
    fail 'policy_release_gate_artifacts must name the SBOM of the release gate'
[[ "${value_of[policy_release_gate_artifacts]}" == *digest* ]] || \
    fail 'policy_release_gate_artifacts must name the released image digest'

echo 'Trademark policy verification: OK'
