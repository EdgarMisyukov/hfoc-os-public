# HFOC OS Security Response Runbook

**Item:** ITEM-380 of the HFOC Appliance Platform productization wave
**Scope:** every HFOC OS release built from this repository and every vendored
dependency listed in `third_party/LICENSES.md`.
**Status:** process defined; the offline gate `scripts/verify-cve-runbook.sh`
enforces this document, and `scripts/verify-product-release-gate.sh` refuses a
release when the runbook or its verifier is missing.

## Purpose

A product that ships a verifiable chain (boot, keys, configuration, update,
audit) also needs a defined answer to "a dependency has a vulnerability". This
runbook is that answer. It is deliberately executable in an offline
environment: nothing here requires a live advisory feed, a scanner subscription
or network access to the build host.

## Intake

Any of the following is an intake source:

- a CVE identifier that names mbedTLS, lwIP, Limine or another vendored
  dependency, reported by a customer, a distributor, an integrator or a
  researcher;
- a vendor advisory from the upstream project;
- an internal finding from the security baseline
  (`release/release-security-baseline.txt`) or from an acceptance run.

Intake is recorded as an issue with the reporter, the source, the date and the
affected dependency. No intake is rejected for arriving without a proof of
concept.

## Triage and severity

Triage answers four questions and records the answers before any fix is
written:

1. Which dependency and which vendored version is affected? The identity comes
   from `third_party/LICENSES.md` (release, commit, archive digest) and
   `third_party/LOCKS.sha256` (the exact vendored file list).
2. Is the vulnerable code on the appliance's reachable path? The appliance runs
   a fixed profile (no shell, no package manager, no interpreter); a
   vulnerability in unused code is recorded as such rather than ignored.
3. What is the severity for this product? The declared scale is
   `CRITICAL` / `HIGH` / `MEDIUM` / `LOW`, derived from the upstream score and
   the reachability answer.
4. Is a workaround available (configuration, policy, feature disable) before a
   fixed build?

## Affected versions

Every release records its `source_revision` in the release artifact manifest and
its dependency identity in the SBOM
(`build/release/sbom/HFOC-APPLIANCE-V1.spdx`). A release is affected when its
`source_revision` is at or before the fixing revision and its vendored lock
(`third_party/LOCKS.sha256`) still contains the vulnerable file digest. The
answer is therefore reproducible offline from the SBOM and the lock file, not
from memory.

## Fix and backport

- Update the vendored dependency or patch the affected file, then regenerate
  the lock (`scripts/generate-mbedtls-lock.sh`,
  `scripts/generate-lwip-lock.sh`) so `third_party/LOCKS.sha256` records the
  new digests.
- The fix lands through the canonical ITEM pipeline: focused host proof, QEMU
  proof where the vulnerable path is reachable, branch CI, self-review, merge
  and green post-merge CI.
- A backport to an older release branch is a new, separately reviewed change;
  it is never a cherry-pick without a proof.

## Advisory and embargo

- A `CRITICAL` or `HIGH` issue is handled under embargo until a fixed release
  exists: no public issue text, no public commit message that names the
  vulnerability, no advisory before the fixed artifact is available.
- The advisory names the CVE (when assigned), the affected releases, the fixed
  release, the severity, the reachability verdict and the workaround. It never
  includes exploit detail beyond what the fix already discloses.
- The advisory is published with the fixed release and the refreshed SBOM.

## SBOM and artifact refresh

A security release regenerates and re-verifies:

1. `scripts/generate-release-sbom.sh` → the SPDX SBOM,
2. `scripts/verify-release-sbom.sh` → the SBOM contract,
3. the release artifact manifest and its verifier,
4. the reproducible ISO digest evidence
   (`scripts/capture-reproducible-image-digests.sh` /
   `scripts/compare-reproducible-image-digests.sh`),
5. `scripts/verify-product-release-gate.sh`, which fails closed when the SBOM or
   the reproducibility evidence is missing or does not describe the current
   source revision.

## Dependency update policy

- Dependencies are vendored and locked: no build step fetches code from the
  network, and the only permitted files under `third_party/` are the ones in
  `third_party/LOCKS.sha256`.
- A dependency update changes the vendored files, the lock, the license
  inventory and the SBOM in the same change, so a release can never describe a
  dependency it does not ship.
- New dependencies require a license review, an entry in
  `third_party/LICENSES.md` and a lock, before they are linked into the
  product.

## Offline gate

`scripts/verify-cve-runbook.sh` enforces the headings and the declared phrases
of this document, and `scripts/verify-product-release-gate.sh` refuses a release
when the runbook or its verifier is missing. Both run offline and both are part
of `make check`, so the process cannot silently disappear from the repository.

## Recording

Every security-relevant action is recorded the same way as the rest of the
product: the intake, the triage verdict, the fix, the advisory and the refresh
are issues and commits that a reader can follow, and the resulting artifact
carries the SBOM and the reproducible digest that prove what was shipped.
