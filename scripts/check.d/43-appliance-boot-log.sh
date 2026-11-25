#!/usr/bin/env bash
# ITEM-410: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The appliance writes its own boot log to its own disk.
#
# Every hardware iteration ended with photographs of a 16-line screen: the
# records the kernel publishes about an install, the network, the local input
# path and its readiness reached the serial stream (which the acceptance host
# cannot capture) and the framebuffer console (where one scrolled record costs
# an iteration), and the bounded structured ring kept them in RAM only. The log
# now lives in a preallocated file of ours on the ESP, is appended by the next
# boot, wraps with a sequence number and a generation marker, and is read back
# out of a raw image by the host reader.
for boot_log_file in \
    kernel/include/appliance_boot_log.h \
    kernel/src/appliance_boot_log.c \
    kernel/include/appliance_boot_log_store.h \
    kernel/src/appliance_boot_log_store.c \
    kernel/include/appliance_gpt_discovery.h \
    kernel/include/efi_fat32_esp.h \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    kernel/src/panic.c \
    kernel/src/production_install_flow.c \
    tests/appliance-boot-log-host.c \
    tests/appliance-boot-log-store-host.c \
    tests/appliance-boot-log-reader-host.py \
    tests/console-host.c \
    tests/efi-fat32-esp-host.c \
    tests/appliance-gpt-discovery-host.c \
    scripts/hfoc-esp-read.py \
    scripts/test-qemu-installed-boot.sh \
    scripts/test-qemu-production-install-flow.sh \
    scripts/hfoc-physical-sata-continue.sh \
    release/hfoc-hardware-acceptance-matrix.txt \
    docs/hfoc-installed-boot.md \
    docs/hfoc-hardware-acceptance.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    test -f "$boot_log_file" || {
        echo "Отсутствует ITEM-410 file: $boot_log_file" >&2
        exit 1
    }
done

# The extent is fixed and bounded: 512 slots of 512 bytes, one header slot and
# 511 records, a whole number of clusters at any FAT cluster size.
for boot_log_format in \
    '#define APPLIANCE_BOOT_LOG_SLOT_SIZE 512U' \
    '#define APPLIANCE_BOOT_LOG_SLOT_COUNT 512U' \
    '#define APPLIANCE_BOOT_LOG_FILE_BYTES' \
    '#define APPLIANCE_BOOT_LOG_PATH "\\EFI\\HFOC\\BOOTLOG.BIN"' \
    '#define APPLIANCE_BOOT_LOG_TEXT_MAX 440U'; do
    grep -Fq "$boot_log_format" kernel/include/appliance_boot_log.h || {
        echo "Формат журнала загрузки не зафиксирован: $boot_log_format" >&2
        exit 1
    }
done
# The slot is one ASCII line closed by a newline and padded with NUL bytes, and
# every slot carries a CRC-32 over its own bytes, so a torn write is detected
# instead of being printed as text.
for boot_log_slot in \
    'appliance_boot_log_render_header' \
    'appliance_boot_log_parse_header' \
    'appliance_boot_log_render_record' \
    'appliance_boot_log_parse_slot' \
    'appliance_boot_log_slot_for_cursor' \
    'appliance_boot_log_advance_cursor' \
    'appliance_boot_log_flush_trigger'; do
    grep -Fq "$boot_log_slot" kernel/include/appliance_boot_log.h || {
        echo "Формат журнала загрузки не публикует: $boot_log_slot" >&2
        exit 1
    }
done
grep -Fq 'UINT32_C(0xEDB88320)' kernel/src/appliance_boot_log.c || {
    echo "checksum журнала не является CRC-32 читателя" >&2
    exit 1
}
for boot_log_slot_rule in \
    'APPLIANCE_BOOT_LOG_SLOT_EMPTY' \
    'APPLIANCE_BOOT_LOG_SLOT_TORN' \
    'APPLIANCE_BOOT_LOG_SLOT_OK'; do
    grep -Fq "$boot_log_slot_rule" kernel/src/appliance_boot_log.c || {
        echo "слот журнала не различает состояние: $boot_log_slot_rule" >&2
        exit 1
    }
done

# The moments that matter are named, and a refusal is one of them: the flush
# policy is a bounded table over the records this kernel publishes, not a
# judgement and not a write per record.
for boot_log_moment in \
    '"[BOOT-CHAIN]"' \
    '"[APPLIANCE-BOOT]"' \
    '"[PRODUCTION-BOOT]"' \
    '"[PRODUCTION-INSTALL]"' \
    '"[PRODUCTION-MGMT-BIND]"' \
    '"[PRODUCTION-NET-MODE]"' \
    '"[PRODUCTION-DHCP]"' \
    '"[PRODUCTION-DNS]"' \
    '"[PRODUCTION-SERVING]"' \
    '"[PRODUCTION-TLS]"' \
    '"[AUDIT-INTEGRITY]"' \
    '"state=UNAVAILABLE"' \
    '"state=REFUSED"' \
    '"state=FAILED"' \
    '"verdict=REFUSED"'; do
    grep -Fq "$boot_log_moment" kernel/src/appliance_boot_log.c || {
        echo "политика сброса не называет момент: $boot_log_moment" >&2
        exit 1
    }
done

# The store attaches only to an ESP that carries this install's own sealed boot
# manifest, never to the installer medium, and it creates one preallocated
# extent of ours.
for boot_log_store in \
    'APPLIANCE_BOOT_LOG_STORE_MANIFEST_PATH "\\boot\\hfoc-boot.man"' \
    'appliance_gpt_find_esp_partition' \
    'efi_fat32_esp_create_fixed_file' \
    'efi_fat32_esp_write_file_range' \
    'APPLIANCE_BOOT_LOG_STORE_NOT_OURS' \
    'APPLIANCE_BOOT_LOG_STORE_NO_TARGET' \
    '[BOOTLOG] state=' \
    'appliance_boot_log_store_panic_flush'; do
    grep -Fq "$boot_log_store" \
        kernel/include/appliance_boot_log_store.h \
        kernel/src/appliance_boot_log_store.c || {
        echo "store журнала не фиксирует: $boot_log_store" >&2
        exit 1
    }
done
# A log that cannot be written says so, once, and never fails a boot.
grep -Fq 'store.unavailable_published = true;' \
    kernel/src/appliance_boot_log_store.c || {
    echo "store журнала не публикует UNAVAILABLE один раз" >&2
    exit 1
}
grep -Fq 'APPLIANCE_BOOT_LOG_STORE_NO_TARGET' \
    kernel/src/appliance_boot_log_store.c || {
    echo "store журнала не сообщает об отсутствии цели" >&2
    exit 1
}

# The one-file primitives: a fixed extent is created once, a range write never
# grows the file and never allocates.
for boot_log_extent in \
    'efi_fat32_esp_create_fixed_file' \
    'efi_fat32_esp_write_file_range' \
    'efi_fat32_esp_read_file_range'; do
    grep -Fq "$boot_log_extent" kernel/include/efi_fat32_esp.h || {
        echo "ESP writer не публикует: $boot_log_extent" >&2
        exit 1
    }
    grep -Fq "$boot_log_extent" kernel/src/efi_fat32_esp.c || {
        echo "ESP writer не реализует: $boot_log_extent" >&2
        exit 1
    }
done
grep -Fq 'reused_out' kernel/include/efi_fat32_esp.h || {
    echo "ESP writer не сообщает о переиспользовании экстента" >&2
    exit 1
}
grep -Fq '% cluster_bytes) != 0U' kernel/src/efi_fat32_esp.c || {
    echo "ESP writer не требует целого числа кластеров в экстенте" >&2
    exit 1
}

# The console hands every accepted record to the log and answers BOOTLOG from
# RAM: the command must not depend on the write path that refused.
grep -Fq 'console_bind_boot_log' kernel/include/console.h || {
    echo "консоль не связывается с журналом" >&2
    exit 1
}
grep -Fq 'boot_log_binding.observe(line, strlen(line));' kernel/src/console.c || {
    echo "консоль не передаёт записи журналу" >&2
    exit 1
}
grep -Fq 'console_command_is(input_line, "bootlog")' kernel/src/console.c || {
    echo "консоль не отвечает на команду BOOTLOG" >&2
    exit 1
}
for boot_log_command in \
    'bootlog' \
    'console.prose.bootlog_unavailable'; do
    grep -Fq "$boot_log_command" kernel/src/console.c || {
        echo "консольная команда журнала не фиксирует: $boot_log_command" >&2
        exit 1
    }
done

# The boot binds the log before the first record, attaches it to the appliance's
# own disk on an installed boot, and never attaches it on the installer medium.
grep -Fq 'kernel_boot_log_boot_init();' kernel/src/kernel.c || {
    echo "ядро не связывает журнал до первой записи" >&2
    exit 1
}
grep -Fq 'kernel_boot_log_attach_disks();' kernel/src/kernel.c || {
    echo "установленная машина не привязывает журнал к своему диску" >&2
    exit 1
}
grep -Fq 'appliance_boot_log_store_flush("readiness");' kernel/src/kernel.c || {
    echo "конец загрузки не сбрасывает журнал" >&2
    exit 1
}
grep -Fq 'appliance_boot_log_store_panic_flush();' kernel/src/panic.c || {
    echo "паника не сбрасывает журнал" >&2
    exit 1
}
grep -Fq 'appliance_boot_log_store_attach_extent(' \
    kernel/src/production_install_flow.c || {
    echo "install flow не привязывает журнал к объявленной цели" >&2
    exit 1
}
# The installer medium must not attach the log at boot: the medium is not the
# declared target, so the attach sits inside the "not an installer medium"
# branch of the boot sequence.
grep -A 8 '#ifndef HFOC_PRODUCTION_INSTALL_MEDIA' kernel/src/kernel.c |
    grep -Fq 'kernel_boot_log_attach_disks();' || {
    echo "установочный носитель привязывает журнал при загрузке" >&2
    exit 1
}

# The records a hardware run quotes are mirrored to the console funnel, so the
# log stores them: the boot chain, the storage verdict, the readiness line, the
# serving path and the management binding.
for boot_log_mirror in \
    '[BOOT-CHAIN] verdict=' \
    '[PRODUCTION-BOOT] status=' \
    '[APPLIANCE-BOOT] runtime=' \
    '[PRODUCTION-SERVING] ' \
    '[PRODUCTION-MGMT-BIND] scope=' \
    '[PRODUCTION-FIRST-BOOT] first_boot=' \
    '[PRODUCTION-MGMT] url='; do
    grep -Fq "$boot_log_mirror" kernel/src/kernel.c || {
        echo "запись не публикуется ядром: $boot_log_mirror" >&2
        exit 1
    }
done
grep -Fq 'console_record_text("[BOOT-CHAIN] verdict=")' kernel/src/kernel.c || {
    echo "запись boot chain не зеркалится на консоль" >&2
    exit 1
}
grep -Fq 'console_record_text("[APPLIANCE-BOOT] runtime=")' kernel/src/kernel.c || {
    echo "запись готовности не зеркалится на консоль" >&2
    exit 1
}

# The host reader prints the log with its sequence numbers, out of an image or
# out of a mounted ESP, and never prints garbage.
for boot_log_reader in \
    '--bootlog' \
    '--bootlog-file' \
    'boot-log:' \
    'BOOTLOG.BIN'; do
    grep -Fq -- "$boot_log_reader" scripts/hfoc-esp-read.py || {
        echo "host reader не поддерживает: $boot_log_reader" >&2
        exit 1
    }
done
grep -Fq 'zlib.crc32' scripts/hfoc-esp-read.py || {
    echo "host reader не проверяет CRC записей" >&2
    exit 1
}
# The runbook's own command reads the log out of the acceptance disk's device
# node (`hfoc-esp-read.py /dev/sda --bootlog`), and a block device reports no
# size through `fstat`: a zero-length mapping is EINVAL, which is what the
# operator hit on the machine whose log was needed.  A reader that maps a
# device at all has to take its length from the device.
for boot_log_device in \
    'BLKGETSIZE64' \
    'def map_image(' \
    'stat.S_ISBLK'; do
    grep -Fq -- "$boot_log_device" scripts/hfoc-esp-read.py || {
        echo "host reader не отображает блочное устройство: $boot_log_device" >&2
        exit 1
    }
done

# The host proofs hold the format, the wrap and the honesty rules.
for boot_log_case in \
    'the boot log checksum is the CRC-32 the host reader computes' \
    'a record whose text was changed is torn and is not text' \
    'writing past the last slot wraps the cursor and the generation' \
    'the reader reconstructs the contiguous run the wrap left behind' \
    'a torn newest slot ends the run: the last complete record is the ' \
    'newest one the reader reports' \
    'the ring counts the records it had to drop' \
    'the install record is flushed at once' \
    'a refusal is flushed at once'; do
    grep -Fq "$boot_log_case" tests/appliance-boot-log-host.c || {
        echo "host proof журнала не проверяет: $boot_log_case" >&2
        exit 1
    }
done
for boot_log_store_case in \
    'the store attaches to the ESP that carries our boot manifest' \
    'the second boot reuses the extent instead of creating it' \
    'a flush after the creation changes no byte of the file ' \
    'a flush after the creation changes no byte of a directory' \
    'probing an ESP that is not ours writes not one byte to it' \
    'a failed flush publishes state=UNAVAILABLE' \
    'the next flush writes what the failed one left staged' \
    'the wrap is marked by a new generation, so a reader can tell old '; do
    grep -Fq "$boot_log_store_case" tests/appliance-boot-log-store-host.c || {
        echo "store proof журнала не проверяет: $boot_log_store_case" >&2
        exit 1
    }
done
grep -Fq 'the fixed extent is created at the declared size' \
    tests/efi-fat32-esp-host.c || {
    echo "ESP proof не проверяет создание экстента" >&2
    exit 1
}
grep -Fq 'every byte the write changed belongs to the extent' \
    tests/efi-fat32-esp-host.c || {
    echo "ESP proof не проверяет, что пишутся только свои кластеры" >&2
    exit 1
}
grep -Fq 'the boot log observes the record the console accepted' \
    tests/console-host.c || {
    echo "console proof не проверяет передачу записей журналу" >&2
    exit 1
}
grep -Fq 'without a bound log the BOOTLOG command says so instead of ' \
    tests/console-host.c || {
    echo "console proof не проверяет честный ответ BOOTLOG" >&2
    exit 1
}
grep -Fq 'the first EFI System Partition is reported in GPT slot order' \
    tests/appliance-gpt-discovery-host.c || {
    echo "GPT proof не проверяет перечисление ESP" >&2
    exit 1
}

# The QEMU proofs read the log out of the finished image, independently of the
# kernel and of the serial stream.
for boot_log_qemu in \
    'assert_boot_log' \
    'scripts/hfoc-esp-read.py' \
    '[APPLIANCE-BOOT] runtime=READY identity=OK' \
    '[BOOTLOG] state=READY path='; do
    grep -Fq "$boot_log_qemu" \
        scripts/test-qemu-production-install-flow.sh || {
        echo "product-flow proof не читает журнал: $boot_log_qemu" >&2
        exit 1
    }
done
# A proof that reads the log out of the finished image has to stop the guest on
# the record the store publishes *after* the device flush, not on the first
# `[BOOTLOG] state=READY` record of the boot: otherwise the proof kills QEMU
# while the records it is about to require are still staged, and the read-back
# fails on a race the proof made itself.
for boot_log_durable in \
    'scripts/test-qemu-production-install-flow.sh' \
    'scripts/test-qemu-installed-boot.sh'; do
    # shellcheck disable=SC2016  # the single-quoted text is the proof's regex
    grep -Fq 'reason=readiness .*flushed=[1-9]' "$boot_log_durable" || {
        echo "proof не ждёт запись о долговечности журнала: $boot_log_durable" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the single-quoted text is what the proof says
for boot_log_installed in \
    'python3 scripts/hfoc-esp-read.py "$DISK" --bootlog' \
    '[BOOT-CHAIN] verdict=ATTESTED' \
    'Appliance boot log on the installed disk:'; do
    grep -Fq "$boot_log_installed" \
        scripts/test-qemu-installed-boot.sh || {
        echo "installed-disk proof не читает журнал: $boot_log_installed" >&2
        exit 1
    }
done

# The hardware run captures the log as evidence and the matrix names it.
grep -Fq 'hfoc_physical_sata_capture_boot_log' \
    scripts/hfoc-physical-sata-continue.sh || {
    echo "аппаратный прогон не снимает журнал" >&2
    exit 1
}
grep -Fq 'APPLIANCE_BOOT_LOG=' \
    scripts/hfoc-physical-sata-continue.sh || {
    echo "аппаратный прогон не записывает журнал в evidence" >&2
    exit 1
}
grep -Fq 'ITEM-410' release/hfoc-hardware-acceptance-matrix.txt || {
    echo "матрица приёмки не называет журнал загрузки" >&2
    exit 1
}
for boot_log_stage in INSTALLER FIRST_BOOT NETWORK_LINK DHCP_DNS AUTH MANAGEMENT; do
    sed -n "/^stage_[0-9]*_id=${boot_log_stage}$/,/^stage_[0-9]*_hardware_observation=/p" \
        release/hfoc-hardware-acceptance-matrix.txt |
        grep -Fq 'ITEM-410' || {
        echo "стадия приёмки не цитирует журнал загрузки: $boot_log_stage" >&2
        exit 1
    }
    sed -n "/^stage_[0-9]*_id=${boot_log_stage}$/,/^stage_[0-9]*_hardware_observation=/p" \
        release/hfoc-hardware-acceptance-matrix.txt |
        grep -Fq -- '--bootlog' || {
        echo "стадия приёмки не называет рецепт чтения: $boot_log_stage" >&2
        exit 1
    }
done

# Host proof membership: the targets exist and the serial suite runs them.
for boot_log_target in \
    test-appliance-boot-log-host \
    test-appliance-boot-log-store-host \
    test-appliance-boot-log-reader-host; do
    grep -Fq "$boot_log_target" Makefile || {
        echo "Makefile не содержит $boot_log_target" >&2
        exit 1
    }
    # shellcheck disable=SC2016
    grep -Fq "\$(MAKE) $boot_log_target" Makefile || {
        echo "test-host-suite не вызывает $boot_log_target" >&2
        exit 1
    }
done

# Documented: the item, where the log lives, the format, the rotation and how a
# human reads it after a hardware run.
for boot_log_doc in \
    docs/hfoc-installed-boot.md \
    docs/hfoc-hardware-acceptance.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-410' "$boot_log_doc" || {
        echo "$boot_log_doc не документирует ITEM-410" >&2
        exit 1
    }
done
for boot_log_recipe in \
    'scripts/hfoc-esp-read.py /dev/sda --bootlog' \
    'scripts/hfoc-esp-read.py --bootlog-file' \
    '\EFI\HFOC\BOOTLOG.BIN' \
    'HFOC-BOOTLOG v=1'; do
    grep -Fq "$boot_log_recipe" docs/hfoc-installed-boot.md || {
        echo "doc не фиксирует рецепт чтения журнала: $boot_log_recipe" >&2
        exit 1
    }
done
grep -Fq -- '--bootlog-file /mnt/EFI/HFOC/BOOTLOG.BIN' \
    docs/hfoc-hardware-acceptance.md || {
    echo "runbook не фиксирует рецепт чтения журнала" >&2
    exit 1
}
