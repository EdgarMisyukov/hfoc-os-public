<!-- SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial -->
# HFOC Trademark and Certified Build Policy

**Item:** ITEM-383 of the HFOC Appliance Platform productization wave
**Depends on:** ITEM-380 (product release gate: SBOM, CVE process, reproducible
release)
**Release contract:** `HFOC-APPLIANCE-V1`
**Machine-readable form:** `release/trademark-and-certified-build-policy.txt`,
checked by `scripts/verify-trademark-policy.sh` and
`tests/test-trademark-policy-host.sh`
**Status:** policy in force for this repository. The certified-build path is
declared and machine-checked, but no certified build has been issued: the
release gate of ITEM-380 is its prerequisite and does not exist yet.

## Purpose

The licence settles what may be done with the code. It does not settle what a
build may be *called*, who may put a certification mark on it, or what a
customer is promised when they buy an appliance. AGPL-3.0-only stops a
third party from turning this code into a closed service; it does not stop the
same third party from rebranding the appliance, from passing a modified build
off as the official product, or from calling an unverified image "certified".
Those are brand and support promises, so this policy states them in writing.

The four questions this policy answers, and where:

| Question | Section |
| --- | --- |
| Which names and marks may be used, by whom, and which uses are prohibited | [1. Names and marks](#1-names-and-marks) |
| How a certified build is identified and labelled | [2. Certified build identity and label](#2-certified-build-identity-and-label) |
| What an OEM or integrator may redistribute, and what travels with it | [3. OEM and integrator redistribution](#3-oem-and-integrator-redistribution) |
| What the community build may and may not claim | [4. Community build claims](#4-community-build-claims) |

## Scope and status

- This policy is not legal advice. It is an engineering policy written by the
  project owner to state intent precisely enough to be machine-checked. The
  agreement texts, the CLA and any trademark filing need a lawyer.
- For an OEM or integrator, the executed commercial agreement, not this policy,
  is the binding instrument. Where this policy is stricter than the agreement,
  the agreement governs the parties to it; where the agreement grants more, the
  agreement governs. This policy never narrows the rights that AGPL-3.0-only
  grants over the code.
- The marks named here are asserted by the project owner as marks used in
  connection with this project. This policy claims no trademark registration
  (`marks_registration_claimed=false`) and no filing is asserted by any file in
  this repository.
- The machine-readable form carries the same statements as
  `release/trademark-and-certified-build-policy.txt`; where the two disagree,
  the document you are reading is the policy and the artefact is a defect.
- References to release-gate artefacts (SBOM, reproducible image digest, release
  gate report, acceptance report) name roles, not paths. The release gate of
  ITEM-380 owns their exact names and formats.

## 1. Names and marks

The project asserts the following marks:

| Mark | Form | Owner | May be used for |
| --- | --- | --- | --- |
| `HFOC OS` | word mark | project owner | the project itself and the unmodified distribution built from this repository |
| `HFOC Appliance` (`HFOC Appliance Platform`) | word mark | project owner | the appliance product line built from this repository |
| `HFOC Certified` | certification mark | project owner | a build that carries a valid certified-build label (section 2), and nothing else |

Who may use them:

- the project owner and its designated maintainers, without restriction;
- any redistributor, including the community, under nominative fair use: the
  marks may be used to refer to the project by name in text, with an accurate
  statement of which build is being discussed and whether it was modified;
- an OEM or integrator, only inside the scope of an executed commercial
  agreement, which is also the only instrument that can grant a mark right.
  No mark right travels with the code licence.

Prohibited uses — each of these is a breach of this policy and, where a licence
is involved, outside the licence:

- using any HFOC mark in a company, product, domain or package name without an
  executed agreement;
- applying `HFOC Certified` to a build that has no valid certified-build label;
- claiming or implying that a community or modified build is certified;
- using any HFOC mark to imply endorsement, sponsorship, partnership or support
  that does not exist;
- removing or obscuring HFOC attribution, licence notices or the build identity
  of a redistributed image.

Two independent duties apply to a modified build: AGPL-3.0-only already requires
prominent notices that the build was changed and forbids misrepresenting the
origin of the code, and this policy additionally forbids any mark use that would
suggest the modified build is the official or certified one.

## 2. Certified build identity and label

A **certified build** is an image of this repository that (a) was produced from
a revision accepted by the product release gate (ITEM-380: SBOM, CVE process and
a reproducible image digest) and (b) carries a certified-build label issued
under an executed agreement and recorded in the certification registry. Nothing
else is a certified build: a green CI run, a signed ISO, a commercial licence, a
purchase order or a support contract do not by themselves make one.

### What the label must carry

| Field | Content |
| --- | --- |
| `release_id` | the release profile the image belongs to (`HFOC-APPLIANCE-V1`) |
| `source_revision` | the exact git revision the image was built from |
| `image_sha256` | the SHA-256 digest of the image, identical on an independent rebuild |
| `release_gate_report` | the release gate report published by ITEM-380 for this build |
| `sbom_digest` | the digest of the SBOM of this exact build |
| `acceptance_report` | the appliance end-to-end acceptance report and its gate verdict |
| `signer_anchor` | the release trust anchor that signed the label and the boot chain |

plus `issued_by` (the vendor or the project owner that issued it, under an
agreement) and `registry` (the certification registry entry that lists it).

### What the vendor must do

- build only from the revision the release gate accepted;
- produce the image so that an independent rebuild yields the recorded
  `image_sha256`;
- publish the SBOM, the release gate report and the acceptance report the label
  references, in a form the customer can keep;
- sign the label with the release trust anchor whose private half stays offline;
- record the issued label in the certification registry before the build is
  sold, and issue a new record after any modification, rebuild or relabel.

### How a customer or auditor verifies the label

1. Recompute the SHA-256 of the image and compare it with the label and with the
   release gate report.
2. Compare `source_revision` and `release_id` with the release gate report and
   with the boot attestation of the running system.
3. Resolve the SBOM and the acceptance report the label references and verify
   their digests.
4. Verify the label signature against the release trust anchor and the registry
   entry.

An auditor who cannot complete all four steps treats the label as unverified.

### What makes a label invalid

- the recomputed image digest differs from the label (`image_sha256` mismatch);
- the release gate report is missing, is for another build, or its verdict is
  not a complete pass;
- the build was modified, rebuilt or relabelled after the label was issued
  without a new record;
- the SBOM or the acceptance report the label references is missing or has a
  different digest;
- the label is not listed in the certification registry, or the registry entry
  was withdrawn.

An invalid label must be withdrawn and must not be quoted in sales material.

### What this repository publishes today

`policy_certified_build_published=false`: no certified build is published from
this repository, because the release gate of ITEM-380 — the SBOM, the CVE
process and the reproducible image digest — is its prerequisite. Flipping that
declared value is a deliberate commit that must reference the recorded label and
the release gate report; the verifier refuses the flip as a weakened claim until
the artefact is updated with them. Until then, any `HFOC Certified` claim about
a build of this repository is invalid, and this policy does not promise a
certification the code cannot verify.

## 3. OEM and integrator redistribution

Redistribution is governed by the licence the redistributor holds:

- on the community track, `AGPL-3.0-only`, with the source and notice duties of
  that licence;
- on the commercial track, `LicenseRef-HFOC-Commercial`, which exists only after
  a separate written agreement executed by the project owner
  (`COMMERCIAL-LICENSING.md`: possession of the source grants no commercial
  rights).

An OEM or integrator acting under an executed commercial agreement may
redistribute binaries built from the licensed revision and inside the scope the
agreement describes. What that redistributor may do *not* do is infer anything
from this policy: the agreement is the binding instrument, and it decides
territory, volume, support and duration.

These obligations travel with every redistributed build, on both tracks:

- ship the verified licence payload, with `LICENSES.sha256` unchanged, so
  `scripts/verify-license-payload.sh` still passes on the installed system
  (`scripts/install-license-payload.sh` is the canonical producer);
- keep every third-party notice and the licence inventory of the shipped
  dependencies;
- disclose the exact release id, the source revision and every modification;
- publish the SBOM of the redistributed build;
- offer the corresponding source of the exact built binaries under their
  declared licence;
- pass these obligations to every further recipient;
- use no HFOC mark beyond what the agreement grants — no mark right travels with
  the licence (`redistribution_mark_use=agreement_only`);
- make no support, warranty or SLA claim unless the agreement grants it.

An OEM that rebrands the appliance under its own mark must keep these
obligations visible in the shipped system (about/licenses surface, notices and
build identity) and must not present the rebranded image as an HFOC-certified
build unless it holds the label of section 2.

## 4. Community build claims

The community build is the same source under `AGPL-3.0-only`. It may be run,
studied, modified and redistributed under that licence, and it may be described
accurately, including the way it differs from the official images.

A community build:

- must not claim to be a certified build, must not carry the certification mark
  and must not quote a certified-build label;
- must not claim vendor support, warranty, an SLA or an official update channel;
- must not use the HFOC marks beyond nominative fair use, and never
  `HFOC Certified`;
- must not present a modified build as the unmodified HFOC OS distribution;
- must offer the complete corresponding source under `AGPL-3.0-only` and keep
  the copyright, licence and modification notices.

Recommended wording for a modified redistribution, which satisfies both the
AGPL notices and this policy: *"built from HFOC OS sources, modified; an
unofficial build, not certified and not supported by the HFOC OS project."*

## 5. Machine-checked policy

`release/trademark-and-certified-build-policy.txt` is the machine-readable form
of this document, in the same `key=value` style as the other `release/*.txt`
matrices, organised into the sections `policy`, `marks`, `permitted_use`,
`prohibited_use`, `certified_label`, `redistribution`, `community_build` and
`legal`.

`scripts/verify-trademark-policy.sh` parses it without external tools and refuses:

- a malformed line, a duplicate key, an empty value or stray whitespace;
- an unknown section (a key that belongs to no declared section) and an
  ambiguous one;
- a missing section and a missing key — deleting a load-bearing statement is a
  weakened policy, not a shorter one;
- an unknown key inside a declared section;
- a weakened or fabricated claim: `community_build_may_claim_certified_build`,
  `community_build_may_claim_vendor_support`,
  `community_build_may_claim_official_update_channel`,
  `community_build_mark_use`, `redistribution_requires_written_agreement`,
  `redistribution_mark_use`, `marks_registration_claimed`,
  `policy_certified_build_published`, `legal_advice` and
  `legal_review_required` each have exactly one accepted value;
- a `policy_document` or `legal_binding_instrument` that does not exist.

`tests/test-trademark-policy-host.sh` proves both directions: the verifier
accepts the real artefact and refuses eighteen weakened, unknown-section,
unknown-key and structural fixtures. The `scripts/check.sh` contract block of
ITEM-383 pins this document's four answers, the label fields, the community
build prohibitions and the "not legal advice / the agreement is binding"
statement, and wires the verifier and the host proof into the canonical suite.

## 6. What this policy does not do

- It is not legal advice and has not been reviewed by a lawyer
  (`legal_review_required=true`).
- It does not register a trademark, and it makes no claim about registrability
  or about the marks being available in any jurisdiction.
- It does not replace the commercial agreement, the EULA, the SLA, the CLA or
  any personal-data terms; those remain on the commercial track.
- It does not certify anything, and it does not create a certification
  programme: issuing a certified build needs the release gate of ITEM-380, a
  recorded label and the vendor or partner process behind it.
- It does not change the licence of any file, and it does not narrow
  `AGPL-3.0-only` rights over the code.

## Related documents

- `LICENSING.md`, `COMMERCIAL-LICENSING.md`, `CONTRIBUTOR_LICENSE_AGREEMENT.md`
- `docs/hfoc-appliance-licensing-and-certification.md` (decision D2 — brand and
  certified build; D5 — free build versus commercial build)
- `scripts/verify-licensing-policy.sh`, `scripts/verify-license-payload.sh`,
  `scripts/install-license-payload.sh`
- ITEM-380 — product release gate (SBOM, CVE process, reproducible release)
- ITEM-382 — licensing surface and notices closure
