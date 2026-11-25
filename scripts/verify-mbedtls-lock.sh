#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
THIRD_PARTY_DIR=${1:-$ROOT/third_party}
LOCK="$THIRD_PARTY_DIR/LOCKS.sha256"
LICENSES="$THIRD_PARTY_DIR/LICENSES.md"
MBEDTLS_DIR="$THIRD_PARTY_DIR/mbedtls"

SECTION=$(mktemp)
EXPECTED=$(mktemp)
FOUND=$(mktemp)
cleanup() {
    rm -f "$SECTION" "$EXPECTED" "$FOUND"
}
trap cleanup EXIT

[[ -f "$LOCK" ]] || {
    echo "Missing source lock: $LOCK" >&2
    exit 1
}
[[ -f "$LICENSES" ]] || {
    echo "Missing license inventory: $LICENSES" >&2
    exit 1
}
[[ -f "$MBEDTLS_DIR/LICENSE" ]] || {
    echo "Missing mbedTLS LICENSE: $MBEDTLS_DIR/LICENSE" >&2
    exit 1
}

awk '/^mbedtls_lock_version=1$/{p=1} /^lwip_lock_version=1$/{p=0} p{print}' \
    "$LOCK" >"$SECTION"
[[ -s "$SECTION" ]] || {
    echo "Source lock is missing mbedTLS section" >&2
    exit 1
}

required_value() {
    local key=$1
    local value
    if [[ $(grep -c "^${key}=" "$SECTION") -ne 1 ]]; then
        echo "mbedTLS source lock is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$SECTION")
    if [[ -z "$value" ]]; then
        echo "mbedTLS source lock has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

lock_version=$(required_value mbedtls_lock_version)
[[ "$lock_version" == "1" ]] || {
    echo "Unsupported mbedTLS lock version: $lock_version" >&2
    exit 1
}

upstream_name=$(required_value upstream_name)
[[ "$upstream_name" == "mbedTLS" ]] || {
    echo "Unexpected upstream_name: $upstream_name" >&2
    exit 1
}

upstream_version=$(required_value upstream_version)
[[ "$upstream_version" == "mbedtls-3.6.7" ]] || {
    echo "Unexpected upstream_version: $upstream_version" >&2
    exit 1
}

upstream_commit=$(required_value upstream_commit)
[[ "$upstream_commit" =~ ^[0-9a-f]{40}$ ]] || {
    echo "Invalid upstream_commit: $upstream_commit" >&2
    exit 1
}
[[ "$upstream_commit" == "068ff080b369adfac81509f9b57b2afabaf82dc5" ]] || {
    echo "Unexpected upstream_commit: $upstream_commit" >&2
    exit 1
}

upstream_url=$(required_value upstream_url)
[[ "$upstream_url" == \
    "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2" ]] || {
    echo "Unexpected upstream_url: $upstream_url" >&2
    exit 1
}

upstream_sha256=$(required_value upstream_sha256)
[[ "$upstream_sha256" =~ ^[0-9a-f]{64}$ ]] || {
    echo "Invalid upstream_sha256: $upstream_sha256" >&2
    exit 1
}
[[ "$upstream_sha256" == \
    "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6" ]] || {
    echo "Unexpected upstream_sha256: $upstream_sha256" >&2
    exit 1
}

upstream_license=$(required_value upstream_license)
[[ "$upstream_license" == "Apache-2.0 OR GPL-2.0-or-later" ]] || {
    echo "Unexpected upstream_license: $upstream_license" >&2
    exit 1
}

expected_count=$(required_value file_count)
[[ "$expected_count" =~ ^[1-9][0-9]*$ ]] || {
    echo "Invalid file_count: $expected_count" >&2
    exit 1
}

if [[ $(grep -c '^mbedtls_files_begin$' "$SECTION") -ne 1 ]]; then
    echo "mbedTLS source lock must contain exactly one mbedtls_files_begin marker" >&2
    exit 1
fi

actual_count=0
while IFS= read -r line; do
    actual_count=$((actual_count + 1))
    if [[ ! "$line" =~ ^([0-9a-f]{64})[[:space:]][[:space:]]([^[:space:]]+)[[:space:]][[:space:]](Apache-2\.0 OR GPL-2\.0-or-later)$ ]]; then
        echo "Invalid mbedTLS lock entry: $line" >&2
        exit 1
    fi
    digest=${BASH_REMATCH[1]}
    path=${BASH_REMATCH[2]}
    license=${BASH_REMATCH[3]}
    [[ "$license" == "Apache-2.0 OR GPL-2.0-or-later" ]] || {
        echo "Unlicensed mbedTLS lock entry: $path" >&2
        exit 1
    }
    [[ "$path" == mbedtls/* ]] || {
        echo "Lock path must start with mbedtls/: $path" >&2
        exit 1
    }
    [[ "$path" != *".."* ]] || {
        echo "Lock path escapes the mbedTLS tree: $path" >&2
        exit 1
    }
    [[ "$path" != *"//"* ]] || {
        echo "Lock path contains duplicate slash: $path" >&2
        exit 1
    }
    if [[ "$path" == *"\\"* ]]; then
        echo "Lock path contains backslash: $path" >&2
        exit 1
    fi
    file="$THIRD_PARTY_DIR/$path"
    [[ -f "$file" && ! -L "$file" ]] || {
        echo "Lock entry is not a regular file: $path" >&2
        exit 1
    }
    actual_digest=$(sha256sum "$file" | awk '{print $1}')
    [[ "$actual_digest" == "$digest" ]] || {
        echo "Altered mbedTLS source: $path" >&2
        exit 1
    }
    printf '%s\n' "$path" >>"$EXPECTED"
done < <(sed -n '/^mbedtls_files_begin$/,$p' "$SECTION" | sed '1d')

[[ "$actual_count" == "$expected_count" ]] || {
    echo "mbedTLS source lock file_count mismatch: expected $expected_count, got $actual_count" >&2
    exit 1
}
if sort "$EXPECTED" | uniq -d | grep -q .; then
    echo "mbedTLS source lock contains duplicate paths" >&2
    exit 1
fi
if find "$MBEDTLS_DIR" -type l -print -quit | grep -q .; then
    echo "mbedTLS tree contains a symlink" >&2
    exit 1
fi
find "$MBEDTLS_DIR" -type f -print | \
    sed "s#^$THIRD_PARTY_DIR/##" | sort >"$FOUND"
if ! cmp -s <(sort "$EXPECTED") "$FOUND"; then
    echo "mbedTLS tree contains missing or extra files relative to the source lock" >&2
    exit 1
fi

grep -Fq 'mbedTLS mbedtls-3.6.7' "$LICENSES" || {
    echo "License inventory does not identify the mbedTLS release" >&2
    exit 1
}
grep -Fq 'Apache-2.0 OR GPL-2.0-or-later' "$LICENSES" || {
    echo "License inventory does not identify Apache-2.0 OR GPL-2.0-or-later" >&2
    exit 1
}
grep -Fq 'third_party/mbedtls/LICENSE' "$LICENSES" || {
    echo "License inventory does not identify the mbedTLS license text" >&2
    exit 1
}
grep -Fq "file_count=$expected_count" "$LICENSES" || {
    echo "License inventory file_count does not match the mbedTLS source lock" >&2
    exit 1
}

echo "mbedTLS source and license lock verification: OK"
