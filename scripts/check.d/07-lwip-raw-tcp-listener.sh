#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# lwIP raw TCP listener contract (item 238).
grep -Fq 'test-lwip-tcp-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp.h || {
    echo "hfoc_lwip_tcp.h не содержит HFOC_LWIP_TCP_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_BACKLOG 4U' \
    kernel/include/hfoc_lwip_tcp.h || {
    echo "hfoc_lwip_tcp.h не содержит HFOC_LWIP_TCP_BACKLOG 4U" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_init' kernel/src/hfoc_lwip_tcp.c || {
    echo "hfoc_lwip_tcp.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_bind' kernel/src/hfoc_lwip_tcp.c || {
    echo "hfoc_lwip_tcp.c не реализует bind" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_listen' kernel/src/hfoc_lwip_tcp.c || {
    echo "hfoc_lwip_tcp.c не реализует listen" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_shutdown' kernel/src/hfoc_lwip_tcp.c || {
    echo "hfoc_lwip_tcp.c не реализует shutdown" >&2; exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp.c; then
    echo "hfoc_lwip_tcp.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
# ITEM-386: the ordinary production boot owns a management listener, so the
# bounded listener contract of `hfoc_lwip_tcp.h` MUST be present in the
# production ELF.  The old blanket `hfoc_lwip_tcp_` prohibition is replaced by
# an exact allow-list; every other `hfoc_lwip_tcp_`-prefixed symbol (the
# connection adapter, the pool, the deadline scheduler, the backlog, the
# backpressure adapters, the quarantine and the isolation probe) is still
# forbidden, because none of them belongs to the serving path.
for production_listener_symbol in \
    hfoc_lwip_tcp_init \
    hfoc_lwip_tcp_bind \
    hfoc_lwip_tcp_listen \
    hfoc_lwip_tcp_shutdown \
    hfoc_lwip_tcp_published \
    hfoc_lwip_tcp_bound \
    hfoc_lwip_tcp_listening \
    hfoc_lwip_tcp_generation \
    hfoc_lwip_tcp_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $production_listener_symbol" >/dev/null || {
        echo "Production kernel не содержит $production_listener_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_)' |
    grep -Ev ' (hfoc_lwip_tcp_(init|bind|listen|shutdown|published|bound|listening|generation|status_string))$'; then
    echo "Production kernel содержит raw TCP listener symbol beyond the serving contract" >&2
    exit 1
fi
for tcp_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'raw tcp listener contract|raw tcp listener' \
            "$tcp_doc"; then
        echo "$tcp_doc не документирует raw TCP listener contract" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **238. Raw TCP listener contract' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 238 завершённым" >&2
    exit 1
}

# lwIP TCP connection lifecycle adapter (item 239).
grep -Fq 'test-lwip-tcp-conn-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-conn-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-conn-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-conn-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_CONN_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_conn.h || {
    echo "hfoc_lwip_tcp_conn.h не содержит HFOC_LWIP_TCP_CONN_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_CONN_POLL_INTERVAL 1U' \
    kernel/include/hfoc_lwip_tcp_conn.h || {
    echo "hfoc_lwip_tcp_conn.h не содержит HFOC_LWIP_TCP_CONN_POLL_INTERVAL 1U" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_conn_accept' \
    kernel/src/hfoc_lwip_tcp_conn.c || {
    echo "hfoc_lwip_tcp_conn.c не реализует accept" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_conn_recv' \
    kernel/src/hfoc_lwip_tcp_conn.c || {
    echo "hfoc_lwip_tcp_conn.c не реализует recv" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_conn_sent' \
    kernel/src/hfoc_lwip_tcp_conn.c || {
    echo "hfoc_lwip_tcp_conn.c не реализует sent" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_conn_poll' \
    kernel/src/hfoc_lwip_tcp_conn.c || {
    echo "hfoc_lwip_tcp_conn.c не реализует poll" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_conn_error' \
    kernel/src/hfoc_lwip_tcp_conn.c || {
    echo "hfoc_lwip_tcp_conn.c не реализует error" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_conn_close' \
    kernel/src/hfoc_lwip_tcp_conn.c || {
    echo "hfoc_lwip_tcp_conn.c не реализует close" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_conn_abort' \
    kernel/src/hfoc_lwip_tcp_conn.c || {
    echo "hfoc_lwip_tcp_conn.c не реализует abort" >&2; exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_conn.c; then
    echo "hfoc_lwip_tcp_conn.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_conn_)'; then
    echo "Production kernel содержит raw TCP connection symbol" >&2
    exit 1
fi
for conn_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'tcp connection lifecycle adapter|tcp connection lifecycle' \
            "$conn_doc"; then
        echo "$conn_doc не документирует TCP connection lifecycle adapter" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **239. TCP connection lifecycle adapter' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 239 завершённым" >&2
    exit 1
}

# lwIP TCP listener QEMU proof (item 240).
grep -Fq 'test-tcp-listener-qemu' Makefile || {
    echo "Makefile не содержит test-tcp-listener-qemu" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-tcp-listener-qemu' Makefile || {
    echo "test-serial/test не содержит test-tcp-listener-qemu" >&2;
    exit 1; }
grep -Fq 'scripts/test-qemu-tcp-listener.sh' Makefile || {
    echo "Makefile не содержит TCP listener QEMU script" >&2; exit 1; }
grep -Fq 'HFOC_TCP_LISTENER_QEMU_TEST' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "virtio_net_frame_qemu.c не содержит HFOC_TCP_LISTENER_QEMU_TEST gate" >&2;
    exit 1; }
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[TCP-LISTENER]'; then
    echo "Production kernel содержит TCP listener marker" >&2
    exit 1
fi
# ITEM-386 narrows this gate to the fixture-only proof symbols: the production
# listener contract is asserted positively above, and the fixture's connection
# adapter and proof state must still never reach the production ELF.
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_conn_|vnf_tcp_)'; then
    echo "Production kernel содержит TCP listener proof symbol" >&2
    exit 1
fi
if grep -Fq 'qemu-tcp-listener' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит TCP listener script" >&2
    exit 1
fi
grep -Fq 'QEMU TCP listener baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-tcp-listener.sh || {
    echo "test-qemu-tcp-listener.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU TCP listener test: OK' \
    scripts/test-qemu-tcp-listener.sh || {
    echo "test-qemu-tcp-listener.sh не содержит success marker" >&2
    exit 1
}
for tcp_listener_marker in \
    '[TCP-LISTENER] proof init' \
    '[TCP-LISTENER] listener live' \
    '[TCP-LISTENER] echo ok' \
    '[TCP-LISTENER] reset ok' \
    '[TCP-LISTENER] timeout ok' \
    '[TCP-LISTENER] backlog ok' \
    '[TCP-LISTENER] balanced pool' \
    '[TCP-LISTENER] clean shutdown' \
    '[TCP-LISTENER] proof OK'; do
    grep -Fq "$tcp_listener_marker" \
        kernel/src/virtio_net_frame_qemu.c || {
        echo "virtio_net_frame_qemu.c lacks $tcp_listener_marker" >&2
        exit 1
    }
    grep -Fq "$tcp_listener_marker" \
        scripts/test-qemu-tcp-listener.sh || {
        echo "test-qemu-tcp-listener.sh lacks $tcp_listener_marker" >&2
        exit 1
    }
done
for tcp_listener_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'tcp listener.*qemu|tcp-listener.*qemu|tcp listener qemu proof' \
            "$tcp_listener_doc"; then
        echo "$tcp_listener_doc не документирует TCP listener QEMU proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **240. TCP listener QEMU proof' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 240 завершённым" >&2
    exit 1
}

# lwIP four-slot connection pool (item 241).
grep -Fq 'test-lwip-tcp-pool-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-pool-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-pool-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-pool-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_POOL_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_pool.h || {
    echo "hfoc_lwip_tcp_pool.h не содержит HFOC_LWIP_TCP_POOL_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_POOL_SLOT_COUNT 4U' \
    kernel/include/hfoc_lwip_tcp_pool.h || {
    echo "hfoc_lwip_tcp_pool.h не содержит HFOC_LWIP_TCP_POOL_SLOT_COUNT 4U" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_init' \
    kernel/src/hfoc_lwip_tcp_pool.c || {
    echo "hfoc_lwip_tcp_pool.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_reserve' \
    kernel/src/hfoc_lwip_tcp_pool.c || {
    echo "hfoc_lwip_tcp_pool.c не реализует reserve" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_publish' \
    kernel/src/hfoc_lwip_tcp_pool.c || {
    echo "hfoc_lwip_tcp_pool.c не реализует publish" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_close' \
    kernel/src/hfoc_lwip_tcp_pool.c || {
    echo "hfoc_lwip_tcp_pool.c не реализует close" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_abort' \
    kernel/src/hfoc_lwip_tcp_pool.c || {
    echo "hfoc_lwip_tcp_pool.c не реализует abort" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_release' \
    kernel/src/hfoc_lwip_tcp_pool.c || {
    echo "hfoc_lwip_tcp_pool.c не реализует release" >&2; exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_pool.c; then
    echo "hfoc_lwip_tcp_pool.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_pool_)'; then
    echo "Production kernel содержит four-slot connection pool symbol" >&2
    exit 1
fi
for pool_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'four-slot connection pool|four-slot pool|connection pool' \
            "$pool_doc"; then
        echo "$pool_doc не документирует four-slot connection pool" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **241. Four-slot connection pool' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 241 завершённым" >&2
    exit 1
}

# lwIP connection deadline scheduler (item 242).
grep -Fq 'test-lwip-tcp-deadline-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-deadline-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-deadline-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-deadline-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_DEADLINE_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_deadline.h || {
    echo "hfoc_lwip_tcp_deadline.h не содержит HFOC_LWIP_TCP_DEADLINE_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT' \
    kernel/include/hfoc_lwip_tcp_deadline.h || {
    echo "hfoc_lwip_tcp_deadline.h не содержит HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_deadline_attach' \
    kernel/src/hfoc_lwip_tcp_deadline.c || {
    echo "hfoc_lwip_tcp_deadline.c не реализует attach" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_deadline_touch' \
    kernel/src/hfoc_lwip_tcp_deadline.c || {
    echo "hfoc_lwip_tcp_deadline.c не реализует touch" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_deadline_begin_close' \
    kernel/src/hfoc_lwip_tcp_deadline.c || {
    echo "hfoc_lwip_tcp_deadline.c не реализует begin-close" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_deadline_check' \
    kernel/src/hfoc_lwip_tcp_deadline.c || {
    echo "hfoc_lwip_tcp_deadline.c не реализует check" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_deadline_cancel' \
    kernel/src/hfoc_lwip_tcp_deadline.c || {
    echo "hfoc_lwip_tcp_deadline.c не реализует cancel" >&2; exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_deadline.c; then
    echo "hfoc_lwip_tcp_deadline.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_deadline_)'; then
    echo "Production kernel содержит connection deadline scheduler symbol" >&2
    exit 1
fi
for deadline_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'deadline scheduler|connection deadline|deadline' \
            "$deadline_doc"; then
        echo "$deadline_doc не документирует connection deadline scheduler" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **242. Connection deadline scheduler' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 242 завершённым" >&2
    exit 1
}

# lwIP listener backlog admission (item 243).
grep -Fq 'test-lwip-tcp-backlog-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-backlog-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-backlog-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-backlog-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_BACKLOG_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_backlog.h || {
    echo "hfoc_lwip_tcp_backlog.h не содержит HFOC_LWIP_TCP_BACKLOG_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_BACKLOG_CAPACITY' \
    kernel/include/hfoc_lwip_tcp_backlog.h || {
    echo "hfoc_lwip_tcp_backlog.h не содержит HFOC_LWIP_TCP_BACKLOG_CAPACITY" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_backlog_init' \
    kernel/src/hfoc_lwip_tcp_backlog.c || {
    echo "hfoc_lwip_tcp_backlog.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_backlog_admit' \
    kernel/src/hfoc_lwip_tcp_backlog.c || {
    echo "hfoc_lwip_tcp_backlog.c не реализует admit" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_backlog_drain' \
    kernel/src/hfoc_lwip_tcp_backlog.c || {
    echo "hfoc_lwip_tcp_backlog.c не реализует drain" >&2; exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_backlog.c; then
    echo "hfoc_lwip_tcp_backlog.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_backlog_)'; then
    echo "Production kernel содержит listener backlog admission symbol" >&2
    exit 1
fi
for backlog_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'listener backlog admission|backlog admission|backlog' \
            "$backlog_doc"; then
        echo "$backlog_doc не документирует listener backlog admission" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **243. Listener backlog admission' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 243 завершённым" >&2
    exit 1
}

# lwIP RX backpressure propagation (item 244).
grep -Fq 'test-lwip-tcp-rx-backpressure-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-rx-backpressure-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-rx-backpressure-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-rx-backpressure-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_RX_BACKPRESSURE_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_rx_backpressure.h || {
    echo "hfoc_lwip_tcp_rx_backpressure.h не содержит HFOC_LWIP_TCP_RX_BACKPRESSURE_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_RX_BACKPRESSURE_PACKET_CAPACITY_MAX' \
    kernel/include/hfoc_lwip_tcp_rx_backpressure.h || {
    echo "hfoc_lwip_tcp_rx_backpressure.h не содержит HFOC_LWIP_TCP_RX_BACKPRESSURE_PACKET_CAPACITY_MAX" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_init' \
    kernel/src/hfoc_lwip_tcp_rx_backpressure.c || {
    echo "hfoc_lwip_tcp_rx_backpressure.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_attach' \
    kernel/src/hfoc_lwip_tcp_rx_backpressure.c || {
    echo "hfoc_lwip_tcp_rx_backpressure.c не реализует attach" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_receive' \
    kernel/src/hfoc_lwip_tcp_rx_backpressure.c || {
    echo "hfoc_lwip_tcp_rx_backpressure.c не реализует receive" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_release' \
    kernel/src/hfoc_lwip_tcp_rx_backpressure.c || {
    echo "hfoc_lwip_tcp_rx_backpressure.c не реализует release" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_rx_backpressure_detach' \
    kernel/src/hfoc_lwip_tcp_rx_backpressure.c || {
    echo "hfoc_lwip_tcp_rx_backpressure.c не реализует detach" >&2; exit 1; }
grep -Fq 'tcp_recved' \
    kernel/src/hfoc_lwip_tcp_rx_backpressure.c || {
    echo "hfoc_lwip_tcp_rx_backpressure.c не вызывает tcp_recved" >&2;
    exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_rx_backpressure.c; then
    echo "hfoc_lwip_tcp_rx_backpressure.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_rx_backpressure_)'; then
    echo "Production kernel содержит RX backpressure symbol" >&2
    exit 1
fi
for rx_backpressure_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'RX backpressure|receive backpressure|backpressure' \
            "$rx_backpressure_doc"; then
        echo "$rx_backpressure_doc не документирует RX backpressure" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **244. RX backpressure propagation' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 244 завершённым" >&2
    exit 1
}

# lwIP TX backpressure propagation (item 245).
grep -Fq 'test-lwip-tcp-tx-backpressure-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-tx-backpressure-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-tx-backpressure-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-tx-backpressure-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_TX_BACKPRESSURE_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_tx_backpressure.h || {
    echo "hfoc_lwip_tcp_tx_backpressure.h не содержит HFOC_LWIP_TCP_TX_BACKPRESSURE_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_CAPACITY_MAX' \
    kernel/include/hfoc_lwip_tcp_tx_backpressure.h || {
    echo "hfoc_lwip_tcp_tx_backpressure.h не содержит HFOC_LWIP_TCP_TX_BACKPRESSURE_RESPONSE_CAPACITY_MAX" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_init' \
    kernel/src/hfoc_lwip_tcp_tx_backpressure.c || {
    echo "hfoc_lwip_tcp_tx_backpressure.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_attach' \
    kernel/src/hfoc_lwip_tcp_tx_backpressure.c || {
    echo "hfoc_lwip_tcp_tx_backpressure.c не реализует attach" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_submit' \
    kernel/src/hfoc_lwip_tcp_tx_backpressure.c || {
    echo "hfoc_lwip_tcp_tx_backpressure.c не реализует submit" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_publish' \
    kernel/src/hfoc_lwip_tcp_tx_backpressure.c || {
    echo "hfoc_lwip_tcp_tx_backpressure.c не реализует publish" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_sent' \
    kernel/src/hfoc_lwip_tcp_tx_backpressure.c || {
    echo "hfoc_lwip_tcp_tx_backpressure.c не реализует sent" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_tx_backpressure_detach' \
    kernel/src/hfoc_lwip_tcp_tx_backpressure.c || {
    echo "hfoc_lwip_tcp_tx_backpressure.c не реализует detach" >&2; exit 1; }
grep -Fq 'tcp_write' \
    kernel/src/hfoc_lwip_tcp_tx_backpressure.c || {
    echo "hfoc_lwip_tcp_tx_backpressure.c не вызывает tcp_write" >&2;
    exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_tx_backpressure.c; then
    echo "hfoc_lwip_tcp_tx_backpressure.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_tx_backpressure_)'; then
    echo "Production kernel содержит TX backpressure symbol" >&2
    exit 1
fi
for tx_backpressure_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'TX backpressure|response progress|neither skips nor duplicates' \
            "$tx_backpressure_doc"; then
        echo "$tx_backpressure_doc не документирует TX backpressure" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **245. TX backpressure propagation' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 245 завершённым" >&2
    exit 1
}

# lwIP stale callback quarantine (item 246).
grep -Fq 'test-lwip-tcp-quarantine-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-quarantine-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-quarantine-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-quarantine-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_QUARANTINE_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_quarantine.h || {
    echo "hfoc_lwip_tcp_quarantine.h не содержит HFOC_LWIP_TCP_QUARANTINE_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT' \
    kernel/include/hfoc_lwip_tcp_quarantine.h || {
    echo "hfoc_lwip_tcp_quarantine.h не содержит HFOC_LWIP_TCP_QUARANTINE_SLOT_COUNT" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_quarantine_init' \
    kernel/src/hfoc_lwip_tcp_quarantine.c || {
    echo "hfoc_lwip_tcp_quarantine.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_quarantine_attach' \
    kernel/src/hfoc_lwip_tcp_quarantine.c || {
    echo "hfoc_lwip_tcp_quarantine.c не реализует attach" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_quarantine_check' \
    kernel/src/hfoc_lwip_tcp_quarantine.c || {
    echo "hfoc_lwip_tcp_quarantine.c не реализует check" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_quarantine_detach' \
    kernel/src/hfoc_lwip_tcp_quarantine.c || {
    echo "hfoc_lwip_tcp_quarantine.c не реализует detach" >&2; exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_quarantine.c; then
    echo "hfoc_lwip_tcp_quarantine.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_quarantine_)'; then
    echo "Production kernel содержит stale callback quarantine symbol" >&2
    exit 1
fi
for quarantine_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'stale callback quarantine|delayed callbacks after abort/reuse' \
            "$quarantine_doc"; then
        echo "$quarantine_doc не документирует stale callback quarantine" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **246. Stale callback quarantine' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 246 завершённым" >&2
    exit 1
}

# lwIP malformed TCP isolation matrix (item 247).
grep -Fq 'test-lwip-tcp-isolation-host' Makefile || {
    echo "Makefile не содержит test-lwip-tcp-isolation-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-tcp-isolation-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-tcp-isolation-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_ISOLATION_VERSION 1U' \
    kernel/include/hfoc_lwip_tcp_isolation.h || {
    echo "hfoc_lwip_tcp_isolation.h не содержит HFOC_LWIP_TCP_ISOLATION_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TCP_ISOLATION_EVENT_REMOTE_ABORT' \
    kernel/include/hfoc_lwip_tcp_isolation.h || {
    echo "hfoc_lwip_tcp_isolation.h не содержит REMOTE_ABORT event" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_tcp_isolation_init' \
    kernel/src/hfoc_lwip_tcp_isolation.c || {
    echo "hfoc_lwip_tcp_isolation.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_isolation_attach' \
    kernel/src/hfoc_lwip_tcp_isolation.c || {
    echo "hfoc_lwip_tcp_isolation.c не реализует attach" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_isolation_inject' \
    kernel/src/hfoc_lwip_tcp_isolation.c || {
    echo "hfoc_lwip_tcp_isolation.c не реализует inject" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_isolation_detach' \
    kernel/src/hfoc_lwip_tcp_isolation.c || {
    echo "hfoc_lwip_tcp_isolation.c не реализует detach" >&2; exit 1; }
grep -Fq 'hfoc_lwip_tcp_pool_abort' \
    kernel/src/hfoc_lwip_tcp_isolation.c || {
    echo "hfoc_lwip_tcp_isolation.c не вызывает pool abort" >&2;
    exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_tcp_isolation.c; then
    echo "hfoc_lwip_tcp_isolation.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (hfoc_lwip_tcp_isolation_)'; then
    echo "Production kernel содержит malformed TCP isolation symbol" >&2
    exit 1
fi
for isolation_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'malformed TCP isolation|affected connection' \
            "$isolation_doc"; then
        echo "$isolation_doc не документирует malformed TCP isolation matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **247. Malformed TCP isolation matrix' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 247 завершённым" >&2
    exit 1
}
