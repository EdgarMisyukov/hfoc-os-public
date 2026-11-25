#!/usr/bin/env bash
# ITEM-453: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The port that Linux sees names itself.
#
# The 2026-09-19 hardware run answered `reason=NO_DEVICE
# detail=NO_CONNECTED_DEVICE` for `05:00.3` - the controller Linux enumerates
# the owner's Logitech receiver on - and the whole record of that controller
# was `pp=3 ppoff=0 portsc=0x000002A0,0x000002A0` for its six ports: two raw
# values out of six, no way to tell "empty" from "never read", and no way to
# tell a port that samples a connect from one that cannot report one at all.
# This slice pins the driver that replaces that silence:
#
#   * the pure bring-up decision (`xhci_port_bring_up_plan`) says what one
#     port needs before its "no device" answer may be believed: assert power
#     when PORTSC.PP is clear, move a link state that does not sample a connect
#     (Disabled, Inactive, Compliance, Test Mode) back to RxDetect with LWS,
#     and give a port that was written to its USB 2.0 TATTDB window (100 ms);
#   * the write that moves a link (`xhci_portsc_link_kick`) clears the link
#     state field before writing RxDetect - OR-ing RxDetect into U3 would ask
#     for Polling - sets LWS, and still never carries PED or a
#     write-one-to-clear change bit;
#   * the driver runs that sequence per port, waits for the connect inside the
#     window instead of sleeping it out, and gives a just-reset controller one
#     shared connect-debounce window before any of its ports is believed
#     empty;
#   * every port of every controller publishes its own `[XHCI-PORT]` line with
#     the raw PORTSC value before and after the bring-up, the decoded
#     power/connect/link state, whether the driver wrote anything and how long
#     it waited - and the line is composed whole or not at all;
#   * the controller's Supported Protocol capabilities are read and published
#     as `[XHCI-PROTO]`: the major revision, the raw port offset and count, the
#     derived raw port range and the raw dwords they were parsed from.  The
#     walk follows the specification's relative Next Capability Pointer, and
#     the group's first port is the offset itself (xHCI 1.1 section 7.2.1:
#     valid values are 1 to MaxPorts) - which is how a raw port number is
#     mapped to the USB2 or the USB3 half of the controller;
#   * both records reach the durable boot log, because the answer to "which
#     port, and what did it say" has to be on the disk the machine came up on;
#   * the host proof drives the decision with the exact PORTSC value the
#     owner's machine published (`0x000002A0`), the wrong link states, the
#     kick write and the protocol layout - including the USB2 half that QEMU's
#     model declares as (offset 5, count 4) for the ports it numbers 5..8;
#   * the QEMU proof names every port index 1..ports exactly once per
#     controller, requires the protocol groups to partition those ports, and
#     boots a SuperSpeed device (`usb-uas`) on a USB3 port: the port's raw
#     PORTSC must carry the SuperSpeed speed value and the device must be
#     published as `speed=SUPER`, not silently refused.
for port_bring_up_file in \
    kernel/include/xhci_trb.h \
    kernel/src/xhci_trb.c \
    kernel/src/xhci.c \
    kernel/src/appliance_boot_log.c \
    tests/xhci-host.c \
    scripts/test-qemu-usb-two-controllers.sh \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $port_bring_up_file ]] || {
        echo "ITEM-453: отсутствует $port_bring_up_file" >&2
        exit 1
    }
done

# 1. The pure decision and the states it reasons about.
for port_bring_up_pure in \
    'XHCI_PORT_LINK_DISABLED 4U' \
    'XHCI_PORT_LINK_RXDETECT 5U' \
    'XHCI_PORT_LINK_INACTIVE 6U' \
    'XHCI_PORT_LINK_COMPLIANCE 10U' \
    'XHCI_PORT_LINK_TEST_MODE 11U' \
    '#define XHCI_PORT_SETTLE_MS 100U' \
    'struct xhci_port_bring_up {' \
    'bool power_on;' \
    'bool link_kick;' \
    'uint32_t settle_ms;' \
    'void xhci_port_bring_up_plan(uint32_t portsc, struct xhci_port_bring_up *plan);' \
    'uint32_t xhci_portsc_link_kick(uint32_t portsc);' \
    'bool xhci_port_link_samples_connect(uint32_t state);' \
    'const char *xhci_port_link_state_string(uint32_t state);'; do
    grep -Fq -- "$port_bring_up_pure" kernel/include/xhci_trb.h || {
        echo "ITEM-453: заголовок не объявляет: $port_bring_up_pure" >&2
        exit 1
    }
done
for port_bring_up_impl in \
    'uint32_t xhci_port_link_state(uint32_t portsc)' \
    'bool xhci_port_link_samples_connect(uint32_t state)' \
    'uint32_t xhci_portsc_link_kick(uint32_t portsc)' \
    'void xhci_port_bring_up_plan(uint32_t portsc, struct xhci_port_bring_up *plan)' \
    'plan->power_on = !plan->powered;' \
    'plan->link_kick = !plan->samples;' \
    'plan->settle_ms = (!plan->connected && (plan->power_on || plan->link_kick))'; do
    grep -Fq -- "$port_bring_up_impl" kernel/src/xhci_trb.c || {
        echo "ITEM-453: решение о порту не реализовано: $port_bring_up_impl" >&2
        exit 1
    }
done
# The kick clears the link state field before it writes RxDetect: OR-ing
# RxDetect into a port in U3 would ask for Polling instead.
grep -Fq -- '(xhci_portsc_neutral(portsc) & ~XHCI_PORTSC_PLS_MASK)' \
    kernel/src/xhci_trb.c || {
    echo "ITEM-453: link-kick не очищает поле PLS перед записью RxDetect" >&2
    exit 1
}

# 2. The driver's sequence: read, decide, write, wait for the connect, and only
#    then return the value the verdict rests on.
for port_bring_up_driver in \
    'static uint32_t xhci_port_bring_up(' \
    'xhci_port_bring_up_plan(first, plan);' \
    'if (plan->power_on) {' \
    'xhci_write32(portsc, 0U, xhci_portsc_claim(first));' \
    'if (plan->link_kick) {' \
    'xhci_write32(portsc, 0U, xhci_portsc_link_kick(first));' \
    'while (waited < plan->settle_ms) {' \
    'if ((xhci_read32(portsc, 0U) & XHCI_PORTSC_CCS) != 0U) {' \
    'static void xhci_ports_settle_after_reset(void)' \
    'xhci_ports_settle_after_reset();' \
    'xhci_log_port_record(port, &bring, portsc, waited);'; do
    grep -Fq -- "$port_bring_up_driver" kernel/src/xhci.c || {
        echo "ITEM-453: драйвер не выполняет шаг подъёма порта: $port_bring_up_driver" >&2
        exit 1
    }
done
# The port record is published before the walk decides anything about a device,
# so an empty port names itself exactly like a populated one.
port_record_line=$(grep -n 'xhci_log_port_record(port, &bring, portsc, waited);' \
    kernel/src/xhci.c | head -1 | cut -d: -f1)
ccs_line=$(grep -n 'if ((portsc & XHCI_PORTSC_CCS) == 0U) {' kernel/src/xhci.c |
    head -1 | cut -d: -f1)
[[ -n $port_record_line && -n $ccs_line && $port_record_line -lt $ccs_line ]] || {
    echo "ITEM-453: запись порта публикуется после решения о устройстве" >&2
    exit 1
}

# 3. The protocol groups: read with the relative next pointer, published with
#    their raw dwords, and mapped to raw ports by the offset itself.
for port_bring_up_proto in \
    'static void xhci_read_protocol_groups(void)' \
    'xhci->protocol_count = 0U;' \
    'offset += next;' \
    'if (next == 0U) {' \
    'base + 12U > xhci->mmio_bytes' \
    'base + 4U > xhci->mmio_bytes' \
    'base + 12U + index * 4U + 4U > xhci->mmio_bytes' \
    'xhci->mmio_bytes = (uint32_t)length;' \
    'static void xhci_log_protocol_records(void)' \
    'xhci_protocol_for_port(xhci->protocols, xhci->protocol_count, port)' \
    'xhci_protocol_group_label(group->major)' \
    'xhci_format_port_record(line, sizeof(line), xhci->location.bus,' \
    'xhci_format_protocol_record('; do
    grep -Fq -- "$port_bring_up_proto" kernel/src/xhci.c || {
        echo "ITEM-453: разбор групп протокола неполон: $port_bring_up_proto" >&2
        exit 1
    }
done
if grep -Fq -- 'offset = (capability >> XHCI_EXT_CAP_NEXT_SHIFT) & 0xFFU;' \
    kernel/src/xhci.c; then
    echo "ITEM-453: указатель следующей возможности снова читается как абсолютный" >&2
    exit 1
fi
for port_bring_up_range in \
    '*first = (uint32_t)group->port_offset;' \
    '*last = (uint32_t)group->port_offset + (uint32_t)group->port_count - 1U;' \
    'group->port_offset == 0U' \
    'group->raw_revision = revision;' \
    'group->raw_name = name_string;' \
    'group->raw_port_info = port_info;' \
    '"[XHCI-PORT] pci="' \
    '"[XHCI-PROTO] pci="'; do
    grep -Fq -- "$port_bring_up_range" kernel/src/xhci_trb.c || {
        echo "ITEM-453: диапазон портов группы разобран неверно: $port_bring_up_range" >&2
        exit 1
    }
done

# 4. Both records are a durable-log flush moment.
for port_bring_up_marker in '"[XHCI-PORT]"' '"[XHCI-PROTO]"'; do
    grep -Fq -- "$port_bring_up_marker" kernel/src/appliance_boot_log.c || {
        echo "ITEM-453: маркер $port_bring_up_marker не попадает в durable-журнал" >&2
        exit 1
    }
done

# 5. The host proof drives the decision with the value the owner's machine
#    published, and the protocol layout with the dwords QEMU's model answers.
for port_bring_up_host in \
    'static void test_port_bring_up_plan(void)' \
    'static void test_port_link_state(void)' \
    'static void test_port_record(void)' \
    'static void test_protocol_capability(void)' \
    'test_port_bring_up_plan();' \
    'test_port_link_state();' \
    'test_port_record();' \
    'test_protocol_capability();' \
    'xhci_port_bring_up_plan(0x000002A0U, &plan);' \
    'XHCI_PORT_LINK_RXDETECT' \
    'XHCI_PORT_SETTLE_MS' \
    'xhci_portsc_link_kick(disabled)' \
    'XHCI_PORTSC_LWS' \
    'xhci_format_port_record(buffer, sizeof(buffer), 0x05U, 0x00U, 0x3U,' \
    'was=0x000002A0 now=0x000002A0' \
    'xhci_format_protocol_record(buffer, sizeof(buffer), 0x05U, 0x00U,' \
    'ports=1-2 psiv=4,5' \
    'a group whose offset is zero declares no first port and is refused'; do
    grep -Fq -- "$port_bring_up_host" tests/xhci-host.c || {
        echo "ITEM-453: host-проба не утверждает: $port_bring_up_host" >&2
        exit 1
    }
done

# 6. The QEMU proof: every port named, the groups a partition, and a
#    SuperSpeed device published as SUPER on a USB3 port.
for port_bring_up_qemu in \
    'records("[XHCI-PORT] ")' \
    'records("[XHCI-PROTO] ")' \
    'has to publish its own [XHCI-PORT] line' \
    'do not partition its ports' \
    "-device 'usb-uas,bus=x0.0'" \
    ' speed=SUPER ' \
    'the port'"'"'s raw PORTSC does not carry the SuperSpeed speed value'; do
    grep -Fq -- "$port_bring_up_qemu" scripts/test-qemu-usb-two-controllers.sh || {
        echo "ITEM-453: QEMU-проба не требует: $port_bring_up_qemu" >&2
        exit 1
    }
done

# 7. The documents record the item, and the console document names both
#    records an operator reads.
for port_bring_up_doc in \
    'ITEM-453' \
    '[XHCI-PORT]' \
    '[XHCI-PROTO]'; do
    grep -Fq -- "$port_bring_up_doc" docs/hfoc-appliance-boot-console.md || {
        echo "ITEM-453: документация консоли не называет: $port_bring_up_doc" >&2
        exit 1
    }
done
