#!/usr/bin/env bash
# ITEM-552: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

item552_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item552_root" || exit 1

# ITEM-552: one command on the laptop, one command on the Debian machine, a file
# in between.
#
# Before this item the diagnostics ended at the screen: the failure snapshot
# lived in RAM, the journal was written "when the medium is reachable", and the
# only way to carry either to the machine that can read them was a photograph.
# This slice pins the export: the session frame and its CRC, the journal's
# persisted session counter, the reserved file's ring and read-back, the one
# operator command, the report's own sources (xHCI, input and the saved command
# snapshot; EHCI and the boot medium), the read-only Debian extractor with its
# identity and ambiguity rules, the host and QEMU proofs, and the fact that the
# QEMU proof's fault injection exists only in its own kernel variant.

# 1. The files the item touches exist.
for item552_file in \
    kernel/include/diag_export.h \
    kernel/src/diag_session.c \
    kernel/src/diag_export.c \
    kernel/include/hfoc_build.h \
    kernel/src/hfoc_build.c \
    kernel/src/install_fat12_journal.c \
    kernel/src/install_journal_core.c \
    kernel/src/install_journal_platform.c \
    kernel/src/xhci.c \
    kernel/src/ehci.c \
    kernel/src/console.c \
    tests/diag-host.c \
    scripts/hfoc-diag-read.py \
    scripts/test-qemu-diag-export.sh \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item552_file ]] || {
        echo "ITEM-552: missing $item552_file" >&2
        exit 1
    }
done

# 2. The session frame: an identifier, a BEGIN, an END with the record count and
#    the CRC, and a continuation prefix for a split record.
for item552_frame in \
    '"[DIAG] id="' \
    '" state=BEGIN build="' \
    '" state=END records="' \
    '" crc="' \
    '#define DIAG_CONTINUATION "[DIAG+] "' \
    '#define DIAG_REPORT_MAX_RECORDS 96U'; do
    grep -Fq -- "$item552_frame" kernel/src/diag_session.c kernel/include/diag_export.h || {
        echo "ITEM-552: the session frame lacks $item552_frame" >&2
        exit 1
    }
done
grep -Fq 'diag_export_crc(crc, record, record_length)' kernel/src/diag_export.c || {
    echo "ITEM-552: the session CRC is not taken over the records" >&2
    exit 1
}
grep -Fq 'diag_export_slot_text(' kernel/src/diag_export.c || {
    echo "ITEM-552: the read-back does not decode the slot frame" >&2
    exit 1
}

# 3. The journal carries the session counter, rings, and reads back.
grep -Fq '#define FAT12_JOURNAL_HEADER_SEQUENCE_OFFSET 25U' \
    kernel/include/install_fat12_journal.h || {
    echo "ITEM-552: the header does not reserve the session counter" >&2
    exit 1
}
grep -Fq 'fat12_journal_rewind(struct fat12_journal *journal,' \
    kernel/include/install_fat12_journal.h || {
    echo "ITEM-552: the reserved file cannot ring" >&2
    exit 1
}
grep -Fq 'fat12_journal_read_slot(' kernel/src/install_journal_platform.c || {
    echo "ITEM-552: the placement cannot read a slot back" >&2
    exit 1
}
grep -Fq 'bool install_journal_rewind(struct install_journal *journal,' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-552: the core has no session restart" >&2
    exit 1
}
grep -Fq 'bool install_journal_write(struct install_journal *journal,' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-552: the core cannot write an export record" >&2
    exit 1
}

# 4. The one operator command, and its two answers.
grep -Fq 'console_command_is(input_line, "diag save")' kernel/src/console.c || {
    echo "ITEM-552: the operator has no diag save command" >&2
    exit 1
}
grep -Fq '"DIAG SAVED id="' kernel/src/console.c || {
    echo "ITEM-552: a save does not name its session" >&2
    exit 1
}
grep -Fq 'diag_failure_short_line(failure, short_line' kernel/src/console.c || {
    echo "ITEM-552: a refused save does not name its reason" >&2
    exit 1
}
grep -Fq '"DIAG FAILED"' kernel/src/diag_failure.c || {
    echo "ITEM-552: the refusal line does not start with DIAG FAILED" >&2
    exit 1
}
grep -Fq 'MENU XHCI INPUT DIAG PTR ARM' kernel/src/console.c || {
    echo "ITEM-552: the help does not list DIAG" >&2
    exit 1
}
# SAVED is only said after the read-back compared the records with what was
# written.  ITEM-557 moved the reason into the failure record; the stage name is
# still the one the comparison reports.
grep -Fq 'diag_fail(DIAG_EXPORT_VERIFY, "BODY_TEXT");' kernel/src/diag_export.c || {
    echo "ITEM-552: a save is not compared record by record" >&2
    exit 1
}
grep -Fq 'diag_fail(DIAG_EXPORT_VERIFY, "END_VERIFY");' kernel/src/diag_export.c || {
    echo "ITEM-552: a save does not verify its end record" >&2
    exit 1
}
for item552_sequence in 'SEQUENCE_COMMIT' 'SEQUENCE_VERIFY'; do
    grep -Fq "\"$item552_sequence\"" kernel/src/diag_export.c || {
        echo "ITEM-552: a save does not verify the persisted session counter" >&2
        exit 1
    }
done

# 5. The report's own sources: the xHCI/input answers through the sink, the
#    EHCI and medium answers, and the build identity.
grep -Fq 'void xhci_diagnostics_report(xhci_report_sink_fn sink' \
    kernel/include/xhci.h || {
    echo "ITEM-552: the xHCI answers have no report sink" >&2
    exit 1
}
grep -Fq 'xhci_diag_sink = sink;' kernel/src/xhci.c || {
    echo "ITEM-552: the display path does not route into the report" >&2
    exit 1
}
grep -Fq 'void ehci_diagnostics_report(ehci_report_sink_fn sink' \
    kernel/include/ehci.h || {
    echo "ITEM-552: the EHCI answers have no report sink" >&2
    exit 1
}
grep -Fq 'xhci_diagnostics_report(diag_sink, NULL);' kernel/src/diag_export.c || {
    echo "ITEM-552: the report does not include the xHCI answers" >&2
    exit 1
}
grep -Fq 'ehci_diagnostics_report(diag_sink, NULL);' kernel/src/diag_export.c || {
    echo "ITEM-552: the report does not include the EHCI answers" >&2
    exit 1
}
grep -Fq 'hfoc_build_id()' kernel/src/diag_export.c || {
    echo "ITEM-552: the report does not name the build" >&2
    exit 1
}
grep -Fq 'BUILD_ID := $(shell git rev-parse --short=12 HEAD' Makefile || {
    echo "ITEM-552: the build identity is not passed to the kernel" >&2
    exit 1
}

# 6. The Debian extractor: read-only, identity by the journal on the medium,
#    ambiguity refused, newest session required to be complete.
for item552_reader in \
    'candidate_devices()' \
    'more than one device carries an HFOC journal' \
    'has no ' \
    'HFOC-DIAG-{session' \
    'zlib.crc32'; do
    grep -Fq -- "$item552_reader" scripts/hfoc-diag-read.py || {
        echo "ITEM-552: the extractor lacks $item552_reader" >&2
        exit 1
    }
done
grep -Fq 'hfoc-install-log-read.py' scripts/hfoc-diag-read.py || {
    echo "ITEM-552: the extractor does not use the proven journal reader" >&2
    exit 1
}

# 7. The fault injection exists only in the proof's kernel variant.
grep -Fq '#ifdef HFOC_XHCI_TIMEOUT_QEMU_TEST' kernel/src/xhci.c || {
    echo "ITEM-552: the proof-only fault injection is not guarded" >&2
    exit 1
}
grep -Fq 'DIAG_EXPORT_CFLAGS := $(PRODUCTION_INSTALLER_CFLAGS) -DHFOC_XHCI_TIMEOUT_QEMU_TEST' \
    Makefile || {
    echo "ITEM-552: the injection is not confined to the proof variant" >&2
    exit 1
}
if sed -n 's/^PRODUCTION_INSTALLER_CFLAGS := \(.*\)$/\1/p' Makefile |
    grep -Fq -- '-DHFOC_XHCI_TIMEOUT_QEMU_TEST'; then
    echo "ITEM-552: a shipped medium carries the fault injection" >&2
    exit 1
fi

# 8. The proofs exist and are registered.
grep -Fq '.PHONY: test-diag-host' Makefile || {
    echo "ITEM-552: the host proof target is missing" >&2
    exit 1
}
grep -Fq '$(MAKE) test-diag-host' Makefile || {
    echo "ITEM-552: the host proof is not in the canonical host suite" >&2
    exit 1
}
grep -Fq '.PHONY: test-diag-export-qemu' Makefile || {
    echo "ITEM-552: the QEMU proof target is missing" >&2
    exit 1
}
grep -Fq 'test-diag-export-qemu: diag-export-qemu-media' Makefile || {
    echo "ITEM-552: the QEMU proof does not build its medium" >&2
    exit 1
}
grep -Fq 'test-diag-export-qemu' Makefile || {
    echo "ITEM-552: the QEMU proof is not registered" >&2
    exit 1
}
for item552_test in \
    test_framing test_crc test_slot_decode test_result_names; do
    grep -Fq "$item552_test" tests/diag-host.c || {
        echo "ITEM-552: missing host proof $item552_test" >&2
        exit 1
    }
done
grep -Fq 'scripts/hfoc-diag-read.py --disk "$STICK"' \
    scripts/test-qemu-diag-export.sh || {
    echo "ITEM-552: the QEMU proof does not extract with the Debian script" >&2
    exit 1
}
grep -Fq 'DIAG SAVED id=' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-552: the QEMU proof does not run the operator command" >&2
    exit 1
}
grep -Fq '[XHCI-CMD] result=TIMEOUT' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-552: the QEMU proof does not require the failure snapshot" >&2
    exit 1
}

# 8a. ITEM-553: a refused save still says what the machine saw, and the Debian
#     probe stays on USB media.
grep -Fq 'diag_fail(DIAG_EXPORT_NO_MEDIUM, "MEDIUM_PROBE");' kernel/src/diag_export.c || {
    echo "ITEM-553: NO_MEDIUM still replaces the machine's state" >&2
    exit 1
}
for item552_detail in 'ehci=' ' medium=' ' journal=' 'step='; do
    grep -Fq -- "$item552_detail" kernel/src/diag_failure.c || {
        echo "ITEM-553: the refusal detail lacks $item552_detail" >&2
        exit 1
    }
done
grep -Fq '"MEDIUM_PROBE"' kernel/src/diag_export.c || {
    echo "ITEM-553: the medium refusal no longer names its stage" >&2
    exit 1
}
grep -Fq '"/usb" not in os.path.realpath(entry / "device")' \
    scripts/hfoc-diag-read.py || {
    echo "ITEM-553: the Debian probe still walks internal disks" >&2
    exit 1
}

# 8b. ITEM-556: the export proof carries two mass-storage devices, so the walk's
#     continuation and the boot-medium selection execute in the driver.
grep -Fq 'FOREIGN=' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-556: the export proof has no foreign device" >&2
    exit 1
}
grep -Fq 'usb-storage,drive=foreign,bus=ehci.0,port=1' \
    scripts/test-qemu-diag-export.sh || {
    echo "ITEM-556: the foreign device is not on the first EHCI port" >&2
    exit 1
}
grep -Fq 'state=BOUND reason=NOT_MEDIUM' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-556: the proof does not require the foreign device to be rejected" >&2
    exit 1
}

# 8c. ITEM-557: the report's build identity is a dependency, not a flag, and the
#     proof refuses a report of another build or of an older session.
grep -Fq 'BUILD_ID_HEADER := build/generated/hfoc-build-id.h' Makefile || {
    echo "ITEM-557: the build identity is not a generated header" >&2
    exit 1
}
grep -Fq 'HFOC_BUILD_ID' build/generated/hfoc-build-id.h || {
    echo "ITEM-557: the generated build identity header is missing" >&2
    exit 1
}
grep -Fq '#include "hfoc-build-id.h"' kernel/src/hfoc_build.c || {
    echo "ITEM-557: the build identity source does not include the generated header" >&2
    exit 1
}
grep -Fq 'EXPECTED_BUILD_ID=' Makefile || {
    echo "ITEM-557: the export proof is not told the build it boots" >&2
    exit 1
}
for item557_assertion in \
    'the report does not name build' \
    'the report header does not name session' \
    'the report mixes session' \
    'the proof medium' ; do
    grep -Fq -- "$item557_assertion" scripts/test-qemu-diag-export.sh || {
        echo "ITEM-557: the export proof lacks the freshness assertion $item557_assertion" >&2
        exit 1
    }
done
grep -Fq '[DIAG] id=' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-557: the proof does not reject a medium that already carries a session" >&2
    exit 1
}

# 8d. ITEM-557: the refusal is a short line with the stage and the cause, a full
#     detail line, a reprint command, and a durable record - executed by the
#     console proof, because the screen is what the operator has.
grep -Fq 'struct diag_failure {' kernel/include/diag_failure.h || {
    echo "ITEM-557: the failure record is not declared" >&2
    exit 1
}
grep -Fq 'const struct diag_failure *diag_export_last_failure(void)' \
    kernel/include/diag_export.h || {
    echo "ITEM-557: the failure record has no accessor" >&2
    exit 1
}
grep -Fq 'diag_fail(DIAG_EXPORT_NO_MEDIUM, "MEDIUM_PROBE")' kernel/src/diag_export.c || {
    echo "ITEM-557: the medium refusal does not name its stage" >&2
    exit 1
}
grep -Fq 'diag_failure_short_line(failure, short_line' kernel/src/console.c || {
    echo "ITEM-557: the console does not print the refusal line" >&2
    exit 1
}
grep -Fq '"step"' kernel/src/diag_failure.c || {
    echo "ITEM-557: the refusal line does not carry the stage" >&2
    exit 1
}
grep -Fq 'console_write("DIAG DETAIL ")' kernel/src/console.c || {
    echo "ITEM-557: the console does not print the full detail" >&2
    exit 1
}
grep -Fq 'console_command_is(input_line, "diag")' kernel/src/console.c || {
    echo "ITEM-557: the refusal cannot be asked for again" >&2
    exit 1
}
grep -Fq '"[DIAG-FAIL] reason="' kernel/src/console.c || {
    echo "ITEM-557: the refusal does not reach the boot log" >&2
    exit 1
}
grep -Fq 'failure->step' kernel/src/console.c || {
    echo "ITEM-557: the console does not read the recorded stage" >&2
    exit 1
}
grep -Fq 'test-diag-failure-qemu:' Makefile || {
    echo "ITEM-557: the refusal proof has no target" >&2
    exit 1
}
grep -Fq 'DIAG FAILED reason=NO_MEDIUM step=MEDIUM_PROBE' \
    scripts/test-qemu-diag-failure.sh || {
    echo "ITEM-557: the refusal proof does not require the short line" >&2
    exit 1
}
grep -Fq 'DIAG LAST reason=NO_MEDIUM step=MEDIUM_PROBE' \
    scripts/test-qemu-diag-failure.sh || {
    echo "ITEM-557: the refusal proof does not require the reprint" >&2
    exit 1
}

# 8e. ITEM-557: every media write takes the shared lock and is verified twice.
grep -Fq 'HFOC_MEDIA_WRITE_LOCKED' scripts/hfoc-media-write.sh || {
    echo "ITEM-557: the media writer does not take the shared lock" >&2
    exit 1
}
grep -Fq 'scripts/hfoc-heavy-lock.sh local --' scripts/hfoc-media-write.sh || {
    echo "ITEM-557: the media writer does not use the shared lock mechanism" >&2
    exit 1
}
for item557_media_refusal in '/dev/nvme' '/dev/sda' 'the settle window'; do
    grep -Fq -- "$item557_media_refusal" scripts/hfoc-media-write.sh || {
        echo "ITEM-557: the media writer does not refuse/verify $item557_media_refusal" >&2
        exit 1
    }
done

# 8f. ITEM-558: the record is a module of its own, the cleanup keeps it, the
#     short line fits one row, and the controller's snapshot reaches the detail.
for item558_file in \
    kernel/include/diag_failure.h \
    kernel/src/diag_failure.c; do
    [[ -f $item558_file ]] || {
        echo "ITEM-558: $item558_file is missing" >&2
        exit 1
    }
done
grep -Fq 'void diag_failure_attempt_reset(struct diag_failure *failure);' \
    kernel/include/diag_failure.h || {
    echo "ITEM-558: the record has no cleanup seam" >&2
    exit 1
}
grep -Fq 'void diag_failure_attempt_reset(struct diag_failure *failure) {' \
    kernel/src/diag_failure.c || {
    echo "ITEM-558: the cleanup is not implemented" >&2
    exit 1
}
grep -Fq 'diag_failure_attempt_reset(&diag_failure);' kernel/src/diag_export.c || {
    echo "ITEM-558: the attempt's cleanup does not go through the record' >&2" >&2
    exit 1
}
grep -Fq 'DIAG_FAILURE_ROW_MAX 80U' kernel/include/diag_failure.h || {
    echo "ITEM-558: the short line has no row bound" >&2
    exit 1
}
grep -Fq 'diag_failure_short_line(failure, short_line' kernel/src/console.c || {
    echo "ITEM-558: the console does not build the short line from the record" >&2
    exit 1
}
grep -Fq 'diag_failure_short_line(failure, short_line' kernel/src/console.c || {
    echo "ITEM-558: the reprint does not use the same rendering" >&2
    exit 1
}
grep -Fq 'failure->primary[0]' kernel/src/diag_failure.c || {
    echo "ITEM-558: the primary cause is not rendered" >&2
    exit 1
}
grep -Fq 'ehci_failure_text()' kernel/src/diag_export.c || {
    echo "ITEM-558: the detail does not carry the controller snapshot" >&2
    exit 1
}
grep -Fq 'ehci_failure_primary()' kernel/src/diag_export.c || {
    echo "ITEM-558: the short line's primary cause is not the controller's" >&2
    exit 1
}
grep -Fq 'test_failure_record_survives_cleanup' tests/diag-host.c || {
    echo "ITEM-558: the cleanup proof is missing" >&2
    exit 1
}
grep -Fq 'test_failure_short_line_drops_whole_fields' tests/diag-host.c || {
    echo "ITEM-558: the short-line bound proof is missing" >&2
    exit 1
}
grep -Fq 'kernel/src/diag_failure.c tests/diag-host.c' Makefile || {
    echo "ITEM-558: the host proof does not link the record module" >&2
    exit 1
}
# The loaded kernel's revision is readable without a medium, on `version`.
grep -Fq 'HFOC OS 1.0.0 X86-64 build=' kernel/src/console.c || {
    echo "ITEM-558: the version command does not print the build identity" >&2
    exit 1
}
grep -Fq 'hfoc_build_id()' kernel/src/console.c || {
    echo "ITEM-558: the build identity is not read from the build" >&2
    exit 1
}
grep -Fq 'HFOC OS 1.0.0 X86-64 build=${EXPECTED_BUILD_ID}' \
    scripts/test-qemu-diag-failure.sh || {
    echo "ITEM-558: the console proof does not require the build identity" >&2
    exit 1
}
# ITEM-559: the export proof's own object tree must track its headers, or a
# stale object can keep publishing another build than the one the proof names.
grep -Fq 'DIAG_EXPORT_DEPS :=' Makefile || {
    echo "ITEM-559: the export kernel's header dependencies are not tracked" >&2
    exit 1
}
grep -Fq -- '-include $(DIAG_EXPORT_DEPS)' Makefile || {
    echo "ITEM-559: the export kernel's dependency files are not included" >&2
    exit 1
}

# 9. The documents record the item.
for item552_doc in 'ITEM-552' 'HFOC-DIAG-'; do
    grep -Fq -- "$item552_doc" docs/ARCHITECTURE.md || {
        echo "docs/ARCHITECTURE.md does not record $item552_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-552' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md does not record ITEM-552" >&2
    exit 1
}


# 12. ITEM-561: the refusal record carries the start evidence.
#
#     The report of a `diag save` that refused at MEDIUM_PROBE is never written:
#     the medium is what it could not reach.  The start facts (the PCI command
#     and status, the handoff, the words at stop, reset and start, the periodic
#     schedule's state) were then only in RAM.  They now travel two ways that
#     survive a screen-only machine: the driver publishes `[EHCI-START]` at the
#     moment the controller is started, and the refusal record keeps the same
#     text as `ehci_start=`, which is what `diag` prints on demand.
grep -Fq 'void diag_failure_set(struct diag_failure *failure,' kernel/src/diag_failure.c || {
    echo "ITEM-561: the refusal record's setter moved without this pin" >&2
    exit 1
}
grep -Fq 'const char *start_detail,' kernel/include/diag_failure.h || {
    echo "ITEM-561: the refusal record has no start-evidence field" >&2
    exit 1
}
grep -Fq 'pieces[piece_count++] = " ehci_start=";' kernel/src/diag_failure.c || {
    echo "ITEM-561: the detail line does not render the start evidence" >&2
    exit 1
}
grep -Fq 'journal_state, journal_reason, ehci_failure_text(),' kernel/src/diag_export.c || {
    echo "ITEM-561: the export does not hand the start evidence to the record" >&2
    exit 1
}
grep -Fq 'ehci_start_evidence(), ehci_controllers_text());' \
    kernel/src/diag_export.c || {
    echo "ITEM-561: the export does not read the start evidence" >&2
    exit 1
}
grep -Fq '#define DIAG_FAILURE_START_MAX 704U' kernel/include/diag_failure.h || {
    echo "ITEM-561: the start evidence has no bound" >&2
    exit 1
}
for item561_diag_check in \
    'the detail carries the start evidence of the refused controller' \
    'the start evidence still fits the record'"'"'s bound'; do
    grep -Fq -- "$item561_diag_check" tests/diag-host.c || {
        echo "ITEM-561: the diagnostic proof lacks $item561_diag_check" >&2
        exit 1
    }
done

# 13. ITEM-562: the refusal record names every controller.
#
#     The run after ITEM-561 reported one aggregated verdict and one start
#     snapshot.  A machine with two USB 2 functions answered with whichever was
#     scanned last, so the record could not say that `00:1A.0` had failed at a
#     reset while `00:1D.0` had no device at all.  The detail now carries the
#     per-controller list beside the chosen controller's start evidence.
grep -Fq 'ehci_start_evidence(), ehci_controllers_text());' \
    kernel/src/diag_export.c || {
    echo "ITEM-562: the export does not hand over the per-controller list" >&2
    exit 1
}
grep -Fq 'pieces[piece_count++] = " ehci_all=";' kernel/src/diag_failure.c || {
    echo "ITEM-562: the detail line does not render the per-controller list" >&2
    exit 1
}
grep -Fq '#define DIAG_FAILURE_CONTROLLERS_MAX EHCI_VERDICT_TEXT_MAX' \
    kernel/include/diag_failure.h || {
    echo "ITEM-562: the per-controller list has no bound" >&2
    exit 1
}
grep -Fq 'const char *controllers);' kernel/include/diag_failure.h || {
    echo "ITEM-562: the refusal record cannot hold the per-controller list" >&2
    exit 1
}
grep -Fq 'the detail carries every controller' tests/diag-host.c || {
    echo "ITEM-562: the diagnostic proof lacks the per-controller assertion" >&2
    exit 1
}
