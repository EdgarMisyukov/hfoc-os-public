#!/usr/bin/env bash
# ITEM-519 (increment 1): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item519_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item519_root" || exit 1

# ITEM-519 increment 1: the transport-free half of USB mass storage.  The
# owner's medium is a USB stick and the kernel cannot see it (item-519-blocker),
# so the item is split into the transport (ITEM-519), the volume format
# (ITEM-520) and the journal (ITEM-521).  This slice holds the first
# increment's contract:
#   * the module is pure - it reaches the wire only through its three
#     callbacks - and names every failure the wire can produce;
#   * BOT is complete: CBW/CSW with signature and tag checks, the residue, the
#     phase error, and the reset recovery the specification requires;
#   * the SCSI command set and the block_device adapter exist;
#   * the host proof drives every one of those shapes and is registered in the
#     host suite.

# 1. The module, its proof and its record exist where the contract says.
for item519_file in \
    kernel/include/usb_storage.h \
    kernel/src/usb_storage.c \
    tests/usb-storage-host.c \
    docs/ROADMAP.md; do
    [[ -f $item519_file ]] || {
        echo "ITEM-519: отсутствует $item519_file" >&2
        exit 1
    }
done

# 2. The module owns no kernel dependency: the BOT layer rides on three
#    callbacks and both fixed structures are its own bytes.
for item519_callback in \
    'bool (*control)' \
    'bool (*bulk_in)' \
    'bool (*bulk_out)'; do
    grep -Fq "$item519_callback" kernel/include/usb_storage.h || {
        echo "ITEM-519: транспорт не описывает колбэк: $item519_callback" >&2
        exit 1
    }
done
grep -Fq '#define USB_STORAGE_CBW_BYTES 31U' kernel/include/usb_storage.h || {
    echo "ITEM-519: CBW не зафиксирован на 31 байт" >&2
    exit 1
}
grep -Fq '#define USB_STORAGE_CSW_BYTES 13U' kernel/include/usb_storage.h || {
    echo "ITEM-519: CSW не зафиксирован на 13 байт" >&2
    exit 1
}

# 3. Every failure the wire allows has a name an operator can read.
for item519_status in \
    USB_STORAGE_OK \
    USB_STORAGE_NULL \
    USB_STORAGE_BAD_TRANSPORT \
    USB_STORAGE_MALFORMED_DESCRIPTOR \
    USB_STORAGE_UNSUPPORTED_INTERFACE \
    USB_STORAGE_MALFORMED_INTERFACE \
    USB_STORAGE_NOT_READY \
    USB_STORAGE_TRANSPORT_ERROR \
    USB_STORAGE_SHORT_TRANSFER \
    USB_STORAGE_BAD_CSW_SIGNATURE \
    USB_STORAGE_CSW_TAG_MISMATCH \
    USB_STORAGE_PHASE_ERROR \
    USB_STORAGE_COMMAND_FAILED \
    USB_STORAGE_SENSE_FAILED \
    USB_STORAGE_BAD_BLOCK_SIZE \
    USB_STORAGE_BAD_CAPACITY \
    USB_STORAGE_OUT_OF_RANGE \
    USB_STORAGE_BAD_ALIGNMENT \
    USB_STORAGE_INVALID_LENGTH; do
    grep -Fq "$item519_status" kernel/include/usb_storage.h || {
        echo "ITEM-519: нет именованного отказа: $item519_status" >&2
        exit 1
    }
done

# 4. BOT is the real protocol: the two signatures, the interface shape this
#    module accepts (and only it), and the reset recovery the specification
#    defines - the class reset request and CLEAR_FEATURE(HALT).
grep -Fq '#define USB_STORAGE_CSW_SIGNATURE 0x53425355U' \
    kernel/include/usb_storage.h || {
    echo "ITEM-519: подпись CSW не закреплена" >&2
    exit 1
}
grep -Fq '#define USB_STORAGE_INTERFACE_PROTOCOL_BOT 0x50U' \
    kernel/include/usb_storage.h || {
    echo "ITEM-519: BOT-протокол не закреплён" >&2
    exit 1
}
for item519_bot_pin in \
    'usb_storage_write_le32(cbw, USB_STORAGE_CBW_SIGNATURE)' \
    'USB_STORAGE_REQUEST_TYPE_CLASS_INTERFACE' \
    'USB_STORAGE_REQUEST_RESET' \
    'USB_STORAGE_REQUEST_CLEAR_FEATURE' \
    'USB_STORAGE_FEATURE_ENDPOINT_HALT'; do
    grep -Fq "$item519_bot_pin" kernel/src/usb_storage.c || {
        echo "ITEM-519: BOT не содержит: $item519_bot_pin" >&2
        exit 1
    }
done

# 5. The SCSI command set and the block_device adapter are present.
for item519_command in \
    USB_SCSI_TEST_UNIT_READY \
    USB_SCSI_REQUEST_SENSE \
    USB_SCSI_INQUIRY \
    USB_SCSI_READ_CAPACITY_10 \
    USB_SCSI_READ_10 \
    USB_SCSI_WRITE_10 \
    USB_SCSI_SYNCHRONIZE_CACHE_10; do
    grep -Fq "$item519_command" kernel/include/usb_storage.h || {
        echo "ITEM-519: SCSI-команда не объявлена: $item519_command" >&2
        exit 1
    }
done
grep -Fq 'backend->device.read_fn = usb_storage_block_read;' \
    kernel/src/usb_storage.c || {
    echo "ITEM-519: block_device не получает чтение" >&2
    exit 1
}
grep -Fq 'backend->device.write_fn = usb_storage_block_write;' \
    kernel/src/usb_storage.c || {
    echo "ITEM-519: block_device не получает запись" >&2
    exit 1
}
grep -Fq 'USB_STORAGE_MAX_BLOCKS_PER_COMMAND' kernel/src/usb_storage.c || {
    echo "ITEM-519: длинный запрос не ограничен одной командой" >&2
    exit 1
}

# 6. The host proof drives the happy path and every failure shape, and the
#    target is registered in the compiled host suite (the manifest check
#    re-derives the count from the serial recipe).
for item519_proof_pin in \
    'short data stage' \
    'CSW' \
    'phase error' \
    'sense data' \
    'CLEAR_FEATURE(HALT)' \
    'the request was split into two bounded BOT commands'; do
    grep -Fiq "$item519_proof_pin" tests/usb-storage-host.c || {
        echo "ITEM-519: host-проба не проверяет: $item519_proof_pin" >&2
        exit 1
    }
done
grep -Fq 'test-usb-storage-host:' Makefile || {
    echo "ITEM-519: цель host-пробы не определена" >&2
    exit 1
}
grep -Fq '$(MAKE) test-usb-storage-host' Makefile || {
    echo "ITEM-519: host-проба не зарегистрирована в host-suite" >&2
    exit 1
}

# 7. The queue says which item it is on, and the roadmap records it.  ITEM-521
#    moved the queue's `## Current item` heading on to its own item, so this
#    pin reads ITEM-519's own historical section; the heading shape itself is
#    held by scripts/check.d/52-queue-current-item.sh.
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
