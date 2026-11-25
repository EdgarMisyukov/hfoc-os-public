# HFOC Product Release Gate: SBOM, Reproducible ISO and CVE Process

**Item:** ITEM-380 of the HFOC Appliance Platform productization wave
**Depends on:** ITEM-347 (release artifact manifest), ITEM-379 (end-to-end
acceptance report), ITEM-381 (hardware acceptance harness and report)
**Release contract:** HFOC-APPLIANCE-V1
**Status:** SBOM, reproducible ISO digest, security response runbook and the
fail-closed release gate implemented and proven by a focused host fixture.

## Purpose

A customer who is asked to trust an appliance asks three questions before the
signature matters: what is inside the artifact, can the artifact be rebuilt,
and what happens when a component turns out to be vulnerable. ITEM-380 answers
all three with artifacts instead of assurances:

| Question | Artifact | Contract |
| --- | --- | --- |
| What is inside? | SPDX 2.3 SBOM | `scripts/verify-release-sbom.sh` |
| Can it be rebuilt? | two-build reproducibility report with the ISO digest | `scripts/compare-reproducible-image-digests.sh` |
| What happens on a vulnerability? | `docs/hfoc-security-response-runbook.md` | `scripts/verify-cve-runbook.sh` |
| May this revision ship? | the gate verdict | `scripts/verify-product-release-gate.sh` |

The gate fails closed: a missing SBOM, missing or stale reproducibility
evidence, or a missing runbook is a `REJECT`, never a warning.

## SBOM

`scripts/generate-release-sbom.sh` writes an SPDX 2.3 tag-value document
(`build/release/sbom/HFOC-APPLIANCE-V1.spdx`) generated offline from what the
repository already pins:

- `third_party/LICENSES.md` supplies each dependency's name, release, commit,
  archive SHA-256, license family and license-text path;
- `third_party/LOCKS.sha256` supplies every vendored file path and its SHA-256;
- `VERSION` and the source revision supply the product identity.

The document contains:

- the product package `HFOC OS` under
  `AGPL-3.0-only OR LicenseRef-HFOC-Commercial`;
- one package per vendored dependency (mbedTLS, lwIP, Limine) with its pinned
  version, download location, archive checksum and declared license;
- the **mbedTLS Apache-2.0 election**: `PackageLicenseConcluded: Apache-2.0`
  plus an `election=Apache-2.0` package comment, so ITEM-382 can consume a
  stable, greppable line;
- one SPDX file entry per locked vendored file with the exact locked SHA-256,
  related to its package by `CONTAINS`;
- `DESCRIBES` and `DEPENDS_ON` relationships that connect the document to the
  product and the product to every dependency.

The document is deterministic: its `Created` timestamp comes from
`SOURCE_DATE_EPOCH` or from the source commit time, never from the wall clock.

`scripts/verify-release-sbom.sh` re-derives every expectation from the
inventory and the lock file and refuses a missing package, a missing or wrong
election, a foreign source revision, a tampered file digest and a file count
that does not match the lock.

## Reproducible ISO digest

`scripts/capture-reproducible-image-digests.sh` records the SHA-256 of the
kernel ELF, the build-input manifest and the ISO; the ISO part is the release
artifact digest that was previously missing (only a sidecar `.sha256` existed).
`scripts/compare-reproducible-image-digests.sh` compares two captures and, when
given a report path, writes the durable evidence
`build/release/reproducibility/comparison-report.txt`:

```
reproducibility_report_version=1
source_revision=<commit>
artifact_hfoc_kernel_elf_first/second=<sha256>
artifact_hfoc_build_input_first/second=<sha256>
artifact_iso_first/second=<sha256>
verdict=REPRODUCIBLE
```

The ISO build is made deterministic in two steps. First, every file date and
the volume creation/modification date are pinned to the source commit time:
`SOURCE_DATE_EPOCH` defaults to the commit timestamp and
`ISO_REPRODUCIBLE_FLAGS` (`--set_all_file_dates`, `--modification-date`) is
applied to every ISO recipe, so filesystem timestamps no longer reach the
image. Second, `scripts/normalize-iso-gpt.py` rewrites the one remaining
volatile part: xorriso generates a random GPT disk GUID and random per-partition
unique GUIDs for the EFI boot partition it embeds. The normalizer derives those
GUIDs from the source revision, recomputes both GPT CRC32 checksums and is
idempotent; it runs on the image before `limine bios-install`, because the
Limine BIOS stage 2 overwrites the primary GPT header at LBA 1. Boot code,
partition layout and file contents are untouched. A byte comparison of two
builds identified exactly these fields and CRC32s as the only differences, and
after normalization two builds are byte-identical.

`make test-reproducible-image-host` builds the tree twice with `make clean` in
between and compares the two captures, including the ISO digest; the release
matrix records it as the `REPRODUCIBILITY` check with the marker
`Reproducible image comparison: OK`.

## Security response runbook

`docs/hfoc-security-response-runbook.md` defines the process: intake,
triage and severity (`CRITICAL`/`HIGH`/`MEDIUM`/`LOW`), affected-version
mapping from the SBOM and `third_party/LOCKS.sha256`, fix and backport through
the canonical ITEM pipeline, advisory and embargo rules, SBOM and artifact
refresh, the locked-vendored-dependency policy, and the offline gate.
`scripts/verify-cve-runbook.sh` enforces the sections and the declared phrases,
so the process cannot silently disappear from the repository.

## Gate

`scripts/verify-product-release-gate.sh` is the decision:

1. the SBOM exists, names the current source revision and passes
   `verify-release-sbom.sh`;
2. the reproducibility report exists, names the current source revision, shows
   every first/second digest pair equal and carries `verdict=REPRODUCIBLE`;
3. the security response runbook exists and passes `verify-cve-runbook.sh`.

Only then does it print `Product release gate: ACCEPT` together with the
release identity, the source revision, the SBOM path and the reproducible ISO
digest. Otherwise it prints `Product release gate: REJECT: <reason>` and exits
non-zero.

`make test-product-release-gate` is the release-time composition: it builds the
ISO twice from clean trees, records the comparison report, generates the SBOM
and applies the gate. The target has no `iso` prerequisite: the recipe runs
`make clean` itself before each build, so an incremental build state can never
become the first captured artifact. It is deliberately not part of
`make test`, because it is the release step rather than a regression test; the
gate's contract is proven by `make test-product-release-gate-host`, and the
reproducibility machinery is proven by `make test-reproducible-image-host`.

## Proof

`make test-product-release-gate-host` proves, offline and without building an
ISO:

- the SBOM generator and verifier agree on the real repository, and the
  verifier rejects a missing package, a missing mbedTLS election, a tampered
  file digest and a foreign source revision;
- the runbook verifier accepts the runbook and rejects one missing a required
  section;
- the gate accepts complete evidence and rejects a missing SBOM, missing
  reproducibility evidence, an unreproducible ISO digest, a non-reproducible
  verdict, evidence from another source revision and a missing runbook.

## Honest limits

- The release-time gate itself is not run in CI: it builds the ISO twice. Its
  contract is proven by the host fixture, the reproducibility machinery by
  `test-reproducible-image-host`, and the release step is documented here.
- The SBOM is generated from the repository's own inventory and locks; it is
  not a scanner and does not query external vulnerability databases. Mapping a
  CVE to affected releases is the runbook's affected-version procedure.
- Physical media, physical boot chain and physical acceptance remain
  `NOT_TESTED` (ITEM-372/ITEM-381); this gate does not turn them into a pass.
- The release signing ceremony (release anchor, offline key) stays with the
  release process described by ITEM-372 and is not part of this item.
- `VERSION` is not changed and no `v1.0.0` is created.
