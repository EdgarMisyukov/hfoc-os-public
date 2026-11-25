#!/usr/bin/env bash
# ITEM-508 increment 27: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The bounded TCP listener pool.
#
# ITEM-508 needs an HTTP-01 listener on port 80 beside the management listener
# on 443, but `hfoc_lwip_tcp` kept exactly one active listener in a single
# pointer: a second `init` was refused and `shutdown` cleared only its own
# slot.  This increment replaces the pointer with a two-slot pool, keeps
# `HFOC_LWIP_TCP_ALREADY_PUBLISHED` for "no free slot" (a fourth concurrent
# listener is still refused by name), and proves in the host test that both
# listeners keep their own netif/config/port policy.  This slice pins the pool,
# the per-listener policy, the moved refusal and the documents; no port is
# opened by the wrapper itself.
for item508_inc27_file in \
    kernel/include/hfoc_lwip_tcp.h \
    kernel/src/hfoc_lwip_tcp.c \
    tests/lwip-tcp-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc27_file ]] || {
        echo "ITEM-508 inc27: отсутствует $item508_inc27_file" >&2
        exit 1
    }
done

# 1. The header owns the bound.
grep -Fq '#define HFOC_LWIP_TCP_LISTENER_COUNT 3U' \
    kernel/include/hfoc_lwip_tcp.h || {
    echo "ITEM-508 inc27: заголовок не объявляет пул слушателей" >&2
    exit 1
}

# 2. The wrapper uses the pool, not a single pointer.
for item508_inc27_source_pin in \
    'hfoc_lwip_tcp_active[HFOC_LWIP_TCP_LISTENER_COUNT]' \
    'static bool hfoc_lwip_tcp_listener_slot_available(void)' \
    'static bool hfoc_lwip_tcp_listener_slot_take(' \
    'static void hfoc_lwip_tcp_listener_slot_release(' \
    '!hfoc_lwip_tcp_listener_slot_available()' \
    'hfoc_lwip_tcp_listener_slot_take(listener)' \
    'hfoc_lwip_tcp_listener_slot_release(listener);'; do
    grep -Fq -- "$item508_inc27_source_pin" kernel/src/hfoc_lwip_tcp.c || {
        echo "ITEM-508 inc27: обёртка не содержит «$item508_inc27_source_pin»" \
            >&2
        exit 1
    }
done
if grep -Fq 'hfoc_lwip_tcp_active = listener;' kernel/src/hfoc_lwip_tcp.c ||
    grep -Fq 'hfoc_lwip_tcp_active = NULL;' kernel/src/hfoc_lwip_tcp.c ||
    grep -Fq 'hfoc_lwip_tcp_active != NULL' kernel/src/hfoc_lwip_tcp.c; then
    echo "ITEM-508 inc27: обёртка всё ещё держит один указатель" >&2
    exit 1
fi
# The refusal keeps its name: the pool moves it to the third listener.
grep -Fq 'HFOC_LWIP_TCP_ALREADY_PUBLISHED' kernel/src/hfoc_lwip_tcp.c || {
    echo "ITEM-508 inc27: отказ на лишний слушатель потерян" >&2
    exit 1
}

# 3. The host proof drives three listeners and the moved refusal.
for item508_inc27_test_pin in \
    'static void test_listener_pool_bound(void)' \
    'test_listener_pool_bound();' \
    'the management listener takes the first pool slot' \
    'the challenge listener takes the second pool slot' \
    'the SSH listener takes the third pool slot' \
    'a fourth concurrent listener is refused while three slots are held' \
    'the management listener binds only the port its policy authorizes' \
    'the challenge listener binds only the port its policy authorizes' \
    'the SSH listener binds only the port its policy authorizes' \
    'a listener cannot borrow the other listener'"'"'s port policy' \
    'the freed slot serves the next listener' \
    'the remaining listeners shut down and release their slots'; do
    grep -Fq -- "$item508_inc27_test_pin" tests/lwip-tcp-host.c || {
        echo "ITEM-508 inc27: проба не содержит «$item508_inc27_test_pin»" \
            >&2
        exit 1
    }
done
if grep -Fq 'test_second_listener_rejected' tests/lwip-tcp-host.c; then
    echo "ITEM-508 inc27: старая проба одного слушателя осталась" >&2
    exit 1
fi

# 4. The documents and the queue name the increment.
grep -Fq 'ITEM-508 increment 27' docs/ROADMAP.md || {
    echo "ITEM-508 inc27: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 27' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc27: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 27' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-508 inc27: очередь не называет инкремент" >&2
    exit 1
}
