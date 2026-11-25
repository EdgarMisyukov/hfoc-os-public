#!/usr/bin/env bash
# ITEM-519 increment 2: contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item519x_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item519x_root" || exit 1

# ITEM-519 increment 2: the xHCI half of USB mass storage.  Increment 1 gave
# the BOT/SCSI core (slice 199); this one gives it a wire:
#   * the DMA pool grows by the storage rings and the bounce page, with the
#     DCBAA still below them and a fourth session slot for a third persistent
#     role;
#   * bulk transfers ride a Normal TRB with IOC on their own ring and their own
#     doorbell; the control path is shared with the HID roles through one
#     ring-and-offset core, so the HID paths do not change;
#   * a port no HID role bound is offered to storage on storage's own budget,
#     with its own record, and the controller publishes taken=YES for it;
#   * the device's bConfigurationValue is kept so SET_CONFIGURATION can use it;
#   * the QEMU proof's phase B is positive: the stick binds, names its
#     capacity, and the first sector's marker proves the bulk data stage.

# 1. The files the increment touches exist.
for item519x_file in \
    kernel/src/xhci.c \
    kernel/include/usb_storage.h \
    kernel/src/usb_storage.c \
    tests/usb-storage-host.c \
    scripts/test-qemu-usb-two-controllers.sh \
    docs/ROADMAP.md; do
    [[ -f $item519x_file ]] || {
        echo "ITEM-519: отсутствует $item519x_file" >&2
        exit 1
    }
done

# 2. The DMA layout and the bulk path.
for item519x_pin in \
    'XHCI_OFF_STORAGE_EP0_RING' \
    'XHCI_OFF_STORAGE_BULK_IN_RING' \
    'XHCI_OFF_STORAGE_BULK_OUT_RING' \
    'XHCI_OFF_STORAGE_DATA' \
    'XHCI_STORAGE_MAX_ATTACH_ATTEMPTS' \
    'xhci_bulk_transfer(' \
    'xhci_storage_attach(' \
    'xhci_storage_configure(' \
    'xhci_log_storage_record(' \
    'XHCI_SESSION_SLOTS 4U'; do
    grep -Fq "$item519x_pin" kernel/src/xhci.c || {
        echo "ITEM-519: xhci.c не содержит: $item519x_pin" >&2
        exit 1
    }
done
# ITEM-545 moved the endpoint context types to the header that owns the context
# builders (kernel/include/xhci_trb.h); the storage path still programs exactly
# these two (xHCI 1.1 table 6-7), so the pin follows them there.
for item519x_ep_type in \
    '#define XHCI_EP_TYPE_BULK_OUT 2U' \
    '#define XHCI_EP_TYPE_BULK_IN 6U'; do
    grep -Fq "$item519x_ep_type" kernel/include/xhci_trb.h || {
        echo "ITEM-519: xhci_trb.h не содержит: $item519x_ep_type" >&2
        exit 1
    }
done
grep -Fq 'xhci_trb_normal(ring->cycle, buffer_physical, length, true)' \
    kernel/src/xhci.c || {
    echo "ITEM-519: bulk-передача не вооружается Normal TRB с IOC" >&2
    exit 1
}
grep -Fq 'xhci_write32(xhci_doorbell(slot_id), 0U, dci);' kernel/src/xhci.c || {
    echo "ITEM-519: bulk-передача не звонит в двербель своего эндпоинта" >&2
    exit 1
}

# 3. The three callbacks the BOT core needs, and the control core the HID
#    roles keep using.
for item519x_callback in \
    'xhci_storage_control(' \
    'xhci_storage_bulk_in(' \
    'xhci_storage_bulk_out(' \
    'xhci_control_transfer_ring(' \
    'xhci_address_device_ring('; do
    grep -Fq "$item519x_callback" kernel/src/xhci.c || {
        echo "ITEM-519: транспорт не собран: $item519x_callback" >&2
        exit 1
    }
done
grep -Fq 'transport.control = xhci_storage_control;' kernel/src/xhci.c || {
    echo "ITEM-519: usb_storage_bind не получает control-колбэк" >&2
    exit 1
}
grep -Fq 'usb_storage_bind(&storage->usb, &transport, &config)' \
    kernel/src/xhci.c || {
    echo "ITEM-519: накопитель не привязывается через BOT-ядро" >&2
    exit 1
}

# 4. The walk offers storage after the HID roles, on its own budget, and the
#    session survives the walk; the controller publishes taken=YES for it.
grep -Fq 'xhci_machine.storage_session == NULL' kernel/src/xhci.c || {
    echo "ITEM-519: обход не проверяет, свободно ли хранилище" >&2
    exit 1
}
grep -Fq 'xhci_machine.storage_session = xhci;' kernel/src/xhci.c || {
    echo "ITEM-519: сессия хранилища не закрепляется" >&2
    exit 1
}
grep -Fq 'xhci_machine.storage_session == session' kernel/src/xhci.c || {
    echo "ITEM-519: сессия хранилища может быть освобождена обходом" >&2
    exit 1
}
grep -Fq 'keyboard->ready || mouse->ready || xhci->storage.bound' \
    kernel/src/xhci.c || {
    echo "ITEM-519: taken= не называет хранилище" >&2
    exit 1
}

# 5. The configuration value SET_CONFIGURATION needs is part of the parsed
#    binding, and the host proof pins it.
grep -Fq 'uint8_t configuration_value;' kernel/include/usb_storage.h || {
    echo "ITEM-519: usb_storage_config не хранит bConfigurationValue" >&2
    exit 1
}
grep -Fq 'found.configuration_value = bytes[5];' kernel/src/usb_storage.c || {
    echo "ITEM-519: разбор конфигурации не читает bConfigurationValue" >&2
    exit 1
}
grep -Fq 'configuration value SET_CONFIGURATION needs' \
    tests/usb-storage-host.c || {
    echo "ITEM-519: host-проба не проверяет bConfigurationValue" >&2
    exit 1
}

# 6. The QEMU phase B is positive now: the stick binds, the record names the
#    capacity, and the first sector's marker proves the data stage.  The old
#    negative assertion for this device is gone.
grep -Fq 'role=STORAGE state=BOUND' scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-519: QEMU-проба не требует state=BOUND" >&2
    exit 1
}
grep -Fq 'sector0=434F4648' scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-519: QEMU-проба не проверяет байты сектора 0" >&2
    exit 1
}
grep -Fq 'HFOC-USB-STORAGE' scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-519: QEMU-проба не кладёт маркер в образ носителя" >&2
    exit 1
}
if grep -Fq 'the non-HID device was not published as REFUSED' \
    scripts/test-qemu-usb-two-controllers.sh; then
    echo "ITEM-519: QEMU-проба всё ещё требует старый REFUSED для накопителя" >&2
    exit 1
fi

# 7. The queue says which item it is on, and the roadmap records it.  ITEM-521
#    moved the queue's `## Current item` heading on to its own item, so this
#    pin reads ITEM-519's own historical section.
grep -A 3 '^## Item ITEM-519$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-519' || {
    echo "ITEM-519: текущий раздел очереди не называет ITEM-519" >&2
    exit 1
}
grep -Fq 'ITEM-519' docs/ROADMAP.md || {
    echo "ITEM-519: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
