#!/usr/bin/env bash
# ITEM-590: a FULL journal still takes the operator's session, and the PHY
#           answer is in that session's journal.  Sourced in pinned order by
#           scripts/check.sh; run ShellCheck on its own.

# The owner's Toshiba booted the installed system with the diagnostic stick
# attached.  The journal bound its reserved FAT12 file and the machine's own
# telemetry filled all 512 slots it declares in about twenty-three seconds; the
# operator's `diag save` then answered `DIAG FAILED reason=NO_MEDIUM
# step=MEDIUM_PROBE` and the stick carried no `[DIAG] BEGIN`/`END` at all.  The
# same boot log carried no `[NET-PHY]` line either, although the serial stream
# printed one.  The claims pinned here:
#
#   * a bound journal that reached its declared capacity is FULL, not a failed
#     medium: the export's own question is `saveable()` (READY or FULL) and the
#     journal's rewind is what recovers the room;
#   * the failed placement and the pre-placement state keep their own names,
#     and a failed write or flush is refused by the step the operator reads;
#   * the exported record names the journal's own state, so a report cannot say
#     UNBOUND about a journal it wrote a verified session into;
#   * the `[NET-PHY]` record is its own record, opened and ended by the
#     emitter, so the console mirror cannot drop it as a tail of the record
#     before it; the boot journal and the exported report print one composition;
#   * the host proofs drive the production chain and the installed-system QEMU
#     proof fills the journal before it saves.
journal_header=kernel/include/install_journal_core.h
journal_source=kernel/src/install_journal_core.c
platform_header=kernel/include/install_journal_platform.h
platform_source=kernel/src/install_journal_platform.c
export_source=kernel/src/diag_export.c
phy_header=kernel/include/appliance_production_network_phy_record.h
phy_source=kernel/src/appliance_production_network_phy_record.c
adapter_source=kernel/src/appliance_production_network_x86.c

grep -Fq 'install_journal_saveable(' "$journal_header" || {
    echo "ITEM-590: the export's own predicate is not declared" >&2
    exit 1
}
grep -Fq 'journal->state == INSTALL_JOURNAL_READY ||' "$journal_source" || {
    echo "ITEM-590: a READY journal is not saveable" >&2
    exit 1
}
grep -Fq 'journal->state == INSTALL_JOURNAL_FULL;' "$journal_source" || {
    echo "ITEM-590: a healthy FULL journal is still refused as a medium" >&2
    exit 1
}
grep -Fq 'if (journal == NULL || !journal->bound) {' "$journal_source" || {
    echo "ITEM-590: saveable() does not require a bound placement" >&2
    exit 1
}
grep -Fq 'install_journal_platform_saveable(void);' "$platform_header" || {
    echo "ITEM-590: the seam declares no saveable predicate" >&2
    exit 1
}
grep -Fq 'return install_journal_saveable(&platform_journal);' "$platform_source" || {
    echo "ITEM-590: the seam does not answer the core's predicate" >&2
    exit 1
}
# The two medium decisions in diag_export_save() are the predicate; a third
# `ready()`-only refusal would put the laptop's bug back.
saveable_uses=$(grep -Fc 'install_journal_platform_saveable()' "$export_source")
[[ "$saveable_uses" == 2 ]] || {
    echo "ITEM-590: the export asks saveable() ${saveable_uses} times, not 2" >&2
    exit 1
}
if grep -Fq 'if (!install_journal_platform_ready()) {' "$export_source"; then
    echo "ITEM-590: the export still refuses on READY alone" >&2
    exit 1
fi
grep -Fq 'install_journal_platform_rewind(sequence)' "$export_source" || {
    echo "ITEM-590: the export does not recover the room through the rewind" >&2
    exit 1
}
grep -Fq 'journal = install_journal_state_string(journal_status.state);' \
    "$export_source" || {
    echo "ITEM-590: the record does not name the journal's own state" >&2
    exit 1
}
grep -Fq 'DIAG_EXPORT_NO_MEDIUM, "MEDIUM_PROBE"' "$export_source" || {
    echo "ITEM-590: a failed medium lost its named refusal" >&2
    exit 1
}
grep -Fq 'DIAG_EXPORT_WRITE, "FLUSH"' "$export_source" || {
    echo "ITEM-590: a failed flush lost its named refusal" >&2
    exit 1
}

# The PHY record is its own record: the emitter opens and ends it.
grep -Fq 'appliance_production_network_phy_record_publish(' "$phy_header" || {
    echo "ITEM-590: no emitter for the PHY record" >&2
    exit 1
}
grep -Fq 'console_journal_begin();' "$phy_source" || {
    echo "ITEM-590: the PHY record does not open its own journal record" >&2
    exit 1
}
grep -Fq 'console_journal_end();' "$phy_source" || {
    echo "ITEM-590: the PHY record is never ended" >&2
    exit 1
}
grep -Fq 'serial_sequence_end(serial_flags);' "$phy_source" || {
    echo "ITEM-590: the PHY record does not close its serial sequence" >&2
    exit 1
}
grep -Fq 'phy->first_error == RTL8168_OK' "$phy_source" || {
    echo "ITEM-590: a PHY with no error would print the name of success" >&2
    exit 1
}
grep -Fq 'NO_PHY_MANAGEMENT' "$phy_source" || {
    echo "ITEM-590: the skip reason is not in the record's composition" >&2
    exit 1
}
# The call site finishes the device record before the PHY record starts - the
# defect was exactly the opposite order.
phy_call=$(awk '
    /log_production_network_phy\(\);/ { print previous2 "|" previous1; exit }
    { previous2 = previous1; previous1 = $0 }
' kernel/src/kernel.c)
[[ "$phy_call" == *'serial_sequence_end(serial_flags);'*'console_journal_end();' ]] || {
    echo "ITEM-590: the PHY record still starts inside the device record" >&2
    exit 1
}
# One composition for both records: the report no longer repeats the field list.
grep -Fq 'appliance_production_network_phy_record_line(&info, phy,' \
    kernel/src/kernel.c || {
    echo "ITEM-590: the diagnostic report composes its own PHY record" >&2
    exit 1
}
if grep -Fq '" bmcr=0x"' kernel/src/kernel.c; then
    echo "ITEM-590: the report still repeats the PHY field list" >&2
    exit 1
fi
grep -Fq 'appliance_production_network_phy_info_from_driver(' "$adapter_source" || {
    echo "ITEM-590: the adapter does not use the shared derivation" >&2
    exit 1
}
if grep -Fq 'info->xid = apn.rtl.chip.xid;' "$adapter_source"; then
    echo "ITEM-590: the adapter still derives the PHY answer twice" >&2
    exit 1
fi

# The proofs.
grep -Fq 'diag export after a FULL journal host test' \
    tests/diag-export-full-host.c || {
    echo "ITEM-590: no host proof of the save after a FULL journal" >&2
    exit 1
}
grep -Fq 'diag save recovers the room through the rewind and saves' \
    tests/diag-export-full-host.c || {
    echo "ITEM-590: the host proof does not require the recovery" >&2
    exit 1
}
grep -Fq 'the two saves carry different session identifiers' \
    tests/diag-export-full-host.c || {
    echo "ITEM-590: the host proof does not require two sessions" >&2
    exit 1
}
grep -Fq 'the refusal names BEGIN_WRITE as the step that failed' \
    tests/diag-export-full-host.c || {
    echo "ITEM-590: the host proof does not name the failed write" >&2
    exit 1
}
grep -Fq 'the refusal names FLUSH as the step that failed' \
    tests/diag-export-full-host.c || {
    echo "ITEM-590: the host proof does not name the failed flush" >&2
    exit 1
}
grep -Fq 'the export refuses a medium that failed' \
    tests/diag-export-full-host.c || {
    echo "ITEM-590: the host proof does not separate FULL from a failed medium" >&2
    exit 1
}
grep -Fq 'hfoc-diag-read.py' tests/diag-export-full-host.c || {
    echo "ITEM-590: the host proof does not use the operator's extractor" >&2
    exit 1
}
grep -Fq 'PHY record journal host test' tests/net-phy-journal-host.c || {
    echo "ITEM-590: no host proof of the PHY record in the journal" >&2
    exit 1
}
# ITEM-592 extended the record with the profile; the pin follows the message,
# which still names the content the proof requires.
grep -Fq 'the journal carries the PHY record with the XID, the access, the ' \
    tests/net-phy-journal-host.c || {
    echo "ITEM-590: the PHY proof does not require the recorded content" >&2
    exit 1
}
grep -Fq 'the journal carries the refusal with the first error and reason' \
    tests/net-phy-journal-host.c || {
    echo "ITEM-590: the PHY proof does not require the refusal evidence" >&2
    exit 1
}
grep -Fq 'the journal carries the skipped record with its reason' \
    tests/net-phy-journal-host.c || {
    echo "ITEM-590: the PHY proof does not require the skipped record" >&2
    exit 1
}
grep -Fq 'the first error survives the later successful poll' \
    tests/net-phy-journal-host.c || {
    echo "ITEM-590: the PHY proof does not keep the first error" >&2
    exit 1
}
grep -Fq 'hfoc-install-log-read.py' tests/net-phy-journal-host.c || {
    echo "ITEM-590: the PHY proof does not read the journal back" >&2
    exit 1
}
grep -Fq 'test-diag-export-full-host' Makefile || {
    echo "ITEM-590: the FULL-journal proof is not a build target" >&2
    exit 1
}
grep -Fq 'test-net-phy-journal-host' Makefile || {
    echo "ITEM-590: the PHY journal proof is not a build target" >&2
    exit 1
}
# The suite entry's own spelling, without the Makefile's `$(MAKE)` prefix (a
# literal dollar in a single-quoted pattern is the info-level finding the CI
# ShellCheck refuses): the target name is unique to that line.
grep -Fq 'MAKE) test-diag-export-full-host' Makefile || {
    echo "ITEM-590: the suite does not run the FULL-journal proof" >&2
    exit 1
}
grep -Fq 'MAKE) test-net-phy-journal-host' Makefile || {
    echo "ITEM-590: the suite does not run the PHY journal proof" >&2
    exit 1
}
grep -Fq '[INSTALL-LOG] state=FULL reason=CAPACITY' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-590: the QEMU proof does not fill the journal first" >&2
    exit 1
}
grep -Fq 'the journal was FULL and two saves produced' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-590: the QEMU proof does not save twice on the recovered journal" >&2
    exit 1
}
grep -Fq 'ITEM-590' docs/ROADMAP.md || {
    echo "ITEM-590: ROADMAP does not record the two fixes" >&2
    exit 1
}
grep -Fq 'ITEM-590' docs/ARCHITECTURE.md || {
    echo "ITEM-590: ARCHITECTURE does not record the record's ownership" >&2
    exit 1
}
