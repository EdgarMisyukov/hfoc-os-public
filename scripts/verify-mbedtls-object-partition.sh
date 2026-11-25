#!/usr/bin/env bash
# ITEM-386 mbedTLS object-partition verification.
#
# The production image carries two complete mbedTLS profiles at once:
#
#   * the ITEM-372 signature-verification profile, whose objects are compiled
#     into build/verify-kernel with MBEDTLS_CONFIG_FILE=hfoc_verify_config.h
#     and route every allocation through the fixed arena in
#     image_signature_ecdsa.c; and
#   * the ITEM-386 production TLS-serving profile, whose objects are compiled
#     into build/tls-kernel with MBEDTLS_CONFIG_FILE=hfoc_mbedtls_tls_config.h
#     and route every allocation through the connection pools.
#
# Sharing a translation unit between those two profiles is what broke the
# TLS 1.3 handshake: the TLS path used to resolve mbedtls_mpi_*/mbedtls_ecp_*/
# mbedtls_md_* to the verification objects, so a connection was built from the
# verification arena and released through the TLS pools.  This verifier is the
# positive contract that keeps the two profiles apart.  It asserts, from the
# built objects themselves:
#
#   1. the TLS closure is the complete library minus the one state-encryption
#      allocation-free AES unit, and compiles no verification object;
#   2. no verification symbol leaks into the TLS closure, and no plain mbedTLS
#      symbol leaks out of it into the verification closure;
#   3. the two object sets are symbol-disjoint, so the link has no duplicate;
#   4. each profile's arithmetic reaches its own allocator hook and never the
#      other profile's hook;
#   5. the namespace header covers every symbol the verification closure
#      exports, so a new verification translation unit cannot silently
#      reintroduce a collision.
#
# Usage: verify-mbedtls-object-partition.sh [--production ELF]
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

VERIFY_OBJ_DIR=build/verify-kernel
TLS_OBJ_DIR=build/tls-kernel
NAMESPACE_HEADER=kernel/include/hfoc_verify_symbols.h
LIBRARY_DIR=third_party/mbedtls/library

fail() {
    echo "mbedTLS object partition verification failed: $*" >&2
    exit 1
}

# Every link-visible symbol an object file defines or references, one per line,
# sorted.  `nm --format=posix` keeps the output stable across locales and
# binutils versions: "name type value [size]".  Only external symbols take part
# in the link, so file-local (static) names are excluded: two profiles may
# legitimately carry identically named static curve tables.
object_symbols() {
    local mode=$1
    shift
    nm --format=posix --extern-only "$mode" "$@" 2>/dev/null |
        awk 'NF >= 2 && $2 ~ /^[A-Za-z]$/ { print $1 }' |
        LC_ALL=C sort -u
}

[[ -d "$VERIFY_OBJ_DIR" ]] || fail "missing $VERIFY_OBJ_DIR (build the kernel first)"
[[ -d "$TLS_OBJ_DIR" ]] || fail "missing $TLS_OBJ_DIR (build the kernel first)"
[[ -f "$NAMESPACE_HEADER" ]] || fail "missing $NAMESPACE_HEADER"

shopt -s nullglob
verify_objs=("$VERIFY_OBJ_DIR"/*.o)
tls_objs=("$TLS_OBJ_DIR"/*.o)
shopt -u nullglob
(( ${#verify_objs[@]} > 0 )) || fail "no verification objects in $VERIFY_OBJ_DIR"
(( ${#tls_objs[@]} > 0 )) || fail "no TLS objects in $TLS_OBJ_DIR"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# 1. The TLS closure is the complete library minus the state AES unit, and it
#    compiles no verification object.
# ---------------------------------------------------------------------------
state_sources=$(
    # `make -p` reports the database and exits non-zero; that is expected here.
    make -p -f Makefile 2>/dev/null |
        sed -n 's/^HFOC_STATE_CRYPTO_MBEDTLS_SOURCES :=[[:space:]]*//p' |
        head -1 || true
)
[[ -n "$state_sources" ]] || fail "could not read the state-encryption source list"
# The variable carries exactly one translation unit today; keep the contract
# explicit so a future second state-profile unit cannot slip through.
[[ "$state_sources" == "third_party/mbedtls/library/aes.c" ]] || {
    fail "unexpected state-encryption source list: $state_sources"
}
expected_tls=()
while IFS= read -r source; do
    base=${source#"$LIBRARY_DIR"/}
    case " $state_sources " in
        *" $source "*) continue ;;
    esac
    expected_tls+=("${base%.c}")
done < <(find "$LIBRARY_DIR" -maxdepth 1 -name '*.c' -print | LC_ALL=C sort)
(( ${#expected_tls[@]} > 0 )) || fail "could not expand the library source list"

actual_tls=()
for object in "${tls_objs[@]}"; do
    actual_tls+=("$(basename "${object%.o}")")
done
if (( ${#actual_tls[@]} != ${#expected_tls[@]} )); then
    fail "TLS closure has ${#actual_tls[@]} objects, expected ${#expected_tls[@]}"
fi
mapfile -t sorted_expected < <(printf '%s\n' "${expected_tls[@]}" | LC_ALL=C sort)
mapfile -t sorted_actual < <(printf '%s\n' "${actual_tls[@]}" | LC_ALL=C sort)
if ! diff -q <(printf '%s\n' "${sorted_expected[@]}") \
        <(printf '%s\n' "${sorted_actual[@]}") >/dev/null; then
    echo "--- expected TLS closure" >&2
    printf '%s\n' "${sorted_expected[@]}" >&2
    echo "--- actual TLS closure" >&2
    printf '%s\n' "${sorted_actual[@]}" >&2
    fail "the TLS object directory is not the complete TLS closure"
fi
for object in "${actual_tls[@]}"; do
    case " $state_sources " in
        *"$LIBRARY_DIR/$object.c"*)
            fail "$object is both a state-encryption and a TLS object"
            ;;
    esac
done
echo "mbedTLS object partition: TLS closure is the complete library minus" \
    "the state AES unit (${#actual_tls[@]} objects)"

# ---------------------------------------------------------------------------
# 2/3. Symbol separation between the two closures.
# ---------------------------------------------------------------------------
object_symbols --defined-only "${verify_objs[@]}" >"$WORK/verify_defined"
object_symbols --defined-only "${tls_objs[@]}" >"$WORK/tls_defined"
object_symbols --undefined-only "${verify_objs[@]}" >"$WORK/verify_undef"
object_symbols --undefined-only "${tls_objs[@]}" >"$WORK/tls_undef"

collisions=$(comm -12 "$WORK/verify_defined" "$WORK/tls_defined")
if [[ -n "$collisions" ]]; then
    echo "$collisions" >&2
    fail "the two profiles define the same symbols"
fi

leaked_verify_refs=$(comm -12 "$WORK/tls_undef" "$WORK/verify_defined")
if [[ -n "$leaked_verify_refs" ]]; then
    echo "$leaked_verify_refs" >&2
    fail "the TLS path references verification-profile symbols"
fi

# Every verification-defined symbol must move into the namespace.
unprefixed=$(grep -v '^hfoc_verify_' "$WORK/verify_defined" || true)
if [[ -n "$unprefixed" ]]; then
    echo "$unprefixed" >&2
    fail "verification objects export symbols outside the hfoc_verify_ namespace"
fi
echo "mbedTLS object partition: verification and TLS closures are" \
    "symbol-disjoint"

# ---------------------------------------------------------------------------
# 4. Each profile allocates through its own hook and never the other's.
# ---------------------------------------------------------------------------
verify_bad=$(grep -E '^hfoc_mbedtls_(calloc|free)$' "$WORK/verify_undef" || true)
if [[ -n "$verify_bad" ]]; then
    echo "$verify_bad" >&2
    fail "a verification object reaches the TLS allocator hook"
fi
tls_bad=$(grep -E '^hfoc_verify_(calloc|free)$' "$WORK/tls_undef" || true)
if [[ -n "$tls_bad" ]]; then
    echo "$tls_bad" >&2
    fail "a TLS object reaches the verification allocator hook"
fi
# The arithmetic both profiles compile must actually reach an allocator, or the
# hook assertion above would pass vacuously.
for profile in "$VERIFY_OBJ_DIR/bignum.o:$VERIFY_OBJ_DIR" \
        "$TLS_OBJ_DIR/bignum.o:$TLS_OBJ_DIR"; do
    object=${profile%%:*}
    [[ -f "$object" ]] || continue
    if [[ "$object" == "$VERIFY_OBJ_DIR"/* ]]; then
        want=hfoc_verify_calloc
    else
        want=hfoc_mbedtls_calloc
    fi
    nm --format=posix --undefined-only "$object" |
        awk '{ print $1 }' | grep -qx "$want" || {
        fail "$object does not reach $want"
    }
done
echo "mbedTLS object partition: each profile reaches only its own allocator hook"

# ---------------------------------------------------------------------------
# 5. The namespace header covers exactly the whole verification closure.
#    Both sides are compared in the namespaced form the header must define.
# ---------------------------------------------------------------------------
sed -n 's/^#define [A-Za-z_][A-Za-z0-9_]* \(hfoc_verify_[A-Za-z0-9_]*\)$/\1/p' \
    "$NAMESPACE_HEADER" | LC_ALL=C sort -u >"$WORK/namespaced"
missing=$(comm -23 "$WORK/verify_defined" "$WORK/namespaced")
if [[ -n "$missing" ]]; then
    echo "$missing" >&2
    fail "$NAMESPACE_HEADER is missing a namespace entry for these symbols"
fi
stale=$(comm -13 "$WORK/verify_defined" "$WORK/namespaced")
if [[ -n "$stale" ]]; then
    echo "$stale" >&2
    fail "$NAMESPACE_HEADER names symbols the verification closure no longer exports"
fi
echo "mbedTLS object partition: $NAMESPACE_HEADER covers all" \
    "$(wc -l <"$WORK/verify_defined" | tr -d ' ') verification symbols"

# ---------------------------------------------------------------------------
# Optional: the linked artifact carries exactly one definition per link-visible
# symbol, so the two profile closures really did stay apart in the link.
# ---------------------------------------------------------------------------
if (( $# > 0 )); then
    [[ "$1" == "--production" ]] || fail "usage: $0 [--production ELF]"
    artifact=${2:-}
    [[ -n "$artifact" && -f "$artifact" ]] || fail "missing artifact: $artifact"
    nm --format=posix --extern-only "$artifact" 2>/dev/null |
        awk 'NF >= 2 && $2 ~ /^[A-Za-z]$/ { print $1 }' |
        LC_ALL=C sort | uniq -d >"$WORK/duplicates"
    if [[ -s "$WORK/duplicates" ]]; then
        head -20 "$WORK/duplicates" >&2
        fail "$artifact has duplicate symbol definitions"
    fi
    echo "mbedTLS object partition: $artifact has no duplicate symbol definition"
fi

echo "mbedTLS object partition verification: OK"
