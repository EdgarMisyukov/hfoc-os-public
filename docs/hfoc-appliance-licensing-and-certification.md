<!-- SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial -->
# HFOC Appliance Platform — licensing and certification decision memo

Status: decision memo for the owner. It is not an implementation ITEM, and it is
not legal advice: the agreement texts, the CLA and any certification filing need
a lawyer, and the crypto and certification parts need a vendor conversation.

## What is already settled

- HFOC-owned material is dual-licensed: `AGPL-3.0-only OR
  LicenseRef-HFOC-Commercial` (`LICENSING.md`, `COMMERCIAL-LICENSING.md`, per
  file SPDX headers).
- The policy is machine-checked: `scripts/verify-licensing-policy.sh` runs from
  `scripts/check.sh` and pins the canonical AGPL text by digest, the legacy MIT
  boundary by commit and blob, and the presence of every licensing document.
- Ownership is audited: `docs/legal/HFOC-OWNERSHIP-AUDIT.md` found no external
  copyright holder in HFOC-owned paths, so the dual licence is the owner's to
  grant.
- Third parties are compatible and pinned: lwIP BSD-3-Clause, Limine BSD-2,
  mbedTLS dual `Apache-2.0 OR GPL-2.0-or-later`
  (`third_party/LICENSES.md`, per-file digests in `third_party/LOCKS.sha256`).
- Binary distribution already carries notices: every ISO installs a verified
  license payload (`scripts/install-license-payload.sh`,
  `scripts/verify-license-payload.sh`, `licenses/LICENSES.sha256`) containing the
  AGPL text, the licensing documents, and the lwIP, mbedTLS and Limine notices.

## Open decisions

### D1 — Commercial agreement package

There is no executable commercial licence yet: `COMMERCIAL-LICENSING.md` states
that rights exist only after a written agreement. Needed before the first paying
pilot: EULA, SLA with measurable targets, personal-data processing terms,
liability limits, pricing and renewal, plus a technical annex from engineering
(delivery contents, support boundaries, validated hardware list, update and
support windows, SLA metrics).

Owner: lawyer, with the technical annex from this repository.

### D2 — Brand and certified build

Copyleft protects against SaaS free-riding; it does not prevent a third party
from rebranding the appliance. For an appliance the durable protection is
trademark, a certified-build identity, the validated hardware list and support.
ITEM-383 fixes this in writing: which names and marks may be used, how a
certified build is identified (release id, acceptance report, digest), what an
OEM or integrator may redistribute, and what the community build may not claim.
The repository artefact is `docs/hfoc-trademark-and-certified-build-policy.md`
with its machine-readable form
`release/trademark-and-certified-build-policy.txt`, checked by
`scripts/verify-trademark-policy.sh`; like this memo it states explicitly that
it is not legal advice and that the executed commercial agreement, not the
policy, is the binding instrument for an OEM or integrator.

Owner: owner (document), ITEM-383 (repository artefact).

### D3 — Crypto provider and certification path

Options, with consequences:

1. **Standard TLS only.** No GOST, no Russian regulated buyers. Cheapest, and it
   keeps the current mbedTLS path; export/cloud markets stay open.
2. **Integrate a certified provider** (for example КриптоПро CSP or ViPNet) in
   the commercial build. Opens the regulated segment, but adds a vendor
   agreement with per-device licensing, redistribution restrictions, and
   typically a prohibition on independent modification — which collides with a
   custom OS unless the integration boundary is agreed contractually.
3. **Hybrid.** Community build on standard TLS; commercial certified build with
   the provider's module. Recommended shape if the regulated segment is the
   target, because it keeps the open core open.

Certification notes: certification requires the vendor to control the build, so
reproducible builds, the SBOM and the acceptance report (ITEM-379/380) are
prerequisites, not paperwork. Decide only against a named anchor customer, with
a written cost and timeline estimate.

Owner: owner plus vendor; input from engineering.

### D4 — Contributor licence agreement

`LICENSING.md` says external contributions may be reviewed but not merged until
the CLA is legally reviewed. Decide explicitly: finish the review and open
contributions (needed for an ecosystem of policy packs and OEM partners), or
state that the repository is closed to contributions. A silent middle state
costs goodwill and invites accidental inbound rights.

Owner: owner (legal review).

### D5 — Free build versus commercial build

State what differs, so customers and integrators are not guessing: crypto
provider, certified images, support and update channel, and any OEM rights. The
code is dual-licensed either way.

Owner: owner, with the datasheet on the commercial track.

## Anti-patterns

- Shipping mbedTLS under `GPL-2.0-or-later` inside a proprietary build: elect
  Apache-2.0 explicitly and record the election (ITEM-382).
- Shipping a binary without notices, or adding a dependency that never reaches
  the license payload (ITEM-382 contracts this).
- Treating the public repository as an implied grant of commercial rights.
- Certifying a build that is not reproducible, or promising GOST without a
  certified provider.
- Letting the certified build and the community build drift apart in ways the
  acceptance report cannot describe.

## Open questions for the lawyer and the vendor

- Which entity signs the commercial agreement, and under which jurisdiction?
- What exactly must the SLA promise for an appliance that fails closed?
- Does the target market require certification of the appliance as a means of
  protection, of the crypto module only, or both?
- What are the vendor's per-device terms, redistribution rules and audit
  requirements for the certified crypto module?
- What liability limits are realistic for a device that sits in front of a
  customer's accounting system?

## Implementation links

- ITEM-382 — licensing surface and notices closure.
- ITEM-383 — trademark and certified build policy.
- ITEM-380 — release gate: SBOM, CVE process, reproducible release.
- Commercial track — datasheet, pilot letters of intent, legal pack, validated
  hardware list, penetration test.
