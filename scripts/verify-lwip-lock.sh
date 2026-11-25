#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
THIRD_PARTY_DIR=${1:-$ROOT/third_party}
LOCK="$THIRD_PARTY_DIR/LOCKS.sha256"
LICENSES="$THIRD_PARTY_DIR/LICENSES.md"
LWIP_DIR="$THIRD_PARTY_DIR/lwip"

EXPECTED=$(mktemp)
FOUND=$(mktemp)
LWIP_SECTION=$(mktemp)
cleanup() {
    rm -f "$EXPECTED" "$FOUND" "$LWIP_SECTION"
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
[[ -f "$LWIP_DIR/COPYING" ]] || {
    echo "Missing lwIP COPYING: $LWIP_DIR/COPYING" >&2
    exit 1
}

awk '/^lwip_lock_version=1$/{p=1} p{print}' "$LOCK" >"$LWIP_SECTION"
[[ -s "$LWIP_SECTION" ]] || {
    echo "Source lock is missing lwIP section" >&2
    exit 1
}

required_value() {
    local key=$1
    local value
    if [[ $(grep -c "^${key}=" "$LWIP_SECTION") -ne 1 ]]; then
        echo "Source lock is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$LWIP_SECTION")
    if [[ -z "$value" ]]; then
        echo "Source lock has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

lock_version=$(required_value lwip_lock_version)
[[ "$lock_version" == "1" ]] || {
    echo "Unsupported lwIP lock version: $lock_version" >&2
    exit 1
}

upstream_name=$(required_value upstream_name)
[[ "$upstream_name" == "lwIP" ]] || {
    echo "Unexpected upstream_name: $upstream_name" >&2
    exit 1
}

upstream_version=$(required_value upstream_version)
[[ "$upstream_version" == "STABLE-2_2_1_RELEASE" ]] || {
    echo "Unexpected upstream_version: $upstream_version" >&2
    exit 1
}

upstream_commit=$(required_value upstream_commit)
[[ "$upstream_commit" =~ ^[0-9a-f]{40}$ ]] || {
    echo "Invalid upstream_commit: $upstream_commit" >&2
    exit 1
}
[[ "$upstream_commit" == "77dcd25a72509eb83f72b033d219b1d40cd8eb95" ]] || {
    echo "Unexpected upstream_commit: $upstream_commit" >&2
    exit 1
}

upstream_url=$(required_value upstream_url)
[[ "$upstream_url" == \
    "https://github.com/lwip-tcpip/lwip/archive/refs/tags/STABLE-2_2_1_RELEASE.tar.gz" ]] || {
    echo "Unexpected upstream_url: $upstream_url" >&2
    exit 1
}

upstream_sha256=$(required_value upstream_sha256)
[[ "$upstream_sha256" =~ ^[0-9a-f]{64}$ ]] || {
    echo "Invalid upstream_sha256: $upstream_sha256" >&2
    exit 1
}
[[ "$upstream_sha256" == \
    "ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539" ]] || {
    echo "Unexpected upstream_sha256: $upstream_sha256" >&2
    exit 1
}

upstream_license=$(required_value upstream_license)
[[ "$upstream_license" == "BSD-3-Clause" ]] || {
    echo "Unexpected upstream_license: $upstream_license" >&2
    exit 1
}

expected_count=$(required_value file_count)
[[ "$expected_count" =~ ^[1-9][0-9]*$ ]] || {
    echo "Invalid file_count: $expected_count" >&2
    exit 1
}

if [[ $(grep -c '^files_begin$' "$LWIP_SECTION") -ne 1 ]]; then
    echo "Source lock must contain exactly one files_begin marker" >&2
    exit 1
fi

actual_count=0
while IFS= read -r line; do
    actual_count=$((actual_count + 1))
    if [[ ! "$line" =~ ^[0-9a-f]{64}[[:space:]]{2}[^[:space:]]+[[:space:]]{2}BSD-3-Clause$ ]]; then
        echo "Invalid lock entry: $line" >&2
        exit 1
    fi
    IFS=' ' read -r digest path license <<<"$line"
    [[ "$license" == "BSD-3-Clause" ]] || {
        echo "Unlicensed lock entry: $path" >&2
        exit 1
    }
    [[ "$path" == lwip/* ]] || {
        echo "Lock path must start with lwip/: $path" >&2
        exit 1
    }
    [[ "$path" != *".."* ]] || {
        echo "Lock path escapes the lwIP tree: $path" >&2
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
        echo "Altered lwIP source: $path" >&2
        exit 1
    }
    printf '%s\n' "$path" >>"$EXPECTED"
done < <(sed -n '/^files_begin$/,$p' "$LWIP_SECTION" | sed '1d')

[[ "$actual_count" == "$expected_count" ]] || {
    echo "Source lock file_count mismatch: expected $expected_count, got $actual_count" >&2
    exit 1
}
if sort "$EXPECTED" | uniq -d | grep -q .; then
    echo "Source lock contains duplicate paths" >&2
    exit 1
fi
if find "$LWIP_DIR" -type l -print -quit | grep -q .; then
    echo "lwIP tree contains a symlink" >&2
    exit 1
fi
find "$LWIP_DIR" -type f -print | \
    sed "s#^$THIRD_PARTY_DIR/##" | sort >"$FOUND"
if ! cmp -s <(sort "$EXPECTED") "$FOUND"; then
    echo "lwIP tree contains missing or extra files relative to the source lock" >&2
    exit 1
fi

grep -Fq 'lwIP STABLE-2_2_1_RELEASE' "$LICENSES" || {
    echo "License inventory does not identify the lwIP release" >&2
    exit 1
}
grep -Fq 'BSD-3-Clause' "$LICENSES" || {
    echo "License inventory does not identify BSD-3-Clause" >&2
    exit 1
}
grep -Fq 'third_party/lwip/COPYING' "$LICENSES" || {
    echo "License inventory does not identify the lwIP license text" >&2
    exit 1
}
grep -Fq "file_count=$expected_count" "$LICENSES" || {
    echo "License inventory file_count does not match the source lock" >&2
    exit 1
}

echo "lwIP source and license lock verification: OK"
