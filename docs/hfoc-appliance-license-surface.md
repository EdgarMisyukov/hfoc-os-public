# HFOC Appliance Licensing Surface and Notices Closure

**Date:** 2026-09-12
**Target:** ITEM-382 of the HFOC Appliance Platform productization wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation and focused proof complete on the feature branch;
branch CI, Ready, merge, and post-merge gates are pending.

## Purpose

The code licensing was already settled and machine-checked before this item:
HFOC-owned files are `AGPL-3.0-only OR LicenseRef-HFOC-Commercial`,
`scripts/verify-licensing-policy.sh` runs from `scripts/check.sh`, and every ISO
already carried a verified license payload. What was missing was the product
surface around it: the management panel exposed no license page, the mbedTLS
`Apache-2.0 OR GPL-2.0-or-later` election was nowhere recorded as a decision,
the payload was verified only as part of other artifact checks rather than as
its own acceptance stage, and nothing forced a newly added dependency to reach
the ISO payload at all.

ITEM-382 closes that surface with one declared source of truth and three
consumers of it:

- `release/hfoc-license-inventory.txt` declares the appliance license and every
  bundled dependency with its version, license identifier, elected license and
  payload-relative notice paths;
- the panel renders it on `/panel/about/licenses` from an allocation-free
  in-kernel mirror (`appliance_license_inventory`);
- `scripts/verify-license-notices.sh` forces every declared notice into the
  license payload and into `LICENSES.sha256`, and `scripts/check.sh` runs that
  contract with negative fixtures, so a dependency that is declared without
  being shipped fails the build;
- `scripts/emit-license-sbom-rows.sh` publishes the SBOM dependency rows the
  ITEM-380 release gate consumes, so the SBOM cannot keep a second list.

## Declared dependency inventory

`release/hfoc-license-inventory.txt` (schema version 1) is one flat key=value
file, so every consumer parses exactly the same bytes:

| dependency | version | license the appliance ships | upstream expression | notice in the payload |
| --- | --- | --- | --- | --- |
| HFOC Appliance OS | `VERSION` | `AGPL-3.0-only OR LicenseRef-HFOC-Commercial` | — | `licenses/AGPL-3.0-only.txt`, `licenses/LICENSING.md`, `licenses/COMMERCIAL-LICENSING.md` |
| lwIP | `STABLE-2_2_1_RELEASE` | `BSD-3-Clause` | — | `licenses/third_party/lwip/COPYING` |
| mbedTLS | `mbedtls-3.6.7` | `Apache-2.0` | `Apache-2.0 OR GPL-2.0-or-later` | `licenses/third_party/mbedtls/LICENSE`, `licenses/notices/mbedtls-Apache-2.0-election.txt` |
| Limine | `12.3.2` | `BSD-2-Clause` | — | `licenses/third_party/limine/COPYING` |

`scripts/verify-license-inventory.sh` rejects an unknown key, a duplicate
dependency id, a notice the repository does not carry, an election that the
offered disjunction does not contain, an election notice without an election,
and the mbedTLS dependency stated as anything other than the elected
`Apache-2.0`. The inventory is the only place a dependency is declared: the
kernel mirror, the payload contract and the SBOM rows are all checked against
it, and `tests/appliance-license-inventory-host.c` compares the compiled-in
mirror field by field with the declared file, so a drift fails the host proof
instead of reaching an image.

## mbedTLS election

mbedTLS is offered upstream under `Apache-2.0 OR GPL-2.0-or-later`. Shipping
it under `GPL-2.0-or-later` inside a proprietary commercial build would be the
anti-pattern the decision memo
(`docs/hfoc-appliance-licensing-and-certification.md`, section "Anti-patterns")
names, so HFOC elects Apache-2.0 and records the election in four places:

- the `mbedtls` dependency entry in the declared inventory, whose `license` and
  `election` keys both read `Apache-2.0` while `license_offered` keeps the
  upstream disjunction;
- `notices/mbedtls-Apache-2.0-election.txt`, which ships in every ISO license
  payload as `licenses/notices/mbedtls-Apache-2.0-election.txt` and states the
  election, the offered expression and where the full Apache-2.0 text ships;
- the panel page, which lists the component under `Apache-2.0` and publishes
  `mbedTLS election Apache-2.0 from Apache-2.0 OR GPL-2.0-or-later`;
- the SBOM rows, which publish `PackageLicenseConcluded: Apache-2.0` and keep
  the disjunction only as `PackageLicenseDeclared`.

The upstream expression stays visible in `third_party/LICENSES.md` and in the
source lock, because those describe upstream and must not be rewritten; the
election is a distribution decision and is recorded where the distribution is
described (`third_party/LICENSES.md` additionally names the elected branch
beside the upstream expression).

## Panel about/licenses page

`/panel/about/licenses` is a panel page and not a tenth management section: the
section model (`APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT`, nine at that item
and eleven after ITEM-506 increment 6) and the
section index stay exact, and the page is published as its own target from an
exact entry in the authorization matrix, administrator-only like every other
panel page.

The page is reachable from the `Licenses` link in the section navigation and
publishes the state its owner reported:

- `UNAVAILABLE` with a reason when no inventory is published — the page then
  publishes no list at all, because it never invents one;
- `DEGRADED` with a reason and the shipped/declared notice counts when the
  declared inventory is published but the payload is incomplete;
- `AVAILABLE` with the declared inventory, the payload root, the checksum
  manifest and the mbedTLS election.

Every dynamic value (owner reason, payload root, manifest, and every component
name, version, license identifier and notice path written from the declared
inventory) passes through the ITEM-384 escaper before it is appended. The page
is allocation-free, owns no I/O, and is measured against the recorded per-page
asset budget: `APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LICENSES` (1993 bytes) in
`kernel/include/appliance_management_web_ui.h` and the same row in
`docs/hfoc-management-web-ui.md`, both enforced by
`tests/appliance-management-web-ui-host.c`.

The page does not read the ISO and does not verify a checksum at runtime: the
frozen baseline has no ISO9660 reader, so a runtime claim about the payload
would be fabricated. What it publishes is the declared inventory, and what makes
that inventory true is the payload contract below — enforced by CI on every
build, and published as the `LICENSE_PAYLOAD` acceptance stage.

## Payload contract and acceptance stage

`scripts/install-license-payload.sh` installs the notices under `notices/`
alongside the license texts, and `scripts/verify-license-payload.sh` now
requires `notices/mbedtls-Apache-2.0-election.txt` and calls
`scripts/verify-license-notices.sh`, so the contract runs on every ISO path in
the `Makefile` (production, fallback, release, and the QEMU image roots).

For every declared dependency the contract requires:

- the notice is a regular file inside the payload and not a symlink;
- `LICENSES.sha256` carries a line for it and the digest matches;
- an election notice states the election, the offered expression and the
  component it elects for;
- no file under `notices/` ships without a declaration.

A failure names the dependency, for example
`License notices: dependency mbedtls election notice missing from the payload:
notices/mbedtls-Apache-2.0-election.txt`.

The `LICENSE_PAYLOAD` acceptance stage is appended to
`release/hfoc-production-end-to-end-acceptance.txt` as stage 25 with
`state=PROVEN`, `kind=gate`, target `test-license-payload-host` and marker
`License payload host test: OK`, and it is listed in
`required_executable_stages`, so the verifier refuses a matrix that silently
drops it. The stage is appended last on purpose: the stage numbers a release
decision already reads for `SBOM_RELEASE` (ITEM-380) and `HARDWARE_ACCEPTANCE`
(ITEM-381) do not move, and the stage count is 25.

## Proofs

- `make test-appliance-license-inventory-host` — the declared inventory against
  the kernel mirror, the mbedTLS election, and the panel page states, escaping
  and budget;
- `make test-license-payload-host` — the installed payload, the manifest
  digest, the renamed/missing/uncovered/undeclared notice fixtures and a
  dependency declared without a shipped notice, every failure naming the
  dependency;
- `make test-appliance-management-web-ui-host` — the measured per-page asset
  budget including the `licenses` row and the page states;
- `make test-appliance-management-panel-host` — target classification,
  session requirement, escaping and the declared inventory through the panel
  dispatch;
- `make test-production-end-to-end-acceptance-host` — the appended
  `LICENSE_PAYLOAD` stage, the re-pinned stage count and the negative matrix
  fixtures;
- `bash scripts/verify-license-inventory.sh`,
  `bash scripts/verify-license-notices.sh` and
  `bash scripts/emit-license-sbom-rows.sh` — the machine contracts themselves,
  which `scripts/check.sh` runs together with their negative fixtures.
- The real TLS 1.3 HTTPS QEMU fixture serves `/panel/about/licenses` with a
  bearer session and asserts the page, `AVAILABLE`, the appliance license, the
  mbedTLS election and the notice paths over the wire
  (`scripts/test-qemu-https-response-matrix.sh`).

## Boundaries

- ITEM-380 owns the release gate, the SBOM document and the signing ceremony:
  this item only publishes the dependency rows that gate consumes, and it does
  not require any ITEM-380 file to exist.
- ITEM-383 owns the trademark and certified-build policy.
- ITEM-384 owns the panel renderer, escaping, the hardening headers and the
  budget mechanism; ITEM-382 adds one page and one data source to it.
- ITEM-385 owns management reachability. The page stays administrator-only.
- The commercial agreement (EULA, SLA, personal-data terms, liability) and the
  CLA decision stay on the commercial track: no repository change can settle
  them.

## Non-Goals

- No change to `VERSION` and no release-number change.
- No new runtime dependency, no external asset, no network access, no
  allocation in any kernel path.
- No rewrite of the upstream license texts or of the source locks, which
  describe upstream as vendored.
- No legal advice: the fixture does not decide contract terms.
