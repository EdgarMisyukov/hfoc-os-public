#!/usr/bin/env bash
# ITEM-550: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself.
item550_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item550_root" || exit 1

# ITEM-550: the command wait says why it failed, and the reason survives the
# boot that failed.
#
# The owner's L875 run reported `reason=ENUMERATION_FAILED
# detail=P1_ENABLE_SLOT_TIMEOUT` and nothing else: the submitted command, its
# place in the command ring, the controller registers at that moment and the
# events the wait consumed were all local to a probe whose session was released
# before the `xhci` command ran.  Two defects print the same detail - a ring
# that never accepted the TRB and a controller that accepted it and never
# completed it - and the run could not say which one the laptop has.
#
# The item keeps a bounded snapshot of the failure point in plain RAM (the
# machine record, never a session), publishes the same three records to the
# durable journal at the failure point, and prints them from the `xhci` command
# afterwards without touching a controller.  It changes no timeout, no
# enumeration step and no interrupt routing.  This slice pins:
#
#   * the vocabulary and the three record shapes, with every field the owner
#     asked for (the command, the controller registers, the event ring, the
#     wait's own counters and the last unmatched completion);
#   * that the enqueue failure and the completion timeout are different
#     outcomes, decided by the pure classifier the host proofs drive;
#   * that nothing is printed or journaled inside the wait loop;
#   * that the record is kept in the machine, copied rather than referenced, so
#     it outlives the session release the failed probe performs;
#   * that the No-Op command is still submitted and checked before any
#     ENABLE_SLOT is sent, which is what makes `ENUMERATION_FAILED` (not
#     `COMMAND_FAILED`) the honest verdict the owner read off the machine;
#   * the host proofs on both halves and the documents that record the item.

# 1. The files the item touches exist.
for item550_file in \
    kernel/include/xhci_trb.h \
    kernel/src/xhci_trb.c \
    kernel/src/xhci.c \
    tests/xhci-host.c \
    tests/xhci-mmio-host.c \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md; do
    [[ -f $item550_file ]] || {
        echo "ITEM-550: missing $item550_file" >&2
        exit 1
    }
done

# 2. The vocabulary and the record shapes are declared where the pure half and
#    the host proof can both use them.
for item550_decl in \
    'enum xhci_command_result {' \
    'XHCI_COMMAND_RESULT_ENQUEUE_FAILED = 1,' \
    'XHCI_COMMAND_RESULT_TIMEOUT = 2' \
    'enum xhci_command_event_kind {' \
    'XHCI_COMMAND_EVENT_FOREIGN = 1,' \
    'struct xhci_command_wait {' \
    'struct xhci_command_snapshot {' \
    'uint32_t raw[4];' \
    'enum xhci_command_event_kind xhci_command_event_kind_of(' \
    'void xhci_command_wait_note(' \
    'enum xhci_command_result xhci_command_result_of(' \
    'void xhci_command_snapshot_keep(' \
    'size_t xhci_format_command_record(' \
    'size_t xhci_format_command_register_record(' \
    'size_t xhci_format_command_event_record('; do
    grep -Fq -- "$item550_decl" kernel/include/xhci_trb.h || {
        echo "ITEM-550: header does not declare $item550_decl" >&2
        exit 1
    }
done
grep -Fq '#define XHCI_COMMAND_RECORD_BYTES 320U' kernel/include/xhci_trb.h || {
    echo "ITEM-550: the command record has no bound" >&2
    exit 1
}

# 3. Every field the owner asked for is in one of the three records.
for item550_field in \
    '"[XHCI-CMD] result="' \
    '" pci="' \
    '" type="' \
    '" index="' \
    '" cycle="' \
    '" phys="' \
    '" trb="' \
    '"[XHCI-CMD-REG] usbcmd="' \
    '" usbsts="' \
    '" crcr="' \
    '" config="' \
    '" dcbaap="' \
    '" erstsz="' \
    '" erstba="' \
    '" erdp="' \
    '" iman="' \
    '"[XHCI-CMD-EVT] index="' \
    '" event="' \
    '" events="' \
    '" unmatched="' \
    '" last="'; do
    grep -Fq -- "$item550_field" kernel/src/xhci_trb.c || {
        echo "ITEM-550: the command records do not carry $item550_field" >&2
        exit 1
    }
done

# 4. The enqueue failure and the completion timeout are different outcomes, and
#    the wait's own event classification feeds the counters.
grep -Fq 'return "ENQUEUE_FAILED";' kernel/src/xhci_trb.c || {
    echo "ITEM-550: an enqueue failure has no name of its own" >&2
    exit 1
}
grep -Fq 'xhci_command_result_of(false, false)' kernel/src/xhci.c || {
    echo "ITEM-550: a refused TRB is not recorded as an enqueue failure" >&2
    exit 1
}
grep -Fq 'xhci_command_result_of(true, false)' kernel/src/xhci.c || {
    echo "ITEM-550: an unanswered command is not recorded as a timeout" >&2
    exit 1
}
grep -Fq 'xhci_command_event_kind_of(&event, physical);' kernel/src/xhci.c || {
    echo "ITEM-550: the wait does not classify the events it reads" >&2
    exit 1
}
grep -Fq 'xhci_command_wait_note(&wait, &event, kind);' kernel/src/xhci.c || {
    echo "ITEM-550: the wait does not count the events it reads" >&2
    exit 1
}
grep -Fq 'if (kind == XHCI_COMMAND_EVENT_OWN) {' kernel/src/xhci.c || {
    echo "ITEM-550: the wait does not recognise its own completion" >&2
    exit 1
}

# 5. Nothing is printed or journaled inside the wait loop: the capture that
#    publishes is a separate function called after the loop gave up.
awk '
    /^static bool xhci_submit_command\(/ { inside = 1 }
    inside && /console_/ { bad = 1 }
    inside && /^}/ { inside = 0 }
    END { exit bad ? 1 : 0 }
' kernel/src/xhci.c || {
    echo "ITEM-550: the wait loop prints or journals while it waits" >&2
    exit 1
}
awk '
    /^static void xhci_command_capture\(/ { inside = 1 }
    inside && /console_publish_journal\(record, length\);/ { published = 1 }
    inside && /^}/ { inside = 0 }
    END { exit published ? 0 : 1 }
' kernel/src/xhci.c || {
    echo "ITEM-550: the failure record never reaches the durable journal" >&2
    exit 1
}
[[ $(grep -Fc 'console_publish_journal(record, length);' kernel/src/xhci.c) -ge 3 ]] || {
    echo "ITEM-550: the three command records are not all published" >&2
    exit 1
}

# 6. The record lives in the machine, not in a session, and it is copied there,
#    so releasing the session cannot take it away.
awk '
    /^struct xhci_machine \{/ { inside = 1 }
    inside && /struct xhci_command_snapshot command;/ { owned = 1 }
    inside && /^\};/ { inside = 0 }
    END { exit owned ? 0 : 1 }
' kernel/src/xhci.c || {
    echo "ITEM-550: the snapshot is not owned by the machine record" >&2
    exit 1
}
grep -Fq 'xhci_command_snapshot_keep(&xhci_machine.command, snapshot);' kernel/src/xhci.c || {
    echo "ITEM-550: the captured snapshot is not kept for the later command" >&2
    exit 1
}
[[ $(grep -Fc '&xhci_machine.command);' kernel/src/xhci.c) -ge 3 ]] || {
    echo "ITEM-550: the xhci command does not print all three kept records" >&2
    exit 1
}
grep -Fq '    xhci_diag_command_lines();' kernel/src/xhci.c || {
    echo "ITEM-550: the xhci command does not print the kept failure" >&2
    exit 1
}
grep -Fq 'if (!xhci_machine.command.valid) {' kernel/src/xhci.c || {
    echo "ITEM-550: the xhci command prints a failure that was never captured" >&2
    exit 1
}

# 7. The No-Op is submitted and checked before any ENABLE_SLOT is sent: a
#    controller that cannot complete its own No-Op reports COMMAND_FAILED, so
#    the `ENUMERATION_FAILED`/`ENABLE_SLOT` verdict the owner read can only come
#    from a machine whose command and event rings already work.
item550_noop=$(grep -n 'xhci_trb_noop_command(' kernel/src/xhci.c | head -1 | cut -d: -f1)
item550_enable=$(grep -n 'xhci_trb_enable_slot(' kernel/src/xhci.c | head -1 | cut -d: -f1)
[[ -n $item550_noop && -n $item550_enable && $item550_noop -lt $item550_enable ]] || {
    echo "ITEM-550: the No-Op no longer precedes the first ENABLE_SLOT" >&2
    exit 1
}
grep -Fq 'if (!xhci_command_ok(&noop, &xhci->detail)) {' kernel/src/xhci.c || {
    echo "ITEM-550: the No-Op result is no longer checked" >&2
    exit 1
}

# 8. The host proofs: the pure four and the real-driver three.
for item550_pure in \
    test_command_wait_success \
    test_command_wait_timeout_without_event \
    test_command_wait_foreign_completion \
    test_command_snapshot_outlives_the_session; do
    grep -Fq "$item550_pure" tests/xhci-host.c || {
        echo "ITEM-550: missing host proof $item550_pure" >&2
        exit 1
    }
done
for item550_driver in \
    test_command_completion_is_taken \
    test_command_timeout_is_captured_before_release \
    test_command_foreign_completion_is_counted; do
    grep -Fq "$item550_driver" tests/xhci-mmio-host.c || {
        echo "ITEM-550: missing driver proof $item550_driver" >&2
        exit 1
    }
done
grep -Fq 'the failure snapshot writes no register and only the doorbell moved' \
    tests/xhci-mmio-host.c || {
    echo "ITEM-550: the snapshot's read-only property is not proven" >&2
    exit 1
}

# 9. The item is recorded where a maintainer looks.
grep -Fq '[XHCI-CMD]' docs/hfoc-appliance-boot-console.md || {
    echo "docs/hfoc-appliance-boot-console.md does not record ITEM-550" >&2
    exit 1
}
grep -Fq 'ITEM-550' docs/hfoc-appliance-boot-console.md || {
    echo "docs/hfoc-appliance-boot-console.md does not name ITEM-550" >&2
    exit 1
}
grep -Fq 'ITEM-550' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md does not record ITEM-550" >&2
    exit 1
}
