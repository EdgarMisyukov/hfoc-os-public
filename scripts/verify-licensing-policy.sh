#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

REPO_ROOT=$(git rev-parse --show-toplevel)
POLICY_ROOT=${HFOC_LICENSE_ROOT:-$REPO_ROOT}
AGPL_SHA256=d8a6cc31abc16b6748c7a21f21611f5a1ec33f67d22ca23d7da1c19b95496bee
MIT_BLOB=38d1129c780e84fe67063208ae26d22cde522838

fail() { echo "Licensing policy: $*" >&2; exit 1; }
need_file() { [[ -f "$POLICY_ROOT/$1" ]] || fail "missing $1"; }
need_text() { grep -Fq -- "$2" "$POLICY_ROOT/$1" || fail "$1 lacks: $2"; }

for path in LICENSE LICENSES/MIT-legacy.txt LICENSING.md \
    COMMERCIAL-LICENSING.md CONTRIBUTOR_LICENSE_AGREEMENT.md \
    CONTRIBUTING.md docs/legal/HFOC-OWNERSHIP-AUDIT.md; do
    need_file "$path"
done

actual_agpl=$(sha256sum "$POLICY_ROOT/LICENSE" | awk '{print $1}')
[[ "$actual_agpl" == "$AGPL_SHA256" ]] || fail 'root LICENSE is not canonical AGPL-3.0-only'
actual_mit=$(git hash-object "$POLICY_ROOT/LICENSES/MIT-legacy.txt")
[[ "$actual_mit" == "$MIT_BLOB" ]] || fail 'legacy MIT text changed'

# The sed expression must match literal Markdown backticks.
# shellcheck disable=SC2016
legacy=$(sed -n 's/^Legacy MIT boundary: `\([0-9a-f]\{40\}\)`$/\1/p' \
    "$POLICY_ROOT/LICENSING.md")
[[ "$legacy" =~ ^[0-9a-f]{40}$ ]] || fail 'invalid or missing legacy boundary'
git -C "$REPO_ROOT" cat-file -e "${legacy}^{commit}" 2>/dev/null || fail 'legacy boundary is not a commit'
[[ $(git -C "$REPO_ROOT" rev-parse "${legacy}:LICENSE") == "$MIT_BLOB" ]] || fail 'legacy boundary does not contain the former MIT LICENSE'

head=$(git -C "$REPO_ROOT" rev-parse HEAD)
if [[ "$head" == "$legacy" ]]; then
    git -C "$REPO_ROOT" diff --quiet -- LICENSE && fail 'no staged atomic transition after legacy boundary'
else
    transition=$(git -C "$REPO_ROOT" rev-list --ancestry-path --reverse "${legacy}..${head}" | {
        IFS= read -r first || true
        cat >/dev/null
        printf '%s' "$first"
    })
    [[ -n "$transition" ]] || fail 'HEAD is not descended from legacy boundary'
    [[ $(git -C "$REPO_ROOT" rev-parse "${transition}^1") == "$legacy" ]] || fail 'transition first parent mismatch'
    transition_license=$(git -C "$REPO_ROOT" show "${transition}:LICENSE" | sha256sum | awk '{print $1}')
    [[ "$transition_license" == "$AGPL_SHA256" ]] || fail 'atomic transition did not install canonical AGPL'
fi

need_text LICENSING.md 'AGPL-3.0-only'
need_text LICENSING.md 'separate written commercial license'
need_text LICENSING.md 'are not revoked'
need_text LICENSING.md 'They are not licensed under the HFOC dual offer.'
need_text COMMERCIAL-LICENSING.md 'grants no commercial-license rights'
need_text CONTRIBUTOR_LICENSE_AGREEMENT.md 'Contributors retain ownership'
need_text CONTRIBUTING.md 'must not be merged'

echo 'Licensing policy verification: OK'
