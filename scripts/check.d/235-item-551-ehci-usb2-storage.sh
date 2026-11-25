#!/usr/bin/env bash
# ITEM-551: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself.
item551_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item551_root" || exit 1

# ITEM-551: the USB 2.0 half the diagnostic medium is actually on.
#
# The owner's L875 boots the stick through the firmware and then cannot reach it
# from the kernel: the machine's only USB 2.0 port belongs to the Intel 7 Series
# EHCI function (`Bus 001: ehci-pci/2p 480M` in the Linux capture of the same
# machine), and the kernel carried no EHCI driver at all.  The xHCI function on
# that machine is the one failing at ENABLE_SLOT, so the one medium the machine
# can be given was the one medium the kernel could not read or write.
#
# This slice pins the minimal path the item adds:
#
#   * the controller is found by its EHCI programming interface and its BAR0
#     alone, with interrupts left disabled and every wait a bounded poll;
#   * the port is reset through the writable subset of PORTSC, and an enabled
#     port is accepted even when the controller model does not fill the speed
#     field (QEMU's EHCI does not), while a low-speed or companion-owned port is
#     refused by name;
#   * the transfer descriptor's PID, toggle, byte count and Active bit, the
#     queue head's characteristics and the circular asynchronous list are the
#     pure half's encoding, proven on the host;
#   * the storage device goes through the existing `usb_storage` core and block
#     device, and the boot-medium guard plus the reserved FAT12 journal are the
#     existing seams, made idempotent so a second controller cannot write a
#     second journal header;
#   * the records name the controller ([EHCI], [EHCI-PORT], [EHCI-STORAGE],
#     [INSTALL-MEDIUM] controller=ehci) and the xHCI no-device record names
#     itself, so the two are never mistaken for one verdict;
#   * the host and QEMU proofs exist, are registered, and the documents record
#     the item.

# 1. The files the item touches exist.
for item551_file in \
    kernel/include/ehci.h \
    kernel/src/ehci.c \
    kernel/src/ehci_queue.c \
    kernel/src/install_journal_platform.c \
    tests/ehci-host.c \
    scripts/test-qemu-ehci-storage.sh \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item551_file ]] || {
        echo "ITEM-551: missing $item551_file" >&2
        exit 1
    }
done

# 2. The controller is EHCI, BAR0 is the only required BAR, and the operational
#    offsets are relative to CAPLENGTH (the defect that made the first run write
#    USBINTR where USBCMD lives).
grep -Fq '#define EHCI_PCI_PROGIF_EHCI 0x20U' kernel/include/ehci.h || {
    echo "ITEM-551: the PCI programming interface is not EHCI's" >&2
    exit 1
}
grep -Fq 'PCI_BAR_REQUIRED(0U), headers,' kernel/src/ehci.c || {
    echo "ITEM-551: the scan does not ask for BAR0 alone" >&2
    exit 1
}
grep -Fq '#define EHCI_OP_USBCMD 0x00U' kernel/include/ehci.h || {
    echo "ITEM-551: USBCMD is not the first operational register" >&2
    exit 1
}
grep -Fq '#define EHCI_OP_PORTSC 0x44U' kernel/include/ehci.h || {
    echo "ITEM-551: PORTSC is not the operational offset EHCI defines" >&2
    exit 1
}

# 3. Interrupts are never enabled: USBINTR is written zero, and no interrupt
#    controller is reached from this driver.
grep -Fq 'ehci_init_write(ops, result, EHCI_OP_USBINTR, 0U);' \
    kernel/src/ehci_init.c || {
    echo "ITEM-551: the controller's interrupt mask is not cleared" >&2
    exit 1
}
if grep -Eq '\b(ioapic|lapic|pic_unmask|irq_route|pic_unmask)\b' \
    kernel/src/ehci.c; then
    echo "ITEM-551: the EHCI driver reaches the interrupt stack" >&2
    exit 1
fi

# 4. The port write rule: only the writable subset of PORTSC is ever written,
#    and an enabled port is not refused because a controller model leaves the
#    speed field at zero.
# ITEM-567: the sequence itself moved to kernel/src/ehci_port.c, which the
# host proof drives through the production MMIO accessor; the pin follows it.
grep -Fq 'state->portsc_set = ehci_portsc_write_value(portsc, EHCI_PORTSC_PR);' \
    kernel/src/ehci_port.c || {
    echo "ITEM-551: the reset request does not go through the PORTSC rule" >&2
    exit 1
}
grep -Fq '#define EHCI_PORTSC_WRITABLE' kernel/include/ehci.h || {
    echo "ITEM-551: the writable PORTSC subset is not declared" >&2
    exit 1
}
grep -Fq ': EHCI_PORT_RESET_NOT_ENABLED;' kernel/src/ehci_port.c || {
    echo "ITEM-551: a port the controller left disabled has no verdict" >&2
    exit 1
}
grep -Fq 'case EHCI_PORT_RESET_NOT_ENABLED:' kernel/src/ehci.c || {
    echo "ITEM-551: an unenabled port is not refused by name" >&2
    exit 1
}
grep -Fq 'EHCI_STATUS_UNSUPPORTED_SPEED, "LOW"' kernel/src/ehci.c || {
    echo "ITEM-551: a low-speed port is not refused by name" >&2
    exit 1
}

# 5. The descriptors and the circular asynchronous list are the driver's own
#    encoding, and the queue head takes the toggle from the descriptor.
# ITEM-560: the queue head, link and descriptor encoders moved into the pure
# ring builders, which the driver calls; the proof checks them field by field.
for item551_encoder in \
    'ehci_qtd_token(' \
    'ehci_qh_endpoint1(' \
    'ehci_link(list_head_physical, false)' \
    'ehci_qh_endpoint2(1U, 0U, 0U)'; do
    grep -Fq -- "$item551_encoder" kernel/src/ehci_queue.c || {
        echo "ITEM-551: the pure half does not use $item551_encoder" >&2
        exit 1
    }
done
grep -Fq 'ehci_ring_transfer_head(' kernel/src/ehci.c || {
    echo "ITEM-551: the driver does not build its transfer head" >&2
    exit 1
}
# ITEM-559: the doorbell itself lives in the schedule engine, which is the code
# the host proof drives; the driver keeps the adapter that rings it.
grep -Fq 'EHCI_USBCMD_IAAD' kernel/src/ehci_engine.c || {
    echo "ITEM-551: the engine never rings the async-advance doorbell" >&2
    exit 1
}
grep -Fq 'uint32_t ehci_qtd_token(' kernel/src/ehci_queue.c || {
    echo "ITEM-551: the token encoder is not in the pure half" >&2
    exit 1
}
grep -Fq 'bool ehci_plan_transfer(' kernel/src/ehci_queue.c || {
    echo "ITEM-551: the transfer planner is not in the pure half" >&2
    exit 1
}
grep -Fq 'uint32_t ehci_portsc_write_value(' kernel/src/ehci_queue.c || {
    echo "ITEM-551: the PORTSC write value is not in the pure half" >&2
    exit 1
}

# 6. The storage and medium seams are the existing ones, and the journal bind
#    cannot write a second header.
grep -Fq 'usb_storage_bind(&device->usb, &transport, &config)' kernel/src/ehci.c || {
    echo "ITEM-551: the device does not go through the shared BOT core" >&2
    exit 1
}
grep -Fq 'usb_storage_block_device_bind(&device->usb, &device->block)' \
    kernel/src/ehci.c || {
    echo "ITEM-551: the device has no block_device view" >&2
    exit 1
}
grep -Fq 'install_medium_identify(&boot, sector,' kernel/src/ehci.c || {
    echo "ITEM-551: the boot-medium guard is not used" >&2
    exit 1
}
grep -Fq 'install_journal_platform_bind_medium(&device->block.device,' \
    kernel/src/ehci.c || {
    echo "ITEM-551: the reserved journal is not bound" >&2
    exit 1
}
grep -Fq 'if (platform_journal.bound) {' kernel/src/install_journal_platform.c || {
    echo "ITEM-551: a second controller can write a second journal header" >&2
    exit 1
}

# 7. The records name their controller.
for item551_record in \
    '"[EHCI] pci="' \
    '"[EHCI-PORT] pci="' \
    '"[EHCI-STORAGE] pci="' \
    '"[INSTALL-MEDIUM] controller=ehci pci="'; do
    grep -Fq -- "$item551_record" kernel/src/ehci.c || {
        echo "ITEM-551: the record is missing $item551_record" >&2
        exit 1
    }
done
grep -Fq '"[INSTALL-MEDIUM] controller=xhci state=UNAVAILABLE "' kernel/src/xhci.c || {
    echo "ITEM-551: the xHCI no-device record does not name itself" >&2
    exit 1
}

# 8. The proofs exist, are registered, and drive the real code.
grep -Fq '.PHONY: test-ehci-host' Makefile || {
    echo "ITEM-551: the host proof target is missing" >&2
    exit 1
}
grep -Fq 'kernel/src/ehci_queue.c tests/ehci-host.c' Makefile || {
    echo "ITEM-551: the host proof does not build the pure half" >&2
    exit 1
}
grep -Fq '$(MAKE) test-ehci-host' Makefile || {
    echo "ITEM-551: the host proof is not in the canonical host suite" >&2
    exit 1
}
grep -Fq '.PHONY: test-ehci-storage-qemu' Makefile || {
    echo "ITEM-551: the QEMU proof target is missing" >&2
    exit 1
}
grep -Fq 'test-ehci-storage-qemu: production-installer-console-media' Makefile || {
    echo "ITEM-551: the QEMU proof does not build its medium" >&2
    exit 1
}
grep -Fq 'test-ehci-storage-qemu' Makefile || {
    echo "ITEM-551: the QEMU proof is not registered" >&2
    exit 1
}
grep -Fq '"$ROOT/scripts/hfoc-serial-view.py"' scripts/test-qemu-ehci-storage.sh || {
    echo "ITEM-551: the QEMU proof reads the raw serial log" >&2
    exit 1
}
grep -Fq -- '-device usb-ehci,id=ehci' scripts/test-qemu-ehci-storage.sh || {
    echo "ITEM-551: the QEMU proof does not boot from an EHCI device" >&2
    exit 1
}
grep -Fq 'hfoc-install-log-read.py --disk "$STICK"' \
    scripts/test-qemu-ehci-storage.sh || {
    echo "ITEM-551: the QEMU proof does not read the journal back" >&2
    exit 1
}
for item551_test in \
    test_qtd_token \
    test_qh_characteristics \
    test_links \
    test_port_decoding \
    test_transfer_plan; do
    grep -Fq "$item551_test" tests/ehci-host.c || {
        echo "ITEM-551: missing host proof $item551_test" >&2
        exit 1
    }
done

# 8a. ITEM-553: the integrated rate matching hub between the EHCI controller and
#     the machine's USB 2.0 ports, and the topology regression that pins it.
for item551_hub in \
    'EHCI_USB_CLASS_HUB 0x09U' \
    'ehci_hub_port_count(const uint8_t *descriptor' \
    'ehci_hub_port_verdict(uint16_t status)' \
    'ehci_hub_port_verdict_string('; do
    grep -Fq -- "$item551_hub" kernel/include/ehci.h kernel/src/ehci_queue.c || {
        echo "ITEM-553: the hub protocol lacks $item551_hub" >&2
        exit 1
    }
done
for item551_hub_code in \
    'ehci_hub_bring_up(controller, hub_address, port, power_good_ms,' \
    'ehci_hub_port_verdict(status)' \
    'controller->device.hub_address = hub_address;'; do
    grep -Fq -- "$item551_hub_code" kernel/src/ehci.c || {
        echo "ITEM-553: the driver does not use $item551_hub_code" >&2
        exit 1
    }
done
grep -Fq 'walk_hub(&state, address, max_packet, report)' kernel/src/ehci_walk.c || {
    echo "ITEM-553: the hub walk is not in the shared walk" >&2
    exit 1
}
grep -Fq 'test_hub_protocol' tests/ehci-host.c || {
    echo "ITEM-553: the hub protocol regression is missing" >&2
    exit 1
}
grep -Fq 'test_hub_topology' tests/ehci-host.c || {
    echo "ITEM-553: the hub topology regression is missing" >&2
    exit 1
}
grep -Fq 'Rate Matching Hub' kernel/src/ehci.c || {
    echo "ITEM-553: the driver does not name the rate matching hub" >&2
    exit 1
}

# 8b. ITEM-554: the queue head words the controller accepts, the hub's class
#     request, and the failure the record now keeps.
# ITEM-560: the transfer head's words (C = 0, DTC = 1, Mult = 1) are built by
# the pure ring code now; the host proof pins each field.
for item551_fix in \
    'ehci_qh_endpoint2(1U, 0U, 0U)' \
    'true,' \
    'EHCI_QH_SPEED_HIGH, endpoint, address'; do
    grep -Fq -- "$item551_fix" kernel/src/ehci_queue.c || {
        echo "ITEM-554: the ring builders lack $item551_fix" >&2
        exit 1
    }
done
grep -Fq 'ehci_hub_get_descriptor_fields(&request_type, &request, &value);' \
    kernel/src/ehci_walk.c || {
    echo "ITEM-554: the shared walk does not use the hub class request" >&2
    exit 1
}
grep -Fq 'ehci_classify_qtds(tokens, qtd_count,' kernel/src/ehci_engine.c || {
    echo "ITEM-554: the driver does not classify every descriptor" >&2
    exit 1
}
grep -Fq 'ehci_capture_failure(controller, &outcome, address, endpoint,' \
    kernel/src/ehci.c || {
    echo "ITEM-554: the driver does not keep the failing transfer" >&2
    exit 1
}
grep -Fq 'ehci_classify_qtds(const uint32_t *tokens' kernel/src/ehci_queue.c || {
    echo "ITEM-554: the per-descriptor classifier is not in the pure half" >&2
    exit 1
}
grep -Fq 'EHCI_HUB_REQUEST_TYPE_DEVICE_IN 0xA0U' kernel/include/ehci.h || {
    echo "ITEM-554: GetHubDescriptor is not the class request 0xA0" >&2
    exit 1
}
grep -Fq 'test_control_seams' tests/ehci-host.c || {
    echo "ITEM-554: the control-path regression is missing" >&2
    exit 1
}

# 8c. ITEM-556: the walk is production code over a substituted transport, and a
#     host proof executes it on the machine's shape (hub, foreign device, stick).
for item556_file in \
    kernel/include/ehci_walk.h \
    kernel/src/ehci_walk.c \
    tests/ehci-walk-host.c; do
    [[ -f $item556_file ]] || {
        echo "ITEM-556: $item556_file is missing" >&2
        exit 1
    }
done
grep -Fq 'struct ehci_walk_ops {' kernel/include/ehci_walk.h || {
    echo "ITEM-556: the walk has no substitutable transport" >&2
    exit 1
}
grep -Fq 'bool ehci_walk_run(const struct ehci_walk_ops *ops,' \
    kernel/include/ehci_walk.h || {
    echo "ITEM-556: the walk entry point is not declared" >&2
    exit 1
}
grep -Fq 'ehci_walk_run(&ops, controller->ports, &report)' kernel/src/ehci.c || {
    echo "ITEM-556: the driver does not run the shared walk" >&2
    exit 1
}
grep -Fq 'uint16_t hub_max_packet,' kernel/include/ehci_walk.h || {
    echo "ITEM-556: the hub transport does not carry its control-pipe size" >&2
    exit 1
}
grep -Fq 'controller->device.ep0_max_packet = hub_max_packet;' kernel/src/ehci.c || {
    echo "ITEM-556: the hub requests do not use the hub's EP0 size" >&2
    exit 1
}
grep -Fq 'test_walk_continues_past_foreign_storage' tests/ehci-walk-host.c || {
    echo "ITEM-556: the walk regression is missing" >&2
    exit 1
}
grep -Fq 'report.attached == 2U && report.probes == 2U' tests/ehci-walk-host.c || {
    echo "ITEM-556: the regression does not pin the two-attachment walk" >&2
    exit 1
}
# ITEM-562: a walk that found no device names which absence it was; the old
# single fallback (NO_DEVICE for every walk that attached nothing) is what let
# the driver report a reset refusal as a descriptor failure.
grep -Fq '"NO_CONNECTED_PORT"' kernel/src/ehci_walk.c || {
    echo "ITEM-556: a walk that attaches nothing does not name that state" >&2
    exit 1
}
grep -Fq '"NO_STORAGE_DEVICE"' kernel/src/ehci_walk.c || {
    echo "ITEM-562: a walk that carried non-storage devices does not name that" >&2
    exit 1
}
grep -Fq '.PHONY: test-ehci-walk-host' Makefile || {
    echo "ITEM-556: the walk proof target is missing" >&2
    exit 1
}
grep -Fq '$(MAKE) test-ehci-walk-host' Makefile || {
    echo "ITEM-556: the walk proof is not in the canonical host suite" >&2
    exit 1
}
# The BIOS -> OS handoff before the reset, with a named verdict.  ITEM-568 moved
# the sequence into its own module and into PCI configuration space, where the
# specification keeps the capability; the pins follow it.
grep -Fq 'ehci_legacy_handoff(controller, io);' kernel/src/ehci.c || {
    echo "ITEM-556: the BIOS -> OS handoff is not run" >&2
    exit 1
}
grep -Fq '#define EHCI_LEGACY_BIOS_OWNED 0x00010000U' \
    kernel/include/ehci_legacy.h || {
    echo "ITEM-556: the legacy capability bits are not declared" >&2
    exit 1
}
grep -Fq 'enum ehci_legacy_result ehci_legacy_handoff_run(' \
    kernel/src/ehci_legacy.c || {
    echo "ITEM-556: the handoff decision is not in the pure half" >&2
    exit 1
}
grep -Fq 'test_the_walk_uses_configuration_space_not_mmio' \
    tests/ehci-legacy-host.c || {
    echo "ITEM-556: the handoff regression is missing" >&2
    exit 1
}
item556_handoff=$(grep -n 'ehci_legacy_handoff(controller, io);' kernel/src/ehci.c | head -1 | cut -d: -f1)
item556_reset=$(grep -n 'ehci_init_run(&init_ops,' kernel/src/ehci.c | head -1 | cut -d: -f1)
[[ -n $item556_handoff && -n $item556_reset && $item556_handoff -lt $item556_reset ]] || {
    echo "ITEM-556: the handoff does not precede the controller reset" >&2
    exit 1
}
# The register state after the reset and the interrupt mask are in the record.
for item556_field in ' legacy=' ' usbintr=0x' ' usbcmd=0x' ' usbsts=0x'; do
    grep -Fq -- "$item556_field" kernel/src/ehci.c || {
        echo "ITEM-556: the record lacks $item556_field" >&2
        exit 1
    }
done
# Interrupts are masked before the controller is reset, and the registers are
# read back after the reset, so a healed controller is proven, not assumed.
# ITEM-561: the mask, the stop and the reset are steps of the initialization
# module now, so the order is checked inside that module and the driver is
# checked for running it.
item556_usbintr=$(grep -n 'ehci_init_write(ops, result, EHCI_OP_USBINTR, 0U);' kernel/src/ehci_init.c | head -1 | cut -d: -f1)
item556_reset_done=$(grep -n 'EHCI_USBCMD_HCRESET,' kernel/src/ehci_init.c | head -1 | cut -d: -f1)
item556_recovery=$(grep -n 'result->usbcmd_after_reset =' kernel/src/ehci_init.c | head -1 | cut -d: -f1)
[[ -n $item556_usbintr && -n $item556_reset && $item556_usbintr -lt $item556_reset ]] || {
    echo "ITEM-556: interrupts are not masked before the controller reset" >&2
    exit 1
}
[[ -n $item556_recovery && -n $item556_reset_done && $item556_reset_done -lt $item556_recovery ]] || {
    echo "ITEM-556: the register recovery read does not follow the reset" >&2
    exit 1
}
# A transfer leaves the DMA pool quiet before the next one rewrites it: the head
# is unlinked, the async-advance doorbell tells the controller it is gone, and
# the acknowledgment is awaited before the caller can reuse the queue head.
# The exported record must carry them too, not only the serial line: the file
# the owner reads back on Debian is the proof, and it is built by the sink.
for item556_sink in \
    'ehci_text_write(&text, " legacy=")' \
    'ehci_text_write(&text, " usbintr=")' \
    'ehci_text_write(&text, " usbcmd=")' \
    'ehci_text_write(&text, " usbsts=")'; do
    grep -Fq -- "$item556_sink" kernel/src/ehci.c || {
        echo "ITEM-556: the exported EHCI record lacks $item556_sink" >&2
        exit 1
    }
done
item556_unlink=$(grep -n 'ops->link_transfer_head(ops->context, false);' kernel/src/ehci_engine.c | head -1 | cut -d: -f1)
item556_iaad=$(grep -n 'EHCI_USBCMD_IAAD' kernel/src/ehci_engine.c | head -1 | cut -d: -f1)
# ITEM-564: the acknowledgement is the stop's own verdict, and it can only be
# reached after the doorbell; the stale-IAA clear also reads this bit, which is
# why the ordering is checked against the acknowledgement itself.
item556_iaa=$(grep -n 'result->cleanup = EHCI_ENGINE_CLEANUP_ACKNOWLEDGED;' kernel/src/ehci_engine.c | head -1 | cut -d: -f1)
[[ -n $item556_unlink && -n $item556_iaad && $item556_unlink -lt $item556_iaad ]] || {
    echo "ITEM-556: the transfer head is not unlinked before the async advance doorbell" >&2
    exit 1
}
[[ -n $item556_iaa && $item556_iaad -lt $item556_iaa ]] || {
    echo "ITEM-556: the controller's departure from the DMA pool is not awaited" >&2
    exit 1
}

# 8d. ITEM-557: the walk waits what USB 2.0 says it owes, and the engine does not
#     rewrite its fixed DMA pool until the controller has confirmed it left it.
for item557_wait in \
    'EHCI_WALK_RESET_RECOVERY_US' \
    'EHCI_WALK_ADDRESS_RECOVERY_US'; do
    grep -Fq -- "$item557_wait" kernel/include/ehci_walk.h || {
        echo "ITEM-557: the walk does not declare $item557_wait" >&2
        exit 1
    }
done
grep -Fq 'void (*delay_us)(void *context, uint32_t microseconds);' \
    kernel/include/ehci_walk.h || {
    echo "ITEM-557: the walk transport has no delay operation" >&2
    exit 1
}
grep -Fq 'ops->delay_us == NULL' kernel/src/ehci_walk.c || {
    echo "ITEM-557: a transport without the recovery waits is not refused" >&2
    exit 1
}
for item557_call in \
    'ops->delay_us(ops->context, EHCI_WALK_RESET_RECOVERY_US);' \
    'state->ops->delay_us(state->ops->context, EHCI_WALK_ADDRESS_RECOVERY_US);'; do
    grep -Fq -- "$item557_call" kernel/src/ehci_walk.c || {
        echo "ITEM-557: the walk does not wait $item557_call" >&2
        exit 1
    }
done
grep -Fq 'ops.delay_us = ehci_walk_delay;' kernel/src/ehci.c || {
    echo "ITEM-557: the driver does not give the walk its clock" >&2
    exit 1
}
grep -Fq 'ehci_wait_us(microseconds);' kernel/src/ehci.c || {
    echo "ITEM-557: the driver's delay adapter does not wait" >&2
    exit 1
}
grep -Fq 'result->stalled = true;' kernel/src/ehci_engine.c || {
    echo "ITEM-557: an unacknowledged async advance does not stop the engine" >&2
    exit 1
}
# ITEM-564: the unacknowledged advance is the stop's verdict, not the
# transfer's cause; the old assignment replaced the cause and is gone.
grep -Fq 'result->cleanup = EHCI_ENGINE_CLEANUP_IGNORED;' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-557: the unacknowledged async advance is not named" >&2
    exit 1
}
if grep -Fq 'result->cause = EHCI_ENGINE_CAUSE_ASYNC_ADVANCE;' \
    kernel/src/ehci_engine.c; then
    echo "ITEM-564: the stop's failure still overwrites the transfer's cause" >&2
    exit 1
fi
grep -Fq 'controller->engine_stalled = outcome.stalled;' kernel/src/ehci.c || {
    echo "ITEM-557: the driver does not keep the engine's stalled verdict" >&2
    exit 1
}
grep -Fq 'controller->engine_stalled,' kernel/src/ehci.c || {
    echo "ITEM-557: the engine keeps using the pool after the controller left unconfirmed" >&2
    exit 1
}
grep -Fq 'test_walk_refuses_a_transport_without_recovery' tests/ehci-walk-host.c || {
    echo "ITEM-557: the refusal of a transport without the waits is not proven" >&2
    exit 1
}
grep -Fq 'EHCI_WALK_RESET_RECOVERY_US' tests/ehci-walk-host.c || {
    echo "ITEM-557: the walk proof does not assert the recovery intervals" >&2
    exit 1
}
grep -Fq 'every reset and SET_ADDRESS is followed by its wait' \
    tests/ehci-walk-host.c || {
    echo "ITEM-557: the walk proof does not assert the wait ordering" >&2
    exit 1
}

# 8e. ITEM-558: the stage is not the cause.  The walk records the transport's
#     text for the failing transfer beside the stage, the controller renders the
#     snapshot (PCI, root port, hub path, address, result, received, qTD tokens,
#     registers), and both the operator's detail and the report carry it.
grep -Fq 'const char *failure_detail;' kernel/include/ehci_walk.h || {
    echo "ITEM-558: the walk report has no primary cause" >&2
    exit 1
}
grep -Fq 'const char *(*last_failure)(void *context);' kernel/include/ehci_walk.h || {
    echo "ITEM-558: the walk transport cannot name the failing transfer" >&2
    exit 1
}
grep -Fq 'static void walk_fail(struct ehci_walk_state *state,' \
    kernel/src/ehci_walk.c || {
    echo "ITEM-558: the walk does not record the stage and the cause together" >&2
    exit 1
}
grep -Fq 'record->detail = walk_transport_text(state);' kernel/src/ehci_walk.c || {
    echo "ITEM-558: the walk's port record does not keep the transport's cause" >&2
    exit 1
}
grep -Fq 'report->failure_detail = hard->detail;' kernel/src/ehci_walk.c || {
    echo "ITEM-558: the walk drops the transport's cause" >&2
    exit 1
}
grep -Fq 'walk_fail(state, EHCI_WALK_OUTCOME_TRANSFER, "DEVICE8");' \
    kernel/src/ehci_walk.c || {
    echo "ITEM-558: the first descriptor refusal does not name its stage" >&2
    exit 1
}
grep -Fq 'report->failure_detail = medium->detail;' kernel/src/ehci_walk.c || {
    echo "ITEM-558: the report does not publish the primary cause" >&2
    exit 1
}
grep -Fq 'const char *ehci_failure_text(void)' kernel/include/ehci.h || {
    echo "ITEM-558: the controller snapshot has no accessor" >&2
    exit 1
}
grep -Fq 'const char *ehci_failure_result(void)' kernel/include/ehci.h || {
    echo "ITEM-558: the primary result has no accessor" >&2
    exit 1
}
for item558_field in 'pci=' ' port=' ' result=' ' index=' ' qtd=' ' addr=' \
    ' ep=' ' hub=' ' received=' ' residual=' ' usbcmd=' ' usbsts=' ' portsc='; do
    grep -Fq -- "\"$item558_field\"" kernel/src/ehci.c || {
        echo "ITEM-558: the rendered snapshot lacks $item558_field" >&2
        exit 1
    }
done
grep -Fq 'ehci_render_failure(controller, is_control, segments, token_count);' \
    kernel/src/ehci.c || {
        echo "ITEM-558: the failure is captured without its snapshot" >&2
        exit 1
    }
# ITEM-564: the report carries the failure as four short records, so a screen
# photo shows every value whole.
grep -Fq '"[EHCI-FAIL] result="' kernel/src/ehci.c || {
    echo "ITEM-558: the report does not carry the controller failure" >&2
    exit 1
}
grep -Fq 'ops.last_failure = ehci_walk_last_failure;' kernel/src/ehci.c || {
    echo "ITEM-558: the driver does not hand the walk its failure text" >&2
    exit 1
}
grep -Fq 'test_walk_keeps_the_primary_cause_of_device8' tests/ehci-walk-host.c || {
    echo "ITEM-558: the DEVICE8 primary-cause proof is missing" >&2
    exit 1
}
grep -Fq 'result=XACTERR' tests/ehci-walk-host.c || {
    echo "ITEM-558: the DEVICE8 proof does not name a transfer result" >&2
    exit 1
}

# 8f. ITEM-559: the controller's own verdict is named beside the descriptor
#     classifier's, the wait stops on a halted controller, the snapshot is taken
#     before the unlink, and the stop's failure stops the engine - executed by a
#     host proof over substituted MMIO and clock.
for item559_file in kernel/include/ehci_engine.h kernel/src/ehci_engine.c \
    tests/ehci-engine-host.c; do
    [[ -f $item559_file ]] || {
        echo "ITEM-559: $item559_file is missing" >&2
        exit 1
    }
done
for item559_cause in 'EHCI_ENGINE_CAUSE_WAIT_EXPIRED' \
    'EHCI_ENGINE_CAUSE_HOST_SYSTEM_ERROR' 'EHCI_ENGINE_CAUSE_HALTED' \
    'EHCI_ENGINE_CAUSE_ASYNC_STOPPED' 'EHCI_ENGINE_CAUSE_ASYNC_ADVANCE' \
    'EHCI_ENGINE_CAUSE_ENGINE_STALLED'; do
    grep -Fq -- "$item559_cause" kernel/include/ehci_engine.h || {
        echo "ITEM-559: the engine does not declare $item559_cause" >&2
        exit 1
    }
done
grep -Fq 'ehci_engine_state_cause(usbcmd, usbsts)' kernel/src/ehci_engine.c || {
    echo "ITEM-559: the wait does not watch the controller's state" >&2
    exit 1
}
grep -Fq 'ehci_engine_snapshot(ops, result);' kernel/src/ehci_engine.c || {
    echo "ITEM-559: the failure snapshot is not taken by the engine" >&2
    exit 1
}
grep -Fq 'ops->link_transfer_head(ops->context, true);' kernel/src/ehci_engine.c || {
    echo "ITEM-559: the engine does not link the transfer head itself" >&2
    exit 1
}
grep -Fq 'ehci_engine_run(&engine, segment_count, budget_us,' kernel/src/ehci.c || {
    echo "ITEM-559: the driver does not run the engine" >&2
    exit 1
}
grep -Fq 'outcome.cause' kernel/src/ehci.c || {
    echo "ITEM-559: the driver drops the engine's cause" >&2
    exit 1
}
grep -Fq 'const char *ehci_failure_primary(void)' kernel/include/ehci.h || {
    echo "ITEM-559: the operator's line has no controller-verdict accessor" >&2
    exit 1
}
grep -Fq 'ehci_failure_primary()' kernel/src/diag_export.c || {
    echo "ITEM-559: the operator's line does not carry the controller's verdict" >&2
    exit 1
}
grep -Fq 'test_hse_halts_the_wait' tests/ehci-engine-host.c || {
    echo "ITEM-559: the HSE halt proof is missing" >&2
    exit 1
}
grep -Fq 'the words at the first refusal show ASE set and ASS clear' \
    tests/ehci-engine-host.c || {
    echo "ITEM-559: the snapshot's position is not proven" >&2
    exit 1
}
grep -Fq 'test_a_refused_doorbell_without_a_halt_keeps_the_pool' \
    tests/ehci-engine-host.c || {
    echo "ITEM-559: the stop's own failure is not proven" >&2
    exit 1
}
grep -Fq '$(MAKE) test-ehci-engine-host' Makefile || {
    echo "ITEM-559: the engine proof is not in the canonical host suite" >&2
    exit 1
}

# 8g. ITEM-560: the controller's starting state, the PCI words that show whether
#     it was a bus master, the ring the production code builds, and the first
#     moment HSE appeared - all checked on the code the machine runs.
for item560_file in kernel/include/ehci.h kernel/src/ehci_queue.c \
    tests/ehci-ring-host.c; do
    [[ -f $item560_file ]] || {
        echo "ITEM-560: $item560_file is missing" >&2
        exit 1
    }
done
grep -Fq 'uint32_t ehci_ring_usbcmd_start(uint32_t after_reset)' \
    kernel/src/ehci_queue.c || {
    echo "ITEM-560: the first start word has no pure builder" >&2
    exit 1
}
grep -Fq 'ehci_ring_usbcmd_start(ehci_init_read(ops, result, EHCI_OP_USBCMD))' \
    kernel/src/ehci_init.c || {
    echo "ITEM-560: the start is not built from the explicit word" >&2
    exit 1
}
grep -Fq 'result->cause = EHCI_INIT_PERIODIC_ON;' kernel/src/ehci_init.c || {
    echo "ITEM-560: a running periodic schedule is not refused" >&2
    exit 1
}
# ITEM-568: the base is programmed, not zeroed.  A zero base plus a PSE write
# from outside the driver is a frame-list fetch at physical address 0, which
# Table 4-19 makes a fatal Host System Error.
grep -Fq 'ehci_init_write(ops, result, EHCI_OP_PERIODICLISTBASE, periodiclistbase);' \
    kernel/src/ehci_init.c || {
    echo "ITEM-560: the periodic list base is not programmed before the start" >&2
    exit 1
}
grep -Fq 'EHCI_FRAME_LIST_TERMINATE' kernel/src/ehci.c || {
    echo "ITEM-568: the periodic frame list is not built" >&2
    exit 1
}
grep -Fq 'ehci_ring_list_head(controller->list_head,' kernel/src/ehci.c || {
    echo "ITEM-560: the list head is not built by the pure ring code" >&2
    exit 1
}
grep -Fq 'ehci_ring_transfer_head(' kernel/src/ehci.c || {
    echo "ITEM-560: the transfer head is not built by the pure ring code" >&2
    exit 1
}
# ITEM-569: the descriptors are addressed through the slot stride, not by
# indexing the C array (a 52-byte structure at a 64-byte stride).
grep -Fq 'ehci_ring_qtd(ehci_qtd_slot(controller, index), next, token,' \
    kernel/src/ehci.c || {
    echo "ITEM-560: the descriptors are not built by the pure ring code" >&2
    exit 1
}
grep -Fq 'controller->pci.status_before = config_dword >> 16U;' \
    kernel/src/ehci.c || {
    echo "ITEM-560: the PCI status is not saved before it is cleared" >&2
    exit 1
}
grep -Fq 'controller->pci.read_back_ok =' kernel/src/ehci.c || {
    echo "ITEM-560: the PCI command is not read back" >&2
    exit 1
}
grep -Fq 'ehci_sample_hse(controller, EHCI_HSE_STAGE_AFTER_SCHEDULE);' \
    kernel/src/ehci.c || {
    echo "ITEM-560: the first HSE is not sampled at the schedule start" >&2
    exit 1
}
grep -Fq 'ehci_sample_hse(controller, EHCI_HSE_STAGE_BEFORE_LINK);' \
    kernel/src/ehci.c || {
    echo "ITEM-560: the first HSE is not sampled before the transfer head is linked" >&2
    exit 1
}
grep -Fq 'bool acpi_dmar_present(void)' kernel/include/acpi.h || {
    echo "ITEM-560: the DMA Remapping declaration is not read" >&2
    exit 1
}
grep -Fq 'acpi_dmar_present()' kernel/src/ehci.c || {
    echo "ITEM-560: the record does not say whether the machine declares DMAR" >&2
    exit 1
}
grep -Fq 'ehci_ring_reachable(controller->dma_base, controller->dma_bytes,' \
    kernel/src/ehci.c || {
    echo "ITEM-560: the pool reachability is not checked before the start" >&2
    exit 1
}
for item560_check in 'the list head carries Mult = 1' \
    'a buffer crossing a page gets the next page' \
    'the start word clears the periodic schedule'; do
    grep -Fq -- "$item560_check" tests/ehci-ring-host.c || {
        echo "ITEM-560: the ring proof lacks $item560_check" >&2
        exit 1
    }
done
grep -Fq 'the words before the link are kept' \
    tests/ehci-engine-host.c || {
    echo "ITEM-560: the engine proof does not pin the snapshot's position" >&2
    exit 1
}
grep -Fq 'static const char prefix[] = "[EHCI-START] ";' kernel/src/ehci.c || {
    echo "ITEM-560: the start evidence is not a record of its own" >&2
    exit 1
}
grep -Fq 'pcists_clr=' kernel/src/ehci.c || {
    echo "ITEM-560: the record does not show the status after the deliberate clear" >&2
    exit 1
}
grep -Fq '$(MAKE) test-ehci-ring-host' Makefile || {
    echo "ITEM-560: the ring proof is not in the canonical host suite" >&2
    exit 1
}

# 9. The documents record the item.
for item551_doc in 'ITEM-551' '[EHCI-STORAGE]' 'INSTALL.LOG'; do
    grep -Fq -- "$item551_doc" docs/ARCHITECTURE.md || {
        echo "docs/ARCHITECTURE.md does not record $item551_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-551' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md does not record ITEM-551" >&2
    exit 1
}

# 9. ITEM-561: the register-level initialization is one testable sequence, and
#    the first operational access is the one that reads CAPLENGTH.
#
#    The hardware run before this item wrote USBINTR, USBCMD = 0 and HCRESET
#    through a helper that added CAPLENGTH to the offset - but CAPLENGTH was
#    still zero at that point, so every one of those writes landed in the
#    capability space, the reset was never issued, and the firmware's periodic
#    schedule survived into the driver's start.  The refusal that named the
#    condition (PERIODIC_ON) was correct; the state it refused was the driver's
#    own, and the promised start evidence was absent because the report is only
#    written after the probe succeeds.
#
#    The pins below hold the fix in place: the offset is read and validated
#    first, the stop is required, the reset is required, the periodic status is
#    waited out with a budget instead of sampled once right after the command
#    word changed, and the start evidence reaches the console and `diag` even
#    when the start refuses the controller.
for item561_file in kernel/include/ehci_init.h kernel/src/ehci_init.c \
    tests/ehci-init-host.c; do
    [[ -f $item561_file ]] || {
        echo "ITEM-561: $item561_file is missing" >&2
        exit 1
    }
done
grep -Fq 'cap = ops->read(ops->context, EHCI_CAP_CAPLENGTH);' \
    kernel/src/ehci_init.c || {
    echo "ITEM-561: CAPLENGTH is not the first register read" >&2
    exit 1
}
grep -Fq 'result->caplength < 0x20U || result->caplength > 0x80U ||' \
    kernel/src/ehci_init.c || {
    echo "ITEM-561: an impossible CAPLENGTH is not refused before it is used" >&2
    exit 1
}
grep -Fq 'result->cause = EHCI_INIT_HALT_FAILED;' kernel/src/ehci_init.c || {
    echo "ITEM-561: a stop the controller does not acknowledge is not named" >&2
    exit 1
}
grep -Fq 'result->cause = EHCI_INIT_PERIODIC_STOP_TIMEOUT;' \
    kernel/src/ehci_init.c || {
    echo "ITEM-561: a periodic schedule that never stops is not named" >&2
    exit 1
}
grep -Fq 'EHCI_USBSTS_PSS, 0U,' kernel/src/ehci_init.c || {
    echo "ITEM-561: the periodic status is not waited out" >&2
    exit 1
}
grep -Fq 'result->usbcmd_after_reset & EHCI_USBCMD_ITC_MASK;' \
    kernel/src/ehci_init.c || {
    echo "ITEM-561: the stop word is not recorded as written" >&2
    exit 1
}
grep -Fq 'result->pse_clear_readback = ehci_init_read(ops, result,
    EHCI_OP_USBCMD);' kernel/src/ehci_init.c || {
    echo "ITEM-561: the stop word is not read back" >&2
    exit 1
}
grep -Fq 'ehci_init_run(&init_ops, controller->mmio_bytes,' kernel/src/ehci.c || {
    echo "ITEM-561: the driver does not run the initialization module" >&2
    exit 1
}
grep -Fq 'ehci_publish_start(controller);' kernel/src/ehci.c || {
    echo "ITEM-561: the start evidence is not published at the start" >&2
    exit 1
}
grep -Fq 'const char *ehci_start_evidence(void)' kernel/include/ehci.h || {
    echo "ITEM-561: the start evidence has no accessor" >&2
    exit 1
}
grep -Fq 'const char *ehci_start_evidence(void)' kernel/src/ehci.c || {
    echo "ITEM-561: the accessor has no definition" >&2
    exit 1
}
grep -Fq 'ehci_start_evidence(), ehci_controllers_text());' \
    kernel/src/diag_export.c || {
    echo "ITEM-561: the refusal record does not carry the start evidence" >&2
    exit 1
}
grep -Fq 'const char *start_detail,' kernel/include/diag_failure.h || {
    echo "ITEM-561: the refusal record cannot hold the start evidence" >&2
    exit 1
}
grep -Fq 'pieces[piece_count++] = " ehci_start=";' kernel/src/diag_failure.c || {
    echo "ITEM-561: the refusal detail does not render the start evidence" >&2
    exit 1
}
grep -Fq 'test-ehci-init-host' Makefile || {
    echo "ITEM-561: the initialization proof is not registered" >&2
    exit 1
}
for item561_check in 'the start succeeds once the delayed PSS has cleared' \
    'the wait for PSS consumed frames instead of an instant check' \
    'a PSS that survives the budget is refused by name' \
    'the two bits are recorded separately' \
    'a reset bit that never clears is named RESET_TIMEOUT' \
    'the start word keeps the interrupt threshold the controller reports' \
    'no operational access landed in the capability space'; do
    grep -Fq -- "$item561_check" tests/ehci-init-host.c || {
        echo "ITEM-561: the initialization proof lacks $item561_check" >&2
        exit 1
    }
done

# The start evidence is one line longer than a single console record part, so
# its publication depends on the console splitting a record that ends with its
# own newline.  The record was marked complete before it was carried, which made
# every mirror write a no-op and left the split loop unable to advance: the
# evidence that was supposed to explain a refusal hung the machine instead.
item561_mirror_count=$(grep -n 'console_mirror_count(room);' kernel/src/console.c |
    head -1 | cut -d: -f1)
item561_mirror_complete=$(grep -n 'mirror_complete = true;' kernel/src/console.c |
    head -1 | cut -d: -f1)
[[ -n $item561_mirror_count && -n $item561_mirror_complete &&
    $item561_mirror_count -lt $item561_mirror_complete ]] || {
    echo "ITEM-561: a record is marked complete before it is carried" >&2
    exit 1
}
grep -Fq 'a record longer than one part is split into continuations' \
    tests/console-host.c || {
    echo "ITEM-561: the long-record console proof is missing" >&2
    exit 1
}
grep -Fq 'no part of that record exceeds the bound it is published under' \
    tests/console-host.c || {
    echo "ITEM-561: the long-record console proof does not check the bound" >&2
    exit 1
}

# 10. ITEM-562: two USB 2 controllers, two verdicts, and a walk that names where
#     each port's path ended.
#
#     The run after ITEM-561 printed `ehci=DESCRIPTOR_FAILED/NO_DEVICE` and a
#     start snapshot of `00:1D.0`, while an earlier run had shown `00:1A.0`.  The
#     driver kept one global verdict, which each controller overwrote, one start
#     snapshot, which each start overwrote, and one walk stage, which the walk's
#     own fallback replaced - so a port that never enabled was reported as a
#     descriptor failure and the controller that answered was whichever the scan
#     reached last.
for item562_file in kernel/include/ehci_verdict.h kernel/src/ehci_verdict.c \
    tests/ehci-verdict-host.c; do
    [[ -f $item562_file ]] || {
        echo "ITEM-562: $item562_file is missing" >&2
        exit 1
    }
done
grep -Fq 'enum ehci_walk_port_state {' kernel/include/ehci_walk.h || {
    echo "ITEM-562: a hub port cannot be empty, up or refused" >&2
    exit 1
}
grep -Fq 'walk_finish(report);' kernel/src/ehci_walk.c || {
    echo "ITEM-562: the walk does not choose its first concrete refusal" >&2
    exit 1
}
grep -Fq 'report->failure_detail = hard->detail;' kernel/src/ehci_walk.c || {
    echo "ITEM-562: the first refusal does not keep its transport cause" >&2
    exit 1
}
grep -Fq 'walk_fail(&state, EHCI_WALK_OUTCOME_PORT_RESET, "ROOT_RESET");' \
    kernel/src/ehci_walk.c || {
    echo "ITEM-562: a root port that does not enable is not named" >&2
    exit 1
}
grep -Fq 'walk_fail(state, EHCI_WALK_OUTCOME_HUB, "HUB_PORT");' \
    kernel/src/ehci_walk.c || {
    echo "ITEM-562: a hub port that does not come up is not named" >&2
    exit 1
}
grep -Fq 'walk_fail(state, EHCI_WALK_OUTCOME_ATTACH, "ATTACH");' \
    kernel/src/ehci_walk.c || {
    echo "ITEM-562: a device that would not attach is not named" >&2
    exit 1
}
grep -Fq '"NO_CONNECTED_PORT"' kernel/src/ehci_walk.c || {
    echo "ITEM-562: an empty machine is not named as one" >&2
    exit 1
}
grep -Fq 'enum ehci_status ehci_walk_outcome_status(enum ehci_walk_outcome outcome)' \
    kernel/src/ehci_walk.c || {
    echo "ITEM-562: a walk outcome has no status" >&2
    exit 1
}
grep -Fq 'ehci_walk_outcome_status(report.outcome)' kernel/src/ehci.c || {
    echo "ITEM-562: the driver does not map the walk's outcome" >&2
    exit 1
}
grep -Fq 'ehci_verdict_fill(&ehci_entries[index],' kernel/src/ehci.c || {
    echo "ITEM-562: each controller has no entry of its own" >&2
    exit 1
}
grep -Fq 'ehci_verdict_choose(ehci_entries, count, &ehci_choice);' \
    kernel/src/ehci.c || {
    echo "ITEM-562: the machine's answer is not chosen from the entries" >&2
    exit 1
}
grep -Fq 'controller->walk_port_count = report.port_count;' kernel/src/ehci.c || {
    echo "ITEM-562: the controller does not keep its per-port paths" >&2
    exit 1
}
grep -Fq 'static char ehci_start_text[EHCI_MAX_CONTROLLERS][EHCI_START_TEXT_MAX];' \
    kernel/src/ehci.c || {
    echo "ITEM-562: one start snapshot still stands for every controller" >&2
    exit 1
}
grep -Fq 'size_t ehci_start_evidence_all(char *buffer, size_t size)' \
    kernel/src/ehci.c || {
    echo "ITEM-562: every controller's start evidence is not reachable" >&2
    exit 1
}
grep -Fq 'const char *ehci_controllers_text(void)' kernel/src/ehci.c || {
    echo "ITEM-562: the per-controller verdict list has no accessor" >&2
    exit 1
}
grep -Fq 'void ehci_diagnostics_print(void)' kernel/src/ehci.c || {
    echo "ITEM-562: the walk evidence has no on-demand printer" >&2
    exit 1
}
grep -Fq 'console_command_is(input_line, "ehci")' kernel/src/console.c || {
    echo "ITEM-562: the console has no command for the controllers" >&2
    exit 1
}
grep -Fq 'ehci_diagnostics_print();' kernel/src/console.c || {
    echo "ITEM-562: the console command does not print the controllers" >&2
    exit 1
}
grep -Fq 'report->outcome = EHCI_WALK_OUTCOME_OK;' kernel/src/ehci_walk.c || {
    echo "ITEM-562: a bound controller does not record that its walk bound" >&2
    exit 1
}
grep -Fq 'a walk that bound its medium reports that outcome' tests/ehci-walk-host.c || {
    echo "ITEM-562: the bound outcome is not pinned by the walk proof" >&2
    exit 1
}
grep -Fq 'test-ehci-verdict-host' Makefile || {
    echo "ITEM-562: the two-controller proof is not registered" >&2
    exit 1
}
for item562_check in 'the first concrete refusal survives the later port' \
    'an empty machine is named as having no connected port' \
    'the walk names the reset, not a descriptor it never read' \
    'the refusal names the hub port' \
    'the empty one is not a failure' \
    'the refused controller keeps its own walk stage' \
    'the answer names the stage, not NO_DEVICE' \
    'the bound medium answers in either scan order' \
    'the attach path'"'"'s own refusal survives the aggregation'; do
    grep -Fq -- "$item562_check" tests/ehci-walk-host.c tests/ehci-verdict-host.c || {
        echo "ITEM-562: the proofs lack $item562_check" >&2
        exit 1
    }
done

# 11. ITEM-563: the root port read path, from the PORTSC address to the walk.
#
#     Both USB 2 controllers reported `ports=2 connected=0` while the saved
#     Linux capture of the same machine has a hub on the EHCI bus.  The walk had
#     decided "nothing is connected" from one PORTSC read taken before anything
#     powered the port, with no debounce window, and it kept no value for a port
#     that read empty - and the power-on write the driver did have wrote no PP,
#     because PP was not in the value helper's set mask.  The pins below hold the
#     measurement: the address uses the validated CAPLENGTH, HCSPARAMS.PPC is
#     honoured, the connect window is bounded and recorded, and every port -
#     connected or not - has its own two reads.
for item563_file in kernel/include/ehci_port.h kernel/src/ehci_port.c \
    tests/ehci-port-host.c; do
    [[ -f $item563_file ]] || {
        echo "ITEM-563: $item563_file is missing" >&2
        exit 1
    }
done
grep -Fq 'uint32_t ehci_port_offset(uint8_t caplength, uint32_t port)' \
    kernel/src/ehci_port.c || {
    echo "ITEM-563: the port offset is not computed from CAPLENGTH" >&2
    exit 1
}
grep -Fq 'return (uint32_t)caplength + EHCI_OP_PORTSC +' kernel/src/ehci_port.c || {
    echo "ITEM-563: the port address does not carry CAPLENGTH" >&2
    exit 1
}
grep -Fq 'EHCI_PORT_SETTLE_STEP_US 10000U' kernel/include/ehci_port.h || {
    echo "ITEM-563: the connect debounce step is not 10 ms" >&2
    exit 1
}
grep -Fq 'EHCI_PORT_SETTLE_WINDOW_US 100000U' kernel/include/ehci_port.h || {
    echo "ITEM-563: the connect window is not bounded" >&2
    exit 1
}
grep -Fq 'if (ppc && (raw & EHCI_PORTSC_PP) == 0U)' kernel/src/ehci_port.c || {
    echo "ITEM-563: HCSPARAMS.PPC is not honoured before the port is called empty" >&2
    exit 1
}
grep -Fq 'state->settle_full = !state->connected &&' kernel/src/ehci_port.c || {
    echo "ITEM-563: an empty port does not record that the window was spent" >&2
    exit 1
}
grep -Fq 'if (!ehci_port_probe(&ops, controller->caplength, port,' \
    kernel/src/ehci.c || {
    echo "ITEM-563: the walk's root port source is not the production probe" >&2
    exit 1
}
grep -Fq '*portsc = controller->port_probes[port - 1U].portsc;' \
    kernel/src/ehci.c || {
    echo "ITEM-563: the walk does not consume the measured port value" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH("[EHCI-PORTSC] pci=");' kernel/src/ehci.c || {
    echo "ITEM-563: a root port's raw PORTSC is not recorded" >&2
    exit 1
}
grep -Fq 'ehci_text_write(&text, " first=");' kernel/src/ehci.c || {
    echo "ITEM-563: only the settled read is recorded, not the first one" >&2
    exit 1
}
grep -Fq 'ehci_measure_ports(controller);' kernel/src/ehci.c || {
    echo "ITEM-563: the ports are not measured before the walk judges" >&2
    exit 1
}
grep -Fq 'for (uint32_t port = 1U;' kernel/src/ehci.c || {
    echo "ITEM-563: not every root port is measured" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH(" hcsparams=0x");' kernel/src/ehci.c || {
    echo "ITEM-563: the record does not carry HCSPARAMS" >&2
    exit 1
}
grep -Fq 'uint32_t ehci_portsc_power_value(uint32_t portsc)' kernel/include/ehci.h || {
    echo "ITEM-563: there is no value that actually sets PP" >&2
    exit 1
}
grep -Fq 'return ((portsc & EHCI_PORTSC_CHANGE) | EHCI_PORTSC_PP) &' \
    kernel/src/ehci_queue.c || {
    echo "ITEM-563: the power value does not set PP" >&2
    exit 1
}
# ITEM-567: the reset path's power step is the module's, proved on the host.
grep -Fq 'ehci_portsc_power_value(portsc));' kernel/src/ehci_port.c || {
    echo "ITEM-563: the reset path does not power the port it resets" >&2
    exit 1
}
grep -Fq 'ehci_portsc_power_value(raw));' kernel/src/ehci_port.c || {
    echo "ITEM-563: the probe does not power the port it probes" >&2
    exit 1
}
grep -Fq 'test-ehci-port-host' Makefile || {
    echo "ITEM-563: the root port proof is not registered" >&2
    exit 1
}
for item563_check in 'the probe read the port'"'"'s own PORTSC, not the dword at BAR + 0x44' \
    'two ports with different words are told apart' \
    'the probe switched the port on because HCSPARAMS says software owns' \
    'a truly empty port says how long it was watched, not just zero' \
    'the helper the driver used before this item wrote no PP at all' \
    'every root port of an empty controller was probed'; do
    grep -Fq -- "$item563_check" tests/ehci-port-host.c || {
        echo "ITEM-563: the root port proof lacks $item563_check" >&2
        exit 1
    }
done

# 12. ITEM-564: the stop's verdict and the transfer's verdict are separate.
#
#     The run after ITEM-563 reported `result=TIMEOUT cause=ASYNC_ADVANCE
#     quiesced=0 stalled=1` with every descriptor still Active: the transfer had
#     timed out, and the *stop's* failure then replaced that cause.  The stop is
#     checked in the order the specification gives it - the schedule's state
#     decides whether a doorbell means anything, a leftover IAA is cleared before
#     the doorbell, the doorbell is rung with the schedule running and read back,
#     and only then is IAA awaited and acknowledged - and the transfer's cause is
#     never assigned again after the snapshot.
for item564_file in kernel/include/ehci_engine.h kernel/src/ehci_engine.c \
    tests/ehci-engine-host.c; do
    [[ -f $item564_file ]] || {
        echo "ITEM-564: $item564_file is missing" >&2
        exit 1
    }
done
grep -Fq 'enum ehci_engine_cleanup {' kernel/include/ehci_engine.h || {
    echo "ITEM-564: the stop has no verdict of its own" >&2
    exit 1
}
grep -Fq 'const char *ehci_engine_cleanup_string(enum ehci_engine_cleanup cleanup)' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-564: the stop's verdict has no name" >&2
    exit 1
}
if grep -Fq 'result->cause = EHCI_ENGINE_CAUSE_ASYNC_ADVANCE;' \
    kernel/src/ehci_engine.c; then
    echo "ITEM-564: the stop's failure replaces the transfer's cause" >&2
    exit 1
fi
grep -Fq 'result->schedule_running =' kernel/src/ehci_engine.c || {
    echo "ITEM-564: the stop does not check whether the schedule is walking" >&2
    exit 1
}
grep -Fq 'result->stale_iaa = true;' kernel/src/ehci_engine.c || {
    echo "ITEM-564: a leftover IAA is not cleared before the doorbell" >&2
    exit 1
}
grep -Fq 'result->cleanup = EHCI_ENGINE_CLEANUP_NOT_RUNNING;' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-564: a stopped schedule still waits for an advance" >&2
    exit 1
}
grep -Fq 'result->cleanup = EHCI_ENGINE_CLEANUP_REFUSED;' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-564: a doorbell the controller dropped is not named" >&2
    exit 1
}
grep -Fq 'result->cleanup = EHCI_ENGINE_CLEANUP_IGNORED;' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-564: an unanswered doorbell is not named" >&2
    exit 1
}
item564_stale=$(grep -n 'result->stale_iaa = true;' kernel/src/ehci_engine.c |
    head -1 | cut -d: -f1)
item564_doorbell=$(grep -n 'result->doorbell = ops->read' kernel/src/ehci_engine.c |
    head -1 | cut -d: -f1)
item564_ack=$(grep -n 'result->cleanup = EHCI_ENGINE_CLEANUP_ACKNOWLEDGED;' \
    kernel/src/ehci_engine.c | head -1 | cut -d: -f1)
[[ -n $item564_stale && -n $item564_doorbell && -n $item564_ack &&
    $item564_stale -lt $item564_doorbell && $item564_doorbell -lt $item564_ack ]] || {
    echo "ITEM-564: the stop runs out of order (stale IAA, doorbell, wait)" >&2
    exit 1
}
grep -Fq 'outcome.transfer != EHCI_TRANSFER_OK ||' kernel/src/ehci.c || {
    echo "ITEM-564: the driver does not record the transfer's verdict" >&2
    exit 1
}
grep -Fq 'outcome.cause != EHCI_ENGINE_CAUSE_NONE || outcome.stalled' \
    kernel/src/ehci.c || {
    echo "ITEM-564: a stop that left the pool unsafe is not recorded" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH("[EHCI-SYNC] pci=");' kernel/src/ehci.c || {
    echo "ITEM-564: a stop that was not confirmed leaves no evidence" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH(" sync=");' kernel/src/ehci.c || {
    echo "ITEM-564: the controller record does not summarise its stop" >&2
    exit 1
}
grep -Fq 'ehci_set_status(controller, EHCI_STATUS_TRANSFER_FAILED,' \
    kernel/src/ehci.c || {
    echo "ITEM-564: a transfer failure is still called a reset timeout" >&2
    exit 1
}
for item564_record in 'EHCI_PUBLISH("[EHCI-FAIL-TOKENS]");' \
    'EHCI_PUBLISH("[EHCI-FAIL-REG] pci=");' \
    'EHCI_PUBLISH("[EHCI-FAIL-SYNC] pci=");'; do
    grep -Fq -- "$item564_record" kernel/src/ehci.c || {
        echo "ITEM-564: the snapshot does not carry $item564_record" >&2
        exit 1
    }
done
for item564_case in \
    'test_the_startup_transition_is_not_a_stop' \
    'test_the_schedule_stopping_mid_wait_is_named_after_the_window' \
    'test_a_stopped_schedule_needs_a_confirmed_halt' \
    'test_an_unconfirmed_halt_keeps_the_pool_out_of_use' \
    'test_a_refused_doorbell_needs_the_confirmed_halt' \
    'test_a_refused_doorbell_without_a_halt_keeps_the_pool' \
    'test_a_delayed_acknowledgement_is_waited_for' \
    'test_a_stale_iaa_is_not_an_acknowledgement'; do
    grep -Fq -- "$item564_case" tests/ehci-engine-host.c || {
        echo "ITEM-564: the engine proof lacks $item564_case" >&2
        exit 1
    }
done

# 13. ITEM-566: the sample cannot stand for a confirmation.
#
#     The run after #735 reported `cause=ASYNC_STOPPED cleanup=NOT_RUNNING
#     quiesced=1 stalled=0` with `usbsts=0x00006000` (ASS=0, PSS=1): the engine
#     read two status bits, declared the schedule stopped and told the driver the
#     pool was safe, although nothing had confirmed that the controller let go of
#     the queue head.  Two decisions are checked here:
#
#       * `ASE=1, ASS=0` is the schedule's start-up transition (USBSTS follows
#         USBCMD at frame boundaries, EHCI 1.0 section 4.8), so it gets a bounded
#         window instead of an immediate stop verdict;
#       * reuse of the pool requires either an acknowledged async-advance
#         doorbell or a halt the controller confirmed (HCHalted read back), and
#         the schedule is re-armed after that halt for the transfers that follow.
#
#     The register words before the link, at the first refusal and after the stop
#     are recorded, and the address the engine uses is the production accessor
#     over CAPLENGTH - proved on a fake BAR, not only against substituted
#     registers.
for item566_file in kernel/include/ehci_regs.h kernel/src/ehci_regs.c \
    tests/ehci-regs-host.c; do
    [[ -f $item566_file ]] || {
        echo "ITEM-566: $item566_file is missing" >&2
        exit 1
    }
done
grep -Fq 'uint32_t ehci_reg_read(const volatile uint8_t *bar, uint8_t caplength,' \
    kernel/include/ehci_regs.h || {
    echo "ITEM-566: the operational window is not addressed through CAPLENGTH" >&2
    exit 1
}
grep -Fq 'ehci_reg_read(controller->regs, controller->caplength, offset)' \
    kernel/src/ehci.c || {
    echo "ITEM-566: the driver does not use the shared accessor" >&2
    exit 1
}
grep -Fq 'EHCI_ENGINE_ASS_TRANSITION_US 2000U' kernel/include/ehci_engine.h || {
    echo "ITEM-566: the start-up window has no bound" >&2
    exit 1
}
grep -Fq 'ehci_engine_stop_and_confirm' kernel/src/ehci_engine.c || {
    echo "ITEM-566: the stop is never confirmed by a halt" >&2
    exit 1
}
# The pool is declared safe in exactly two ways: an acknowledged doorbell, or a
# halt the controller confirmed.  A third `quiesced = true` would be the defect
# this item removes.
grep -Fq 'result->quiesced = result->halted_confirmed;' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-566: the confirmed halt is not what makes the pool safe" >&2
    exit 1
}
grep -Fq 'without HCHalted the pool must not be reused' tests/ehci-engine-host.c || {
    echo "ITEM-566: the reuse prohibition is not proven" >&2
    exit 1
}
if grep -Fq 'result->cleanup = EHCI_ENGINE_CLEANUP_NOT_RUNNING;' \
        kernel/src/ehci_engine.c &&
    ! grep -Fq 'result->halted_confirmed = ehci_engine_stop_and_confirm(' \
        kernel/src/ehci_engine.c; then
    echo "ITEM-566: a status reading still declares the pool safe" >&2
    exit 1
fi
grep -Fq 'result->cleanup = EHCI_ENGINE_CLEANUP_ACKNOWLEDGED;' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-566: an acknowledged doorbell is no longer the confirmation" >&2
    exit 1
}
grep -Fq 'result->halted_confirmed = ehci_engine_stop_and_confirm(' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-566: the halt is not the fallback confirmation" >&2
    exit 1
}
# The QEMU shape: the doorbell write is consumed immediately and IAA appears on
# a later frame.  The read-back cannot be the verdict, and a leftover IAA must
# be cleared before the next link or QEMU never advances the async schedule.
grep -Fq 'const bool accepted =' kernel/src/ehci_engine.c || {
    echo "ITEM-566: the doorbell read-back is treated as the verdict again" >&2
    exit 1
}
grep -Fq 'the immediate read-back had already lost the doorbell bit' \
    tests/ehci-engine-host.c || {
    echo "ITEM-566: the delayed acknowledgement after a lost read-back is not proven" >&2
    exit 1
}
grep -Fq 'if ((initial_status & EHCI_USBSTS_IAA) != 0U)' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-566: a leftover IAA is not cleared before the next link" >&2
    exit 1
}
grep -Fq 'ops->write(ops->context, EHCI_OP_USBSTS, EHCI_USBSTS_IAA);' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-566: the halt/re-arm leaves a pending acknowledgement behind" >&2
    exit 1
}
grep -Fq 'result->usbcmd_before_link = ops->read(ops->context, EHCI_OP_USBCMD);' \
    kernel/src/ehci_engine.c || {
    echo "ITEM-566: the words before the link are not kept" >&2
    exit 1
}
grep -Fq 'result->usbcmd_first_refusal = usbcmd;' kernel/src/ehci_engine.c || {
    echo "ITEM-566: the words at the first refusal are not kept" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH("[EHCI-FAIL-STATE] pci=");' kernel/src/ehci.c || {
    echo "ITEM-566: the three register snapshots have no record" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH(" halt=");' kernel/src/ehci.c || {
    echo "ITEM-566: the record does not say whether the loop was confirmed" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH(" restart=");' kernel/src/ehci.c || {
    echo "ITEM-566: the record does not say whether the schedule was re-armed" >&2
    exit 1
}
grep -Fq 'test-ehci-regs-host' Makefile || {
    echo "ITEM-566: the operational window proof is not registered" >&2
    exit 1
}
for item566_check in 'USBCMD lands at BAR + CAPLENGTH + 0' \
    'the capability dword at BAR + 0 is not written' \
    'without CAPLENGTH the write would hit BAR + 0' \
    'the engine ran over the real adapter and the chain retired' \
    'the halt and the re-arm did not touch the capability dword'; do
    grep -Fq -- "$item566_check" tests/ehci-regs-host.c || {
        echo "ITEM-566: the window proof lacks $item566_check" >&2
        exit 1
    }
done

# 14. ITEM-567: the root port reset is the specification's sequence, and its
#     words reach the report.
#
#     The run on build 3264431b reported `status=PORT_RESET_TIMEOUT detail=PR`
#     on both controllers while the port read `0x00001005` - connect and enable
#     already set - and the words that would have decided the case had been
#     written to the console journal, which was unbound, so the report carried
#     only the pre-walk port record.  The pins below hold the sequence to EHCI
#     1.0 Table 2-16 bit 8 (PR set with PE zero, 50 ms hold, explicit zero,
#     the controller's 2 ms completion bound, PE as the usable outcome), the
#     one re-clear of a latched PR, the words kept on the controller, and the
#     `[EHCI-PORT]` record in the report.
for item567_file in kernel/include/ehci_port.h kernel/src/ehci_port.c \
    kernel/src/ehci.c tests/ehci-port-reset-host.c Makefile; do
    [[ -f $item567_file ]] || {
        echo "ITEM-567: $item567_file is missing" >&2
        exit 1
    }
done
grep -Fq 'enum ehci_port_reset_result ehci_port_reset_run(' \
    kernel/include/ehci_port.h || {
    echo "ITEM-567: the reset has no module entry point" >&2
    exit 1
}
grep -Fq '#define EHCI_PORT_RESET_HOLD_US 50000U' kernel/include/ehci_port.h || {
    echo "ITEM-567: the reset is not held for the specification's 50 ms" >&2
    exit 1
}
grep -Fq '#define EHCI_PORT_RESET_SETTLE_US 2000U' kernel/include/ehci_port.h || {
    echo "ITEM-567: the specification's 2 ms completion bound has no sample" >&2
    exit 1
}
grep -Fq 'uint32_t portsc_cleared_readback;' kernel/include/ehci_port.h || {
    echo "ITEM-567: the 2 ms completion sample is not kept" >&2
    exit 1
}
grep -Fq 'EHCI_PORT_RESET_PR_LATCHED,' kernel/include/ehci_port.h || {
    echo "ITEM-567: a latched PR has no verdict of its own" >&2
    exit 1
}
grep -Fq 'ops->delay_us(ops->context, EHCI_PORT_RESET_HOLD_US);' \
    kernel/src/ehci_port.c || {
    echo "ITEM-567: the hold is not the specification's interval" >&2
    exit 1
}
grep -Fq 'state->portsc_set = ehci_portsc_write_value(portsc, EHCI_PORTSC_PR);' \
    kernel/src/ehci_port.c || {
    echo "ITEM-567: the set word is not PR with the port's power kept" >&2
    exit 1
}
grep -Fq 'state->portsc_cleared = ehci_portsc_write_value(state->portsc_hold, 0U);' \
    kernel/src/ehci_port.c || {
    echo "ITEM-567: the reset is not terminated by an explicit PR-zero write" >&2
    exit 1
}
grep -Fq 'ops->delay_us(ops->context, EHCI_PORT_RESET_SETTLE_US);' \
    kernel/src/ehci_port.c || {
    echo "ITEM-567: the 2 ms completion bound is not sampled" >&2
    exit 1
}
grep -Fq 'if (!state->cleared_twice) {' kernel/src/ehci_port.c || {
    echo "ITEM-567: a latched PR is never cleared a second time" >&2
    exit 1
}
grep -Fq 'while ((portsc & EHCI_PORTSC_PE) == 0U &&' kernel/src/ehci_port.c || {
    echo "ITEM-567: the completion is not awaited past the single sample" >&2
    exit 1
}
grep -Fq '(void)ehci_port_reset_run(&ops, controller->caplength, port,' \
    kernel/src/ehci.c || {
    echo "ITEM-567: the driver does not run the module's sequence" >&2
    exit 1
}
grep -Fq 'controller->reset_probes[port - 1U] = state;' kernel/src/ehci.c || {
    echo "ITEM-567: the reset's words are not kept on the controller" >&2
    exit 1
}
grep -Fq 'case EHCI_PORT_RESET_PR_LATCHED:' kernel/src/ehci.c || {
    echo "ITEM-567: a latched PR is not classified by the driver" >&2
    exit 1
}
grep -Fq 'ehci_text_write(&reset_text, "[EHCI-PORT] pci=");' kernel/src/ehci.c || {
    echo "ITEM-567: the reset has no record in the report" >&2
    exit 1
}
grep -Fq 'ehci_text_write(&reset_text, " rb2=");' kernel/src/ehci.c || {
    echo "ITEM-567: the report does not carry the 2 ms sample" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH(" rb2=");' kernel/src/ehci.c || {
    echo "ITEM-567: the journal record does not carry the 2 ms sample" >&2
    exit 1
}
if grep -Fq 'EHCI_PORT_RESET_BUDGET_US' kernel/src/ehci.c; then
    echo "ITEM-567: the driver still carries a second reset bound" >&2
    exit 1
fi
grep -Fq 'test-ehci-port-reset-host' Makefile || {
    echo "ITEM-567: the reset proof is not registered" >&2
    exit 1
}
for item567_check in \
    'the set word is PP|CSC|PR: power and change bits travel with the ' \
    'the controller does not self-clear PR: the hold word still has it' \
    'the clear is an explicit write with PR zero' \
    'the reset address is neither USBCMD, USBSTS nor CONFIGFLAG' \
    'no reset write landed outside this port'"'"'s PORTSC' \
    'a port that enables eight polls after the clear is a reset OK' \
    'a port the controller never enables is NOT_ENABLED' \
    'the latched reset is cleared exactly once more' \
    'power good is waited for, not assumed'; do
    grep -Fq -- "$item567_check" tests/ehci-port-reset-host.c || {
        echo "ITEM-567: the reset proof lacks $item567_check" >&2
        exit 1
    }
done

# 15. ITEM-568: the handoff is in configuration space, and a halted controller
#     is not a port fault.
#
#     Build 52012c48 on the Toshiba still refused with PORT_RESET_TIMEOUT/PR,
#     and the words named the real fault: `hse_usbcmd=0x00080010` (PSE set, RS
#     and ASE clear) and `hse_usbsts=0x00007010` (PSS|RECL|HCH|HSE) - a
#     controller halted by a host system error, with a periodic schedule the
#     driver never enabled.  The handoff that exists to take the controller from
#     the firmware read the EHCI extended capabilities from MMIO at EECP*4
#     instead of PCI configuration space at EECP, so it never found the legacy
#     capability (`legacy=NOT_FOUND`) and never asked the firmware to let go.
#     The pins below hold the corrected address space, the valid periodic frame
#     list, the controller-state verdict and the proofs.
for item568_file in kernel/include/ehci_legacy.h kernel/src/ehci_legacy.c \
    tests/ehci-legacy-host.c tests/ehci-masks-host.c tests/ehci-port-reset-host.c; do
    [[ -f $item568_file ]] || {
        echo "ITEM-568: $item568_file is missing" >&2
        exit 1
    }
done
grep -Fq 'configuration space of the first EHCI extended capability' \
    kernel/include/ehci_legacy.h || {
    echo "ITEM-568: the handoff does not quote the specification's address space" >&2
    exit 1
}
grep -Fq 'offset = state->eecp;' kernel/src/ehci_legacy.c || {
    echo "ITEM-568: the capability walk does not start at the config-space offset" >&2
    exit 1
}
if grep -Fq 'eecp * 4U' kernel/src/ehci_legacy.c kernel/src/ehci.c; then
    echo "ITEM-568: the EECP is still scaled as a dword index" >&2
    exit 1
fi
grep -Fq 'next = (capability >> 8U) & 0xFFU;' kernel/src/ehci_legacy.c || {
    echo "ITEM-568: the next capability is not read as a byte offset" >&2
    exit 1
}
grep -Fq 'ops->write32(ops->context, state->legacy_offset, state->sup_written);' \
    kernel/src/ehci_legacy.c || {
    echo "ITEM-568: the OS-owned semaphore is not written in configuration space" >&2
    exit 1
}
grep -Fq 'state->ctl_before & ~EHCI_LEGACY_CTL_ENABLES' \
    kernel/src/ehci_legacy.c || {
    echo "ITEM-568: the legacy SMI enables are not disabled" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH("[EHCI-LEGACY] pci=");' kernel/src/ehci.c || {
    echo "ITEM-568: the handoff has no record" >&2
    exit 1
}
grep -Fq 'ehci_text_write(&legacy_text, "[EHCI-LEGACY] pci=");' kernel/src/ehci.c || {
    echo "ITEM-568: the handoff record does not reach the report" >&2
    exit 1
}
grep -Fq 'EHCI_FRAME_LIST_TERMINATE' kernel/include/ehci.h || {
    echo "ITEM-568: the periodic frame list has no terminate entry" >&2
    exit 1
}
grep -Fq '(physical % EHCI_PAGE_BYTES) != 0U' kernel/src/ehci.c || {
    echo "ITEM-568: the frame list alignment is not checked" >&2
    exit 1
}
grep -Fq 'EHCI_PORT_RESET_CONTROLLER_HALTED' kernel/include/ehci_port.h || {
    echo "ITEM-568: a halted controller has no port-reset verdict" >&2
    exit 1
}
grep -Fq 'static void ehci_port_sample(' kernel/src/ehci_port.c || {
    echo "ITEM-568: the controller's words are not sampled through the reset" >&2
    exit 1
}
grep -Fq 'if (ehci_port_controller_halted(state, EHCI_PORT_RESET_POINT_BEFORE)) {' \
    kernel/src/ehci_port.c || {
    echo "ITEM-568: the reset is attempted on a halted controller" >&2
    exit 1
}
grep -Fq 'case EHCI_PORT_RESET_CONTROLLER_HALTED:' kernel/src/ehci.c || {
    echo "ITEM-568: the halted controller is not classified by the driver" >&2
    exit 1
}
grep -Fq 'EHCI_STATUS_HOST_SYSTEM_ERROR' kernel/include/ehci.h || {
    echo "ITEM-568: there is no controller-level host system error status" >&2
    exit 1
}
grep -Fq 'ehci_text_write(&reset_text, " hse=");' kernel/src/ehci.c || {
    echo "ITEM-568: the report does not carry the HSE point" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH(" hcmd=0x");' kernel/src/ehci.c || {
    echo "ITEM-568: the journal record does not carry the HSE words" >&2
    exit 1
}
grep -Fq 'controller->reset_pcists[port - 1U] =' kernel/src/ehci.c || {
    echo "ITEM-568: the PCI status at the HSE sample is not kept" >&2
    exit 1
}
grep -Fq 'test-ehci-legacy-host' Makefile || {
    echo "ITEM-568: the handoff proof is not registered" >&2
    exit 1
}
grep -Fq 'test-ehci-masks-host' Makefile || {
    echo "ITEM-568: the mask proof is not registered" >&2
    exit 1
}
for item568_check in \
    'the handoff read outside the PCI configuration space, or at a ' \
    'USBLEGSUP is read at the configuration-space offset EECP' \
    'the first read is at EECP, not at EECP*4' \
    'the legacy SMI enables are cleared after the handoff' \
    'a BIOS that never lets go is a named timeout'; do
    grep -Fq -- "$item568_check" tests/ehci-legacy-host.c || {
        echo "ITEM-568: the handoff proof lacks $item568_check" >&2
        exit 1
    }
done
for item568_check in \
    '0x00080010 is PSE set with RS and ASE clear' \
    '0x00007010 is HSE with the controller halted and the periodic ' \
    'the write mask is exactly the writable subset' \
    'USBLEGSUP.OS-owned is bit 24'; do
    grep -Fq -- "$item568_check" tests/ehci-masks-host.c || {
        echo "ITEM-568: the mask proof lacks $item568_check" >&2
        exit 1
    }
done

# 16. ITEM-569: the DMA structures are the 64-bit format, and the pool's regions
#     are disjoint.
#
#     Build 2f6479d5 passed the handoff and the root reset and then reported
#     HSE at IN_WAIT on DEVICE8 - the first transfer.  The pool used a 32-byte
#     qTD stride and a 64-byte QH slot while EHCI 1.0 Appendix B defines a
#     52-byte qTD-64 and a 68-byte QH-64: on a controller with AC64 = 1 the
#     bytes read as `ext_buffer[0]` are the next descriptor's `next` link, so
#     the controller names a 64-bit buffer address it was never given (master
#     abort, Table 4-19: fatal).  The frame list also started at DMA+0x3000,
#     which is where the control data buffer lives.  The pins below hold the
#     format, the strides, the separated layout and the proofs.
for item569_file in tests/ehci-dma64-host.c kernel/include/ehci.h \
    kernel/src/ehci.c kernel/src/ehci_queue.c; do
    [[ -f $item569_file ]] || {
        echo "ITEM-569: $item569_file is missing" >&2
        exit 1
    }
done
grep -Fq '#define EHCI_QTD64_BYTES 52U' kernel/include/ehci.h || {
    echo "ITEM-569: the 64-bit descriptor size is not the appendix's" >&2
    exit 1
}
grep -Fq '#define EHCI_QH64_BYTES 68U' kernel/include/ehci.h || {
    echo "ITEM-569: the 64-bit queue head size is not the appendix's" >&2
    exit 1
}
grep -Fq '#define EHCI_QTD_STRIDE 64U' kernel/include/ehci.h || {
    echo "ITEM-569: the descriptor stride does not hold the 64-bit format" >&2
    exit 1
}
grep -Fq '#define EHCI_QH_STRIDE 128U' kernel/include/ehci.h || {
    echo "ITEM-569: the queue head slot does not hold the 64-bit format" >&2
    exit 1
}
grep -Fq 'volatile uint32_t ext_buffer[EHCI_QTD_BUFFERS];' kernel/include/ehci.h || {
    echo "ITEM-569: the descriptor has no extended buffer words" >&2
    exit 1
}
grep -Fq 'volatile uint32_t overlay_ext_buffer[EHCI_QTD_BUFFERS];' \
    kernel/include/ehci.h || {
    echo "ITEM-569: the queue head has no extended overlay words" >&2
    exit 1
}
grep -Fq '_Static_assert(sizeof(struct ehci_qtd) == EHCI_QTD64_BYTES,' \
    kernel/include/ehci.h || {
    echo "ITEM-569: the descriptor's size is not asserted" >&2
    exit 1
}
grep -Fq 'EHCI_FRAME_LIST_OFFSET (EHCI_BOUNCE_OFFSET + EHCI_BOUNCE_BYTES)' \
    kernel/include/ehci.h || {
    echo "ITEM-569: the frame list is not separated from the data buffers" >&2
    exit 1
}
grep -Fq '"a control transfer'"'"'s data buffer overlaps the frame list"' \
    kernel/include/ehci.h || {
    echo "ITEM-569: the control data / frame list separation is not asserted" >&2
    exit 1
}
grep -Fq 'qtd->ext_buffer[index] = (uint32_t)(cursor >> 32U);' \
    kernel/src/ehci_queue.c || {
    echo "ITEM-569: the builder does not write the buffer's upper half" >&2
    exit 1
}
grep -Fq 'controller->ac64 =' kernel/src/ehci.c || {
    echo "ITEM-569: the format decision is not kept from HCCPARAMS" >&2
    exit 1
}
grep -Fq 'ehci_ring_qtd(ehci_qtd_slot(controller, index), next, token,' \
    kernel/src/ehci.c || {
    echo "ITEM-569: the driver does not build descriptors at the stride" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH("[EHCI-DMA] pci=");' kernel/src/ehci.c || {
    echo "ITEM-569: the DMA record is missing" >&2
    exit 1
}
grep -Fq 'ehci_text_write(&dma_text, "[EHCI-DMA] pci=");' kernel/src/ehci.c || {
    echo "ITEM-569: the DMA record does not reach the report" >&2
    exit 1
}
grep -Fq 'ehci_publish_hex64(' kernel/src/ehci.c || {
    echo "ITEM-569: the addresses are not printed with both halves" >&2
    exit 1
}
grep -Fq 'hccparams=0x' kernel/src/ehci.c || {
    echo "ITEM-569: the raw HCCPARAMS is not recorded" >&2
    exit 1
}
grep -Fq 'test-ehci-dma64-host' Makefile || {
    echo "ITEM-569: the 64-bit format proof is not registered" >&2
    exit 1
}
for item569_check in \
    'the descriptor is the 64-bit format'"'"'s 52 bytes, not 32' \
    'the queue head is the 64-bit format'"'"'s 68 bytes, not 48' \
    'the descriptor stride holds the 64-bit format' \
    'the descriptor names the 64-bit buffer address in both halves' \
    'a buffer above 4 GiB keeps its upper half' \
    'the list head'"'"'s 68 bytes end before the transfer head'"'"'s slot' \
    'the control data buffer does not start at the frame list' \
    'a data write left the frame list untouched' \
    'AC64 is bit 0 of HCCPARAMS (Table 2-7)'; do
    grep -Fq -- "$item569_check" tests/ehci-dma64-host.c || {
        echo "ITEM-569: the format proof lacks $item569_check" >&2
        exit 1
    }
done

# 17. ITEM-570: the hub's wPortStatus speed bits are the specification's.
#
#     The run of b10dc93c reached HUB_PORT: the high-speed stick behind the rate
#     matching hub was refused as a low-speed device (which would need split
#     transactions).  USB 2.0 Table 11-21 defines wPortStatus bit 9 as low-speed
#     and bit 10 as high-speed; the driver had 0x0400 (bit 10) as LOW_SPEED and
#     0x0800 (bit 11, test mode) as HIGH_SPEED, one position high, so every
#     high-speed device behind a hub was classified low-speed.  The pins below
#     hold the bits, the raw words the walk keeps beside the verdict, and the
#     proofs; split transactions and the timeouts are deliberately untouched.
grep -Fq '#define EHCI_HUB_PORT_STATUS_LOW_SPEED 0x0200U' kernel/include/ehci.h || {
    echo "ITEM-570: wPortStatus low-speed is not bit 9" >&2
    exit 1
}
grep -Fq '#define EHCI_HUB_PORT_STATUS_HIGH_SPEED 0x0400U' \
    kernel/include/ehci.h || {
    echo "ITEM-570: wPortStatus high-speed is not bit 10" >&2
    exit 1
}
if grep -Fq '#define EHCI_HUB_PORT_STATUS_LOW_SPEED 0x0400U' kernel/include/ehci.h ||
    grep -Fq '#define EHCI_HUB_PORT_STATUS_HIGH_SPEED 0x0800U' \
        kernel/include/ehci.h; then
    echo "ITEM-570: the shifted speed bits are still declared" >&2
    exit 1
fi
grep -Fq '0x0503U) == EHCI_HUB_PORT_HIGH_SPEED' tests/ehci-host.c || {
    echo "ITEM-570: the classifier regression does not feed 0x0503" >&2
    exit 1
}
grep -Fq '0x0303U) == EHCI_HUB_PORT_LOW_SPEED' tests/ehci-host.c || {
    echo "ITEM-570: the classifier regression does not feed 0x0303" >&2
    exit 1
}
grep -Fq '0x0103U) == EHCI_HUB_PORT_FULL_SPEED' tests/ehci-host.c || {
    echo "ITEM-570: the classifier regression does not feed 0x0103" >&2
    exit 1
}
grep -Fq '0x0800U) != EHCI_HUB_PORT_HIGH_SPEED' tests/ehci-host.c || {
    echo "ITEM-570: the test-mode word is not refused as high speed" >&2
    exit 1
}
grep -Fq 'uint16_t hub_status;' kernel/include/ehci_walk.h || {
    echo "ITEM-570: the walk does not keep the raw wPortStatus" >&2
    exit 1
}
grep -Fq 'uint16_t hub_change;' kernel/include/ehci_walk.h || {
    echo "ITEM-570: the walk does not keep the raw wPortChange" >&2
    exit 1
}
grep -Fq 'const char *hub_verdict;' kernel/include/ehci_walk.h || {
    echo "ITEM-570: the walk does not keep the verdict beside the words" >&2
    exit 1
}
grep -Fq '*verdict_out = ehci_hub_port_verdict_string(verdict);' \
    kernel/src/ehci.c || {
    echo "ITEM-570: the driver does not report the verdict it made" >&2
    exit 1
}
grep -Fq '*status_out = status;' kernel/src/ehci.c || {
    echo "ITEM-570: the driver does not report the hub's status word" >&2
    exit 1
}
grep -Fq 'ehci_text_write(&path_text, " wstatus=");' kernel/src/ehci.c || {
    echo "ITEM-570: the path record does not carry wPortStatus" >&2
    exit 1
}
grep -Fq 'ehci_text_write(&path_text, " wchange=");' kernel/src/ehci.c || {
    echo "ITEM-570: the path record does not carry wPortChange" >&2
    exit 1
}
grep -Fq 'record->hub_verdict != NULL' kernel/src/ehci.c || {
    echo "ITEM-570: the path record does not carry the verdict" >&2
    exit 1
}
grep -Fq 'if (verdict != EHCI_HUB_PORT_HIGH_SPEED) {' kernel/src/ehci.c || {
    echo "ITEM-570: the high-speed gate was changed beyond the speed bits" >&2
    exit 1
}
grep -Fq 'test_a_high_speed_hub_port_reaches_the_medium' tests/ehci-walk-host.c || {
    echo "ITEM-570: the high-speed walk regression is missing" >&2
    exit 1
}
grep -Fq 'test_a_slower_hub_port_is_refused_with_its_raw_words' \
    tests/ehci-walk-host.c || {
    echo "ITEM-570: the raw-word refusal regression is missing" >&2
    exit 1
}
for item570_check in \
    'the high-speed device was addressed through the walk' \
    'the high-speed device was attached and its medium verified' \
    'the record keeps the raw wPortStatus and wPortChange' \
    'the record keeps the verdict the raw word produced'; do
    grep -Fq -- "$item570_check" tests/ehci-walk-host.c || {
        echo "ITEM-570: the walk regression lacks $item570_check" >&2
        exit 1
    }
done

# 18. ITEM-571: the sequence commit names which step failed, and the diag
#     refusal carries the device's own answer.
#
#     The run of 547b736b reached the medium (MATCH/OK, storage BOUND, reads=524,
#     writes=127, no transport errors) and refused at VERIFY/SEQUENCE_COMMIT:
#     the commit point wrote the journal header and flushed it, and a failure of
#     either came out as one bool, so the refusal could not say which step failed
#     nor what the device answered.  The pins below hold the two steps apart,
#     the block result each of them got, the storage snapshot (opcode, CSW
#     status/residue, sense and its validity), the diag line that prints them
#     without a journal, and the selected device's status after a first candidate
#     refused.
grep -Fq 'FAT12_JOURNAL_REFUSED_HEADER_WRITE,' kernel/include/install_fat12_journal.h || {
    echo "ITEM-571: the header write has no status of its own" >&2
    exit 1
}
grep -Fq 'FAT12_JOURNAL_REFUSED_FLUSH,' kernel/include/install_fat12_journal.h || {
    echo "ITEM-571: the flush has no status of its own" >&2
    exit 1
}
grep -Fq 'commit->header_written' kernel/src/install_journal_platform.c || {
    echo "ITEM-571: the commit receipt does not say the header landed" >&2
    exit 1
}
grep -Fq 'commit->flush_status' kernel/src/install_journal_platform.c || {
    echo "ITEM-571: the commit receipt does not keep the flush result" >&2
    exit 1
}
grep -Fq 'install_journal_platform_commit_text(commit_text,' kernel/src/diag_export.c || {
    echo "ITEM-571: the diag refusal does not carry the commit receipt" >&2
    exit 1
}
grep -Fq 'diag_failure_set_evidence(&diag_failure, commit_text, storage_text);' \
    kernel/src/diag_export.c || {
    echo "ITEM-571: the diag refusal does not carry the device snapshot" >&2
    exit 1
}
grep -Fq 'struct usb_storage_fault {' kernel/include/usb_storage.h || {
    echo "ITEM-571: the storage fault snapshot is not declared" >&2
    exit 1
}
grep -Fq 'usb_storage_record_fault(storage, USB_STORAGE_COMMAND_FAILED, cb,' \
    kernel/src/usb_storage.c || {
    echo "ITEM-571: the failed command is not recorded before REQUEST SENSE" >&2
    exit 1
}
grep -Fq 'storage->fault.sense_attempted = true;' kernel/src/usb_storage.c || {
    echo "ITEM-571: REQUEST SENSE's own result is not kept apart" >&2
    exit 1
}
grep -Fq 'storage->fault.sense_valid = storage->last_sense_valid;' \
    kernel/src/usb_storage.c || {
    echo "ITEM-571: the sense validity is not carried into the snapshot" >&2
    exit 1
}
grep -Fq 'bool ehci_storage_fault_text(char *buffer, size_t size);' kernel/include/ehci.h || {
    echo "ITEM-571: the controller does not expose the device snapshot" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH("[EHCI-STORAGE-FAULT] pci=");' kernel/src/ehci.c ||
    grep -Fq '"[EHCI-STORAGE-FAULT] pci="' kernel/src/ehci.c || {
    echo "ITEM-571: the report does not carry the device snapshot" >&2
    exit 1
}
grep -Fq 'ehci_set_status(controller, EHCI_STATUS_OK, "BOUND");' kernel/src/ehci.c || {
    echo "ITEM-571: a bound device leaves the first refusal's status in place" >&2
    exit 1
}
for item571_test in \
    'a refused header write is HEADER_WRITE, not a flush failure' \
    'a refused flush is FLUSH, not the same value as the header write' \
    'the flush receipt keeps that the header landed and the block result'; do
    grep -Fq -- "$item571_test" tests/install-fat12-journal-host.c || {
        echo "ITEM-571: the commit proof lacks $item571_test" >&2
        exit 1
    }
done
for item571_test in \
    'the snapshot keeps the flush'"'"'s opcode, not REQUEST SENSE' \
    'the snapshot keeps the failed CSW status' \
    'REQUEST SENSE ran, and its own result is kept apart' \
    'the device'"'"'s sense key/ASC/ASCQ are kept and marked valid' \
    'the snapshot keeps the WRITE'"'"'s opcode and operation' \
    'a response code that is neither 0x70 nor 0x71 is not trusted' \
    'a failed flush is an I/O error, not an unsupported command' \
    'the success path leaves no fault snapshot'; do
    grep -Fq -- "$item571_test" tests/usb-storage-host.c || {
        echo "ITEM-571: the storage proof lacks $item571_test" >&2
        exit 1
    }
done
grep -Fq 'test_failure_carries_the_commit_evidence' tests/diag-host.c || {
    echo "ITEM-571: the diag proof does not pin the evidence" >&2
    exit 1
}

# 19. ITEM-572: the unsupported flush has one accepted answer, and the cache the
#     device reports decides what a flush is allowed to mean.
#
#     The run of 7ed4d733 named the refusal: commit=FLUSH blk=IO_ERROR,
#     op=35/COMMAND_FAILED FLUSH csw=1, sense=OK/70-05-20-00 - the stick rejects
#     SYNCHRONIZE CACHE (10) as an invalid command.  The Debian probe of the same
#     stick (serial 12345678, read-only: INQUIRY, MODE SENSE page 08h) answered a
#     valid caching page with WCE=0, so a flush has nothing to flush.  The pins
#     below hold the exact classification, the cache query and its UNKNOWN case,
#     the policy that follows, and the proofs that keep real errors real.
grep -Fq 'USB_STORAGE_FLUSH_UNSUPPORTED,' kernel/include/usb_storage.h || {
    echo "ITEM-572: the unsupported flush has no status" >&2
    exit 1
}
grep -Fq 'enum usb_storage_status usb_storage_query_cache(struct usb_storage *storage);' \
    kernel/include/usb_storage.h || {
    echo "ITEM-572: the caching page has no query" >&2
    exit 1
}
grep -Fq 'static bool usb_storage_flush_is_unsupported(' kernel/src/usb_storage.c || {
    echo "ITEM-572: the unsupported flush is not classified separately" >&2
    exit 1
}
grep -Fq 'fault->sense_key == 0x05U && fault->sense_asc == 0x20U &&' \
    kernel/src/usb_storage.c || {
    echo "ITEM-572: the accepted sense is not exactly 05/20/00" >&2
    exit 1
}
grep -Fq 'fault->sense_ascq == 0x00U;' kernel/src/usb_storage.c || {
    echo "ITEM-572: the sense ASCQ is not part of the classification" >&2
    exit 1
}
grep -Fq 'fault->sense_status == USB_STORAGE_OK && fault->sense_valid &&' \
    kernel/src/usb_storage.c || {
    echo "ITEM-572: an unusable sense is not excluded from the acceptance" >&2
    exit 1
}
grep -Fq 'if (storage->cache_known && !storage->wce) {' kernel/src/usb_storage.c || {
    echo "ITEM-572: a write-through device is still asked to flush" >&2
    exit 1
}
grep -Fq 'storage->flush_policy = USB_STORAGE_FLUSH_WRITE_THROUGH;' \
    kernel/src/usb_storage.c || {
    echo "ITEM-572: the write-through policy is not recorded" >&2
    exit 1
}
grep -Fq 'USB_STORAGE_FLUSH_UNSUPPORTED_CACHE_UNKNOWN;' kernel/src/usb_storage.c || {
    echo "ITEM-572: the unknown-cache case is not recorded" >&2
    exit 1
}
grep -Fq 'return USB_STORAGE_FLUSH_UNSUPPORTED;' kernel/src/usb_storage.c || {
    echo "ITEM-572: an enabled cache with a rejected flush is not an error" >&2
    exit 1
}
grep -Fq '(void)usb_storage_query_cache(storage);' kernel/src/usb_storage.c || {
    echo "ITEM-572: the cache is not read at bind" >&2
    exit 1
}
grep -Fq 'an absent caching page is UNKNOWN, never WCE=0' tests/usb-storage-host.c || {
    echo "ITEM-572: the UNKNOWN case is not proven" >&2
    exit 1
}
grep -Fq 'no SYNCHRONIZE CACHE command is sent to a write-through device' \
    tests/usb-storage-host.c || {
    echo "ITEM-572: the write-through skip is not proven" >&2
    exit 1
}
grep -Fq 'a cache-enabled device that cannot flush is a real error' \
    tests/usb-storage-host.c || {
    echo "ITEM-572: the WCE=1 refusal is not proven" >&2
    exit 1
}
grep -Fq 'a flush whose REQUEST SENSE failed is still an error' \
    tests/usb-storage-host.c || {
    echo "ITEM-572: the failed-sense case is not proven" >&2
    exit 1
}
grep -Fq 'a different sense stays a command failure, not an unsupported ' \
    tests/usb-storage-host.c || {
    echo "ITEM-572: the other-sense case is not proven" >&2
    exit 1
}
grep -Fq 'a device that supports the flush answers it' tests/usb-storage-host.c || {
    echo "ITEM-572: the supported flush is not proven" >&2
    exit 1
}
grep -Fq 'an unusable caching page is UNKNOWN, not WCE=0' tests/usb-storage-host.c || {
    echo "ITEM-572: the unusable page is not proven" >&2
    exit 1
}
grep -Fq 'EHCI_PUBLISH(" flush=");' kernel/src/ehci.c || {
    echo "ITEM-572: the journal record does not carry the flush policy" >&2
    exit 1
}
grep -Fq 'ehci_text_write(&text, " flush=");' kernel/src/ehci.c || {
    echo "ITEM-572: the report does not carry the flush policy" >&2
    exit 1
}

# 20. ITEM-573: what a refused xHCI controller read is kept, and the export
#     answers "was the pointer detected" from RAM.
#
#     The owner's run of ad16a337 left the mouse's USB 3.0 socket answered by
#     NO_DEVICE/NO_CONNECTED_DEVICE: the report could not say whether a device
#     was plugged, which port it sat on, or how far enumeration reached.  The
#     driver had read every port (raw PORTSC before and after its own
#     preparation), the Supported Protocol groups, USBCMD/USBSTS and each HID
#     role's identity and counters - but a visit that binds nothing releases its
#     session, and the release zeroed all of it after copying five fields.  The
#     pins below hold the durable store, the copy taken before the release, the
#     report records it renders without touching MMIO, the pointer verdict
#     (BOUND / REFUSED / SEEN / NOT_DETECTED), and the proof that the extracted
#     production file carries them after the controller is gone.
grep -Fq '#define XHCI_EVIDENCE_CONTROLLERS 8U' kernel/include/xhci_evidence.h || {
    echo "ITEM-573: the evidence store has no controller bound" >&2
    exit 1
}
grep -Fq '#define XHCI_EVIDENCE_PORTS 32U' kernel/include/xhci_evidence.h || {
    echo "ITEM-573: the evidence store has no port bound" >&2
    exit 1
}
grep -Fq 'struct xhci_evidence_port port_evidence[XHCI_EVIDENCE_PORTS];' \
    kernel/src/xhci.c || {
    echo "ITEM-573: the session does not keep every port's two raw words" >&2
    exit 1
}
grep -Fq 'struct xhci_evidence_port {' kernel/include/xhci_evidence.h || {
    echo "ITEM-573: the port evidence has no shape" >&2
    exit 1
}
grep -Fq 'uint32_t was;' kernel/include/xhci_evidence.h || {
    echo "ITEM-573: the port evidence has no before word" >&2
    exit 1
}
grep -Fq 'uint32_t now;' kernel/include/xhci_evidence.h || {
    echo "ITEM-573: the port evidence has no after word" >&2
    exit 1
}
grep -Fq 'xhci_evidence_keep_session(session, false);' kernel/src/xhci.c || {
    echo "ITEM-573: a surveyed session is not kept before the release" >&2
    exit 1
}
grep -Fq 'xhci_evidence_keep_session(session, true);' kernel/src/xhci.c || {
    echo "ITEM-573: the release does not keep the controller's final evidence" >&2
    exit 1
}
grep -Fq 'xhci_evidence_reset();' kernel/src/xhci.c || {
    echo "ITEM-573: the boot's walk does not own the store" >&2
    exit 1
}
grep -Fq 'static void evidence_mouse_seen(struct xhci_evidence_mouse *mouse) {' \
    kernel/src/xhci_evidence.c || {
    echo "ITEM-573: the store does not derive the SEEN verdict" >&2
    exit 1
}
grep -Fq 'if (mouse->bound || mouse->seen) {' kernel/src/xhci_evidence.c || {
    echo "ITEM-573: the SEEN derivation can overwrite a bound or seen pointer" >&2
    exit 1
}
grep -Fq 'if (controller->ports[port].connected == 0U) {' \
    kernel/src/xhci_evidence.c || {
    echo "ITEM-573: the SEEN derivation does not require a reported connect" >&2
    exit 1
}
grep -Fq 'evidence_text_copy(mouse->detail, sizeof(mouse->detail),' \
    kernel/src/xhci_evidence.c || {
    echo "ITEM-573: a detected device's failure stage is not kept" >&2
    exit 1
}
grep -Fq 'line_text(&line, "[XHCI-CTRL] pci=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-573: no [XHCI-CTRL] record is rendered" >&2
    exit 1
}
grep -Fq 'line_text(&line, "[XHCI-PORT] pci=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-573: no [XHCI-PORT] record is rendered" >&2
    exit 1
}
grep -Fq 'line_text(&line, "[XHCI-MOUSE] verdict=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-573: the pointer verdict is not rendered" >&2
    exit 1
}
grep -Fq 'xhci_evidence_report(xhci_evidence_bridge_sink, &bridge);' \
    kernel/src/xhci.c || {
    echo "ITEM-573: the export does not print the kept evidence" >&2
    exit 1
}
for item573_test in \
    'the record does not read the source after the keep' \
    'one controller is kept after its session was zeroed' \
    'the start verdict survives the release' \
    'USBCMD and USBSTS survive the release' \
    'port 1 keeps both raw words and its actions' \
    'port 2 keeps the word before and after the link kick' \
    'the Supported Protocol group is kept' \
    "the mouse role's identity and endpoint are kept" \
    "the mouse role's transfer counters are kept" \
    'a role that was never seen publishes no record' \
    'the same PCI function is one record, not two' \
    'two controllers keep two records' \
    'a machine with no pointer publishes NOT_DETECTED' \
    'a seen pointer keeps its identity and port' \
    'a bound pointer publishes the submitted/completed/decoded counts' \
    'a connected port with no HID role is a detected device' \
    "the detected device's controller and port are named" \
    "the detected device keeps the pointer's own detail" \
    "the pointer verdict never borrows the controller's role reason" \
    'an unidentified device has no invented identity' \
    'no port with a connect leaves the pointer not detected'; do
    grep -Fq -- "$item573_test" tests/xhci-evidence-host.c || {
        echo "ITEM-573: the evidence proof lacks $item573_test" >&2
        exit 1
    }
done
grep -Fq 'build/test-xhci-evidence-host' Makefile || {
    echo "ITEM-573: the evidence proof is not built from its own source" >&2
    exit 1
}
grep -Fq '$(MAKE) test-xhci-evidence-host' Makefile || {
    echo "ITEM-573: the bounded host suite does not run the evidence proof" >&2
    exit 1
}
# The driver's own host proof links kernel/src/xhci.c; the evidence module is a
# new dependency of that file, so the proof's link line must carry it.
sed -n '/^test-xhci-host:/,/^[^[:space:]]/p' Makefile |
    grep -Fq 'kernel/src/xhci_evidence.c' || {
    echo "ITEM-573: the xHCI host proof does not link the evidence module" >&2
    exit 1
}
grep -Fq 'was=0x[0-9A-F]+ now=0x[0-9A-F]+ ' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-573: the export proof does not require the port's two words" >&2
    exit 1
}
grep -Fq 'state=(LIVE|RELEASED) status=[A-Z_]+ ' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-573: the export proof does not require the controller's state" >&2
    exit 1
}
grep -Fq 'verdict=SEEN detail=[^ ]+ seen=1 bound=[01] controller=' \
    scripts/test-qemu-diag-export.sh || {
    echo "ITEM-573: the export proof does not require the detected pointer" >&2
    exit 1
}

# 21. ITEM-574/576: the Intel USB port-routing stage, the documented 8086:1E31
#     participants, and the registers that show what the firmware left.
#
#     The c5581b6c run proved the xHCI controller saw no device on any of its
#     eight ports (all PORTSC=0x2A0, CCS=0) while the EHCI companion exposed two
#     ports and saw none either.  Linux's early xHCI quirk
#     (drivers/usb/host/pci-quirks.c, usb_enable_intel_xhci_ports()) programs
#     four Intel registers before the controller is halted: USB3PRM into
#     USB3_PSSEN, then USB2PRM into XUSB2PR, both verified by read-back.  The
#     ae67d8ee run then measured the Toshiba's own registers: 0/F/0/F - every
#     switchable USB2 line with EHCI, every SuperSpeed termination off - so the
#     stage was missing and the machine's routing hypothesis was confirmed.
#
#     ITEM-576 corrects how the participants are found: according to the
#     Intel 7 Series chipset description (sections 5.19, 16.2.1.3 and the USB
#     routing registers in 17.1.33-34), the switchable USB2 lines belong to
#     D29:F0 (00:1D.0) and D26:F0 (00:1A.0) is a separate controller carrying
#     the boot medium behind its rate-matching hub.  Two EHCI functions are the
#     documented topology, not an ambiguity; the old "N_PORTS = low protected
#     routing bits" guess and the root-CCS loss check are gone (a root CCS is
#     not proof of the downstream medium: the RMH keeps it set).  The 1E31
#     fields are four bits each and the masks become the routing values.
#
#     The pins below hold the register offsets, the 1E31 participant addresses
#     and fields, the device gate, the mask/read refusals, the write/read-back/
#     restore paths, the topology/field/raw-word export, the RMH regression and
#     the production-path host proof.
grep -Fq '#define XHCI_ROUTE_XUSB2PR 0x00D0U' kernel/include/xhci_route.h || {
    echo "ITEM-574: the USB2 routing register offset is not pinned" >&2
    exit 1
}
grep -Fq '#define XHCI_ROUTE_USB2PRM 0x00D4U' kernel/include/xhci_route.h || {
    echo "ITEM-574: the USB2 routing mask offset is not pinned" >&2
    exit 1
}
grep -Fq '#define XHCI_ROUTE_USB3_PSSEN 0x00D8U' kernel/include/xhci_route.h || {
    echo "ITEM-574: the SuperSpeed enable offset is not pinned" >&2
    exit 1
}
grep -Fq '#define XHCI_ROUTE_USB3PRM 0x00DCU' kernel/include/xhci_route.h || {
    echo "ITEM-574: the SuperSpeed mask offset is not pinned" >&2
    exit 1
}
grep -Fq '#define XHCI_ROUTE_USB2_FIELD 0x0000000FU' kernel/include/xhci_route.h || {
    echo "ITEM-576: the four-bit 1E31 USB2 field is not pinned" >&2
    exit 1
}
grep -Fq '#define XHCI_ROUTE_USB3_FIELD 0x0000000FU' kernel/include/xhci_route.h || {
    echo "ITEM-576: the four-bit 1E31 SuperSpeed field is not pinned" >&2
    exit 1
}
grep -Fq '#define XHCI_ROUTE_1E31_XHCI_DEVICE 0x14U' kernel/include/xhci_route.h || {
    echo "ITEM-576: the xHCI participant address D20:F0 is not pinned" >&2
    exit 1
}
grep -Fq '#define XHCI_ROUTE_1E31_MEDIUM_EHCI_DEVICE 0x1AU' kernel/include/xhci_route.h || {
    echo "ITEM-576: the medium controller D26:F0 is not pinned" >&2
    exit 1
}
grep -Fq '#define XHCI_ROUTE_1E31_ROUTE_EHCI_DEVICE 0x1DU' kernel/include/xhci_route.h || {
    echo "ITEM-576: the routing controller D29:F0 is not pinned" >&2
    exit 1
}
grep -Fq 'case 0x1E31U:' kernel/src/xhci_route.c || {
    echo "ITEM-574: the L875's 7 Series controller is not in the device gate" >&2
    exit 1
}
for item574_outcome in \
    'XHCI_ROUTE_SKIP_NOT_INTEL' \
    'XHCI_ROUTE_SKIP_UNKNOWN_DEVICE' \
    'XHCI_ROUTE_SKIP_UNVERIFIED_TOPOLOGY' \
    'XHCI_ROUTE_REFUSE_TOPOLOGY' \
    'XHCI_ROUTE_REFUSE_READ' \
    'XHCI_ROUTE_REFUSE_MASK' \
    'XHCI_ROUTE_FAIL_WRITE' \
    'XHCI_ROUTE_FAIL_READBACK'; do
    grep -Fq "$item574_outcome" kernel/src/xhci_route.c || {
        echo "ITEM-574: the routing stage lacks $item574_outcome" >&2
        exit 1
    }
done
grep -Fq 'XHCI_ROUTE_PCI_PROGIF_EHCI 0x20U' kernel/src/xhci_route.c || {
    echo "ITEM-576: the participant class filter is not pinned" >&2
    exit 1
}
grep -Fq 'route_read_participant(ops, context, &medium)' kernel/src/xhci_route.c || {
    echo "ITEM-576: the medium controller is not verified by address" >&2
    exit 1
}
grep -Fq 'detail_participant_failure(result, &medium, &route);' kernel/src/xhci_route.c || {
    echo "ITEM-576: a topology refusal does not name the failed participant" >&2
    exit 1
}
grep -Fq '(result->before.xusb2pr & ~XHCI_ROUTE_USB2_FIELD) | writable_usb2' \
    kernel/src/xhci_route.c || {
    echo "ITEM-576: the routing value is not composed from the mask and field" >&2
    exit 1
}
grep -Fq 'route_restore(ops, context, bus, device, function, result);' \
    kernel/src/xhci_route.c || {
    echo "ITEM-574: a failed write is not restored" >&2
    exit 1
}
if grep -Fq 'excluded = (1U << ehci_ports) - 1U;' kernel/src/xhci_route.c; then
    echo "ITEM-576: the N_PORTS-to-routing-bits assumption is still there" >&2
    exit 1
fi
if grep -Fq 'XHCI_PORTSC_CCS' kernel/src/xhci.c | grep -q 'route'; then
    echo "ITEM-576: the routing stage still reads a root CCS" >&2
    exit 1
fi
grep -Fq 'xhci_route_apply();' kernel/src/xhci.c || {
    echo "ITEM-574: the driver does not run the routing stage" >&2
    exit 1
}
grep -Fq 'xhci_route_ports(&ops, NULL, xhci->location.bus, xhci->location.device,' \
    kernel/src/xhci.c || {
    echo "ITEM-576: the driver does not hand the controller's own address to the rule" >&2
    exit 1
}
grep -Fq 'xhci_route_snapshot(xhci, &xhci->route_before);' kernel/src/xhci.c || {
    echo "ITEM-574: the routing registers are not read before initialization" >&2
    exit 1
}
grep -Fq 'xhci_route_snapshot(xhci, &xhci->route_after);' kernel/src/xhci.c || {
    echo "ITEM-574: the routing registers are not read after initialization" >&2
    exit 1
}
grep -Fq 'controller->route_before = session->route_before;' kernel/src/xhci.c || {
    echo "ITEM-574: the routing snapshot does not survive the release" >&2
    exit 1
}
grep -Fq 'controller->route_usb2_field = session->route_usb2_field;' kernel/src/xhci.c || {
    echo "ITEM-576: the routing field does not survive the release" >&2
    exit 1
}
grep -Fq 'controller->settle_ms = session->settle_ms;' kernel/src/xhci.c || {
    echo "ITEM-574: the shared connect window does not survive the release" >&2
    exit 1
}
grep -Fq 'line_text(&line, "[XHCI-ROUTE] pci=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-574: no [XHCI-ROUTE] record is rendered" >&2
    exit 1
}
grep -Fq 'line_text(&line, "[XHCI-LEGACY] pci=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-574: no [XHCI-LEGACY] record is rendered" >&2
    exit 1
}
grep -Fq 'line_text(&line, " field=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-576: the routing field is not in the exported record" >&2
    exit 1
}
grep -Fq 'line_text(&line, " settle_pending=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-574: the controller record hides the shared connect window" >&2
    exit 1
}
for item576_route_test in \
    'the documented topology is routed' \
    'exactly two registers were written' \
    'SuperSpeed terminations are enabled before the USB2 switch-over' \
    'the USB2 routing register is written second' \
    'the measured USB2 mask becomes the routing value' \
    'the measured SuperSpeed mask becomes the PSSEN value' \
    'the four-bit fields and targets are recorded' \
    'the after snapshot is the verified read-back' \
    'the record names both participants and the written values' \
    'bits outside the four-bit USB2 field are preserved' \
    'bits outside the four-bit SuperSpeed field are preserved' \
    'a missing routing EHCI is a topology refusal' \
    'the refusal names the participant that did not answer' \
    'no register is written without the topology' \
    'a wrong function at D26:F0 is not the documented medium controller' \
    'the refusal names the class mismatch' \
    'a non-Intel function at D29:F0 is not the routing companion' \
    'the refusal names the vendor mismatch' \
    'a 1E31 outside D20:F0 is refused' \
    'the refusal names the address that does not match the rule' \
    "another vendor's controller is not routed" \
    'a controller whose rule is not verified is evidence-only' \
    'an unknown Intel controller is not guessed' \
    'a refused configuration read stops the routing' \
    'an all-ones register is a read failure, not a routing state' \
    'a USB2 mask beyond the four-bit field is refused' \
    'an impossible mask is not written' \
    'a SuperSpeed mask beyond the four-bit field is refused' \
    'masks that name no switchable port leave the registers alone' \
    'a refused USB2 write is reported as a write failure' \
    'a transient refusal is undone by the restore' \
    'both routing registers are back to their previous values' \
    'a persistent refusal stays a write failure' \
    'a refused register cannot be restored and the record says so' \
    'a read-back that does not hold the written value is a failure' \
    'the previous routing is restored' \
    'the lying register is put back to its previous value' \
    'a persistent lie stays a read-back failure' \
    'a register that cannot hold the old value is reported unrestored'; do
    grep -Fq -- "$item576_route_test" tests/xhci-route-host.c || {
        echo "ITEM-576: the routing proof lacks $item576_route_test" >&2
        exit 1
    }
done
grep -Fq 'the RMH root port stays connected while the child medium is gone' \
    tests/ehci-walk-host.c || {
    echo "ITEM-576: the RMH root-CCS regression is missing" >&2
    exit 1
}
for item574_evidence_test in \
    "the firmware's routing registers are kept before the visit" \
    "the routing stage's verdict, participants and values are kept" \
    'the BIOS/OS handoff result is kept' \
    'the shared connect window is kept even when per-port waits were 0'; do
    grep -Fq -- "$item574_evidence_test" tests/xhci-evidence-host.c || {
        echo "ITEM-574: the evidence proof lacks $item574_evidence_test" >&2
        exit 1
    }
done
grep -Fq 'build/test-xhci-route-host' Makefile || {
    echo "ITEM-574: the routing proof is not built" >&2
    exit 1
}
grep -Fq '$(MAKE) test-xhci-route-host' Makefile || {
    echo "ITEM-574: the bounded host suite does not run the routing proof" >&2
    exit 1
}
grep -Fq 'phase=BEFORE read=(OK|FAIL)' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-574: the export proof does not require the routing registers" >&2
    exit 1
}
grep -Fq 'action=SKIP_NOT_INTEL' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-574: the export proof does not require the non-Intel refusal" >&2
    exit 1
}
grep -Fq 'settle_ms=[0-9]+ settle_bound_ms=[0-9]+ settle_pending=' \
    scripts/test-qemu-diag-export.sh || {
    echo "ITEM-574: the export proof does not require the shared window" >&2
    exit 1
}

# 23. ITEM-577: the DMA geometry the controller asked for, and the first fatal
#     condition kept before cleanup.
#
#     The 1ad25c25 run applied the routing and the connections appeared, but
#     the controller halted with HSE/HCH and the driver reported
#     ENABLE_SLOT_TIMEOUT: no scratchpad buffers had ever been allocated (the
#     driver read no HCSPARAMS2 and left DCBAA[0] zero), and every slot's
#     Device Context was placed one record apart instead of one 32-record area
#     apart.  The pins below hold the scratchpad allocation and its DCBAA[0]
#     publication before USBCMD.RS, the run-time pool layout with the 32-record
#     slot areas, the memory refusals, the first fatal snapshot and the command
#     stop, the exported DMA/fatal records, the production-path host proof and
#     the QEMU file assertions.
grep -Fq 'static uint32_t xhci_scratchpad_count(uint32_t hcsparams2) {' \
    kernel/src/xhci.c || {
    echo "ITEM-577: the HCSPARAMS2 scratchpad count has no decoder" >&2
    exit 1
}
grep -Fq 'return ((hcsparams2 >> 21U) & 0x1FU) << 5U |' kernel/src/xhci.c || {
    echo "ITEM-577: the high five scratchpad bits are not read" >&2
    exit 1
}
grep -Fq 'xhci->scratchpad_count = xhci_scratchpad_count(xhci->hcs_params2);' \
    kernel/src/xhci.c || {
    echo "ITEM-577: the scratchpad count is not taken from HCSPARAMS2" >&2
    exit 1
}
grep -Fq 'xhci->scratchpad_page_bytes = 4096U;' kernel/src/xhci.c || {
    echo "ITEM-577: the supported PAGESIZE does not select the scratchpad page" >&2
    exit 1
}
grep -Fq 'xhci->dcbaa[0] = xhci->scratchpad_array_physical;' kernel/src/xhci.c || {
    echo "ITEM-577: DCBAA[0] does not name the scratchpad pointer array" >&2
    exit 1
}
grep -Fq 'XHCI_DEVICE_CONTEXT_ENTRIES * (uint32_t)xhci->context_bytes;' \
    kernel/src/xhci.c || {
    echo "ITEM-577: the slot area is not 32 context records" >&2
    exit 1
}
grep -Fq 'xhci->device_contexts_bytes = xhci->slots_enabled * slot_area;' \
    kernel/src/xhci.c || {
    echo "ITEM-577: the device context region does not follow the slot areas" >&2
    exit 1
}
grep -Fq '(uint64_t)(slot - 1U) * xhci->slot_area_bytes;' kernel/src/xhci.c || {
    echo "ITEM-577: the DCBAA entry still uses the record size as the stride" >&2
    exit 1
}
grep -Fq 'xhci->scratchpad_array_bytes = xhci->scratchpad_count * 8U;' \
    kernel/src/xhci.c || {
    echo "ITEM-577: the scratchpad pointer array size is not computed" >&2
    exit 1
}
grep -Fq 'if (pool_bytes > XHCI_DMA_MAX_BYTES) {' kernel/src/xhci.c || {
    echo "ITEM-577: the pooled regions are not bounded before allocation" >&2
    exit 1
}
grep -Fq '(void)pmm_free_contiguous_pages(xhci->dma_physical, xhci->dma_pages);' \
    kernel/src/xhci.c || {
    echo "ITEM-577: a too-small reused pool is not returned" >&2
    exit 1
}
grep -Fq 'static void xhci_fatal_capture(const char *cause) {' kernel/src/xhci.c || {
    echo "ITEM-577: the first fatal controller snapshot has no capture" >&2
    exit 1
}
grep -Fq 'static bool xhci_fatal_stop(void) {' kernel/src/xhci.c || {
    echo "ITEM-577: HSE/HCH is not checked as a command gate" >&2
    exit 1
}
grep -Fq 'if ((spins & 0x3FU) == 0U && xhci_fatal_stop()) {' kernel/src/xhci.c || {
    echo "ITEM-577: the fatal condition is not checked while the wait runs" >&2
    exit 1
}
grep -Fq '*detail = xhci->fatal_valid ? xhci->fatal_cause' kernel/src/xhci.c || {
    echo "ITEM-577: a fatal refusal still reports a command timeout" >&2
    exit 1
}
grep -Fq 'if (xhci->fatal_valid) {' kernel/src/xhci.c || {
    echo "ITEM-577: a fatal controller is not stopped" >&2
    exit 1
}
grep -Fq 'line_text(&line, "[XHCI-DMA] pci=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-577: no [XHCI-DMA] record is rendered" >&2
    exit 1
}
grep -Fq 'line_text(&line, "[XHCI-FATAL] pci=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-577: no [XHCI-FATAL] record is rendered" >&2
    exit 1
}
grep -Fq 'line_text(&line, " spb=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-577: the scratchpad count is not exported" >&2
    exit 1
}
grep -Fq 'line_text(&line, " slot=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-577: the slot area size is not exported" >&2
    exit 1
}
grep -Fq 'line_text(&line, " dcbaa0=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-577: DCBAA[0] is not exported" >&2
    exit 1
}
for item577_test in \
    'a controller with 16 scratchpads starts' \
    "HCSPARAMS2's scratchpad count is read, not a constant" \
    'the supported PAGESIZE selects 4 KiB scratchpad pages' \
    'DCBAA[0] names the scratchpad pointer array' \
    'DCBAA[0] was published before USBCMD.RS was written' \
    "the controller's DCBAA[0] is the pointer array" \
    'every scratchpad entry is a page-aligned page inside the pool' \
    'every scratchpad page is zeroed' \
    'a controller with no scratchpads starts' \
    'a zero HCSPARAMS2 count is honoured' \
    'DCBAA[0] stays zero when the controller needs no scratchpad' \
    'no scratchpad page is allocated for a zero count' \
    'an allocation failure refuses the controller' \
    'the refusal is the DMA status, not a later command timeout' \
    'a pool larger than the bound is refused, not half-allocated' \
    'the slot-area controller starts' \
    'the context record size follows HCCPARAMS1.CSZ' \
    'the slot area is 32 records, not one' \
    'both enabled slots have a device context address' \
    'adjacent slots are one full area apart' \
    'each slot area lies inside the pool' \
    'the first and second slot areas do not overlap' \
    'the two-slot controller starts' \
    'slot 1 is addressed' \
    'slot 2 is addressed' \
    "slot 2's write leaves slot 1's EP0 context untouched" \
    "the two slots' EP0 contexts are different records" \
    "the second slot's area is a full slot area away" \
    'the fatal-snapshot controller starts' \
    'a command after HSE is refused' \
    'the first fatal condition is recorded' \
    'the snapshot names HSE as the cause' \
    "the snapshot keeps the controller's command/status registers" \
    'the refusal names the fatal cause, not a command timeout' \
    'the fatal controller is given no further command doorbell' \
    'a later fatal capture does not displace the first snapshot' \
    'no further command is submitted to a fatal controller' \
    'the second refusal never reaches the command ring'; do
    grep -Fq -- "$item577_test" tests/xhci-dma-host.c || {
        echo "ITEM-577: the DMA proof lacks $item577_test" >&2
        exit 1
    }
done
grep -Fq 'build/test-xhci-dma-host' Makefile || {
    echo "ITEM-577: the DMA proof is not built" >&2
    exit 1
}
grep -Fq '$(MAKE) test-xhci-dma-host' Makefile || {
    echo "ITEM-577: the bounded host suite does not run the DMA proof" >&2
    exit 1
}
grep -Fq 'no [XHCI-DMA] record names the pool geometry' \
    scripts/test-qemu-diag-export.sh || {
    echo "ITEM-577: the export proof does not require the DMA record" >&2
    exit 1
}
grep -Fq 'spb=0 page=0 ctx=32 slot=1024' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-577: the export proof does not pin the QEMU geometry" >&2
    exit 1
}

# 24. ITEM-578: the report descriptor reaches its parser, and the second role
#     gets a fresh USB address.
#
#     The a853d322 run read the device on port 2 and refused it as "not a
#     keyboard" while the pointer record still said no pointer attempt had been
#     made.  Two faults produced that record: the Data Stage TRB named the
#     generic descriptor buffer while the report-descriptor parser read the
#     buffer next to it (so a successful transfer was classified from zeros),
#     and the pointer role re-addressed a device the keyboard role had already
#     addressed, without a port reset (so its Address Device failed at the
#     wire).  The pins below hold the one-buffer handover, the role's own
#     status/read result, the forced port reset between roles and on re-bind,
#     the per-role evidence and its [XHCI-RDESC] record, and the production-path
#     host proof.
grep -Fq 'static uint32_t xhci_device_report_descriptor_offset(uint8_t role) {' \
    kernel/src/xhci.c || {
    echo "ITEM-578: the report-descriptor buffer has no shared offset" >&2
    exit 1
}
grep -Fq 'xhci_trb_data(1U, xhci_dma_physical(data_buffer), length,' \
    kernel/src/xhci.c || {
    echo "ITEM-578: the Data Stage TRB does not name the caller's buffer" >&2
    exit 1
}
grep -Fq 'device, xhci_device_report_descriptor_offset(device->role),' \
    kernel/src/xhci.c || {
    echo "ITEM-578: the report-descriptor request names another buffer" >&2
    exit 1
}
grep -Fq 'uint8_t *descriptor = xhci_dma_at(buffer);' kernel/src/xhci.c || {
    echo "ITEM-578: the parser does not read the buffer the read names" >&2
    exit 1
}
grep -Fq 'device->report_descriptor_read = XHCI_EVIDENCE_RDESC_READ_FAILED;' \
    kernel/src/xhci.c || {
    echo "ITEM-578: a failed report-descriptor read is not recorded" >&2
    exit 1
}
grep -Fq 'xhci_detail_port(port, "REPORT_DESCRIPTOR_READ",' kernel/src/xhci.c || {
    echo "ITEM-578: a failed read is not published as a read failure" >&2
    exit 1
}
grep -Fq 'xhci_attach_refuse(device, XHCI_STATUS_NOT_A_MOUSE,' kernel/src/xhci.c || {
    echo "ITEM-578: the pointer's classification has no status of its own" >&2
    exit 1
}
grep -Fq 'XHCI_STATUS_NOT_A_MOUSE,' kernel/include/xhci.h || {
    echo "ITEM-578: NOT_A_MOUSE is not part of the status set" >&2
    exit 1
}
grep -Fq 'return "NOT_A_MOUSE";' kernel/src/xhci_trb.c || {
    echo "ITEM-578: the pointer status has no name" >&2
    exit 1
}
grep -Fq 'static bool xhci_port_reset(uint32_t port, uint8_t *speed, bool force) {' \
    kernel/src/xhci.c || {
    echo "ITEM-578: the port reset cannot be forced" >&2
    exit 1
}
grep -Fq 'if (force || (value & XHCI_PORTSC_PED) == 0U) {' kernel/src/xhci.c || {
    echo "ITEM-578: an enabled port is never reset" >&2
    exit 1
}
grep -Fq 'if (!xhci_port_reset(port, &reopened, true) ||' kernel/src/xhci.c || {
    echo "ITEM-578: the role transition does not reset the port" >&2
    exit 1
}
grep -Fq 'ready = xhci_port_reset(port, &speed, true);' kernel/src/xhci.c || {
    echo "ITEM-578: the re-bind still skips the reset of an enabled port" >&2
    exit 1
}
grep -Fq 'const bool fallback_parsed =' kernel/src/xhci.c || {
    echo "ITEM-578: the SET_PROTOCOL fallback has no parsed result" >&2
    exit 1
}
grep -Fq '? xhci_parse_mouse_report_layout(device, (uint8_t)port,' \
    kernel/src/xhci.c || {
    echo "ITEM-578: the fallback does not parse for the role it binds" >&2
    exit 1
}
grep -Fq 'xhci_attach_refuse(device, xhci->status, "ADDRESS_DEVICE",' \
    kernel/src/xhci.c || {
    echo "ITEM-578: an Address Device refusal is not published for the role" >&2
    exit 1
}
grep -Fq 'xhci_attach_refuse(device, XHCI_STATUS_OK, "BOUND", "OK");' \
    kernel/src/xhci.c || {
    echo "ITEM-578: a bound role does not record its binding stage" >&2
    exit 1
}
grep -Fq 'kept->attempted = device->attempt_made ? 1U : 0U;' kernel/src/xhci.c || {
    echo "ITEM-578: the role attempt does not reach the durable evidence" >&2
    exit 1
}
grep -Fq 'uint8_t attempted;' kernel/include/xhci_evidence.h || {
    echo "ITEM-578: the evidence role has no attempt record" >&2
    exit 1
}
grep -Fq '#define XHCI_EVIDENCE_RDESC_READ_FAILED 2U' kernel/include/xhci_evidence.h || {
    echo "ITEM-578: the evidence has no failed-read state" >&2
    exit 1
}
grep -Fq 'line_text(&line, "[XHCI-RDESC] pci=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-578: no [XHCI-RDESC] record is rendered" >&2
    exit 1
}
grep -Fq 'line_text(&line, " read=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-578: the report-descriptor read result is not exported" >&2
    exit 1
}
grep -Fq 'line_hex_bytes(&line, role->report_descriptor_bytes, copies);' \
    kernel/src/xhci_evidence.c || {
    echo "ITEM-578: the report-descriptor bytes are not exported" >&2
    exit 1
}
grep -Fq 'if (mouse->detail[0] == '\''\0'\'') {' kernel/src/xhci_evidence.c || {
    echo "ITEM-578: the pointer verdict still borrows another role's detail" >&2
    exit 1
}
grep -Fq '"PORT_CONNECTED");' kernel/src/xhci_evidence.c || {
    echo "ITEM-578: a detected pointer with no attempt has no own detail" >&2
    exit 1
}
grep -Fq 'the pointer verdict never borrows the controller' \
    tests/xhci-evidence-host.c || {
    echo "ITEM-578: the evidence proof does not pin the pointer's own detail" >&2
    exit 1
}
for item578_test in \
    'the keyboard parser accepts the descriptor the device sent' \
    'the keyboard layout is built from the descriptor' \
    'the Data Stage TRB names the report-descriptor buffer' \
    'the pointer parser accepts the descriptor the device sent' \
    'the pointer layout is built from the descriptor' \
    "the pointer's Data Stage TRB names the report-descriptor buffer" \
    'a failed report-descriptor transfer refuses the role' \
    'the refusal is an enumeration failure, not a descriptor verdict' \
    'the detail names the failed read' \
    "a failed read is never reported as 'not a keyboard'" \
    'the model saw exactly one report-descriptor request' \
    "the keyboard role refuses the pointer's descriptor" \
    'a second Address Device without a reset fails' \
    'the model refused the address of a device that is still addressed' \
    'the refusal names the Address Device stage' \
    'the production walk binds the pointer on the same controller' \
    'the pointer role is ready' \
    "the bound pointer carries the descriptor's own layout" \
    'the walk reset the port before the second role addressed the device' \
    'the production walk never re-addresses an addressed device' \
    'a boot mouse that refuses SET_PROTOCOL binds through its descriptor' \
    'the fallback marked the pointer as a report-protocol device' \
    "the fallback parsed the pointer's own layout" \
    "the keyboard's record names its own stage" \
    "the keyboard's reason is its own status" \
    "the pointer's record is a separate, bound attempt" \
    "the pointer's reason is its own binding, not the keyboard's refusal" \
    "both roles' report-descriptor reads succeeded" \
    "the read length is the descriptor's own length" \
    'the exported bytes are the descriptor the device sent' \
    'the classification result is kept per role' \
    'the pointer verdict is bound' \
    'the keyboard record is rendered with its own reason' \
    'the pointer record is rendered with its own binding' \
    'the report-descriptor record names its result, length and bytes' \
    'the refused device keeps two role records' \
    'the keyboard refused the descriptor as a keyboard' \
    'the pointer refused the same descriptor as a pointer' \
    "the pointer's record never carries the keyboard's reason" \
    'the pointer is published as seen, not bound' \
    "the pointer verdict names the pointer's own stage"; do
    grep -Fq -- "$item578_test" tests/xhci-handover-host.c || {
        echo "ITEM-578: the handover proof lacks $item578_test" >&2
        exit 1
    }
done
grep -Fq 'build/test-xhci-handover-host' Makefile || {
    echo "ITEM-578: the handover proof is not built" >&2
    exit 1
}
grep -Fq '$(MAKE) test-xhci-handover-host' Makefile || {
    echo "ITEM-578: the bounded host suite does not run the handover proof" >&2
    exit 1
}
sed -n '/^test-xhci-handover-host:/,/^[^[:space:]]/p' Makefile |
    grep -Fq 'kernel/src/xhci_evidence.c' || {
    echo "ITEM-578: the handover proof does not link the evidence module" >&2
    exit 1
}

# 25. ITEM-579: one physical device is one path, and a Halted endpoint is
#     repaired or taken out of service.
#
#     The 10231684 run bound the pointer (iface 0, ep81) and the keyboard
#     (iface 1, ep82) of one 04D9:A09F as two independent devices: the keyboard
#     retry reset the port, re-addressed the device with a new slot, and left
#     the pointer's endpoint Halted after a USB transaction error - `reports=0`
#     while `transfers=17`.  The pins below hold the shared physical path (one
#     slot/address/EP0/configuration, per-interface endpoints and queues), the
#     re-bind that joins a live path instead of resetting it, the endpoint
#     repair commands, the split submitted/completed/decoded counters and the
#     wheel the pointer event carries.
grep -Fq 'struct xhci_device_path {' kernel/src/xhci.c || {
    echo "ITEM-579: there is no physical device path" >&2
    exit 1
}
grep -Fq 'static struct xhci_device_path *xhci_session_path(struct xhci_session *session,' \
    kernel/src/xhci.c || {
    echo "ITEM-579: a port cannot resolve its physical path" >&2
    exit 1
}
grep -Fq 'static bool xhci_path_live(const struct xhci_device_path *path) {' \
    kernel/src/xhci.c || {
    echo "ITEM-579: a live path is not distinguished from a free one" >&2
    exit 1
}
grep -Fq 'static void xhci_attach_abort(struct xhci_hid_device *device) {' \
    kernel/src/xhci.c || {
    echo "ITEM-579: detaching one role from its path has no entry point" >&2
    exit 1
}
grep -Fq 'static bool xhci_path_establish(struct xhci_hid_device *device,' \
    kernel/src/xhci.c || {
    echo "ITEM-579: the physical path is not established once" >&2
    exit 1
}
grep -Fq 'path = xhci_session_path(xhci, (uint8_t)port);' kernel/src/xhci.c || {
    echo "ITEM-579: the attach does not join an existing path" >&2
    exit 1
}
grep -Fq 'config = usb_hid_parse_config_role(path->config, path->config_length, role);' \
    kernel/src/xhci.c || {
    echo "ITEM-579: each role does not walk the device's cached configuration" >&2
    exit 1
}
grep -Fq 'path->roles |= (uint8_t)(1U << device->role);' kernel/src/xhci.c || {
    echo "ITEM-579: a binding is not attached to its path" >&2
    exit 1
}
grep -Fq 'if (path != NULL && xhci_path_live(path)) {' kernel/src/xhci.c || {
    echo "ITEM-579: the re-bind still resets a live path" >&2
    exit 1
}
grep -Fq 'xhci_session_path(xhci, (uint8_t)port) == NULL' kernel/src/xhci.c || {
    echo "ITEM-579: the role transition resets a live path" >&2
    exit 1
}
grep -Fq 'XHCI_TRB_RESET_ENDPOINT = 14,' kernel/include/xhci_trb.h || {
    echo "ITEM-579: Reset Endpoint has no TRB type" >&2
    exit 1
}
grep -Fq 'XHCI_TRB_SET_TR_DEQUEUE = 16,' kernel/include/xhci_trb.h || {
    echo "ITEM-579: Set TR Dequeue Pointer has no TRB type" >&2
    exit 1
}
grep -Fq 'struct xhci_trb xhci_trb_reset_endpoint(uint8_t cycle, uint8_t slot_id,' \
    kernel/src/xhci_trb.c || {
    echo "ITEM-579: Reset Endpoint has no TRB builder" >&2
    exit 1
}
grep -Fq 'struct xhci_trb xhci_trb_set_tr_dequeue(uint8_t cycle, uint8_t slot_id,' \
    kernel/src/xhci_trb.c || {
    echo "ITEM-579: Set TR Dequeue Pointer has no TRB builder" >&2
    exit 1
}
grep -Fq 'static bool xhci_endpoint_restart(struct xhci_hid_device *device) {' \
    kernel/src/xhci.c || {
    echo "ITEM-579: a Halted endpoint has no repair" >&2
    exit 1
}
grep -Fq 'if (device->endpoint_state == XHCI_EP_STATE_HALTED) {' kernel/src/xhci.c || {
    echo "ITEM-579: the completion path does not detect a Halted endpoint" >&2
    exit 1
}
grep -Fq 'xhci_publish_endpoint_action(device, "LOST", xhci->detail);' \
    kernel/src/xhci.c || {
    echo "ITEM-579: an unrepairable endpoint keeps pretending to be alive" >&2
    exit 1
}
grep -Fq 'static bool xhci_path_restore(struct xhci_session *session,' \
    kernel/src/xhci.c || {
    echo "ITEM-579: a required physical reset has no path-wide restore" >&2
    exit 1
}
grep -Fq '++device->completion_count;' kernel/src/xhci.c || {
    echo "ITEM-579: completions are not counted apart from submissions" >&2
    exit 1
}
grep -Fq 'line_text(&line, " submitted=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-579: the role record does not name the submitted count" >&2
    exit 1
}
grep -Fq 'line_text(&line, " completions=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-579: the role record does not name the completion count" >&2
    exit 1
}
grep -Fq 'line_text(&line, " restarts=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-579: the role record does not name the repair count" >&2
    exit 1
}
grep -Fq 'line_text(&line, " wheels=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-579: the pointer record does not name the wheel count" >&2
    exit 1
}
grep -Fq 'line_text(&line, " epstate=");' kernel/src/xhci_evidence.c || {
    echo "ITEM-579: the role record does not name the endpoint state" >&2
    exit 1
}
grep -Fq '#define USB_HID_USAGE_GENERIC_WHEEL 0x38U' kernel/src/usb_hid.c || {
    echo "ITEM-579: the wheel usage has no name" >&2
    exit 1
}
grep -Fq 'found.wheel_offset = (uint8_t)field_start;' kernel/src/usb_hid.c || {
    echo "ITEM-579: the pointer layout does not carry the wheel" >&2
    exit 1
}
grep -Fq 'event_out->wheel = (int16_t)wheel_raw;' kernel/src/usb_hid.c || {
    echo "ITEM-579: the pointer event does not carry the wheel" >&2
    exit 1
}
grep -Fq 'uint32_t usb_hid_mouse_state_wheels(const struct usb_hid_mouse_state *state)' \
    kernel/src/usb_hid.c || {
    echo "ITEM-579: the wheel has no counter" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE(" mouse_wheels=");' kernel/src/kernel.c || {
    echo "ITEM-579: the production record does not name the wheel" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE(" mouse_completions=");' kernel/src/kernel.c || {
    echo "ITEM-579: the production record does not split the pointer counters" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE(" kbd_completions=");' kernel/src/kernel.c || {
    echo "ITEM-579: the production record does not split the keyboard counters" >&2
    exit 1
}
for item579_test in \
    'the composite keyboard interface is the silent candidate' \
    'the walk binds the pointer and leaves the keyboard to the retry' \
    'the pointer is bound after the walk' \
    'the model delivered a pointer report to the armed endpoint' \
    'the pointer decoded the report before the retry' \
    'the button press was decoded' \
    'a pointer event was queued' \
    'the event carries the movement the report carried' \
    'the retry binds the keyboard in the pointer' \
    'the keyboard role is bound after the retry' \
    'both roles of the composite device share one slot' \
    'each role keeps its own interrupt endpoint' \
    'the retry performed no port reset on the live pointer' \
    'the retry performed no second Address Device' \
    'the pointer decoded a report after the keyboard re-bind' \
    'both movements were counted' \
    'the second event carries the second movement' \
    'the separate keyboard on the other port is bound' \
    'the walk keeps searching after the silent candidate' \
    'the retry does not replace the bound keyboard' \
    'the retry never resets the pointer' \
    'the two physical devices have their own slots' \
    'the pointer decodes while the separate keyboard is bound' \
    'the driver reset the halted endpoint' \
    'the driver re-established the dequeue pointer' \
    'the failed transfer was not decoded as a report' \
    'a recovered endpoint keeps the pointer ready' \
    'the next report decodes after the endpoint recovery' \
    'the driver attempted the endpoint reset' \
    'a failed reset takes the pointer out of readiness' \
    'the machine sees the pointer as no longer attached' \
    'both composite roles are live for the restore proof' \
    'the per-endpoint repair was attempted first' \
    'a physical reset restores every role of the device' \
    'the restore reset the port once' \
    'the restore re-established the device once and the second role joined it' \
    'both roles share the restored path' \
    'the restored pointer endpoint accepts a report' \
    'the pointer decodes after the device-wide restore' \
    'the report-protocol pointer is bound' \
    'the pointer was bound through its report descriptor' \
    'the report-protocol report decoded' \
    'the event carries movement and the wheel'; do
    grep -Fq -- "$item579_test" tests/xhci-composite-host.c || {
        echo "ITEM-579: the composite proof lacks $item579_test" >&2
        exit 1
    }
done
grep -Fq 'build/test-xhci-composite-host' Makefile || {
    echo "ITEM-579: the composite proof is not built" >&2
    exit 1
}
grep -Fq '$(MAKE) test-xhci-composite-host' Makefile || {
    echo "ITEM-579: the bounded host suite does not run the composite proof" >&2
    exit 1
}

# 26. ITEM-580: the export takes one live snapshot, and it cannot erase history.
#
#     The 10231684 report published `[XHCI-INPUT] role=MOUSE ... completions=0
#     reports=0 epstate=DISABLED` beside a live `[INPUT-DIAG]` that had counted
#     thousands of reports: the durable evidence was frozen at initialization.
#     The export now refreshes the working fields of every live session (and
#     the pointer verdict) once, before composing, and the merge keeps the
#     visit's evidence: the original verdict and refusal reason, the routing,
#     handoff, port and protocol records, the DMA geometry, the first fatal
#     snapshot and every released session's record.  `[INPUT-DIAG]` prints
#     `submitted=` and `completions=` from the two counters it used to conflate
#     under `completed=`.
grep -Fq 'static void xhci_evidence_collect_session(' kernel/src/xhci.c || {
    echo "ITEM-580: the visit and the live state have no shared collector" >&2
    exit 1
}
grep -Fq 'static void xhci_evidence_refresh_live(void) {' kernel/src/xhci.c || {
    echo "ITEM-580: the export has no live snapshot" >&2
    exit 1
}
grep -Fq 'void xhci_evidence_controller_refresh(' \
    kernel/src/xhci_evidence.c || {
    echo "ITEM-580: the live merge has no store entry point" >&2
    exit 1
}
grep -Fq '    xhci_evidence_refresh_live();' kernel/src/xhci.c || {
    echo "ITEM-580: the report is composed without a live snapshot" >&2
    exit 1
}
grep -Fq 'kept->usbcmd = live->usbcmd;' kernel/src/xhci_evidence.c || {
    echo "ITEM-580: the merge does not take the running registers" >&2
    exit 1
}
grep -Fq 'kept->roles[role] = live->roles[role];' kernel/src/xhci_evidence.c || {
    echo "ITEM-580: the merge does not take the live role records" >&2
    exit 1
}
grep -Fq 'xhci_diag_text(line, " submitted=");' kernel/src/xhci.c || {
    echo "ITEM-580: the role line does not name submitted" >&2
    exit 1
}
grep -Fq 'xhci_diag_text(line, " completions=");' kernel/src/xhci.c || {
    echo "ITEM-580: the role line does not name completions" >&2
    exit 1
}
grep -Fq 'xhci_diag_u64(line, device->completion_count);' kernel/src/xhci.c || {
    echo "ITEM-580: the role line still prints the submitted counter twice" >&2
    exit 1
}
for item580_test in \
    'the export carries all three input records' \
    'the role line names submitted and completions, not completed' \
    'the live role line carries the three counters' \
    'the kept role record agrees with the live role line' \
    'the pointer record agrees with the live role line' \
    'the export shows submitted greater than completions and both real' \
    'the kept role record names the running endpoint, not DISABLED' \
    'the kept role record still says the role is bound' \
    'the second export carries the newer report count' \
    'the second kept record agrees with the second live line' \
    'the second pointer record agrees with the second live line' \
    'the kept record grew between the two exports'; do
    grep -Fq -- "$item580_test" tests/xhci-composite-host.c || {
        echo "ITEM-580: the live-export proof lacks $item580_test" >&2
        exit 1
    }
done
for item580_evidence_test in \
    "the live refresh keeps the visit's refusal verdict" \
    'the live refresh takes the running registers' \
    'the live refresh does not claim a released session is live' \
    'the first fatal snapshot survives the live refresh' \
    'the routing evidence survives the live refresh' \
    'the port evidence survives the live refresh' \
    'the live role counters and endpoint state are the ones exported' \
    'the released controller keeps its own role counters' \
    'the live controller takes the refreshed counters'; do
    grep -Fq -- "$item580_evidence_test" tests/xhci-evidence-host.c || {
        echo "ITEM-580: the evidence proof lacks $item580_evidence_test" >&2
        exit 1
    }
done

# 27. ITEM-581: the installed machine's own answers - the boot log on the
#     screen, the live network state and the panel/entropy readiness.
#
#     The owner's installed Toshiba showed no address, `bootlog` returned no
#     visible output, and `credentials` refused with NO_RANDOM.  The dump
#     published every line through the record funnel, which is serial-only;
#     the console had no network view; the export carried neither the current
#     addressing state nor the panel gate nor the entropy stage.  The pins
#     below hold the screen sink, the explicit EMPTY/UNAVAILABLE states, the
#     one status composition shared by console and export, and the entropy
#     CPUID/stage evidence.
grep -Fq 'appliance_boot_log_store_line_display(&line);' \
    kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-581: the boot log dump does not paint its lines" >&2
    exit 1
}
grep -Fq '"BOOTLOG: EMPTY records=0 window=0' \
    kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-581: an empty reading does not say EMPTY" >&2
    exit 1
}
grep -Fq 'kernel_boot_log_display,' kernel/src/kernel.c || {
    echo "ITEM-581: the store is not bound to the display sink" >&2
    exit 1
}
grep -Fq 'static void kernel_boot_log_display(const char *text) {' \
    kernel/src/kernel.c || {
    echo "ITEM-581: no display sink mirrors the reading" >&2
    exit 1
}
for item581_test in \
    'the dump paints its own header on the screen' \
    'the header names the staged record count' \
    'the newest staged record is on the screen' \
    'the dump ends on the screen by saying how much it showed' \
    'the reading is never published back into the journal' \
    'the dump leaves the ring exactly as it found it' \
    'the second dump ends the same way' \
    'the second dump publishes nothing either' \
    'the console dump answers UNAVAILABLE on the screen'; do
    grep -Fq -- "$item581_test" tests/appliance-boot-log-store-host.c || {
        echo "ITEM-581: the boot log proof lacks $item581_test" >&2
        exit 1
    }
done
grep -Fq 'void kernel_production_status_report(kernel_status_sink_fn sink,' \
    kernel/src/kernel.c || {
    echo "ITEM-581: there is no one status composition" >&2
    exit 1
}
for item581_line in '[NET-STATUS]' '[NET-ADDR]' '[PANEL]' \
    '[RNG]' '[ADMIN]'; do
    grep -Fq -- "$item581_line" kernel/src/kernel.c || {
        echo "ITEM-581: the status report lacks $item581_line" >&2
        exit 1
    }
done
grep -Fq 'kernel_production_status_report(diag_sink, NULL);' \
    kernel/src/diag_export.c || {
    echo "ITEM-581: diag save does not carry the status report" >&2
    exit 1
}
grep -Fq 'kernel_production_status_report(console_status_report_sink, NULL);' \
    kernel/src/console.c || {
    echo "ITEM-581: the console has no live network status" >&2
    exit 1
}
grep -Fq 'discover=' kernel/src/kernel.c || {
    echo "ITEM-581: the status report does not name the DHCP exchange" >&2
    exit 1
}
grep -Fq 'appliance_production_entropy_failure_stage' kernel/include/appliance_production_entropy.h || {
    echo "ITEM-581: the entropy failure stage has no owner" >&2
    exit 1
}
grep -Fq 'appliance_production_entropy_source_for_cpuid' kernel/include/appliance_production_entropy.h || {
    echo "ITEM-581: the CPU profile selection is not a pure function" >&2
    exit 1
}
grep -Fq 'a CPU with RDRAND only selects RDRAND' tests/entropy-diagnostics-host.c || {
    echo "ITEM-581: the CPU profiles are not proven" >&2
    exit 1
}
grep -Fq '\[NET-STATUS\]' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-581: the export proof does not require the network record" >&2
    exit 1
}
# A medium without the production owners still links the console and `diag
# save` and still prints all five records: the fields exist and say
# UNAVAILABLE, and `reason=` names the missing plane.  Without this file the
# diagnostic and installer media failed to link the console's `network`
# command, and an export from them carried no status records at all.
grep -Fq '#if !defined(HFOC_PRODUCTION_NETWORK) || !defined(HFOC_PRODUCTION_TLS)' \
    kernel/src/kernel_status_report_profile.c || {
    echo "ITEM-581: the profile fallback is not compiled only without the owners" >&2
    exit 1
}
for item581_profile_line in \
    '[NET-STATUS] device=NONE readiness=UNAVAILABLE' \
    '[NET-ADDR] state=UNAVAILABLE lease=NONE attempt=0/0' \
    'reason=PROFILE_NO_PRODUCTION_NETWORK' \
    '[PANEL] v=0 state=UNAVAILABLE reason=PROFILE_NO_PRODUCTION_TLS' \
    '[RNG] source=NONE state=UNAVAILABLE stage=NONE' \
    '[ADMIN] state=UNAVAILABLE last=UNKNOWN'; do
    grep -Fq -- "$item581_profile_line" \
        kernel/src/kernel_status_report_profile.c || {
        echo "ITEM-581: the profile fallback lacks $item581_profile_line" >&2
        exit 1
    }
done
grep -Fq 'defined(HFOC_PRODUCTION_TLS)' kernel/src/kernel.c || {
    echo "ITEM-581: the production report is not compiled only with its owners" >&2
    exit 1
}
# The serving render is bounded at 704 bytes; a 512-byte line would truncate
# the [PANEL] record and hide the certificate and listener it exists to name.
grep -Fq '#define KERNEL_STATUS_LINE_MAX 768U' kernel/src/kernel.c || {
    echo "ITEM-581: the status line buffer cannot hold the serving render" >&2
    exit 1
}
grep -Fq '"[ADMIN] state="' kernel/src/kernel.c || {
    echo "ITEM-581: the administrator gate is not a machine field" >&2
    exit 1
}
grep -Fq 'reason=PROFILE_NO_PRODUCTION_NETWORK' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-581: the export proof does not require the medium's reason" >&2
    exit 1
}
grep -Fq 'cpuid_max=[0-9]+ rdrand=[01] rdseed=[01]' \
    scripts/test-qemu-diag-export.sh || {
    echo "ITEM-581: the export proof does not require the entropy fields" >&2
    exit 1
}
grep -Fq '^\[PANEL\] v=[01] state=' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-581: the export proof does not accept the medium's panel record" >&2
    exit 1
}
grep -Fq '^\[ADMIN\] state=' scripts/test-qemu-diag-export.sh || {
    echo "ITEM-581: the export proof does not require the administrator record" >&2
    exit 1
}
# The console answer is proven on the machine that has a live lease and a
# serving panel: the production network proof types `network` and requires the
# leased address, the panel gate, the CPU profile and the administrator state
# in the screen transcript.
grep -Fq 'for character in "network":' scripts/test-qemu-production-network.sh || {
    echo "ITEM-581: the network proof never types the console command" >&2
    exit 1
}
for item581_console_field in \
    '"state=ACTIVE", "lease=ACQUIRED", "address=10.0.2.15/24"' \
    '"gateway=10.0.2.2", "dns=10.0.2.2"' \
    '"v=1 state=", "listener="' \
    'wait_console_record("[RNG]")' \
    'wait_console_record("[ADMIN]")'; do
    grep -Fq -- "$item581_console_field" \
        scripts/test-qemu-production-network.sh || {
        echo "ITEM-581: the console proof lacks $item581_console_field" >&2
        exit 1
    }
done

# 28. ITEM-582: the diagnostic export from the installed system to a separate,
#     separately prepared USB stick.
#
#     The installed Toshiba finished bootlog but diag save answered
#     NO_MEDIUM/MEDIUM_PROBE with ehci=DESCRIPTOR_FAILED/CONFIG_TOTAL,
#     medium=UNCHECKED/NOT_PROBED and journal=UNBOUND/NONE, and the refusal
#     ended before the network/panel/entropy records were composed.  The pins
#     below hold the per-candidate configuration evidence, the diagnostic
#     stick's own identity binding (the installer guard untouched), the
#     post-boot rescan that skips devices already bound, and the RAM snapshot
#     that exists without a medium.
grep -Fq 'INSTALL_MEDIUM_DIAGNOSTIC_DISK_ID' kernel/include/install_medium.h || {
    echo "ITEM-582: the diagnostic medium has no identity of its own" >&2
    exit 1
}
grep -Fq 'install_medium_diagnostic_identify' kernel/src/install_medium.c || {
    echo "ITEM-582: the diagnostic identity rule is missing" >&2
    exit 1
}
grep -Fq 'INSTALL_MEDIUM_MBR_TYPE_FAT12' kernel/include/install_medium.h || {
    echo "ITEM-582: the diagnostic binding does not require a FAT12 partition" >&2
    exit 1
}
grep -Fq 'install_journal_platform_bind_diagnostic' \
    kernel/src/install_journal_platform.c || {
    echo "ITEM-582: the journal platform cannot bind a diagnostic medium" >&2
    exit 1
}
grep -Fq 'reason = "BOOT_MEDIUM"' kernel/src/install_journal_platform.c || {
    echo "ITEM-582: the diagnostic binding does not refuse the boot medium" >&2
    exit 1
}
grep -Fq 'install_journal_platform_bind_diagnostic(&device->block.device,' \
    kernel/src/ehci.c || {
    echo "ITEM-582: the EHCI probe never offers the diagnostic medium" >&2
    exit 1
}
grep -Fq 'install_journal_platform_bind_diagnostic(' kernel/src/xhci.c || {
    echo "ITEM-582: the xHCI probe never offers the diagnostic medium" >&2
    exit 1
}
grep -Fq 'bool (*port_skip)(void *context, uint32_t port);' \
    kernel/include/ehci_walk.h || {
    echo "ITEM-582: the walk cannot skip a port a bound device occupies" >&2
    exit 1
}
grep -Fq 'ehci_rescan_ports(controller);' kernel/src/ehci.c || {
    echo "ITEM-582: the installed system never rescans for a later stick" >&2
    exit 1
}
grep -Fq 'EHCI_CONFIG_RECORDS' kernel/src/ehci.c || {
    echo "ITEM-582: the configuration evidence is not kept per candidate" >&2
    exit 1
}
grep -Fq '[EHCI-CONFIG]' kernel/src/ehci.c || {
    echo "ITEM-582: no record names the refused candidate's bytes" >&2
    exit 1
}
grep -Fq 'record->setup[7]' kernel/src/ehci.c || {
    echo "ITEM-582: the configuration record does not carry the setup request" >&2
    exit 1
}
grep -Fq 'diag_export_snapshot_records' kernel/include/diag_export.h || {
    echo "ITEM-582: the RAM snapshot has no accessor" >&2
    exit 1
}
grep -Fq 'diag_snapshot_active = true;' kernel/src/diag_export.c || {
    echo "ITEM-582: a save without a medium keeps no snapshot" >&2
    exit 1
}
grep -Fq 'diag snapshot' kernel/src/console.c || {
    echo "ITEM-582: the console cannot print the RAM snapshot again" >&2
    exit 1
}
grep -Fq 'print_diag_snapshot();' kernel/src/console.c || {
    echo "ITEM-582: the refusal does not print the snapshot" >&2
    exit 1
}
grep -Fq 'the diagnostic id on a FAT12 partition is the diagnostic medium' \
    tests/install-medium-host.c || {
    echo "ITEM-582: the diagnostic identity rule is not proven" >&2
    exit 1
}

# The diagnostic stick is prepared on the Debian side and read back by the same
# reader the operator uses; the check builds one and proves the placement is
# accepted, so the format the kernel binds is exercised without QEMU.
grep -Fq '0x48464344' scripts/hfoc-diag-stick-prepare.py || {
    echo "ITEM-582: the preparation tool does not stamp the diagnostic id" >&2
    exit 1
}
grep -Fq 'hfoc-install-media-prealloc.py' scripts/hfoc-diag-stick-prepare.py || {
    echo "ITEM-582: the tool does not reuse the reserved-file builder" >&2
    exit 1
}
grep -Fq 'for wanted in ((0xEF,), (0x01, 0x04, 0x06, 0x0B, 0x0C)):' \
    scripts/hfoc-install-log-read.py || {
    echo "ITEM-582: the reader does not accept the diagnostic FAT12 partition" >&2
    exit 1
}
item582_stick=$(mktemp "${TMPDIR:-/tmp}/hfoc-diag-stick.XXXXXX")
rm -f "$item582_stick"
if ! python3 scripts/hfoc-diag-stick-prepare.py --output "$item582_stick" \
        --force >/dev/null 2>"$item582_stick.log"; then
    echo "ITEM-582: the diagnostic stick cannot be prepared" >&2
    cat "$item582_stick.log" >&2 || true
    rm -f "$item582_stick" "$item582_stick.log"
    exit 1
fi
if ! python3 scripts/hfoc-install-log-read.py "$item582_stick" 2>/dev/null |
        grep -Fq 'fat12-preallocated'; then
    echo "ITEM-582: the prepared stick carries no readable reserved journal" >&2
    python3 scripts/hfoc-install-log-read.py "$item582_stick" >&2 || true
    rm -f "$item582_stick" "$item582_stick.log"
    exit 1
fi
rm -f "$item582_stick" "$item582_stick.log"
