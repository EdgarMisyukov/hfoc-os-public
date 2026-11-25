<!-- SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial -->
# HFOC OS Ownership and License Boundary Audit

## Audit basis

The audit was performed from the complete reachable Git history using
`git shortlog -sne --all` and path-scoped `git log --all` queries before the
license transition. Each observed author identity is listed with the evidence
that it is Edgar Misyukov or owner-authorized automation.

| Git identity | Commit evidence | Owner-control basis |
|---|---|---|
| `EdgarMisyukov <mis@hfoc.ru>` | 245 reachable commits in the pre-transition audit, including `000df83f199f261c6b7398d51b2d60fddbcc3bc8` | Primary maintainer identity using the owner's project email. |
| `Edgar Misyukov <308947928+EdgarMisyukov@users.noreply.github.com>` | Initial import `b1fc50b6027c3851c6e5b39caeb6b434cf3f59f3`, including the former root `LICENSE` | GitHub-provided private-email alias for the same repository owner account. |

## HFOC-owned paths

Repository material outside `third_party/` and dependency/build output is
treated as HFOC-owned only where the audit evidence establishes owner control.

## Legacy MIT material

The commit recorded as the legacy boundary in `LICENSING.md` and all its
HFOC-owned ancestors retain their already-granted MIT rights.

## Third-party exclusions

`third_party/lwip/`, the prepared Limine dependency tree, Limine artifacts in
ISOs, and future declared dependencies remain under upstream licenses.

## Audit result

No unresolved external copyright owner was identified in an HFOC-owned path.
The transition may proceed. Any later-discovered external ownership remains
under its existing grant until an additional valid grant is obtained.
