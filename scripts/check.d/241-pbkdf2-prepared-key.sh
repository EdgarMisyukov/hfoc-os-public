#!/usr/bin/env bash
# CI-SPEEDUP-4: PBKDF2-HMAC-SHA-256 prepares the key once per derivation.
#           Sourced in pinned order by scripts/check.sh; run ShellCheck on its
#           own.

# The measurement of the main HTTPS proof put 390 s of it into credential
# verifications: one PBKDF2 derivation at 200,000 iterations cost 15-19 s of
# guest time and every round re-hashed both HMAC pads.  The claims pinned here:
#
#   * the pads are hashed once per derivation - the prepared states are
#     absorbed with the new SHA-256 seam, and a round copies both states and
#     the byte counter instead of rebuilding the pads;
#   * a key longer than one block is normalised once, not once per round;
#   * the round still uses `u` as its own message and output;
#   * the prepared states are zeroed on every exit path of the derivation;
#   * every PBKDF2 parameter, the record format and the reference vectors are
#     unchanged, and the saved-record verification and the compression budget
#     are proven by the host test;
#   * the host test is built with the compression counter it asserts on.
proof_credential=kernel/src/appliance_admin_credential.c
# ITEM-619: the resumable credential job embeds the prepared key, so its named
# type lives in the module header while the prepare/hash/zero code stays here.
proof_credential_header=kernel/include/appliance_admin_credential.h
proof_sha=kernel/include/sha256.h
proof_host=tests/appliance-admin-credential-host.c

grep -Fq 'sha256_absorb_block' "$proof_sha" || {
    echo "CI-SPEEDUP-4: SHA-256 has no explicit block seam" >&2
    exit 1
}
grep -Fq 'enum sha256_status sha256_absorb_block(struct sha256 *context)' \
    kernel/src/sha256.c || {
    echo "CI-SPEEDUP-4: the block seam is not implemented" >&2
    exit 1
}
grep -Fq 'total_bytes += SHA256_BLOCK_SIZE;' kernel/src/sha256.c || {
    echo "CI-SPEEDUP-4: the absorbed block does not move the byte counter" >&2
    exit 1
}
grep -Fq 'struct appliance_admin_credential_hmac_key {' \
    "$proof_credential_header" || {
    echo "CI-SPEEDUP-4: the prepared HMAC key is not a named state" >&2
    exit 1
}
grep -Fq 'appliance_admin_credential_hmac_key_prepare(' "$proof_credential" || {
    echo "CI-SPEEDUP-4: the key is not prepared once per derivation" >&2
    exit 1
}
grep -Fq 'sha256_absorb_block(&prepared->inner)' "$proof_credential" || {
    echo "CI-SPEEDUP-4: the inner pad is not absorbed once" >&2
    exit 1
}
grep -Fq 'sha256_absorb_block(&prepared->outer)' "$proof_credential" || {
    echo "CI-SPEEDUP-4: the outer pad is not absorbed once" >&2
    exit 1
}
grep -Fq 'inner = prepared->inner;' "$proof_credential" || {
    echo "CI-SPEEDUP-4: a round does not copy the prepared inner state" >&2
    exit 1
}
grep -Fq 'outer = prepared->outer;' "$proof_credential" || {
    echo "CI-SPEEDUP-4: a round does not copy the prepared outer state" >&2
    exit 1
}
# ITEM-619: the round lives in the resumable job; the call still passes the
# prepared key and uses `u` as both the message and the output.
grep -Fq '&kdf->prepared, kdf->u, sizeof(kdf->u), kdf->u' \
    "$proof_credential" || {
    echo "CI-SPEEDUP-4: the round no longer uses u as its own output" >&2
    exit 1
}
grep -Fq 'round < iterations' "$proof_credential" || {
    echo "CI-SPEEDUP-4: the PBKDF2 iteration loop is gone" >&2
    exit 1
}
grep -Fq 'appliance_admin_credential_zeroize(&kdf->prepared, sizeof(kdf->prepared));' \
    "$proof_credential" || {
    echo "CI-SPEEDUP-4: the prepared states are not zeroed" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT 200000U' \
    kernel/include/appliance_admin_credential.h || {
    echo "CI-SPEEDUP-4: the PBKDF2 iteration count changed" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ADMIN_CREDENTIAL_RECORD_LABEL "pbkdf2-sha256"' \
    kernel/include/appliance_admin_credential.h || {
    echo "CI-SPEEDUP-4: the record label changed" >&2
    exit 1
}
grep -Fq 'hashlib.pbkdf2_hmac' "$proof_host" || {
    echo "CI-SPEEDUP-4: the vectors are not compared with an independent reference" >&2
    exit 1
}
grep -Fq 'the saved record still verifies its password' "$proof_host" || {
    echo "CI-SPEEDUP-4: the saved-record check is not proven" >&2
    exit 1
}
grep -Fq 'one derivation costs two compressions per round and two pads' \
    "$proof_host" || {
    echo "CI-SPEEDUP-4: the compression budget is not asserted" >&2
    exit 1
}
grep -Fq 'sha256_host_test_compressions' "$proof_host" || {
    echo "CI-SPEEDUP-4: the host proof does not read the compression counter" >&2
    exit 1
}
grep -Fq -- '-DHFOC_SHA256_HOST_TEST' Makefile || {
    echo "CI-SPEEDUP-4: the host KDF proof is not built with the counter" >&2
    exit 1
}
grep -Fq 'sha256_absorb_block' tests/sha256-host.c || {
    echo "CI-SPEEDUP-4: the block seam has no host proof" >&2
    exit 1
}
grep -Fq 'CI-SPEEDUP-4' docs/ROADMAP.md || {
    echo "CI-SPEEDUP-4: ROADMAP does not record the derivation change" >&2
    exit 1
}
