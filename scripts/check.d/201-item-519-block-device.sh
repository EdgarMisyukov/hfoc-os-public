#!/usr/bin/env bash
# ITEM-519 increment 4: contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item519b_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item519b_root" || exit 1

# ITEM-519 increment 4: the block layer's view of the bound USB stick.
#   * the transport binding and the block_device binding happen in the same
#     step, and the second verdict is published either way: BLOCK_BOUND with
#     the geometry the block layer enforces, or BLOCK_REFUSED naming the
#     adapter's own status;
#   * the write proof drives the medium through that view, not past it, and a
#     stick without a view is refused by name (NO_BLOCK_DEVICE);
#   * the driver publishes the handle (xhci_storage_block_device) the journal
#     of ITEM-521 will own, valid for the boot because the session is held;
#   * the host proof shows the view's flush reaches the device as
#     SYNCHRONIZE CACHE (10).

# 1. The files the increment touches exist.
for item519b_file in \
    kernel/src/xhci.c \
    kernel/include/xhci.h \
    kernel/include/usb_storage.h \
    tests/usb-storage-host.c \
    scripts/test-qemu-usb-two-controllers.sh \
    docs/ROADMAP.md; do
    [[ -f $item519b_file ]] || {
        echo "ITEM-519: отсутствует $item519b_file" >&2
        exit 1
    }
done

# 2. The transport and the block view are bound together, and the view is part
#    of the device the session holds.
for item519b_pin in \
    'struct usb_storage_block_device block;' \
    'bool block_bound;' \
    'enum block_device_status block_status;' \
    'usb_storage_block_device_bind(&storage->usb, &storage->block)' \
    'storage->block_bound = storage->block_status == BLOCK_DEVICE_OK;'; do
    grep -Fq "$item519b_pin" kernel/src/xhci.c || {
        echo "ITEM-519: xhci.c не содержит: $item519b_pin" >&2
        exit 1
    }
done

# 3. The verdict is published either way, and the refusal names the adapter's
#    own status - never a stick described as usable without a view.
for item519b_record in \
    'state=BLOCK_BOUND reason=OK block=' \
    'state=BLOCK_REFUSED reason=' \
    'block_device_status_string(storage->block_status)' \
    'xhci_log_storage_block_record(storage);'; do
    grep -Fq "$item519b_record" kernel/src/xhci.c || {
        echo "ITEM-519: нет записи о block_device: $item519b_record" >&2
        exit 1
    }
done

# 4. The published handle exists, is declared, and refuses a stick without a
#    transport binding or without a block view.
grep -Fq 'struct block_device *xhci_storage_block_device(void)' \
    kernel/src/xhci.c || {
    echo "ITEM-519: драйвер не публикует block_device накопителя" >&2
    exit 1
}
grep -Fq 'return &storage->block.device;' kernel/src/xhci.c || {
    echo "ITEM-519: геттер не отдаёт block_device" >&2
    exit 1
}
grep -Fq 'if (!storage->bound || !storage->block_bound) {' kernel/src/xhci.c || {
    echo "ITEM-519: геттер не отказывает устройству без block-вида" >&2
    exit 1
}
grep -Fq 'struct block_device *xhci_storage_block_device(void);' \
    kernel/include/xhci.h || {
    echo "ITEM-519: xhci.h не объявляет block_device накопителя" >&2
    exit 1
}
grep -Fq 'struct block_device;' kernel/include/xhci.h || {
    echo "ITEM-519: xhci.h не объявляет тип block_device" >&2
    exit 1
}

# 5. The write proof goes through the block layer - and a run that bypasses it
#    is caught here rather than by reading the diff.  The proof is extracted so
#    the attach path's direct sector-0 read (increment 2) is not mistaken for a
#    bypass.
item519b_proof=$(sed -n '/^bool xhci_storage_write_proof(void) {$/,/^}$/p' \
    kernel/src/xhci.c)
[[ -n $item519b_proof ]] || {
    echo "ITEM-519: не найдена проба записи" >&2
    exit 1
}
for item519b_call in \
    'block_device_read(block, offset,' \
    'block_device_write(block, offset,' \
    'block_device_flush(block)' \
    'reason = "NO_BLOCK_DEVICE";'; do
    grep -Fq "$item519b_call" <<<"$item519b_proof" || {
        echo "ITEM-519: проба записи не идёт через block_device: $item519b_call" >&2
        exit 1
    }
done
for item519b_bypass in \
    'usb_storage_read(' \
    'usb_storage_write(' \
    'usb_storage_synchronize_cache('; do
    if grep -Fq "$item519b_bypass" <<<"$item519b_proof"; then
        echo "ITEM-519: проба записи обходит block_device: $item519b_bypass" >&2
        exit 1
    fi
done

# 6. The host proof drives the adapter and its flush, and keeps the refusal of
#    an unbound stick.
for item519b_host in \
    'test_block_device_reads_and_writes();' \
    'test_block_device_flush_reaches_the_device();' \
    "the block view returns the medium's bytes" \
    'the flush reached the device as SYNCHRONIZE CACHE (10)' \
    'an unbound stick has no block view'; do
    grep -Fq "$item519b_host" tests/usb-storage-host.c || {
        echo "ITEM-519: host-проба не проверяет: $item519b_host" >&2
        exit 1
    }
done

# 7. The QEMU phase B requires the block view with the geometry it enforces,
#    and still requires the write proof to succeed.
for item519b_qemu in \
    'state=BLOCK_BOUND' \
    'block=512" not in line or " blocks=2048" not in line' \
    'reason=OK write=OK verify=OK restore=OK'; do
    grep -Fq "$item519b_qemu" scripts/test-qemu-usb-two-controllers.sh || {
        echo "ITEM-519: QEMU-проба не проверяет: $item519b_qemu" >&2
        exit 1
    }
done

# 8. The queue says which item it is on, and the roadmap records the increment.
#    ITEM-521 moved the queue's `## Current item` heading on to its own item,
#    so this pin reads ITEM-519's own historical section.
grep -A 3 '^## Item ITEM-519$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-519' || {
    echo "ITEM-519: текущий раздел очереди не называет ITEM-519" >&2
    exit 1
}
grep -Fq 'state=BLOCK_BOUND' docs/ROADMAP.md || {
    echo "ITEM-519: docs/ROADMAP.md не записывает block-вид" >&2
    exit 1
}
