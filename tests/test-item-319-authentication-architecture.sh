#!/usr/bin/env bash
set -Eeuo pipefail

export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

DOC=docs/hfoc-appliance-management-authentication-architecture.md
DOC_AUTHZ=docs/hfoc-appliance-management-authorization.md
DOC_SECRET=docs/hfoc-appliance-secret-protection.md
DOC_STORAGE=docs/hfoc-appliance-persistent-storage-namespaces.md
DOC_CONFIG=docs/hfoc-appliance-configuration-integrity.md
DOC_SECRET_PERSISTENCE=docs/hfoc-appliance-certificate-secret-persistence.md
DOC_RUNTIME=docs/hfoc-appliance-runtime-diagnostics-logs.md
DOC_HEALTH_API=docs/hfoc-appliance-https-health-operational-status.md
DOC_UPDATE=docs/hfoc-appliance-update-package-architecture.md
DOC_UPDATE_MANIFEST=docs/hfoc-appliance-update-manifest-validation.md
DOC_UPDATE_INTEGRITY=docs/hfoc-appliance-update-integrity-verification.md
DOC_RECOVERY=docs/hfoc-appliance-recovery-mode.md
DOC_FACTORY=docs/hfoc-appliance-factory-reset.md
DOC_REBOOT=docs/hfoc-appliance-persistent-config-reboot-acceptance.md
ROADMAP_PLAN=docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md
ROADMAP_PUBLIC=docs/ROADMAP.md
README=README.md
ARCH=docs/ARCHITECTURE.md
AUDIT=docs/hfoc-appliance-platform-architecture-audit.md

fail() {
    echo "Item 319 authentication architecture test: $*" >&2
    exit 1
}

[[ -f "$DOC" ]] ||
    fail "missing $DOC"
[[ -f "$DOC_AUTHZ" ]] ||
    fail "missing $DOC_AUTHZ"
[[ -f "$DOC_SECRET" ]] ||
    fail "missing $DOC_SECRET"
[[ -f "$DOC_STORAGE" ]] ||
    fail "missing $DOC_STORAGE"
[[ -f "$DOC_CONFIG" ]] ||
    fail "missing $DOC_CONFIG"
[[ -f "$DOC_SECRET_PERSISTENCE" ]] ||
    fail "missing $DOC_SECRET_PERSISTENCE"
[[ -f "$DOC_RUNTIME" ]] ||
    fail "missing $DOC_RUNTIME"
[[ -f "$DOC_HEALTH_API" ]] ||
    fail "missing $DOC_HEALTH_API"
[[ -f "$DOC_UPDATE" ]] ||
    fail "missing $DOC_UPDATE"
[[ -f "$DOC_UPDATE_MANIFEST" ]] ||
    fail "missing $DOC_UPDATE_MANIFEST"
[[ -f "$DOC_UPDATE_INTEGRITY" ]] ||
    fail "missing $DOC_UPDATE_INTEGRITY"
[[ -f "$DOC_RECOVERY" ]] ||
    fail "missing $DOC_RECOVERY"
[[ -f "$DOC_FACTORY" ]] ||
    fail "missing $DOC_FACTORY"
[[ -f "$DOC_REBOOT" ]] ||
    fail "missing $DOC_REBOOT"
[[ -f "$ROADMAP_PLAN" ]] ||
    fail "missing $ROADMAP_PLAN"
[[ -f "$ROADMAP_PUBLIC" ]] ||
    fail "missing $ROADMAP_PUBLIC"
[[ -f "$README" ]] ||
    fail "missing $README"
[[ -f "$ARCH" ]] ||
    fail "missing $ARCH"
[[ -f "$AUDIT" ]] ||
    fail "missing $AUDIT"

for section in \
    '## Purpose' \
    '## Current State' \
    '## Security Principles' \
    '## Principal And Credential Model' \
    '## Session Semantics' \
    '## Request Authentication Semantics' \
    '## Failure Handling' \
    '## Secure Defaults' \
    '## Wave E Boundaries' \
    '## Non-Goals'; do
    grep -Fq -- "$section" "$DOC" ||
        fail "architecture document is missing section: $section"
done

for phrase in \
    'no production management authentication' \
    'auth=unavailable' \
    'TLS 1.3' \
    'deny-by-default' \
    'opaque bearer session token' \
    'Authorization: Bearer' \
    'no default administrative password' \
    'never write credentials or session secrets to logs' \
    'ITEM-320 owns administrative authentication implementation' \
    'ITEM-321 owns management authorization implementation' \
    'ITEM-322 owns secret and credential protection implementation'; do
    grep -Fq -- "$phrase" "$DOC" ||
        fail "architecture document is missing required phrase: $phrase"
done

grep -Fq -- '- [x] **ITEM-319**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-319 complete"
grep -Fq -- '- [x] **ITEM-320**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-320 complete"
grep -Fq -- '- [x] **ITEM-321**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-321 complete"
grep -Fq -- '- [x] **ITEM-322**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-322 complete"
grep -Fq -- '- [x] **ITEM-323**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-323 complete"
grep -Fq -- '- [x] **ITEM-324**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-324 complete"
grep -Fq -- '- [x] **ITEM-325**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-325 complete"
grep -Fq -- '- [x] **ITEM-326**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-326 complete"
grep -Fq -- '- [x] **ITEM-327**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-327 complete"
grep -Fq -- '- [x] **ITEM-328**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-328 complete"
grep -Fq -- '- [x] **ITEM-329**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-329 complete"
grep -Fq -- '- [x] **ITEM-330**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-330 complete"
grep -Fq -- '- [x] **ITEM-331**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-331 complete"
grep -Fq -- '- [x] **ITEM-332**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-332 complete"
grep -Fq -- '- [x] **ITEM-333**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-333 complete"
grep -Fq -- '- [x] **ITEM-334**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-334 complete"
grep -Fq -- '- [x] **ITEM-335**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-335 complete"
grep -Fq -- '- [x] **ITEM-336**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-336 complete"
grep -Fq -- '- [x] **ITEM-337**' "$ROADMAP_PLAN" ||
    fail "roadmap plan does not mark ITEM-337 complete"

grep -Fq -- 'ITEM-319 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-319 as implemented"
grep -Fq -- 'ITEM-320 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-320 as implemented"
grep -Fq -- 'ITEM-321 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-321 as implemented"
grep -Fq -- 'ITEM-322 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-322 as implemented"
grep -Fq -- 'ITEM-323 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-323 as implemented"
grep -Fq -- 'ITEM-324 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-324 as implemented"
grep -Fq -- 'ITEM-325 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-325 as implemented"
grep -Fq -- 'ITEM-326 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-326 as implemented"
grep -Fq -- 'ITEM-327 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-327 as implemented"
grep -Fq -- 'management authentication architecture' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe management authentication architecture"
grep -Fq -- 'administrative authentication' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe administrative authentication"
grep -Fq -- 'management authorization' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe management authorization"
grep -Fq -- 'secret and credential protection' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe secret and credential protection"
grep -Fq -- 'persistent storage namespaces' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe persistent storage namespaces"
grep -Fq -- 'configuration integrity' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe configuration integrity"
grep -Fq -- 'certificate and secret persistence' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe certificate and secret persistence"
grep -Fq -- 'appliance health model' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe appliance health model"
grep -Fq -- 'runtime diagnostics and logs' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe runtime diagnostics and logs"
grep -Fq -- 'HTTPS health and operational status' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe HTTPS health and operational status"
grep -Fq -- 'ITEM-329 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-329 as implemented"
grep -Fq -- 'appliance update package architecture' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe appliance update package architecture"
grep -Fq -- 'ITEM-330 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-330 as implemented"
grep -Fq -- 'update manifest and compatibility validation' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe update manifest and compatibility validation"
grep -Fq -- 'ITEM-331 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-331 as implemented"
grep -Fq -- 'update integrity and signature verification' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe update integrity and signature verification"
grep -Fq -- 'ITEM-332 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-332 as implemented"
grep -Fq -- 'appliance recovery mode' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe appliance recovery mode"
grep -Fq -- 'ITEM-333 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-333 as implemented"
grep -Fq -- 'appliance factory reset' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe appliance factory reset"
grep -Fq -- 'ITEM-334 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-334 as implemented"
grep -Fq -- 'integrated appliance boot acceptance' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe integrated appliance boot acceptance"
grep -Fq -- 'ITEM-335 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-335 as implemented"
grep -Fq -- 'persistent configuration reboot acceptance' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe persistent configuration reboot acceptance"
grep -Fq -- 'ITEM-336 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-336 as implemented"
grep -Fq -- 'management plane QEMU acceptance' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe management plane QEMU acceptance"
grep -Fq -- 'ITEM-337 (реализован)' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not record ITEM-337 as implemented"
grep -Fq -- 'platform release acceptance' "$ROADMAP_PUBLIC" ||
    fail "public roadmap does not describe platform release acceptance"

grep -Fq -- 'hfoc-appliance-management-authentication-architecture.md' "$README" ||
    fail "README does not link the management authentication architecture"
grep -Fq -- 'hfoc-appliance-administrative-authentication.md' "$README" ||
    fail "README does not link the administrative authentication architecture"
grep -Fq -- 'hfoc-appliance-management-authorization.md' "$README" ||
    fail "README does not link the management authorization architecture"
grep -Fq -- 'hfoc-appliance-secret-protection.md' "$README" ||
    fail "README does not link the secret and credential protection architecture"
grep -Fq -- 'hfoc-appliance-persistent-storage-namespaces.md' "$README" ||
    fail "README does not link the persistent storage namespaces architecture"
grep -Fq -- 'hfoc-appliance-configuration-integrity.md' "$README" ||
    fail "README does not link the configuration integrity architecture"
grep -Fq -- 'hfoc-appliance-certificate-secret-persistence.md' "$README" ||
    fail "README does not link the certificate and secret persistence architecture"
grep -Fq -- 'hfoc-appliance-health-model.md' "$README" ||
    fail "README does not link the appliance health model architecture"
grep -Fq -- 'hfoc-appliance-runtime-diagnostics-logs.md' "$README" ||
    fail "README does not link the runtime diagnostics/log architecture"
grep -Fq -- 'hfoc-appliance-https-health-operational-status.md' "$README" ||
    fail "README does not link the HTTPS health operational status architecture"
grep -Fq -- 'hfoc-appliance-update-package-architecture.md' "$README" ||
    fail "README does not link the update package architecture"
grep -Fq -- 'hfoc-appliance-update-manifest-validation.md' "$README" ||
    fail "README does not link the update manifest validation architecture"
grep -Fq -- 'hfoc-appliance-update-integrity-verification.md' "$README" ||
    fail "README does not link the update integrity verification architecture"
grep -Fq -- 'hfoc-appliance-recovery-mode.md' "$README" ||
    fail "README does not link the appliance recovery mode architecture"
grep -Fq -- 'hfoc-appliance-factory-reset.md' "$README" ||
    fail "README does not link the appliance factory reset architecture"
grep -Fq -- 'hfoc-appliance-integrated-boot-acceptance.md' "$README" ||
    fail "README does not link the integrated boot acceptance architecture"
grep -Fq -- 'hfoc-appliance-persistent-config-reboot-acceptance.md' "$README" ||
    fail "README does not link the persistent config reboot acceptance architecture"
grep -Fq -- 'hfoc-appliance-platform-release-acceptance.md' "$README" ||
    fail "README does not link the platform release acceptance architecture"
grep -Fq -- 'hfoc-appliance-management-authentication-architecture.md' "$ARCH" ||
    fail "architecture document does not link the management authentication architecture"
grep -Fq -- 'hfoc-appliance-administrative-authentication.md' "$ARCH" ||
    fail "architecture document does not link the administrative authentication architecture"
grep -Fq -- 'hfoc-appliance-management-authorization.md' "$ARCH" ||
    fail "architecture document does not link the management authorization architecture"
grep -Fq -- 'hfoc-appliance-secret-protection.md' "$ARCH" ||
    fail "architecture document does not link the secret and credential protection architecture"
grep -Fq -- 'hfoc-appliance-persistent-storage-namespaces.md' "$ARCH" ||
    fail "architecture document does not link the persistent storage namespaces architecture"
grep -Fq -- 'hfoc-appliance-configuration-integrity.md' "$ARCH" ||
    fail "architecture document does not link the configuration integrity architecture"
grep -Fq -- 'hfoc-appliance-certificate-secret-persistence.md' "$ARCH" ||
    fail "architecture document does not link the certificate and secret persistence architecture"
grep -Fq -- 'hfoc-appliance-health-model.md' "$ARCH" ||
    fail "architecture document does not link the appliance health model architecture"
grep -Fq -- 'hfoc-appliance-runtime-diagnostics-logs.md' "$ARCH" ||
    fail "architecture document does not link the runtime diagnostics/log architecture"
grep -Fq -- 'hfoc-appliance-https-health-operational-status.md' "$ARCH" ||
    fail "architecture document does not link the HTTPS health operational status architecture"
grep -Fq -- 'hfoc-appliance-update-package-architecture.md' "$ARCH" ||
    fail "architecture document does not link the update package architecture"
grep -Fq -- 'hfoc-appliance-update-manifest-validation.md' "$ARCH" ||
    fail "architecture document does not link the update manifest validation architecture"
grep -Fq -- 'hfoc-appliance-update-integrity-verification.md' "$ARCH" ||
    fail "architecture document does not link the update integrity verification architecture"
grep -Fq -- 'hfoc-appliance-recovery-mode.md' "$ARCH" ||
    fail "architecture document does not link the appliance recovery mode architecture"
grep -Fq -- 'hfoc-appliance-factory-reset.md' "$ARCH" ||
    fail "architecture document does not link the appliance factory reset architecture"
grep -Fq -- 'hfoc-appliance-integrated-boot-acceptance.md' "$ARCH" ||
    fail "architecture document does not link the integrated boot acceptance architecture"
grep -Fq -- 'hfoc-appliance-persistent-config-reboot-acceptance.md' "$ARCH" ||
    fail "architecture document does not link the persistent config reboot acceptance architecture"
grep -Fq -- 'hfoc-appliance-management-plane-qemu-acceptance.md' "$ARCH" ||
    fail "architecture document does not link the management plane QEMU acceptance architecture"
grep -Fq -- 'hfoc-appliance-platform-release-acceptance.md' "$ARCH" ||
    fail "architecture document does not link the platform release acceptance architecture"
grep -Fq -- 'ITEM-337' "$AUDIT" ||
    fail "appliance audit does not record ITEM-337 complete"

for section in \
    '## Purpose' \
    '## Architecture Decisions' \
    '## Artifact Contract' \
    '## Metadata Contract' \
    '## Installation Stages' \
    '## Compatibility Rules' \
    '## Rollback Expectations' \
    '## Failure Boundaries' \
    '## Recovery Metadata Ownership' \
    '## Wave H Boundaries' \
    '## Non-Goals'; do
    grep -Fq -- "$section" "$DOC_UPDATE" ||
        fail "update package architecture document is missing section: $section"
done

grep -Fq -- 'recovery.metadata' "$DOC_UPDATE" ||
    fail "update package architecture document does not describe recovery metadata"
grep -Fq -- 'ITEM-330' "$DOC_UPDATE" ||
    fail "update package architecture document does not assign manifest validation to ITEM-330"
grep -Fq -- 'no production update transport' "$DOC_UPDATE" ||
    fail "update package architecture document does not reject production transport"

for section in \
    '## Purpose' \
    '## Current State' \
    '## Manifest Format' \
    '## Validation' \
    '## Compatibility Gate' \
    '## Ownership And Failure Handling' \
    '## Wave H Ownership'; do
    grep -Fq -- "$section" "$DOC_UPDATE_MANIFEST" ||
        fail "update manifest validation document is missing section: $section"
done

grep -Fq -- 'appliance_update_manifest' "$DOC_UPDATE_MANIFEST" ||
    fail "update manifest validation document does not describe the module"
grep -Fq -- 'update_compatibility' "$DOC_UPDATE_MANIFEST" ||
    fail "update manifest validation document does not describe compatibility delegation"
grep -Fq -- 'ITEM-331' "$DOC_UPDATE_MANIFEST" ||
    fail "update manifest validation document does not leave integrity verification to ITEM-331"

for section in \
    '## Summary' \
    '## Record Model' \
    '## Bounded Log' \
    '## Usage Boundary'; do
    grep -Fq -- "$section" "$DOC_RUNTIME" ||
        fail "runtime diagnostics document is missing section: $section"
done

grep -Fq -- '## Purpose' "$DOC_SECRET" ||
    fail "secret protection document is missing section: Purpose"
grep -Fq -- '## Secret Kinds' "$DOC_SECRET" ||
    fail "secret protection document is missing section: Secret Kinds"
grep -Fq -- '## Lifecycle And Rotation' "$DOC_SECRET" ||
    fail "secret protection document is missing section: Lifecycle And Rotation"
grep -Fq -- '## At-Rest Record' "$DOC_SECRET" ||
    fail "secret protection document is missing section: At-Rest Record"
grep -Fq -- 'canonical one-way SHA-256' "$DOC_SECRET" ||
    fail "secret protection document does not describe canonical SHA-256 fingerprints"
grep -Fq -- 'zeroized' "$DOC_SECRET" ||
    fail "secret protection document does not describe zeroization"

grep -Fq -- '## Purpose' "$DOC_STORAGE" ||
    fail "persistent storage namespaces document is missing section: Purpose"
grep -Fq -- '## Namespace Table' "$DOC_STORAGE" ||
    fail "persistent storage namespaces document is missing section: Namespace Table"
grep -Fq -- '## Sensitivity' "$DOC_STORAGE" ||
    fail "persistent storage namespaces document is missing section: Sensitivity"
grep -Fq -- '## Registry Contract' "$DOC_STORAGE" ||
    fail "persistent storage namespaces document is missing section: Registry Contract"
grep -Fq -- 'system.config' "$DOC_STORAGE" ||
    fail "persistent storage namespaces document does not describe system config namespace"
grep -Fq -- 'recovery.metadata' "$DOC_STORAGE" ||
    fail "persistent storage namespaces document does not describe recovery metadata namespace"
grep -Fq -- 'classify_record_key' "$DOC_STORAGE" ||
    fail "persistent storage namespaces document does not describe record-key classification"

grep -Fq -- '## Purpose' "$DOC_CONFIG" ||
    fail "configuration integrity document is missing section: Purpose"
grep -Fq -- '## Record Catalogue' "$DOC_CONFIG" ||
    fail "configuration integrity document is missing section: Record Catalogue"
grep -Fq -- '## Classification' "$DOC_CONFIG" ||
    fail "configuration integrity document is missing section: Classification"
grep -Fq -- '## Recovery Policy' "$DOC_CONFIG" ||
    fail "configuration integrity document is missing section: Recovery Policy"
grep -Fq -- 'system.hostname' "$DOC_CONFIG" ||
    fail "configuration integrity document does not describe system hostname"
grep -Fq -- 'KEEP_LAST_KNOWN_GOOD' "$DOC_CONFIG" ||
    fail "configuration integrity document does not describe last-known-good recovery"
grep -Fq -- 'EXPLICIT_ERROR' "$DOC_CONFIG" ||
    fail "configuration integrity document does not describe explicit error recovery"

grep -Fq -- '## Purpose' "$DOC_SECRET_PERSISTENCE" ||
    fail "certificate and secret persistence document is missing section: Purpose"
grep -Fq -- '## Record Catalogue' "$DOC_SECRET_PERSISTENCE" ||
    fail "certificate and secret persistence document is missing section: Record Catalogue"
grep -Fq -- '## At-Rest Record Policy' "$DOC_SECRET_PERSISTENCE" ||
    fail "certificate and secret persistence document is missing section: At-Rest Record Policy"
grep -Fq -- '## HTTPS Readiness' "$DOC_SECRET_PERSISTENCE" ||
    fail "certificate and secret persistence document is missing section: HTTPS Readiness"
grep -Fq -- 'v1:certificate.https=' "$DOC_SECRET_PERSISTENCE" ||
    fail "certificate and secret persistence document does not describe HTTPS certificate record"
grep -Fq -- 'v1:secret.admin.credential=' "$DOC_SECRET_PERSISTENCE" ||
    fail "certificate and secret persistence document does not describe admin credential record"
grep -Fq -- 'v1:secret.https.private_key.wrapped=' "$DOC_SECRET_PERSISTENCE" ||
    fail "certificate and secret persistence document does not describe wrapped key record"
grep -Fq -- 'raw private-key' "$DOC_SECRET_PERSISTENCE" ||
    fail "certificate and secret persistence document does not reject raw private-key persistence"
grep -Fq -- 'HTTPS_SERVICE_DEPENDENCY_CERTIFICATE_READY' "$DOC_SECRET_PERSISTENCE" ||
    fail "certificate and secret persistence document does not describe HTTPS dependency"

echo "Item 319 management authentication architecture test: OK"
