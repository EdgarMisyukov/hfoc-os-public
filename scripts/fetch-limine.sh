#!/usr/bin/env bash
set -Eeuo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
VERSION=${HFOC_LIMINE_VERSION:-12.3.2}
ARCHIVE="limine-binary-${VERSION}.tar.gz"
EXPECTED_SHA256=${HFOC_LIMINE_EXPECTED_SHA256:-433ac5933ef7918b6a6751db0a81e47fa122581dfb32430bc257be686f3906e5}
URL=${HFOC_LIMINE_URL:-https://github.com/Limine-Bootloader/Limine/releases/download/v${VERSION}/limine-binary.tar.gz}
DEPS=${HFOC_LIMINE_DEPS_DIR:-$ROOT/deps}
TARGET="$DEPS/limine"
DEFAULT_CACHE_HOME=${RUNNER_TOOL_CACHE:-${XDG_CACHE_HOME:-${HOME:-$ROOT/.cache}}}
CACHE_DIR=${HFOC_LIMINE_CACHE_DIR:-$DEFAULT_CACHE_HOME/hfoc-os/limine}
CACHE="$CACHE_DIR/$ARCHIVE"
STAMP_VALUE="version=${VERSION} sha256=${EXPECTED_SHA256}"
DOWNLOAD_TMP=
PREPARE_TMP=

cleanup() {
    rm -f "${DOWNLOAD_TMP:-}"
    rm -rf "${PREPARE_TMP:-}"
}
trap cleanup EXIT

[[ "$VERSION" =~ ^[A-Za-z0-9._-]+$ ]] || {
    echo "Недопустимая версия Limine: $VERSION" >&2
    exit 2
}
[[ "$EXPECTED_SHA256" =~ ^[0-9a-f]{64}$ ]] || {
    echo 'Недопустимая контрольная сумма Limine' >&2
    exit 2
}

archive_is_valid() {
    local archive=$1
    printf '%s  %s\n' "$EXPECTED_SHA256" "$archive" |
        sha256sum -c - >/dev/null 2>&1
}

prepared_tree_is_valid() {
    local tree=$1
    local required

    for required in limine limine-bios.sys limine-bios-cd.bin \
                    limine-bios-hdd.h limine-uefi-cd.bin BOOTX64.EFI; do
        [[ -f "$tree/$required" ]] || return 1
    done
}

mkdir -p "$DEPS" "$CACHE_DIR"

if [[ -f "$CACHE" ]] && archive_is_valid "$CACHE"; then
    echo "Limine cache: hit ($CACHE)"
else
    if [[ -f "$CACHE" ]]; then
        echo "Limine cache: повреждён, выполняется повторная загрузка" >&2
        rm -f "$CACHE"
    else
        echo "Limine cache: miss ($CACHE)"
    fi

    DOWNLOAD_TMP=$(mktemp "$CACHE_DIR/.${ARCHIVE}.tmp.XXXXXX")
    curl --fail --location --proto '=https' --tlsv1.2 \
        --retry 4 --retry-all-errors --retry-delay 2 \
        --connect-timeout 20 --max-time 300 \
        --output "$DOWNLOAD_TMP" "$URL"
    archive_is_valid "$DOWNLOAD_TMP" || {
        echo 'Загруженный архив Limine не прошёл проверку SHA-256' >&2
        exit 1
    }
    mv "$DOWNLOAD_TMP" "$CACHE"
    DOWNLOAD_TMP=
fi

if [[ -f "$TARGET/.hfoc-prepared" ]] &&
        [[ $(<"$TARGET/.hfoc-prepared") == "$STAMP_VALUE" ]] &&
        prepared_tree_is_valid "$TARGET"; then
    echo "Limine preparation: hit ($TARGET)"
    exit 0
fi

echo "Limine preparation: miss ($TARGET)"
PREPARE_TMP=$(mktemp -d "$DEPS/.limine.prepare.XXXXXX")
tar -xzf "$CACHE" -C "$PREPARE_TMP" --strip-components=1
make -C "$PREPARE_TMP"
prepared_tree_is_valid "$PREPARE_TMP" || {
    echo 'Подготовленное дерево Limine неполное' >&2
    exit 1
}
printf '%s\n' "$STAMP_VALUE" >"$PREPARE_TMP/.hfoc-prepared"
rm -rf "$TARGET"
mv "$PREPARE_TMP" "$TARGET"
PREPARE_TMP=

echo "Limine ${VERSION} подготовлен."
