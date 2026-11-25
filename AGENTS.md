# HFOC OS development rules

# HFOC OS Implementation Agent Contract

## 1. Repository Hygiene

- Before any work: verify clean worktree (`git status --short` empty), fetch
  origin, switch to `main`, fast-forward to `origin/main`.
- Record the full SHA via `git rev-parse HEAD` before and after every session.

## 2. Branch Discipline

- Each new feature lives in its own feature branch, branched from the current
  `origin/main`.
- Every feature branch maps to a single Draft PR targeting `main`.
- Never commit or push directly to `main`.
- Never force-push, `reset --hard`, delete branches, or weaken CI/test gates.
- DeepSeekX automation is authorized to mark its own completed Draft PRs as Ready
  and merge them into `main` after all merge gates in this contract are green.

## 3. Scope Control

- APIC, IOAPIC, PIC masking, x2APIC, SMP, APIC timer, and IRQ routing are out of scope unless explicitly listed in the active queue item or its existing approved project documentation.
- Any scope creep must be flagged immediately; do not implement unapproved extensions.

## 3a. Disk and hardware-acceptance policy

- `/dev/nvme0n1` is the host system disk: it is never written, partitioned,
  formatted, reset or touched by any acceptance path, test, script or agent.
- `/dev/sda` is the dedicated hardware-acceptance disk and may be written,
  partitioned, installed to and reset, but only by an acceptance run that has
  passed the identity guard: the disk model, the disk serial and the running
  root device must match the declared acceptance-device constants, and a
  mismatch must abort before the first write (exit 90 in
  `scripts/hfoc-physical-sata-continue.sh` is the reference implementation).
- Hardware acceptance is never part of CI, of a QEMU proof or of the shared
  self-hosted runner jobs; it runs only on the acceptance host, and its result
  is recorded as a hardware proof, distinct from a QEMU proof.
- Before the first write of a hardware-acceptance run, the run appends an audit
  line to the acceptance log: who or what started it, when, the disk model and
  serial, the declared acceptance-device constants and the image digest. The
  acceptance report quotes that line.
- A physical acceptance run belongs to a hardware-acceptance queue item, and it
  stays human-triggered until the first complete successful hardware run on the
  declared disk: the unattended worker never starts it on its own.
- Swapping, renaming or replacing the acceptance disk requires updating the
  declared model and serial in the same commit; the guard is the only thing
  that makes the standing permission safe.

## 4. Canonical ITEM Pipeline

Every ITEM, without exception, runs the same pipeline from start to finish:

```
push
  |
  v
create Draft PR
  |
  v
start CI
  |
  +--> [in parallel] prepare the next ITEM
  |
  v
CI GREEN
  |
  v
self-review
  |
  v
mark Ready
  |
  v
squash merge
  |
  v
post-merge CI
  |
  v
immediately begin the next implementation
```

Step detail:

1. Implement the ITEM locally, following the RED → GREEN discipline in section 5.
2. Commit one logical commit per meaningful step and `push` the feature branch.
3. Open exactly one Draft PR targeting `main`.
4. Start CI on the Draft PR.
5. While CI runs, prepare the next ITEM in parallel: read its spec/plan, inspect
   the affected architecture, code, tests and dependencies, and refine the local
   implementation plan. Do not branch off or commit the next ITEM until the
   current one is merged.
6. When CI is GREEN, perform the final self-review required by section 9.
7. Mark the Draft PR Ready.
8. Squash-merge into `main` once every merge gate is green. **A green PR is
   merged, not parked**: the moment CI on the PR's HEAD is GREEN and the gates
   of section 9 pass, mark the Draft PR Ready and squash-merge it in the same
   session. Leaving a green PR open - Draft or Ready - is a contract violation,
   not caution. The only states a PR may sit in are "CI running", "CI red and
   being fixed", or "merged".
9. Verify post-merge CI on `main` is GREEN.
10. Immediately begin the next ITEM. The queue is never idle between items and
    never waits for fresh user instruction.

- A separate design/spec/plan PR is NOT required before implementation.
- Before coding, inspect the current architecture, existing documentation, relevant code, tests, dependencies, and the active queue item.
- Form a focused implementation plan locally in the current agent context, then proceed directly to implementation.
- Existing specs and plans are reference material and must not be duplicated merely to start coding.
- Do not create or wait for CI on documentation-only, design-only, spec-only, or plan-only PRs unless documentation itself is the explicit deliverable of the ITEM.

## 5. Implementation Discipline

- Follow the locally prepared implementation plan sequentially: RED → GREEN.
- One logical commit per meaningful implementation step.
- Run focused local tests during implementation and `git diff --check` before committing.
- Local heavy work (builds, ISO, host/acceptance/QEMU suites) runs through
  `scripts/hfoc-heavy-lock.sh local -- <command>`, so it waits for a live CI
  phase on the shared host instead of overlapping it; the tool also keeps the
  host priority settings (ITEM-370A, `docs/ci-self-hosted-runner.md`).

## 6. Failure Handling

- On a test failure, architectural ambiguity, merge conflict, or scope violation: stop immediately. Produce a root-cause report with the failing command, observed vs. expected output, and relevant diagnostic data.
- Do not invent workarounds without user approval.

## 7. Final Report

Every session ends with a mandatory final report containing:

- Old and new full SHA (`git rev-parse HEAD`).
- Changed files (`git diff --stat` / `git diff --check`).
- Test commands and results.
- CI URL and status.
- PR URL and status.
- `git status` output.
- Identified risks.

## 8. Integrity

- Never fabricate SHAs, CI status, or test results. Obtain the full SHA only via `git rev-parse HEAD`.

## 9. Merge Gate

- **Every green PR is squash-merged.** When CI on the PR's HEAD is GREEN and the
  gates below pass, the PR is marked Ready and squash-merged immediately in the
  same session; a green PR is never left open for a later instruction, and
  "waiting for the owner to say merge" is not a state this contract allows. A PR
  may stay open only while CI is running, or while a red CI is being fixed.
- A separate final self-review (pipeline step 6) is required before merging.
- DeepSeekX may mark its own Draft PR as Ready and squash-merge it into `main`
  autonomously when all required checks are green, the PR is clean/mergeable,
  and the current item is the oldest eligible item in the active queue.
- After merge, DeepSeekX must synchronize local `main` with `origin/main` and
  verify post-merge CI GREEN before advancing the queue.
- Advancement is immediate: the next eligible ITEM starts as soon as
  post-merge CI is GREEN, using the preparation performed in parallel during
  pipeline step 5. Do not pause the queue between ITEMs.

HFOC OS is a freestanding monolithic x86-64 educational operating system.

## Build and test

Before modifying code:

1. Run `make clean`.
2. Run `make check`.
3. Inspect the current boot sequence.
4. Reproduce the reported problem.

After modifying code:

1. Run `make clean`.
2. Run `make check`.
3. Run `make iso`.
4. Run the relevant QEMU test.
5. Include actual terminal output in the report.

## Debugging

Do not guess the cause of a kernel failure.

Use, as appropriate:

- `addr2line`
- `objdump`
- `readelf`
- `nm`
- QEMU `-d int,guest_errors`
- QEMU trace events
- GDB remote debugging with `make debug`

For interrupt problems, verify each stage separately:

1. Device generates interrupt.
2. PIC or APIC receives it.
3. Interrupt is delivered to CPU.
4. Correct IDT vector is selected.
5. ISR stack frame is valid.
6. C handler runs.
7. EOI is sent.
8. The interrupt repeats.

## Architecture

Current target:

- x86-64
- Limine boot protocol
- legacy 8259 PIC
- PIT
- PS/2 i8042 keyboard
- framebuffer console

Do not introduce Local APIC, IOAPIC, SMP or a custom UEFI loader unless explicitly requested.

## Versioning

Do not increase VERSION merely because an attempted fix was written.

Increase VERSION only after:

- the problem is reproduced;
- the fix is tested in QEMU;
- `make check` passes;
- boot behavior is confirmed.

Update:

- VERSION
- README.md
- RELEASE-NOTES.md

## Git

Before work:

- run `git status`;
- do not overwrite unrelated user changes.

After work:

- show `git diff --stat`;
- show `git status`;
- commit only after successful verification.
