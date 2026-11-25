#!/usr/bin/env bash
# CI-SPEEDUP-3: the expected refusal is answered, not timed out.
#           Sourced in pinned order by scripts/check.sh; run ShellCheck on its
#           own.

# The speedup measurement that followed PR #761 found 121 s of the main HTTPS
# proof belonging to no typed answer: the credential-durability boot's
# expected-refusal call drove the whole sign-in conversation and then waited out
# the 120-second `ADMIN SESSION` marker for a session a retired password can
# never be issued.  The claims pinned here:
#
#   * the proof asks the helper for that refusal by name (`refuse`), with the
#     new login and the retired password, and the old "the login mode failed,
#     so the refusal happened" expectation is gone;
#   * the helper requires the console's own denied line, fixes the serial
#     position before the password is typed (so a refusal that predates the
#     attempt cannot satisfy it) and fails on an issued session, another
#     refusal reason, an ended conversation or no answer;
#   * the standard `login-gate` still follows it, so the new pair and the
#     after-boot policy are still proven in the same boot;
#   * the host witness that drives all of those paths is registered and run by
#     the canonical suite.
proof=scripts/test-qemu-production-service-https.sh
grep -Fq 'admin-enroll.py" refuse' "$proof" || {
    echo "CI-SPEEDUP-3: the durability proof does not ask for the expected refusal" >&2
    exit 1
}
# shellcheck disable=SC2016  # the proof's spelling, not this script's value
grep -Fq '"$ADMIN_SECRET" "$ACCOUNT_NEW_LOGIN"' "$proof" || {
    echo "CI-SPEEDUP-3: the refusal attempt does not present the retired password" >&2
    exit 1
}
if grep -Fq 'The old password still signs in on the local console' "$proof"; then
    echo "CI-SPEEDUP-3: the timeout-shaped refusal expectation is still in the proof" >&2
    exit 1
fi
grep -Fq 'APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED' "$proof" || {
    echo "CI-SPEEDUP-3: the refusal check does not name the denied line" >&2
    exit 1
}
grep -Fq 'attempt_offset = len(serial_text())' "$proof" || {
    echo "CI-SPEEDUP-3: the refusal answer is not scoped to this attempt" >&2
    exit 1
}
grep -Fq 'HFOC_ADMIN_REFUSE_TIMEOUT' "$proof" || {
    echo "CI-SPEEDUP-3: the missing-answer deadline is not bounded" >&2
    exit 1
}
# ITEM-590: the console prints the refusal in several writes and the host reads
# the file, so an answer counts only once its terminator is in the log.  Run
# 42075493165 read 31 characters of the expected line, called the prefix
# "another reason" and failed a proof whose guest had printed the whole denial.
grep -Fq 'for line in tail.split("\n")[:-1]:' "$proof" || {
    echo "ITEM-590: a refusal read while it is printed is not waited out" >&2
    exit 1
}
grep -Fq 'a refusal read while it is still being printed is waited out' \
    tests/https-refusal-answer-host.py || {
    echo "ITEM-590: the witness does not pin the torn refusal read" >&2
    exit 1
}
refuse_line=$(grep -n 'admin-enroll.py" refuse' "$proof" | head -n 1 | cut -d: -f1)
gate_line=$(grep -n 'admin-enroll.py" login-gate' "$proof" | head -n 1 | cut -d: -f1)
if [[ -z $refuse_line || -z $gate_line || $refuse_line -ge $gate_line ]]; then
    echo "CI-SPEEDUP-3: the new-password gate gesture no longer follows the refusal" >&2
    exit 1
fi
grep -Fq 'test-https-refusal-answer-host:' Makefile || {
    echo "Makefile has no test-https-refusal-answer-host target" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile's recipe, not this script's value
grep -Fq '$(MAKE) test-https-refusal-answer-host' Makefile || {
    echo "test-host-suite does not run test-https-refusal-answer-host" >&2
    exit 1
}
grep -Fq 'CI-SPEEDUP-3' docs/ROADMAP.md || {
    echo "CI-SPEEDUP-3: ROADMAP does not record the refusal answer" >&2
    exit 1
}
