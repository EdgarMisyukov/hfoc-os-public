SHELL := /bin/bash
CC ?= gcc
LD ?= ld
VERSION := $(shell cat VERSION)
IMAGE := build/hfoc-os-$(VERSION).iso
KERNEL := build/hfoc-kernel.elf
LIMINE := deps/limine
BUILD_INPUT_MANIFEST := build/hfoc-build-input.txt
BUILD_INPUT_FILES := boot/limine.conf boot/limine-installer.conf \
    kernel/linker.ld VERSION Makefile \
    scripts/check.sh scripts/hfoc-install-media-stage.sh \
    scripts/hfoc-host-seed-provision.py \
    scripts/hfoc-install-media-manifest.py \
    scripts/hfoc-installed-config.py \
    $(wildcard scripts/check.d/*.sh)

# HFOC OS 0.2.x intentionally uses the legacy 8259 PIC. The deterministic
# QEMU profile disables the virtual Local APIC for interactive runs. Existing
# APIC/IOAPIC tests use explicit APIC-enabled profiles; Stage 2 already
# provides a verified IRQ0/PIT IOAPIC shadow route, and the remaining future
# decision is Stage 3 PIC masking.
QEMU_MACHINE ?= q35
QEMU_CPU ?= max,-apic
QEMU_COMMON := -machine $(QEMU_MACHINE) -m 512M -cpu $(QEMU_CPU) \
    -display gtk,grab-on-hover=on \
    -cdrom $(IMAGE) -serial stdio -no-reboot

CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
    -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -Ikernel/include -Ithird_party/lwip/src/include
# ITEM-552: the build identity the diagnostic report carries.  The revision is
# the commit the kernel was compiled from (a build outside a repository answers
# "unknown" and still boots), and the version is the VERSION file's value, so a
# report read on another machine says which build produced it.
#
# ITEM-557: the identity is a generated header, not a -D flag.  A flag is not a
# dependency: make has no reason to rebuild a "hfoc_build.c" whose source did
# not change, so a tree checked out at another revision - exactly what the
# diag-export object tree was - kept publishing the revision it was first
# compiled at (measured: build=2ef08db50f50 for a kernel built from 35b2188d).
# The header is rewritten only when its content changes, and the one file that
# includes it then rebuilds and the kernel relinks, so the report cannot name a
# revision the binary does not come from.
BUILD_ID := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
BUILD_ID_HEADER := build/generated/hfoc-build-id.h
$(shell mkdir -p build/generated; \
        printf '#ifndef HFOC_GENERATED_BUILD_ID_H\n#define HFOC_GENERATED_BUILD_ID_H\n#define HFOC_BUILD_ID "%s"\n#define HFOC_VERSION_STRING "%s"\n#endif\n' \
            '$(BUILD_ID)' '$(VERSION)' > '$(BUILD_ID_HEADER).new'; \
        cmp -s '$(BUILD_ID_HEADER).new' '$(BUILD_ID_HEADER)' || \
            mv '$(BUILD_ID_HEADER).new' '$(BUILD_ID_HEADER)'; \
        rm -f '$(BUILD_ID_HEADER).new')
CFLAGS += -Ibuild/generated
ASFLAGS := -g -ffreestanding -fno-pic -fno-pie -m64 -mcmodel=kernel -mno-red-zone
# ITEM-380 reproducible release: pin every ISO file date and the volume
# creation/modification date to the source commit time, so two builds of the
# same revision produce the same ISO digest.  SOURCE_DATE_EPOCH may be
# overridden by the release environment.
SOURCE_DATE_EPOCH ?= $(shell git log -1 --format=%ct 2>/dev/null || echo 0)
ISO_REPRODUCIBLE_DATE := $(shell date -u -d @$(SOURCE_DATE_EPOCH) +%Y%m%d%H%M%S00 2>/dev/null || echo 1970010100000000)
ISO_REPRODUCIBLE_FLAGS := --set_all_file_dates $(ISO_REPRODUCIBLE_DATE) --modification-date=$(ISO_REPRODUCIBLE_DATE)
# xorriso generates random GPT GUIDs for the embedded EFI partition; the seed
# makes them a function of the source revision, so the same revision produces
# the same ISO digest.
ISO_GPT_SEED := $(shell git rev-parse HEAD 2>/dev/null || echo unknown)
LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 \
    --build-id=none -T kernel/linker.ld

C_SOURCES := $(shell find kernel/src -type f -name '*.c' -print | sort)
ASM_SOURCES := $(shell find kernel/src -type f -name '*.S' -print | sort)
C_OBJECTS := $(patsubst kernel/src/%.c,build/obj/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst kernel/src/%.S,build/obj/%.o,$(ASM_SOURCES))
OBJECTS := $(C_OBJECTS) $(ASM_OBJECTS)

# ITEM-372 verification-only mbedTLS profile.  The production kernel and the
# boot-chain proof kernel link this bounded subset; no TLS, X.509, PK, PSA,
# RNG or cipher source is part of it, and every allocation inside it goes
# through the fixed arena of image_signature_ecdsa.c.
HFOC_VERIFY_MBEDTLS_SOURCES := \
	third_party/mbedtls/library/asn1parse.c \
	third_party/mbedtls/library/asn1write.c \
	third_party/mbedtls/library/bignum.c \
	third_party/mbedtls/library/bignum_core.c \
	third_party/mbedtls/library/bignum_mod.c \
	third_party/mbedtls/library/bignum_mod_raw.c \
	third_party/mbedtls/library/constant_time.c \
	third_party/mbedtls/library/ecdsa.c \
	third_party/mbedtls/library/ecp.c \
	third_party/mbedtls/library/ecp_curves.c \
	third_party/mbedtls/library/ecp_curves_new.c \
	third_party/mbedtls/library/md.c \
	third_party/mbedtls/library/platform.c \
	third_party/mbedtls/library/platform_util.c \
	third_party/mbedtls/library/sha256.c
HFOC_VERIFY_MBEDTLS_CFLAGS := \
	-DHFOC_IMAGE_SIGNATURE_MBEDTLS \
	-Ithird_party/mbedtls/include \
	-DMBEDTLS_CONFIG_FILE='"hfoc_verify_config.h"'
HFOC_VERIFY_KERNEL_OBJ_DIR := build/verify-kernel
HFOC_VERIFY_KERNEL_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(HFOC_VERIFY_KERNEL_OBJ_DIR)/%.o,$(HFOC_VERIFY_MBEDTLS_SOURCES))
HFOC_VERIFY_HOST_OBJ_DIR := build/verify-host
HFOC_VERIFY_HOST_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(HFOC_VERIFY_HOST_OBJ_DIR)/%.o,$(HFOC_VERIFY_MBEDTLS_SOURCES))

$(HFOC_VERIFY_KERNEL_OBJ_DIR)/%.o: third_party/mbedtls/library/%.c \
		kernel/include/hfoc_verify_config.h \
		kernel/include/hfoc_verify_symbols.h \
		kernel/include/hfoc_verify_alloc.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(HFOC_VERIFY_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

$(HFOC_VERIFY_HOST_OBJ_DIR)/%.o: third_party/mbedtls/library/%.c \
		kernel/include/hfoc_verify_config.h \
		kernel/include/hfoc_verify_symbols.h \
		kernel/include/hfoc_verify_alloc.h
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		$(HFOC_VERIFY_MBEDTLS_CFLAGS) -Ikernel/include -Itests \
		-c $< -o $@

# ITEM-373 state-encryption crypto profile.  It links exactly one Mbed TLS
# primitive, AES-256-CTR, for the sealed state volume; HMAC and PBKDF2 are
# built on the HFOC SHA-256 implementation.  The production kernel object is
# recompiled with the state profile, and the focused host proof links the same
# object shape from build/state-crypto-host.
HFOC_STATE_CRYPTO_MBEDTLS_SOURCES := \
	third_party/mbedtls/library/aes.c
HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS := \
	-Ithird_party/mbedtls/include \
	-DMBEDTLS_CONFIG_FILE='"hfoc_state_crypto_config.h"'
HFOC_STATE_CRYPTO_KERNEL_OBJ_DIR := build/state-crypto-kernel
HFOC_STATE_CRYPTO_KERNEL_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(HFOC_STATE_CRYPTO_KERNEL_OBJ_DIR)/%.o,$(HFOC_STATE_CRYPTO_MBEDTLS_SOURCES))
HFOC_STATE_CRYPTO_HOST_OBJ_DIR := build/state-crypto-host
HFOC_STATE_CRYPTO_HOST_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(HFOC_STATE_CRYPTO_HOST_OBJ_DIR)/%.o,$(HFOC_STATE_CRYPTO_MBEDTLS_SOURCES))

$(HFOC_STATE_CRYPTO_KERNEL_OBJ_DIR)/%.o: third_party/mbedtls/library/%.c \
		kernel/include/hfoc_state_crypto_config.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

$(HFOC_STATE_CRYPTO_HOST_OBJ_DIR)/%.o: third_party/mbedtls/library/%.c \
		kernel/include/hfoc_state_crypto_config.h
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		$(HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS) -Ikernel/include -Itests \
		-c $< -o $@
# ITEM-386 production TLS-serving mbedTLS profile.  The ordinary-boot
# management listener terminates TLS 1.3 and generates one self-signed P-256
# certificate at first boot, so the production kernel links the TLS, X.509,
# PK, PSA and CTR-DRBG sources under a dedicated profile header.
#
# ITEM-386 object-partition rule (the last blocker of the item): every mbedTLS
# translation unit that reads the configuration or installs an allocator hook
# is compiled once per profile into its own object directory, with that
# profile's -DMBEDTLS_CONFIG_FILE and its own include/define set.  No object is
# shared between the TLS profile and the verification profile.
#
# The TLS closure is therefore the whole library, not "the library minus the
# objects the other profiles happen to provide".  Sharing a translation unit
# between the two profiles is what broke the handshake: the TLS path resolved
# mbedtls_mpi_*/mbedtls_ecp_*/mbedtls_md_* to the verification objects, which
# are compiled with MBEDTLS_PLATFORM_CALLOC_MACRO=hfoc_verify_calloc, so the
# connection was built from the verification arena while the TLS path released
# it through hfoc_mbedtls_free — the cross-arena free that surfaced in-kernel
# as MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE at the ServerHello write.
#
# The verification profile keeps the plain mbedTLS symbol names from the TLS
# profile's point of view by moving its own closure into the
# hfoc_verify_mbedtls_* namespace (kernel/include/hfoc_verify_symbols.h), so
# both complete sets link into one image without a duplicate symbol.
#
# ITEM-373 state encryption is the one deliberate exception, and it is not a
# shared object: the state profile's aes.o is a strict superset of the TLS
# profile's AES shape (it adds mbedtls_aes_crypt_ctr, which only the sealed
# state volume uses), it is compiled in its own directory under its own frozen
# header, and it reaches no allocator hook in either profile.  The TLS closure
# therefore consumes AES from the state object instead of compiling a second,
# strictly weaker copy of the same translation unit.  The pin below asserts
# that exclusion is the only one.
# Nothing here reaches C_SOURCES, which stays free of third-party sources.
HFOC_TLS_MBEDTLS_CFLAGS := \
	-DHFOC_PRODUCTION_TLS \
	-Ithird_party/mbedtls/include \
	-DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_tls_config.h"'
HFOC_TLS_MBEDTLS_ALL_SOURCES := $(shell find third_party/mbedtls/library -type f -name '*.c' -print | sort)
HFOC_TLS_MBEDTLS_STATE_SOURCES := $(HFOC_STATE_CRYPTO_MBEDTLS_SOURCES)
HFOC_TLS_MBEDTLS_SOURCES := $(filter-out $(HFOC_TLS_MBEDTLS_STATE_SOURCES),$(HFOC_TLS_MBEDTLS_ALL_SOURCES))
# ITEM-386: the TLS-serving mbedTLS objects are pure crypto; they must not see
# the lwIP include path, or mbedTLS picks up lwIP's AF_INET6 and then requires
# a platform inet_pton the freestanding kernel does not have.
HFOC_TLS_MBEDTLS_BASE_CFLAGS := $(filter-out -Ithird_party/lwip/src/include,$(CFLAGS))
HFOC_TLS_KERNEL_OBJ_DIR := build/tls-kernel
HFOC_TLS_KERNEL_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(HFOC_TLS_KERNEL_OBJ_DIR)/%.o,$(HFOC_TLS_MBEDTLS_SOURCES))
HFOC_TLS_KERNEL_DEPS := $(patsubst third_party/mbedtls/library/%.c,$(HFOC_TLS_KERNEL_OBJ_DIR)/%.d,$(HFOC_TLS_MBEDTLS_SOURCES))

$(HFOC_TLS_KERNEL_OBJ_DIR)/%.o: third_party/mbedtls/library/%.c \
		kernel/include/hfoc_mbedtls_tls_config.h \
		kernel/include/hfoc_mbedtls_config.h \
		kernel/include/hfoc_mbedtls_alloc.h \
		kernel/include/hfoc_mbedtls_platform.h
	@mkdir -p $(@D)
	$(CC) $(HFOC_TLS_MBEDTLS_BASE_CFLAGS) $(HFOC_TLS_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

LWIP_PROTO_SOURCES := \
	third_party/lwip/src/core/def.c \
	third_party/lwip/src/core/inet_chksum.c \
	third_party/lwip/src/core/ip.c \
	third_party/lwip/src/core/ipv4/etharp.c \
	third_party/lwip/src/core/ipv4/icmp.c \
	third_party/lwip/src/core/ipv4/ip4.c \
	third_party/lwip/src/core/ipv4/ip4_addr.c \
	third_party/lwip/src/core/mem.c \
	third_party/lwip/src/core/memp.c \
	third_party/lwip/src/core/netif.c \
	third_party/lwip/src/core/pbuf.c \
	third_party/lwip/src/core/raw.c \
	third_party/lwip/src/core/stats.c \
	third_party/lwip/src/core/sys.c \
	third_party/lwip/src/core/tcp.c \
	third_party/lwip/src/core/tcp_in.c \
	third_party/lwip/src/core/tcp_out.c \
	third_party/lwip/src/core/timeouts.c \
	third_party/lwip/src/core/udp.c \
	third_party/lwip/src/netif/ethernet.c

LWIP_TCP_SOURCES := $(LWIP_PROTO_SOURCES)

# ITEM-362A production NIC/lwIP backend and ITEM-364 production service/HTTPS
# activation. The ordinary-boot kernel links the frozen lwIP protocol profile,
# compiles the production network backend with the lwIP-facing composition
# enabled, and compiles the production service activation glue. All macro-gated
# proof builds keep the lwIP protocol translation unit as a stub.
#
# ITEM-475 (round 14): the ordinary production build also keeps the legacy
# 8259 PIC + PIT as the 100 Hz tick source. The owner's machine measured the
# periodic Local APIC timer at ~2.4 Hz, so the verified runtime transition is
# compiled out here (`lapic_timer_runtime_transition_allowed()` returns false,
# `tick_source=pit`); the drill/QEMU builds that prove the transition --
# $(LAPIC_RUNTIME_ISO) and the oneshot/x2apic/fallback/scheduler proof kernels
# -- do not define the macro and keep it.
# ITEM-601: the ordinary kernel includes the opaque SSH boot header, whose
# owner header needs mbedTLS types.  Every production-derived profile (SNI,
# ACME, forwarding, ...) compiles kernel.c with PRODUCTION_CFLAGS, so the
# include path and the TLS profile config travel with those flags; a per-file
# rule that needs another mbedTLS profile appends its own -D after this one.
PRODUCTION_CFLAGS := $(CFLAGS) -DHFOC_PRODUCTION_NETWORK -DHFOC_PRODUCTION_LWIP -DHFOC_PRODUCTION_SERVICES -DHFOC_PRODUCTION_STATE_ENCRYPTION -DHFOC_PRODUCTION_TLS -DHFOC_PRODUCTION_PIT_TICK \
	-Ithird_party/mbedtls/include -DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_tls_config.h"'
PRODUCTION_LWIP_OBJ_DIR := build/prodnet-lwip
PRODUCTION_LWIP_OBJS := $(patsubst third_party/lwip/src/%.c,$(PRODUCTION_LWIP_OBJ_DIR)/%.o,$(LWIP_PROTO_SOURCES))
PRODUCTION_LWIP_DEPS := $(patsubst third_party/lwip/src/%.c,$(PRODUCTION_LWIP_OBJ_DIR)/%.d,$(LWIP_PROTO_SOURCES))

SSH_QEMU_OBJ := build/ssh-qemu/appliance_production_ssh.o
SSH_QEMU_OBJECTS := $(filter-out build/obj/appliance_production_ssh.o,$(OBJECTS)) $(SSH_QEMU_OBJ)
SSH_QEMU_KERNEL := build/ssh-qemu/hfoc-kernel.elf
SSH_QEMU_ISO := build/ssh-qemu/hfoc-os-$(VERSION)-ssh-qemu.iso
SSH_QEMU_ISO_ROOT := build/ssh-qemu/iso_root

FALLBACK_OBJ_DIR := build/fallback/obj
FALLBACK_C_OBJS := $(patsubst kernel/src/%.c,$(FALLBACK_OBJ_DIR)/%.o,$(C_SOURCES))
FALLBACK_ASM_OBJS := $(patsubst kernel/src/%.S,$(FALLBACK_OBJ_DIR)/%.o,$(ASM_SOURCES))
FALLBACK_OBJS := $(FALLBACK_C_OBJS) $(FALLBACK_ASM_OBJS)
FALLBACK_DEPS := $(patsubst kernel/src/%.c,$(FALLBACK_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(FALLBACK_OBJ_DIR)/%.d,$(ASM_SOURCES))
FALLBACK_KERNEL := build/fallback/hfoc-kernel.elf
FALLBACK_ISO := build/fallback/hfoc-os-$(VERSION)-fallback.iso
FALLBACK_ISO_ROOT := build/fallback/iso_root
FALLBACK_CFLAGS := $(CFLAGS) -DHFOC_IRQ0_ROUTE_FAILURE_TEST

ONESHOT_OBJ_DIR := build/oneshot/obj
ONESHOT_C_OBJS := $(patsubst kernel/src/%.c,$(ONESHOT_OBJ_DIR)/%.o,$(C_SOURCES))
ONESHOT_ASM_OBJS := $(patsubst kernel/src/%.S,$(ONESHOT_OBJ_DIR)/%.o,$(ASM_SOURCES))
ONESHOT_OBJS := $(ONESHOT_C_OBJS) $(ONESHOT_ASM_OBJS)
ONESHOT_DEPS := $(patsubst kernel/src/%.c,$(ONESHOT_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(ONESHOT_OBJ_DIR)/%.d,$(ASM_SOURCES))
ONESHOT_KERNEL := build/oneshot/hfoc-kernel.elf
ONESHOT_ISO := build/oneshot/hfoc-os-$(VERSION)-oneshot.iso
ONESHOT_ISO_ROOT := build/oneshot/iso_root
ONESHOT_CFLAGS := $(CFLAGS) -DHFOC_LAPIC_TIMER_ONESHOT_TEST

# ITEM-475 (round 14): the drill/QEMU kernel that keeps the verified Local
# APIC timer runtime transition.  The ordinary production build compiles it out
# (`HFOC_PRODUCTION_PIT_TICK`), so the APIC-focused QEMU proofs that assert the
# `[LAPIC-TIMER] runtime` transition, the PIC disable and the INT=0x42 trace
# run here instead of on the production ISO, which now ticks on PIT.
LAPIC_RUNTIME_OBJ_DIR := build/lapic-runtime/obj
LAPIC_RUNTIME_C_OBJS := $(patsubst kernel/src/%.c,$(LAPIC_RUNTIME_OBJ_DIR)/%.o,$(C_SOURCES))
LAPIC_RUNTIME_ASM_OBJS := $(patsubst kernel/src/%.S,$(LAPIC_RUNTIME_OBJ_DIR)/%.o,$(ASM_SOURCES))
LAPIC_RUNTIME_OBJS := $(LAPIC_RUNTIME_C_OBJS) $(LAPIC_RUNTIME_ASM_OBJS)
LAPIC_RUNTIME_DEPS := $(patsubst kernel/src/%.c,$(LAPIC_RUNTIME_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(LAPIC_RUNTIME_OBJ_DIR)/%.d,$(ASM_SOURCES))
LAPIC_RUNTIME_KERNEL := build/lapic-runtime/hfoc-kernel.elf
LAPIC_RUNTIME_ISO := build/lapic-runtime/hfoc-os-$(VERSION)-lapic-runtime.iso
LAPIC_RUNTIME_ISO_ROOT := build/lapic-runtime/iso_root
LAPIC_RUNTIME_CFLAGS := $(CFLAGS)

IRQ3_7_OBJ_DIR := build/irq3-7/obj
IRQ3_7_C_OBJS := $(patsubst kernel/src/%.c,$(IRQ3_7_OBJ_DIR)/%.o,$(C_SOURCES))
IRQ3_7_ASM_OBJS := $(patsubst kernel/src/%.S,$(IRQ3_7_OBJ_DIR)/%.o,$(ASM_SOURCES))
IRQ3_7_OBJS := $(IRQ3_7_C_OBJS) $(IRQ3_7_ASM_OBJS)
IRQ3_7_DEPS := $(patsubst kernel/src/%.c,$(IRQ3_7_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(IRQ3_7_OBJ_DIR)/%.d,$(ASM_SOURCES))
IRQ3_7_KERNEL := build/irq3-7/hfoc-kernel.elf
IRQ3_7_ISO := build/irq3-7/hfoc-os-$(VERSION)-irq3-7.iso
IRQ3_7_ISO_ROOT := build/irq3-7/iso_root
IRQ3_7_CFLAGS := $(CFLAGS) -DHFOC_ISA_IRQ3_7_QEMU_TEST

IRQ8_15_OBJ_DIR := build/irq8-15/obj
IRQ8_15_C_OBJS := $(patsubst kernel/src/%.c,$(IRQ8_15_OBJ_DIR)/%.o,$(C_SOURCES))
IRQ8_15_ASM_OBJS := $(patsubst kernel/src/%.S,$(IRQ8_15_OBJ_DIR)/%.o,$(ASM_SOURCES))
IRQ8_15_OBJS := $(IRQ8_15_C_OBJS) $(IRQ8_15_ASM_OBJS)
IRQ8_15_DEPS := $(patsubst kernel/src/%.c,$(IRQ8_15_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(IRQ8_15_OBJ_DIR)/%.d,$(ASM_SOURCES))
IRQ8_15_KERNEL := build/irq8-15/hfoc-kernel.elf
IRQ8_15_ISO := build/irq8-15/hfoc-os-$(VERSION)-irq8-15.iso
IRQ8_15_ISO_ROOT := build/irq8-15/iso_root
IRQ8_15_CFLAGS := $(CFLAGS) -DHFOC_ISA_IRQ8_15_QEMU_TEST

X2APIC_OBJ_DIR := build/x2apic/obj
X2APIC_C_OBJS := $(patsubst kernel/src/%.c,$(X2APIC_OBJ_DIR)/%.o,$(C_SOURCES))
X2APIC_ASM_OBJS := $(patsubst kernel/src/%.S,$(X2APIC_OBJ_DIR)/%.o,$(ASM_SOURCES))
X2APIC_OBJS := $(X2APIC_C_OBJS) $(X2APIC_ASM_OBJS)
X2APIC_DEPS := $(patsubst kernel/src/%.c,$(X2APIC_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(X2APIC_OBJ_DIR)/%.d,$(ASM_SOURCES))
X2APIC_KERNEL := build/x2apic/hfoc-kernel.elf
X2APIC_ISO := build/x2apic/hfoc-os-$(VERSION)-x2apic.iso
X2APIC_ISO_ROOT := build/x2apic/iso_root
X2APIC_CFLAGS := $(CFLAGS) -DHFOC_X2APIC_TEST

VMM_AS_OBJ_DIR := build/vmm-as/obj
VMM_AS_C_OBJS := $(patsubst kernel/src/%.c,$(VMM_AS_OBJ_DIR)/%.o,$(C_SOURCES))
VMM_AS_ASM_OBJS := $(patsubst kernel/src/%.S,$(VMM_AS_OBJ_DIR)/%.o,$(ASM_SOURCES))
VMM_AS_OBJS := $(VMM_AS_C_OBJS) $(VMM_AS_ASM_OBJS)
VMM_AS_DEPS := $(patsubst kernel/src/%.c,$(VMM_AS_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(VMM_AS_OBJ_DIR)/%.d,$(ASM_SOURCES))
VMM_AS_KERNEL := build/vmm-as/hfoc-kernel.elf
VMM_AS_ISO := build/vmm-as/hfoc-os-$(VERSION)-vmm-as.iso
VMM_AS_ISO_ROOT := build/vmm-as/iso_root
VMM_AS_CFLAGS := $(CFLAGS) -DHFOC_VMM_AS_QEMU_TEST

SCHED_YIELD_OBJ_DIR := build/sched-yield/obj
SCHED_YIELD_C_OBJS := $(patsubst kernel/src/%.c,$(SCHED_YIELD_OBJ_DIR)/%.o,$(C_SOURCES))
SCHED_YIELD_ASM_OBJS := $(patsubst kernel/src/%.S,$(SCHED_YIELD_OBJ_DIR)/%.o,$(ASM_SOURCES))
SCHED_YIELD_OBJS := $(SCHED_YIELD_C_OBJS) $(SCHED_YIELD_ASM_OBJS)
SCHED_YIELD_DEPS := $(patsubst kernel/src/%.c,$(SCHED_YIELD_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(SCHED_YIELD_OBJ_DIR)/%.d,$(ASM_SOURCES))
SCHED_YIELD_KERNEL := build/sched-yield/hfoc-kernel.elf
SCHED_YIELD_ISO := build/sched-yield/hfoc-os-$(VERSION)-sched-yield.iso
SCHED_YIELD_ISO_ROOT := build/sched-yield/iso_root
SCHED_YIELD_CFLAGS := $(CFLAGS) -DHFOC_SCHEDULER_YIELD_QEMU_TEST

SCHED_TICK_OBJ_DIR := build/sched-tick/obj
SCHED_TICK_C_OBJS := $(patsubst kernel/src/%.c,$(SCHED_TICK_OBJ_DIR)/%.o,$(C_SOURCES))
SCHED_TICK_ASM_OBJS := $(patsubst kernel/src/%.S,$(SCHED_TICK_OBJ_DIR)/%.o,$(ASM_SOURCES))
SCHED_TICK_OBJS := $(SCHED_TICK_C_OBJS) $(SCHED_TICK_ASM_OBJS)
SCHED_TICK_DEPS := $(patsubst kernel/src/%.c,$(SCHED_TICK_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(SCHED_TICK_OBJ_DIR)/%.d,$(ASM_SOURCES))
SCHED_TICK_KERNEL := build/sched-tick/hfoc-kernel.elf
SCHED_TICK_ISO := build/sched-tick/hfoc-os-$(VERSION)-sched-tick.iso
SCHED_TICK_ISO_ROOT := build/sched-tick/iso_root
SCHED_TICK_CFLAGS := $(CFLAGS) -DHFOC_SCHEDULER_TICK_QEMU_TEST

SCHED_WAIT_OBJ_DIR := build/sched-wait/obj
SCHED_WAIT_C_OBJS := $(patsubst kernel/src/%.c,$(SCHED_WAIT_OBJ_DIR)/%.o,$(C_SOURCES))
SCHED_WAIT_ASM_OBJS := $(patsubst kernel/src/%.S,$(SCHED_WAIT_OBJ_DIR)/%.o,$(ASM_SOURCES))
SCHED_WAIT_OBJS := $(SCHED_WAIT_C_OBJS) $(SCHED_WAIT_ASM_OBJS)
SCHED_WAIT_DEPS := $(patsubst kernel/src/%.c,$(SCHED_WAIT_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(SCHED_WAIT_OBJ_DIR)/%.d,$(ASM_SOURCES))
SCHED_WAIT_KERNEL := build/sched-wait/hfoc-kernel.elf
SCHED_WAIT_ISO := build/sched-wait/hfoc-os-$(VERSION)-sched-wait.iso
SCHED_WAIT_ISO_ROOT := build/sched-wait/iso_root
SCHED_WAIT_CFLAGS := $(CFLAGS) -DHFOC_SCHEDULER_WAIT_QEMU_TEST

SCHED_EXIT_OBJ_DIR := build/sched-exit/obj
SCHED_EXIT_C_OBJS := $(patsubst kernel/src/%.c,$(SCHED_EXIT_OBJ_DIR)/%.o,$(C_SOURCES))
SCHED_EXIT_ASM_OBJS := $(patsubst kernel/src/%.S,$(SCHED_EXIT_OBJ_DIR)/%.o,$(ASM_SOURCES))
SCHED_EXIT_OBJS := $(SCHED_EXIT_C_OBJS) $(SCHED_EXIT_ASM_OBJS)
SCHED_EXIT_DEPS := $(patsubst kernel/src/%.c,$(SCHED_EXIT_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(SCHED_EXIT_OBJ_DIR)/%.d,$(ASM_SOURCES))
SCHED_EXIT_KERNEL := build/sched-exit/hfoc-kernel.elf
SCHED_EXIT_ISO := build/sched-exit/hfoc-os-$(VERSION)-sched-exit.iso
SCHED_EXIT_ISO_ROOT := build/sched-exit/iso_root
SCHED_EXIT_CFLAGS := $(CFLAGS) -DHFOC_SCHEDULER_EXIT_QEMU_TEST

SCHED_STRESS_OBJ_DIR := build/sched-stress/obj
SCHED_STRESS_C_OBJS := $(patsubst kernel/src/%.c,$(SCHED_STRESS_OBJ_DIR)/%.o,$(C_SOURCES))
SCHED_STRESS_ASM_OBJS := $(patsubst kernel/src/%.S,$(SCHED_STRESS_OBJ_DIR)/%.o,$(ASM_SOURCES))
SCHED_STRESS_OBJS := $(SCHED_STRESS_C_OBJS) $(SCHED_STRESS_ASM_OBJS)
SCHED_STRESS_DEPS := $(patsubst kernel/src/%.c,$(SCHED_STRESS_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(SCHED_STRESS_OBJ_DIR)/%.d,$(ASM_SOURCES))
SCHED_STRESS_KERNEL := build/sched-stress/hfoc-kernel.elf
SCHED_STRESS_ISO := build/sched-stress/hfoc-os-$(VERSION)-sched-stress.iso
SCHED_STRESS_ISO_ROOT := build/sched-stress/iso_root
SCHED_STRESS_CFLAGS := $(CFLAGS) -DHFOC_SCHEDULER_STRESS_QEMU_TEST

PROCESS_SCHED_OBJ_DIR := build/process-sched/obj
PROCESS_SCHED_C_OBJS := $(patsubst kernel/src/%.c,$(PROCESS_SCHED_OBJ_DIR)/%.o,$(C_SOURCES))
PROCESS_SCHED_ASM_OBJS := $(patsubst kernel/src/%.S,$(PROCESS_SCHED_OBJ_DIR)/%.o,$(ASM_SOURCES))
PROCESS_SCHED_OBJS := $(PROCESS_SCHED_C_OBJS) $(PROCESS_SCHED_ASM_OBJS)
PROCESS_SCHED_DEPS := $(patsubst kernel/src/%.c,$(PROCESS_SCHED_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(PROCESS_SCHED_OBJ_DIR)/%.d,$(ASM_SOURCES))
PROCESS_SCHED_KERNEL := build/process-sched/hfoc-kernel.elf
PROCESS_SCHED_ISO := build/process-sched/hfoc-os-$(VERSION)-process-sched.iso
PROCESS_SCHED_ISO_ROOT := build/process-sched/iso_root
PROCESS_SCHED_CFLAGS := $(CFLAGS) -DHFOC_PROCESS_SCHEDULER_QEMU_TEST

USER_IRETQ_OBJ_DIR := build/user-iretq/obj
USER_IRETQ_C_OBJS := $(patsubst kernel/src/%.c,$(USER_IRETQ_OBJ_DIR)/%.o,$(C_SOURCES))
USER_IRETQ_ASM_OBJS := $(patsubst kernel/src/%.S,$(USER_IRETQ_OBJ_DIR)/%.o,$(ASM_SOURCES))
USER_IRETQ_OBJS := $(USER_IRETQ_C_OBJS) $(USER_IRETQ_ASM_OBJS)
USER_IRETQ_DEPS := $(patsubst kernel/src/%.c,$(USER_IRETQ_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(USER_IRETQ_OBJ_DIR)/%.d,$(ASM_SOURCES))
USER_IRETQ_KERNEL := build/user-iretq/hfoc-kernel.elf
USER_IRETQ_ISO := build/user-iretq/hfoc-os-$(VERSION)-user-iretq.iso
USER_IRETQ_ISO_ROOT := build/user-iretq/iso_root
USER_IRETQ_CFLAGS := $(CFLAGS) -DHFOC_USER_IRETQ_QEMU_TEST

SYSCALL_OBJ_DIR := build/syscall/obj
SYSCALL_C_OBJS := $(patsubst kernel/src/%.c,$(SYSCALL_OBJ_DIR)/%.o,$(C_SOURCES))
SYSCALL_ASM_OBJS := $(patsubst kernel/src/%.S,$(SYSCALL_OBJ_DIR)/%.o,$(ASM_SOURCES))
SYSCALL_OBJS := $(SYSCALL_C_OBJS) $(SYSCALL_ASM_OBJS)
SYSCALL_DEPS := $(patsubst kernel/src/%.c,$(SYSCALL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(SYSCALL_OBJ_DIR)/%.d,$(ASM_SOURCES))
SYSCALL_KERNEL := build/syscall/hfoc-kernel.elf
SYSCALL_ISO := build/syscall/hfoc-os-$(VERSION)-syscall.iso
SYSCALL_ISO_ROOT := build/syscall/iso_root
SYSCALL_CFLAGS := $(CFLAGS) -DHFOC_SYSCALL_QEMU_TEST

USER_INIT_OBJ_DIR := build/user-init/obj
USER_INIT_C_OBJS := $(patsubst kernel/src/%.c,$(USER_INIT_OBJ_DIR)/%.o,$(C_SOURCES))
USER_INIT_ASM_OBJS := $(patsubst kernel/src/%.S,$(USER_INIT_OBJ_DIR)/%.o,$(ASM_SOURCES))
USER_INIT_OBJS := $(USER_INIT_C_OBJS) $(USER_INIT_ASM_OBJS)
USER_INIT_DEPS := $(patsubst kernel/src/%.c,$(USER_INIT_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(USER_INIT_OBJ_DIR)/%.d,$(ASM_SOURCES))
USER_INIT_KERNEL := build/user-init/hfoc-kernel.elf
USER_INIT_ISO := build/user-init/hfoc-os-$(VERSION)-user-init.iso
USER_INIT_ISO_ROOT := build/user-init/iso_root
USER_INIT_CFLAGS := $(CFLAGS) -DHFOC_USER_INIT_QEMU_TEST
USER_EXEC_HELLO := build/user-init/bin/hello.elf
USER_EXEC_HELLO_OBJ := build/user-init/bin/hello.o
USER_EXEC_HELLO_BIN := build/user-init/bin/hello.bin
USER_EXEC_CRASH := build/user-init/bin/crash.elf
USER_EXEC_CRASH_OBJ := build/user-init/bin/crash.o
USER_EXEC_CRASH_BIN := build/user-init/bin/crash.bin
USER_EXEC_EXEC := build/user-init/bin/exec-ok.elf
USER_EXEC_EXEC_OBJ := build/user-init/bin/exec-ok.o
USER_EXEC_EXEC_BIN := build/user-init/bin/exec-ok.bin
USER_EXEC_MISSING := build/user-init/bin/exec-missing.elf
USER_EXEC_MISSING_OBJ := build/user-init/bin/exec-missing.o
USER_EXEC_MISSING_BIN := build/user-init/bin/exec-missing.bin
USER_EXEC_SPIN := build/user-init/bin/spin.elf
USER_EXEC_SPIN_OBJ := build/user-init/bin/spin.o
USER_EXEC_SPIN_BIN := build/user-init/bin/spin.bin
USER_EXEC_ROOTFS := build/user-init/user-rootfs

# ITEM-626: the native user runtime and the external programs built from it.
# These are real static ELF64 images linked at 0x400000; the ordinary rootfs
# carries them under /bin.  Nothing here is a kernel command table.
USER_PROGRAM_CFLAGS := -std=gnu11 -Os -Wall -Wextra -Wpedantic \
    -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Iuser/libhfoc
USER_PROGRAM_NAMES := echo pwd ls cat mkdir rm cp mv wc grep true false hfoc sh sleep
USER_PROGRAM_ELFS := $(addprefix build/user/bin/,$(addsuffix .elf,$(USER_PROGRAM_NAMES)))
USER_LIB_SOURCES := $(shell find user/libhfoc -type f -name '*.c' -print | sort)
USER_LIB_OBJECTS := $(patsubst user/libhfoc/%.c,build/user/lib/%.o,$(USER_LIB_SOURCES))
USER_CRT_OBJECT := build/user/lib/crt0.o
USER_TEST_PIPETEST := build/user/bin/pipetest.elf
USER_ROOTFS_STAGE := build/rootfs-stage
ROOTFS_INPUTS := $(shell find rootfs -type f -print | sort)

BOOT_POLICY_OBJ_DIR := build/boot-policy/obj
BOOT_POLICY_C_OBJS := $(patsubst kernel/src/%.c,$(BOOT_POLICY_OBJ_DIR)/%.o,$(C_SOURCES))
BOOT_POLICY_ASM_OBJS := $(patsubst kernel/src/%.S,$(BOOT_POLICY_OBJ_DIR)/%.o,$(ASM_SOURCES))
BOOT_POLICY_OBJS := $(BOOT_POLICY_C_OBJS) $(BOOT_POLICY_ASM_OBJS)
BOOT_POLICY_DEPS := $(patsubst kernel/src/%.c,$(BOOT_POLICY_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(BOOT_POLICY_OBJ_DIR)/%.d,$(ASM_SOURCES))
BOOT_POLICY_KERNEL := build/boot-policy/hfoc-kernel.elf
BOOT_POLICY_ISO := build/boot-policy/hfoc-os-$(VERSION)-boot-policy.iso
BOOT_POLICY_ISO_ROOT := build/boot-policy/iso_root
BOOT_POLICY_CFLAGS := $(CFLAGS) -DHFOC_BOOT_POLICY_QEMU_TEST

BLOCK_DEVICE_OBJ_DIR := build/block-device/obj
BLOCK_DEVICE_C_OBJS := $(patsubst kernel/src/%.c,$(BLOCK_DEVICE_OBJ_DIR)/%.o,$(C_SOURCES))
BLOCK_DEVICE_ASM_OBJS := $(patsubst kernel/src/%.S,$(BLOCK_DEVICE_OBJ_DIR)/%.o,$(ASM_SOURCES))
BLOCK_DEVICE_OBJS := $(BLOCK_DEVICE_C_OBJS) $(BLOCK_DEVICE_ASM_OBJS)
BLOCK_DEVICE_DEPS := $(patsubst kernel/src/%.c,$(BLOCK_DEVICE_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(BLOCK_DEVICE_OBJ_DIR)/%.d,$(ASM_SOURCES))
BLOCK_DEVICE_KERNEL := build/block-device/hfoc-kernel.elf
BLOCK_DEVICE_ISO := build/block-device/hfoc-os-$(VERSION)-block-device.iso
BLOCK_DEVICE_ISO_ROOT := build/block-device/iso_root
BLOCK_DEVICE_CFLAGS := $(CFLAGS) -DHFOC_BLOCK_DEVICE_QEMU_TEST

DATA_REGION_OBJ_DIR := build/data-region/obj
DATA_REGION_C_OBJS := $(patsubst kernel/src/%.c,$(DATA_REGION_OBJ_DIR)/%.o,$(C_SOURCES))
DATA_REGION_ASM_OBJS := $(patsubst kernel/src/%.S,$(DATA_REGION_OBJ_DIR)/%.o,$(ASM_SOURCES))
DATA_REGION_OBJS := $(DATA_REGION_C_OBJS) $(DATA_REGION_ASM_OBJS)
DATA_REGION_DEPS := $(patsubst kernel/src/%.c,$(DATA_REGION_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(DATA_REGION_OBJ_DIR)/%.d,$(ASM_SOURCES))
DATA_REGION_KERNEL := build/data-region/hfoc-kernel.elf
DATA_REGION_ISO := build/data-region/hfoc-os-$(VERSION)-data-region.iso
DATA_REGION_ISO_ROOT := build/data-region/iso_root
DATA_REGION_CFLAGS := $(CFLAGS) -DHFOC_DATA_REGION_QEMU_TEST

BLOCK_CACHE_OBJ_DIR := build/block-cache/obj
BLOCK_CACHE_C_OBJS := $(patsubst kernel/src/%.c,$(BLOCK_CACHE_OBJ_DIR)/%.o,$(C_SOURCES))
BLOCK_CACHE_ASM_OBJS := $(patsubst kernel/src/%.S,$(BLOCK_CACHE_OBJ_DIR)/%.o,$(ASM_SOURCES))
BLOCK_CACHE_OBJS := $(BLOCK_CACHE_C_OBJS) $(BLOCK_CACHE_ASM_OBJS)
BLOCK_CACHE_DEPS := $(patsubst kernel/src/%.c,$(BLOCK_CACHE_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(BLOCK_CACHE_OBJ_DIR)/%.d,$(ASM_SOURCES))
BLOCK_CACHE_KERNEL := build/block-cache/hfoc-kernel.elf
BLOCK_CACHE_ISO := build/block-cache/hfoc-os-$(VERSION)-block-cache.iso
BLOCK_CACHE_ISO_ROOT := build/block-cache/iso_root
BLOCK_CACHE_CFLAGS := $(CFLAGS) -DHFOC_BLOCK_CACHE_QEMU_TEST

HFOCFS_QEMU_OBJ_DIR := build/hfocfs-qemu/obj
HFOCFS_QEMU_C_OBJS := $(patsubst kernel/src/%.c,$(HFOCFS_QEMU_OBJ_DIR)/%.o,$(C_SOURCES))
HFOCFS_QEMU_ASM_OBJS := $(patsubst kernel/src/%.S,$(HFOCFS_QEMU_OBJ_DIR)/%.o,$(ASM_SOURCES))
HFOCFS_QEMU_OBJS := $(HFOCFS_QEMU_C_OBJS) $(HFOCFS_QEMU_ASM_OBJS)
HFOCFS_QEMU_DEPS := $(patsubst kernel/src/%.c,$(HFOCFS_QEMU_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(HFOCFS_QEMU_OBJ_DIR)/%.d,$(ASM_SOURCES))
HFOCFS_QEMU_KERNEL := build/hfocfs-qemu/hfoc-kernel.elf
HFOCFS_QEMU_ISO := build/hfocfs-qemu/hfoc-os-$(VERSION)-hfocfs.iso
HFOCFS_QEMU_ISO_ROOT := build/hfocfs-qemu/iso_root
HFOCFS_QEMU_CFLAGS := $(CFLAGS) -DHFOC_HFOCFS_QEMU_TEST

HFOCFS_WORKSPACE_QEMU_OBJ_DIR := build/hfocfs-workspace-qemu/obj
HFOCFS_WORKSPACE_QEMU_C_OBJS := $(patsubst kernel/src/%.c,$(HFOCFS_WORKSPACE_QEMU_OBJ_DIR)/%.o,$(C_SOURCES))
HFOCFS_WORKSPACE_QEMU_ASM_OBJS := $(patsubst kernel/src/%.S,$(HFOCFS_WORKSPACE_QEMU_OBJ_DIR)/%.o,$(ASM_SOURCES))
HFOCFS_WORKSPACE_QEMU_OBJS := $(HFOCFS_WORKSPACE_QEMU_C_OBJS) $(HFOCFS_WORKSPACE_QEMU_ASM_OBJS)
HFOCFS_WORKSPACE_QEMU_DEPS := $(patsubst kernel/src/%.c,$(HFOCFS_WORKSPACE_QEMU_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(HFOCFS_WORKSPACE_QEMU_OBJ_DIR)/%.d,$(ASM_SOURCES))
HFOCFS_WORKSPACE_QEMU_KERNEL := build/hfocfs-workspace-qemu/hfoc-kernel.elf
HFOCFS_WORKSPACE_QEMU_ISO := build/hfocfs-workspace-qemu/hfoc-os-$(VERSION)-hfocfs-workspace.iso
HFOCFS_WORKSPACE_QEMU_ISO_ROOT := build/hfocfs-workspace-qemu/iso_root
HFOCFS_WORKSPACE_QEMU_CFLAGS := $(CFLAGS) -DHFOC_HFOCFS_WORKSPACE_QEMU_TEST

CONFIG_JOURNAL_OBJ_DIR := build/config-journal/obj
CONFIG_JOURNAL_C_OBJS := $(patsubst kernel/src/%.c,$(CONFIG_JOURNAL_OBJ_DIR)/%.o,$(C_SOURCES))
CONFIG_JOURNAL_ASM_OBJS := $(patsubst kernel/src/%.S,$(CONFIG_JOURNAL_OBJ_DIR)/%.o,$(ASM_SOURCES))
CONFIG_JOURNAL_OBJS := $(CONFIG_JOURNAL_C_OBJS) $(CONFIG_JOURNAL_ASM_OBJS)
CONFIG_JOURNAL_DEPS := $(patsubst kernel/src/%.c,$(CONFIG_JOURNAL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(CONFIG_JOURNAL_OBJ_DIR)/%.d,$(ASM_SOURCES))
CONFIG_JOURNAL_KERNEL := build/config-journal/hfoc-kernel.elf
CONFIG_JOURNAL_ISO := build/config-journal/hfoc-os-$(VERSION)-config-journal.iso
CONFIG_JOURNAL_ISO_ROOT := build/config-journal/iso_root
CONFIG_JOURNAL_CFLAGS := $(CFLAGS) -DHFOC_CONFIG_JOURNAL_QEMU_TEST

PHYSICAL_STORAGE_OBJ_DIR := build/physical-storage/obj
PHYSICAL_STORAGE_C_OBJS := $(patsubst kernel/src/%.c,$(PHYSICAL_STORAGE_OBJ_DIR)/%.o,$(C_SOURCES))
PHYSICAL_STORAGE_ASM_OBJS := $(patsubst kernel/src/%.S,$(PHYSICAL_STORAGE_OBJ_DIR)/%.o,$(ASM_SOURCES))
PHYSICAL_STORAGE_OBJS := $(PHYSICAL_STORAGE_C_OBJS) $(PHYSICAL_STORAGE_ASM_OBJS)
PHYSICAL_STORAGE_DEPS := $(patsubst kernel/src/%.c,$(PHYSICAL_STORAGE_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(PHYSICAL_STORAGE_OBJ_DIR)/%.d,$(ASM_SOURCES))
PHYSICAL_STORAGE_KERNEL := build/physical-storage/hfoc-kernel.elf
PHYSICAL_STORAGE_ISO := build/physical-storage/hfoc-os-$(VERSION)-physical-storage.iso
PHYSICAL_STORAGE_ISO_ROOT := build/physical-storage/iso_root
PHYSICAL_STORAGE_CFLAGS := $(CFLAGS) -DHFOC_PHYSICAL_STORAGE_QEMU_TEST

PHYSICAL_SATA_ACCEPTANCE_DIR := build/physical-sata-acceptance
PHYSICAL_SATA_ACCEPTANCE_AHCI_OBJ := \
	$(PHYSICAL_SATA_ACCEPTANCE_DIR)/obj/ahci_qemu.o
PHYSICAL_SATA_ACCEPTANCE_OBJS := \
	$(filter-out $(PHYSICAL_STORAGE_OBJ_DIR)/ahci_qemu.o,$(PHYSICAL_STORAGE_OBJS)) \
	$(PHYSICAL_SATA_ACCEPTANCE_AHCI_OBJ)
PHYSICAL_SATA_ACCEPTANCE_KERNEL := \
	$(PHYSICAL_SATA_ACCEPTANCE_DIR)/hfoc-kernel.elf
PHYSICAL_SATA_ACCEPTANCE_ISO := \
	$(PHYSICAL_SATA_ACCEPTANCE_DIR)/hfoc-os-$(VERSION)-physical-sata-acceptance.iso
PHYSICAL_SATA_ACCEPTANCE_ISO_ROOT := \
	$(PHYSICAL_SATA_ACCEPTANCE_DIR)/iso_root
PHYSICAL_SATA_ACCEPTANCE_CFLAGS := \
	$(PHYSICAL_STORAGE_CFLAGS) -DHFOC_PHYSICAL_SATA_ACCEPTANCE_TEST

BOOT_CHAIN_OBJ_DIR := build/boot-chain/obj
BOOT_CHAIN_C_OBJS := $(patsubst kernel/src/%.c,$(BOOT_CHAIN_OBJ_DIR)/%.o,$(C_SOURCES))
BOOT_CHAIN_ASM_OBJS := $(patsubst kernel/src/%.S,$(BOOT_CHAIN_OBJ_DIR)/%.o,$(ASM_SOURCES))
BOOT_CHAIN_MBEDTLS_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(BOOT_CHAIN_OBJ_DIR)/mbedtls/%.o,$(HFOC_VERIFY_MBEDTLS_SOURCES))
BOOT_CHAIN_VECTOR_OBJ := $(BOOT_CHAIN_OBJ_DIR)/boot-chain-vector.o
BOOT_CHAIN_OBJS := $(BOOT_CHAIN_C_OBJS) $(BOOT_CHAIN_ASM_OBJS) $(BOOT_CHAIN_MBEDTLS_OBJS) $(BOOT_CHAIN_VECTOR_OBJ)
BOOT_CHAIN_KERNEL := build/boot-chain/hfoc-kernel.elf
BOOT_CHAIN_ISO := build/boot-chain/hfoc-os-$(VERSION)-boot-chain.iso
BOOT_CHAIN_ISO_ROOT := build/boot-chain/iso_root
BOOT_CHAIN_CFLAGS := $(CFLAGS) -DHFOC_BOOT_CHAIN_QEMU_TEST \
	$(HFOC_VERIFY_MBEDTLS_CFLAGS) -Itests

PRODUCTION_INSTALLER_OBJ_DIR := build/production-installer/obj
PRODUCTION_INSTALLER_C_OBJS := $(patsubst kernel/src/%.c,$(PRODUCTION_INSTALLER_OBJ_DIR)/%.o,$(C_SOURCES))
PRODUCTION_INSTALLER_ASM_OBJS := $(patsubst kernel/src/%.S,$(PRODUCTION_INSTALLER_OBJ_DIR)/%.o,$(ASM_SOURCES))
# ITEM-517: `deps/limine/limine-bios.sys` is the loader Limine's BIOS stage2
# reads from the installed boot volume.  It is linked into the installer kernel
# as a binary blob and is added to PRODUCTION_INSTALLER_OBJS only - the product
# kernel links neither this object nor the 22,516-byte stage blob header, so
# `make kernel` keeps its bytes.  `ld -r -b binary` derives the symbol names
# from the path it is given: _binary_deps_limine_limine_bios_sys_{start,end}.
PRODUCTION_INSTALLER_LIMINE_BIOS_SYS_OBJ := \
	build/production-installer/limine_bios_sys.o
PRODUCTION_INSTALLER_OBJS := $(PRODUCTION_INSTALLER_C_OBJS) \
	$(PRODUCTION_INSTALLER_ASM_OBJS) \
	$(PRODUCTION_INSTALLER_LIMINE_BIOS_SYS_OBJ) \
	$(HFOC_STATE_CRYPTO_KERNEL_OBJS)
PRODUCTION_INSTALLER_DEPS := $(patsubst kernel/src/%.c,$(PRODUCTION_INSTALLER_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(PRODUCTION_INSTALLER_OBJ_DIR)/%.d,$(ASM_SOURCES))
PRODUCTION_INSTALLER_KERNEL := build/production-installer/hfoc-kernel.elf
PRODUCTION_INSTALLER_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer.iso
PRODUCTION_INSTALLER_ISO_ROOT := build/production-installer/iso_root
# ITEM-394: the product install flow exists only on the installer medium, and
# it runs only when the medium carries an install declaration.  The appliance
# image compiles neither the flow nor its records.
# ITEM-517: the installer kernel also carries the pinned Limine BIOS stage
# blobs; `deps/limine` is the include path of `limine-bios-hdd.h` (the
# 22,516-byte stage image) and the product build does not get it.
PRODUCTION_INSTALLER_CFLAGS := $(CFLAGS) -DHFOC_PRODUCTION_INSTALLER_QEMU_TEST \
	-DHFOC_PRODUCTION_INSTALL_MEDIA -Ideps/limine

# ITEM-552: the console medium the diagnostic-export proof boots.  The only
# difference from the shipped console medium is the fault-injection switch
# (-DHFOC_XHCI_TIMEOUT_QEMU_TEST): the proof must exercise the failure snapshot
# with the medium on the independent EHCI path, and no shipped kernel carries
# the switch.  The block lives here, after the production
# installer's own variables, because every value here is expanded with := and
# the pinned limine-bios-sys object is named below.
DIAG_EXPORT_OBJ_DIR := build/diag-export/obj
DIAG_EXPORT_C_OBJS := $(patsubst kernel/src/%.c,$(DIAG_EXPORT_OBJ_DIR)/%.o,$(C_SOURCES))
DIAG_EXPORT_ASM_OBJS := $(patsubst kernel/src/%.S,$(DIAG_EXPORT_OBJ_DIR)/%.o,$(ASM_SOURCES))
DIAG_EXPORT_OBJS := $(DIAG_EXPORT_C_OBJS) $(DIAG_EXPORT_ASM_OBJS) \
	$(PRODUCTION_INSTALLER_LIMINE_BIOS_SYS_OBJ)
# ITEM-559: this tree's header dependencies were not tracked, so the generated
# build identity (and every header) could change under a stale object: the
# export proof kept booting a kernel of another revision and only the identity
# assertion caught it.  The dependency files are included below like the other
# profiles'.
DIAG_EXPORT_DEPS := $(patsubst kernel/src/%.c,$(DIAG_EXPORT_OBJ_DIR)/%.d,$(C_SOURCES)) \
	$(patsubst kernel/src/%.S,$(DIAG_EXPORT_OBJ_DIR)/%.d,$(ASM_SOURCES))
DIAG_EXPORT_KERNEL := build/diag-export/hfoc-kernel.elf
DIAG_EXPORT_ISO := build/diag-export/hfoc-os-$(VERSION)-diag-export.iso
DIAG_EXPORT_ISO_ROOT := build/diag-export/iso_root
DIAG_EXPORT_CFLAGS := $(PRODUCTION_INSTALLER_CFLAGS) -DHFOC_XHCI_TIMEOUT_QEMU_TEST

# The authorised medium: the confirmation token, the one disk serial it may
# write, and the management scope the installed appliance publishes.  The
# defaults are the QEMU proof's values; a hardware run overrides all three
# (docs/hfoc-production-install-flow.md, docs/hfoc-hardware-acceptance.md).
HFOC_INSTALL_MEDIA_CONFIRM ?= ITEM-394-UNATTENDED-INSTALL
HFOC_INSTALL_MEDIA_TARGET ?= QM00001
HFOC_INSTALL_MEDIA_MANAGEMENT ?= local-only
# ITEM-398: the operator's layout declaration for the install medium.  Empty
# leaves it undeclared, and the run then preserves a target disk that carries an
# existing system instead of wiping it; 'whole-disk' is the explicit choice for
# a disk whose content is expendable (the acceptance run on /dev/sda).
HFOC_INSTALL_MEDIA_LAYOUT ?=
# ITEM-446: the network the installed appliance comes up on.  Empty leaves it
# undeclared and the appliance discovers an address, which is the pre-item
# behaviour; 'dhcp' states that choice explicitly and 'static' also needs the
# address (a.b.c.d/prefix), the gateway and the DNS.  The declaration is
# validated by the build and again by the kernel before the first write.
HFOC_INSTALL_MEDIA_NETWORK ?=
HFOC_INSTALL_MEDIA_ADDRESS ?=
HFOC_INSTALL_MEDIA_GATEWAY ?=
HFOC_INSTALL_MEDIA_DNS ?=
# ITEM-446: the declaration the network proof installs from.  The address is the
# one the QEMU user-mode network serves, so the installed disk can prove the
# static lease without a DHCP exchange.
HFOC_INSTALL_MEDIA_NETWORK_PROOF_ADDRESS ?= 10.0.2.15/24
HFOC_INSTALL_MEDIA_NETWORK_PROOF_GATEWAY ?= 10.0.2.2
HFOC_INSTALL_MEDIA_NETWORK_PROOF_DNS ?= 10.0.2.3
# ITEM-449: the forward the forward proof installs from.  The target is the
# host side of the QEMU user-mode network, the client is what the guest sees a
# host-forwarded connection as, and the listen port is the one the appliance
# serves the relay on.  The declaration is validated by the build and again by
# the kernel before the first write.
HFOC_INSTALL_MEDIA_FORWARD ?=
HFOC_INSTALL_MEDIA_FORWARD_TARGET ?=
HFOC_INSTALL_MEDIA_FORWARD_TARGET_PORT ?=
HFOC_INSTALL_MEDIA_FORWARD_LISTEN_PORT ?=
HFOC_INSTALL_MEDIA_FORWARD_CLIENTS ?=
HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET ?= 10.0.2.2
HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT ?= 18443
HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT ?= 19443
HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS ?= 10.0.2.2
HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS ?= 10.0.2.2
# ITEM-449: the same declaration with a client list that does not include the
# source a host-forwarded connection arrives as.  The relay proof boots it and
# requires the refusal, so the access rule is proven, not assumed.
HFOC_INSTALL_MEDIA_FORWARD_DENIED_CLIENTS ?= 10.0.2.9
# ITEM-490: which entry a medium offers first.  `unattended` (the default) keeps
# every existing medium byte-identical and is what the automated proofs need;
# `console` puts the operator entry - the mandatory storage screen - first.
HFOC_INSTALL_MEDIA_ENTRY ?= unattended
# ITEM-509 increment 27 (509.7c-2): the language a medium declares.  Empty means
# no directive, which the kernel reads as the default `en`, so every existing
# medium keeps its exact token bytes; the language proof builds its medium with
# `ru` by setting this variable.
HFOC_INSTALL_MEDIA_LANGUAGE ?=
PRODUCTION_INSTALLER_FORWARD_ISO_ROOT := build/production-installer/iso_root-forward
PRODUCTION_INSTALLER_FORWARD_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-forward.iso
PRODUCTION_INSTALLER_FORWARD_SSH_ISO_ROOT := build/production-installer/iso_root-forward-ssh-test
PRODUCTION_INSTALLER_FORWARD_SSH_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-forward-ssh-test.iso
PRODUCTION_INSTALLER_FORWARD_DENIED_ISO_ROOT := build/production-installer/iso_root-forward-denied
PRODUCTION_INSTALLER_FORWARD_DENIED_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-forward-denied.iso
PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT := build/production-installer/iso_root-authorised
PRODUCTION_INSTALLER_AUTHORISED_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-authorised.iso
# ITEM-490: the same authorised declaration, staged so the *operator* entry is
# the medium's first (default) entry - the one that opens the storage screen and
# waits for a person to name the disk.  It is the medium a human boots, and what
# the disk-screen proof boots; every automated medium keeps the unattended entry
# first, because there is nobody at its keyboard.
PRODUCTION_INSTALLER_CONSOLE_ISO_ROOT := build/production-installer/iso_root-console
PRODUCTION_INSTALLER_CONSOLE_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-console.iso
# ITEM-598: the operator-provisioned medium.  It carries a 32-byte seed record
# generated by the trusted host CSPRNG, but no repository/CI secret: the caller
# must provide the record path explicitly.  The ISO remains operator-local.
HFOC_INSTALL_MEDIA_HOST_SEED ?=
# ITEM-606: the owner's trusted-Debian SSH client public key and the
# fingerprint confirmed there.  Both must be set together; the stage refuses a
# key module without the matching fingerprint.  The private key is never read.
HFOC_INSTALL_MEDIA_SSH_KEY ?=
HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM ?=
export HFOC_INSTALL_MEDIA_SSH_KEY HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM
# ITEM-636: the installed system's root initramfs carries the native userland.
# `make iso` packs the staged rootfs, which has /bin/sh and the utilities built
# from user/, while the install-media stage used the bare `rootfs` source and
# therefore sealed an archive with an empty /bin.  The production media name the
# same staged tree explicitly; host proofs that pass their own fixture keep the
# `rootfs` default and are unaffected.
HFOC_INSTALL_MEDIA_ROOTFS ?= $(USER_ROOTFS_STAGE)
export HFOC_INSTALL_MEDIA_ROOTFS
PRODUCTION_INSTALLER_HOST_SEEDED_ISO_ROOT := build/production-installer/iso_root-host-seeded
PRODUCTION_INSTALLER_HOST_SEEDED_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-host-seeded.iso
# ITEM-398: the same authorised declaration with the destructive layout spelled
# out, so the preserving proof can drive the run the flow used to hardcode.
PRODUCTION_INSTALLER_WHOLE_DISK_ISO_ROOT := build/production-installer/iso_root-whole-disk
PRODUCTION_INSTALLER_WHOLE_DISK_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-whole-disk.iso
# ITEM-399: the same authorised declaration with the management interface opted
# in, so a proof can boot the *installed* disk and read the scope the medium
# wrote into the installed configuration (`[PRODUCTION-MGMT-BIND]
# scope=MANAGEMENT_INTERFACE source=boot-declaration`).
PRODUCTION_INSTALLER_NETWORK_ISO_ROOT := build/production-installer/iso_root-network
PRODUCTION_INSTALLER_NETWORK_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-network.iso
PRODUCTION_INSTALLER_MANAGEMENT_ISO_ROOT := build/production-installer/iso_root-management
PRODUCTION_INSTALLER_MANAGEMENT_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-management.iso
# ITEM-509 increment 27 (509.7c-2): the same authorised declaration with the
# language declared, so a proof can install from a Russian medium and read the
# record back from the disk the run produced.
PRODUCTION_INSTALLER_LANGUAGE_ISO_ROOT := build/production-installer/iso_root-language
PRODUCTION_INSTALLER_LANGUAGE_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-language.iso
# ITEM-588: the state-preserving update medium.  Its own root and its own file,
# so the declaration it carries (`HFOC-INSTALL-LAYOUT=update`) never mixes with
# the install media's - a medium is a declaration, and a cached ISO must not
# stand for a declaration it was not built with.
PRODUCTION_INSTALLER_UPDATE_ISO_ROOT := build/production-installer/iso_root-update
PRODUCTION_INSTALLER_UPDATE_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-update.iso
# ITEM-606: the same update declaration carrying the committed test-only SSH
# public key.  It exists only for the QEMU proof that an owner-confirmed key
# module is written into the installed state; the shipped update medium never
# gains a test key.
PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO_ROOT := build/production-installer/iso_root-update-ssh-key
PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-update-ssh-key.iso
PRODUCTION_INSTALLER_SSH_KEY_FIXTURE := tests/fixtures/ssh/id_ecdsa.pub
PRODUCTION_INSTALLER_SSH_KEY_FIXTURE_CONFIRM := SHA256:MGSoetNGMLzNYvBWHBYJACHQk3NBN9s25zLPjPbEDMM
# ITEM-637: the owner's update medium shape - management scope plus the
# owner-confirmed public key.  The installed-persistence proof installs from
# the management medium, applies this update, and then proves that a file in
# /home/owner survives the update and two reboots.  A dedicated root keeps the
# declaration from mixing with the local-only SSH-key update proof.
PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO_ROOT := build/production-installer/iso_root-update-management-ssh-key
PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-update-management-ssh-key.iso
# ITEM-638: the full standard update declaration - confirmation, target,
# management scope, update layout, the confirmed SSH key, a static address and
# an enabled forward with four clients.  It is over the old 512-byte bound and
# is the medium the capacity proof applies to an installed disk.  The second
# root is the deliberate negative control: after staging, 600 comment bytes are
# appended so the declaration is over the new finite bound; the flow must name
# TOKEN_TOO_LARGE and write nothing.
PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO_ROOT := build/production-installer/iso_root-update-capacity
PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-update-capacity.iso
PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO_ROOT := build/production-installer/iso_root-update-oversized
PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO := build/production-installer/hfoc-os-$(VERSION)-production-installer-update-oversized.iso

HFOC_EFI_HELPER := build/efi/hfoc-installer.efi
HFOC_EFI_HELPER_ISO := build/efi/hfoc-efi-installer.iso
HFOC_EFI_HELPER_EFI_IMAGE := build/efi/limine-uefi-cd.bin
HFOC_EFI_HELPER_ISO_ROOT := build/efi/iso_root
HFOC_EFI_HELPER_OBJ_DIR := build/efi-helper/obj
HFOC_EFI_HELPER_SOURCES := \
	boot/efi/hfoc_efi_installer.c \
	boot/efi/hfoc_efi_installer_uefi.c \
	kernel/src/appliance_gpt_discovery.c \
	kernel/src/block_device.c \
	kernel/src/efi_boot_environment.c \
	kernel/src/efi_boot_variables.c \
	kernel/src/efi_fat32_esp.c \
	kernel/src/installer_hardware_discovery.c \
	kernel/src/kstring.c \
	kernel/src/sha256.c
HFOC_EFI_HELPER_OBJS := $(patsubst %.c,$(HFOC_EFI_HELPER_OBJ_DIR)/%.o,$(HFOC_EFI_HELPER_SOURCES))
HFOC_EFI_HELPER_DEPS := $(HFOC_EFI_HELPER_OBJS:.o=.d)
HFOC_EFI_HELPER_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
	-ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	-mcmodel=large \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fshort-wchar -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	-Iboot/efi -Ikernel/include

PERSISTENT_CONFIG_OBJ_DIR := build/persistent-config/obj
PERSISTENT_CONFIG_C_OBJS := $(patsubst kernel/src/%.c,$(PERSISTENT_CONFIG_OBJ_DIR)/%.o,$(C_SOURCES))
PERSISTENT_CONFIG_ASM_OBJS := $(patsubst kernel/src/%.S,$(PERSISTENT_CONFIG_OBJ_DIR)/%.o,$(ASM_SOURCES))
PERSISTENT_CONFIG_OBJS := $(PERSISTENT_CONFIG_C_OBJS) $(PERSISTENT_CONFIG_ASM_OBJS)
PERSISTENT_CONFIG_DEPS := $(patsubst kernel/src/%.c,$(PERSISTENT_CONFIG_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(PERSISTENT_CONFIG_OBJ_DIR)/%.d,$(ASM_SOURCES))
PERSISTENT_CONFIG_KERNEL := build/persistent-config/hfoc-kernel.elf
PERSISTENT_CONFIG_ISO := build/persistent-config/hfoc-os-$(VERSION)-persistent-config.iso
PERSISTENT_CONFIG_ISO_ROOT := build/persistent-config/iso_root
PERSISTENT_CONFIG_CFLAGS := $(CFLAGS) -DHFOC_APPLIANCE_PERSISTENT_CONFIG_QEMU_TEST

NETWORK_CONFIG_OBJ_DIR := build/network-config/obj
NETWORK_CONFIG_C_OBJS := $(patsubst kernel/src/%.c,$(NETWORK_CONFIG_OBJ_DIR)/%.o,$(C_SOURCES))
NETWORK_CONFIG_ASM_OBJS := $(patsubst kernel/src/%.S,$(NETWORK_CONFIG_OBJ_DIR)/%.o,$(ASM_SOURCES))
NETWORK_CONFIG_OBJS := $(NETWORK_CONFIG_C_OBJS) $(NETWORK_CONFIG_ASM_OBJS)
NETWORK_CONFIG_DEPS := $(patsubst kernel/src/%.c,$(NETWORK_CONFIG_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(NETWORK_CONFIG_OBJ_DIR)/%.d,$(ASM_SOURCES))
NETWORK_CONFIG_KERNEL := build/network-config/hfoc-kernel.elf
NETWORK_CONFIG_ISO := build/network-config/hfoc-os-$(VERSION)-network-config.iso
NETWORK_CONFIG_ISO_ROOT := build/network-config/iso_root
NETWORK_CONFIG_CFLAGS := $(CFLAGS) -DHFOC_APPLIANCE_NETWORK_CONFIG_QEMU_TEST

# ITEM-507 increment 6: the two-certificate selection fixture.
#
# The proof has to serve two declared names two different certificates on a
# live listener, and the operator import path cannot declare them in the QEMU
# fixture: it refuses a pair until the appliance has validated its wall clock
# (ITEM-502/503), and the fixture reaches no SNTP server, because the SNTP port
# is 123 and the test host runs unprivileged.  The proof therefore boots a
# kernel of its own: the production configuration plus
# `HFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST`, which writes two
# test-only pairs through the same durable writer a panel commit uses and sets
# a test-only account.  Only the kernel entry point and the fixture object
# carry the define; every other object is the production object, so the fixture
# kernel is the production kernel plus one seeding step.
SNI_SELECTION_OBJ_DIR := build/sni-selection/obj
SNI_SELECTION_CFLAGS := $(PRODUCTION_CFLAGS) \
	-DHFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST
SNI_SELECTION_OBJECTS := $(filter-out build/obj/kernel.o,$(OBJECTS)) \
	$(SNI_SELECTION_OBJ_DIR)/kernel.o \
	$(SNI_SELECTION_OBJ_DIR)/appliance_certificate_name_fixture_qemu.o
SNI_SELECTION_DEPS := $(SNI_SELECTION_OBJ_DIR)/kernel.d \
	$(SNI_SELECTION_OBJ_DIR)/appliance_certificate_name_fixture_qemu.d
SNI_SELECTION_KERNEL := build/sni-selection/hfoc-kernel.elf
SNI_SELECTION_ISO := build/hfoc-os-$(VERSION)-sni-fixture.iso
SNI_SELECTION_ISO_ROOT := build/sni-selection/iso_root

# ITEM-508 increment 15: the offline ACME directory exchange.
#
# The proof dials `https://10.0.2.2:18445/directory` through the increment-13
# binding and must anchor a chain the frozen profile can parse; the production
# anchor is RSA.  The kernel is therefore the production kernel plus a fixture
# anchor object and one driver, and the production anchor object is filtered
# out of the link so the two definitions of the anchor symbols cannot meet.
ACME_DIRECTORY_OBJ_DIR := build/acme-directory/obj
ACME_DIRECTORY_CFLAGS := $(PRODUCTION_CFLAGS) \
	-DHFOC_APPLIANCE_ACME_DIRECTORY_QEMU_TEST
ACME_DIRECTORY_OBJECTS := $(filter-out build/obj/kernel.o \
		build/obj/appliance_acme_trust_anchor.o \
		build/obj/appliance_acme_directory_qemu.o \
		build/obj/appliance_acme_directory_anchor_fixture.o,$(OBJECTS)) \
	$(ACME_DIRECTORY_OBJ_DIR)/kernel.o \
	$(ACME_DIRECTORY_OBJ_DIR)/appliance_acme_directory_qemu.o \
	$(ACME_DIRECTORY_OBJ_DIR)/appliance_acme_directory_anchor_fixture.o
ACME_DIRECTORY_DEPS := $(ACME_DIRECTORY_OBJ_DIR)/kernel.d \
	$(ACME_DIRECTORY_OBJ_DIR)/appliance_acme_directory_qemu.d \
	$(ACME_DIRECTORY_OBJ_DIR)/appliance_acme_directory_anchor_fixture.d
ACME_DIRECTORY_KERNEL := build/acme-directory/hfoc-kernel.elf
ACME_DIRECTORY_ISO := build/acme-directory/hfoc-os-$(VERSION)-acme-directory.iso
ACME_DIRECTORY_ISO_ROOT := build/acme-directory/iso_root

# ITEM-508 increment 17: the offline ACME order flow.
#
# The same shape as the directory proof above and the same fixture anchor; a
# second driver drives one complete order (account, order, authorization,
# challenge, finalize, certificate) against the grown stub, on its own fixed
# port so both proofs can run concurrently in the parallel QEMU phase.
ACME_ORDER_OBJ_DIR := build/acme-order/obj
ACME_ORDER_CFLAGS := $(PRODUCTION_CFLAGS) \
	-DHFOC_APPLIANCE_ACME_ORDER_QEMU_TEST
ACME_ORDER_OBJECTS := $(filter-out build/obj/kernel.o \
		build/obj/appliance_acme_trust_anchor.o \
		build/obj/appliance_acme_order_qemu.o \
		build/obj/appliance_acme_directory_anchor_fixture.o \
		build/obj/appliance_acme_fixture_clock_qemu.o,$(OBJECTS)) \
	$(ACME_ORDER_OBJ_DIR)/kernel.o \
	$(ACME_ORDER_OBJ_DIR)/appliance_acme_order_qemu.o \
	$(ACME_ORDER_OBJ_DIR)/appliance_acme_directory_anchor_fixture.o \
	$(ACME_ORDER_OBJ_DIR)/appliance_acme_fixture_clock_qemu.o
ACME_ORDER_DEPS := $(ACME_ORDER_OBJ_DIR)/kernel.d \
	$(ACME_ORDER_OBJ_DIR)/appliance_acme_order_qemu.d \
	$(ACME_ORDER_OBJ_DIR)/appliance_acme_directory_anchor_fixture.d \
	$(ACME_ORDER_OBJ_DIR)/appliance_acme_fixture_clock_qemu.d
ACME_ORDER_KERNEL := build/acme-order/hfoc-kernel.elf
ACME_ORDER_ISO := build/acme-order/hfoc-os-$(VERSION)-acme-order.iso
ACME_ORDER_ISO_ROOT := build/acme-order/iso_root

# ITEM-508 increment 36: the wire proof of the production order path.  The
# family is the production kernel plus the fixture anchor and the shared
# fixture clock; the boot composition inside it dials the offline stub and the
# fixture entry asks for exactly one order.  Its own port keeps it parallel to
# the fixture order proof.
ACME_BOOT_ORDER_OBJ_DIR := build/acme-boot-order/obj
ACME_BOOT_ORDER_CFLAGS := $(PRODUCTION_CFLAGS) \
	-DHFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST \
	-DHFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_URL='"https://10.0.2.2:18449/directory"'
ACME_BOOT_ORDER_OBJECTS := $(filter-out build/obj/kernel.o \
		build/obj/appliance_acme_trust_anchor.o \
		build/obj/appliance_acme_order_qemu.o \
		build/obj/appliance_acme_directory_anchor_fixture.o \
		build/obj/appliance_acme_fixture_clock_qemu.o \
		build/obj/appliance_production_acme_order_boot.o,$(OBJECTS)) \
	$(ACME_BOOT_ORDER_OBJ_DIR)/kernel.o \
	$(ACME_BOOT_ORDER_OBJ_DIR)/appliance_production_acme_order_boot.o \
	$(ACME_BOOT_ORDER_OBJ_DIR)/appliance_acme_directory_anchor_fixture.o \
	$(ACME_BOOT_ORDER_OBJ_DIR)/appliance_acme_fixture_clock_qemu.o
ACME_BOOT_ORDER_DEPS := $(ACME_BOOT_ORDER_OBJ_DIR)/kernel.d \
	$(ACME_BOOT_ORDER_OBJ_DIR)/appliance_production_acme_order_boot.d \
	$(ACME_BOOT_ORDER_OBJ_DIR)/appliance_acme_directory_anchor_fixture.d \
	$(ACME_BOOT_ORDER_OBJ_DIR)/appliance_acme_fixture_clock_qemu.d
ACME_BOOT_ORDER_KERNEL := build/acme-boot-order/hfoc-kernel.elf
ACME_BOOT_ORDER_ISO := build/acme-boot-order/hfoc-os-$(VERSION)-acme-boot-order.iso
ACME_BOOT_ORDER_ISO_ROOT := build/acme-boot-order/iso_root

# ITEM-508 increment 29: the HTTP-01 wire proof.
#
# The listener under proof is the production listener: this family is the
# production kernel plus one arming hook, and it needs no fixture trust anchor,
# because the proof speaks plain HTTP to guest port 80 and never starts a
# handshake.  The hook arms exactly one token with the same validated clock the
# kernel loop hands the listener's `poll`; a fixture that read its own clock
# could arm an answer that is already past its 900 s window.
ACME_HTTP01_OBJ_DIR := build/acme-http01/obj
ACME_HTTP01_CFLAGS := $(PRODUCTION_CFLAGS) \
	-DHFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST
ACME_HTTP01_OBJECTS := $(filter-out build/obj/kernel.o \
		build/obj/appliance_acme_http01_fixture_qemu.o \
		build/obj/appliance_acme_fixture_clock_qemu.o,$(OBJECTS)) \
	$(ACME_HTTP01_OBJ_DIR)/kernel.o \
	$(ACME_HTTP01_OBJ_DIR)/appliance_acme_http01_fixture_qemu.o \
	$(ACME_HTTP01_OBJ_DIR)/appliance_acme_fixture_clock_qemu.o
ACME_HTTP01_DEPS := $(ACME_HTTP01_OBJ_DIR)/kernel.d \
	$(ACME_HTTP01_OBJ_DIR)/appliance_acme_http01_fixture_qemu.d \
	$(ACME_HTTP01_OBJ_DIR)/appliance_acme_fixture_clock_qemu.d
ACME_HTTP01_KERNEL := build/acme-http01/hfoc-kernel.elf
ACME_HTTP01_ISO := build/acme-http01/hfoc-os-$(VERSION)-acme-http01.iso
ACME_HTTP01_ISO_ROOT := build/acme-http01/iso_root

PERSISTENT_NETWORK_CONFIG_OBJ_DIR := build/persistent-network-config/obj
PERSISTENT_NETWORK_CONFIG_C_OBJS := $(patsubst kernel/src/%.c,$(PERSISTENT_NETWORK_CONFIG_OBJ_DIR)/%.o,$(C_SOURCES))
PERSISTENT_NETWORK_CONFIG_ASM_OBJS := $(patsubst kernel/src/%.S,$(PERSISTENT_NETWORK_CONFIG_OBJ_DIR)/%.o,$(ASM_SOURCES))
PERSISTENT_NETWORK_CONFIG_OBJS := $(PERSISTENT_NETWORK_CONFIG_C_OBJS) $(PERSISTENT_NETWORK_CONFIG_ASM_OBJS)
PERSISTENT_NETWORK_CONFIG_DEPS := $(patsubst kernel/src/%.c,$(PERSISTENT_NETWORK_CONFIG_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(PERSISTENT_NETWORK_CONFIG_OBJ_DIR)/%.d,$(ASM_SOURCES))
PERSISTENT_NETWORK_CONFIG_KERNEL := build/persistent-network-config/hfoc-kernel.elf
PERSISTENT_NETWORK_CONFIG_ISO := build/persistent-network-config/hfoc-os-$(VERSION)-persistent-network-config.iso
PERSISTENT_NETWORK_CONFIG_ISO_ROOT := build/persistent-network-config/iso_root
PERSISTENT_NETWORK_CONFIG_CFLAGS := $(CFLAGS) -DHFOC_APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_TEST

BACKUP_RESTORE_OBJ_DIR := build/backup-restore/obj
BACKUP_RESTORE_C_OBJS := $(patsubst kernel/src/%.c,$(BACKUP_RESTORE_OBJ_DIR)/%.o,$(C_SOURCES))
BACKUP_RESTORE_ASM_OBJS := $(patsubst kernel/src/%.S,$(BACKUP_RESTORE_OBJ_DIR)/%.o,$(ASM_SOURCES))
BACKUP_RESTORE_OBJS := $(BACKUP_RESTORE_C_OBJS) $(BACKUP_RESTORE_ASM_OBJS)
BACKUP_RESTORE_DEPS := $(patsubst kernel/src/%.c,$(BACKUP_RESTORE_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(BACKUP_RESTORE_OBJ_DIR)/%.d,$(ASM_SOURCES))
BACKUP_RESTORE_KERNEL := build/backup-restore/hfoc-kernel.elf
BACKUP_RESTORE_ISO := build/backup-restore/hfoc-os-$(VERSION)-backup-restore.iso
BACKUP_RESTORE_ISO_ROOT := build/backup-restore/iso_root
BACKUP_RESTORE_CFLAGS := $(CFLAGS) -DHFOC_BACKUP_RESTORE_QEMU_TEST

SUPERVISOR_COMPOSITION_OBJ_DIR := build/supervisor-composition/obj
SUPERVISOR_COMPOSITION_C_OBJS := $(patsubst kernel/src/%.c,$(SUPERVISOR_COMPOSITION_OBJ_DIR)/%.o,$(C_SOURCES))
SUPERVISOR_COMPOSITION_ASM_OBJS := $(patsubst kernel/src/%.S,$(SUPERVISOR_COMPOSITION_OBJ_DIR)/%.o,$(ASM_SOURCES))
SUPERVISOR_COMPOSITION_OBJS := $(SUPERVISOR_COMPOSITION_C_OBJS) $(SUPERVISOR_COMPOSITION_ASM_OBJS)
SUPERVISOR_COMPOSITION_DEPS := $(patsubst kernel/src/%.c,$(SUPERVISOR_COMPOSITION_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(SUPERVISOR_COMPOSITION_OBJ_DIR)/%.d,$(ASM_SOURCES))
SUPERVISOR_COMPOSITION_KERNEL := build/supervisor-composition/hfoc-kernel.elf
SUPERVISOR_COMPOSITION_ISO := build/supervisor-composition/hfoc-os-$(VERSION)-supervisor-composition.iso
SUPERVISOR_COMPOSITION_ISO_ROOT := build/supervisor-composition/iso_root
SUPERVISOR_COMPOSITION_CFLAGS := $(CFLAGS) \
	-DHFOC_SERVICE_SUPERVISOR_QEMU_TEST \
	-DHFOC_APPLIANCE_HTTPS_SERVICE_QEMU_TEST \
	-DHFOC_APPLIANCE_PERSISTENT_SERVICE_CONFIG_QEMU_TEST \
	-DHFOC_APPLIANCE_SERVICE_FAILURE_HANDLER_QEMU_TEST \
	-DHFOC_APPLIANCE_PRODUCTION_SERVICE_HTTPS_QEMU_TEST \
	-DHFOC_APPLIANCE_PRODUCTION_MANAGEMENT_QEMU_TEST \
	-DHFOC_APPLIANCE_FIRST_BOOT_INTEGRATION_QEMU_TEST \
	-DHFOC_APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_QEMU_TEST \
	-DHTTPS_SERVICE_LIFECYCLE_HOST_TEST \
	-DAPPLIANCE_MANAGEMENT_PANEL_HOST_TEST

LOCAL_CONTROL_OBJ_DIR := build/local-control/obj
LOCAL_CONTROL_C_OBJS := $(patsubst kernel/src/%.c,$(LOCAL_CONTROL_OBJ_DIR)/%.o,$(C_SOURCES))
LOCAL_CONTROL_ASM_OBJS := $(patsubst kernel/src/%.S,$(LOCAL_CONTROL_OBJ_DIR)/%.o,$(ASM_SOURCES))
LOCAL_CONTROL_OBJS := $(LOCAL_CONTROL_C_OBJS) $(LOCAL_CONTROL_ASM_OBJS)
LOCAL_CONTROL_DEPS := $(patsubst kernel/src/%.c,$(LOCAL_CONTROL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(LOCAL_CONTROL_OBJ_DIR)/%.d,$(ASM_SOURCES))
LOCAL_CONTROL_KERNEL := build/local-control/hfoc-kernel.elf
LOCAL_CONTROL_ISO := build/local-control/hfoc-os-$(VERSION)-local-control.iso
LOCAL_CONTROL_ISO_ROOT := build/local-control/iso_root
LOCAL_CONTROL_CFLAGS := $(CFLAGS) -DHFOC_LOCAL_CONTROL_QEMU_TEST

RECOVERY_EXECUTOR_OBJ_DIR := build/recovery-executor/obj
RECOVERY_EXECUTOR_C_OBJS := $(patsubst kernel/src/%.c,$(RECOVERY_EXECUTOR_OBJ_DIR)/%.o,$(C_SOURCES))
RECOVERY_EXECUTOR_ASM_OBJS := $(patsubst kernel/src/%.S,$(RECOVERY_EXECUTOR_OBJ_DIR)/%.o,$(ASM_SOURCES))
RECOVERY_EXECUTOR_OBJS := $(RECOVERY_EXECUTOR_C_OBJS) $(RECOVERY_EXECUTOR_ASM_OBJS)
RECOVERY_EXECUTOR_DEPS := $(patsubst kernel/src/%.c,$(RECOVERY_EXECUTOR_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(RECOVERY_EXECUTOR_OBJ_DIR)/%.d,$(ASM_SOURCES))
RECOVERY_EXECUTOR_KERNEL := build/recovery-executor/hfoc-kernel.elf
RECOVERY_EXECUTOR_ISO := build/recovery-executor/hfoc-os-$(VERSION)-recovery-executor.iso
RECOVERY_EXECUTOR_ISO_ROOT := build/recovery-executor/iso_root
RECOVERY_EXECUTOR_CFLAGS := $(CFLAGS) \
	-DHFOC_APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_QEMU_TEST

NETWORK_FOUNDATION_OBJ_DIR := build/network-foundation/obj
NETWORK_FOUNDATION_C_OBJS := $(patsubst kernel/src/%.c,$(NETWORK_FOUNDATION_OBJ_DIR)/%.o,$(C_SOURCES))
NETWORK_FOUNDATION_ASM_OBJS := $(patsubst kernel/src/%.S,$(NETWORK_FOUNDATION_OBJ_DIR)/%.o,$(ASM_SOURCES))
NETWORK_FOUNDATION_OBJS := $(NETWORK_FOUNDATION_C_OBJS) $(NETWORK_FOUNDATION_ASM_OBJS)
NETWORK_FOUNDATION_DEPS := $(patsubst kernel/src/%.c,$(NETWORK_FOUNDATION_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(NETWORK_FOUNDATION_OBJ_DIR)/%.d,$(ASM_SOURCES))
NETWORK_FOUNDATION_KERNEL := build/network-foundation/hfoc-kernel.elf
NETWORK_FOUNDATION_ISO := build/network-foundation/hfoc-os-$(VERSION)-network-foundation.iso
NETWORK_FOUNDATION_ISO_ROOT := build/network-foundation/iso_root
NETWORK_FOUNDATION_CFLAGS := $(CFLAGS) -DHFOC_NETWORK_FOUNDATION_QEMU_TEST

VIRTIO_FOUNDATION_OBJ_DIR := build/virtio-foundation/obj
VIRTIO_FOUNDATION_C_OBJS := $(patsubst kernel/src/%.c,$(VIRTIO_FOUNDATION_OBJ_DIR)/%.o,$(C_SOURCES))
VIRTIO_FOUNDATION_ASM_OBJS := $(patsubst kernel/src/%.S,$(VIRTIO_FOUNDATION_OBJ_DIR)/%.o,$(ASM_SOURCES))
VIRTIO_FOUNDATION_OBJS := $(VIRTIO_FOUNDATION_C_OBJS) $(VIRTIO_FOUNDATION_ASM_OBJS)
VIRTIO_FOUNDATION_DEPS := $(patsubst kernel/src/%.c,$(VIRTIO_FOUNDATION_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(VIRTIO_FOUNDATION_OBJ_DIR)/%.d,$(ASM_SOURCES))
VIRTIO_FOUNDATION_KERNEL := build/virtio-foundation/hfoc-kernel.elf
VIRTIO_FOUNDATION_ISO := build/virtio-foundation/hfoc-os-$(VERSION)-virtio-foundation.iso
VIRTIO_FOUNDATION_ISO_ROOT := build/virtio-foundation/iso_root
VIRTIO_FOUNDATION_CFLAGS := $(CFLAGS) -DHFOC_VIRTIO_FOUNDATION_QEMU_TEST

VIRTIO_NET_FRAME_OBJ_DIR := build/virtio-net-frame/obj
VIRTIO_NET_FRAME_C_OBJS := $(patsubst kernel/src/%.c,$(VIRTIO_NET_FRAME_OBJ_DIR)/%.o,$(C_SOURCES))
VIRTIO_NET_FRAME_ASM_OBJS := $(patsubst kernel/src/%.S,$(VIRTIO_NET_FRAME_OBJ_DIR)/%.o,$(ASM_SOURCES))
VIRTIO_NET_FRAME_OBJS := $(VIRTIO_NET_FRAME_C_OBJS) $(VIRTIO_NET_FRAME_ASM_OBJS)
VIRTIO_NET_FRAME_DEPS := $(patsubst kernel/src/%.c,$(VIRTIO_NET_FRAME_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(VIRTIO_NET_FRAME_OBJ_DIR)/%.d,$(ASM_SOURCES))
VIRTIO_NET_FRAME_KERNEL := build/virtio-net-frame/hfoc-kernel.elf
VIRTIO_NET_FRAME_ISO := build/virtio-net-frame/hfoc-os-$(VERSION)-virtio-net-frame.iso
VIRTIO_NET_FRAME_ISO_ROOT := build/virtio-net-frame/iso_root
VIRTIO_NET_FRAME_CFLAGS := $(CFLAGS) -DHFOC_VIRTIO_NET_FRAME_QEMU_TEST

PACKET_BOUNDARY_OBJ_DIR := build/packet-boundary/obj
PACKET_BOUNDARY_C_OBJS := $(patsubst kernel/src/%.c,$(PACKET_BOUNDARY_OBJ_DIR)/%.o,$(C_SOURCES))
PACKET_BOUNDARY_ASM_OBJS := $(patsubst kernel/src/%.S,$(PACKET_BOUNDARY_OBJ_DIR)/%.o,$(ASM_SOURCES))
PACKET_BOUNDARY_OBJS := $(PACKET_BOUNDARY_C_OBJS) $(PACKET_BOUNDARY_ASM_OBJS)
PACKET_BOUNDARY_DEPS := $(patsubst kernel/src/%.c,$(PACKET_BOUNDARY_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(PACKET_BOUNDARY_OBJ_DIR)/%.d,$(ASM_SOURCES))
PACKET_BOUNDARY_KERNEL := build/packet-boundary/hfoc-kernel.elf
PACKET_BOUNDARY_ISO := build/packet-boundary/hfoc-os-$(VERSION)-packet-boundary.iso
PACKET_BOUNDARY_ISO_ROOT := build/packet-boundary/iso_root
PACKET_BOUNDARY_CFLAGS := $(CFLAGS) -DHFOC_VIRTIO_NET_FRAME_QEMU_TEST -DHFOC_PACKET_BOUNDARY_QEMU_TEST

ARP_ICMP_OBJ_DIR := build/arp-icmp/obj
ARP_ICMP_C_OBJS := $(patsubst kernel/src/%.c,$(ARP_ICMP_OBJ_DIR)/%.o,$(C_SOURCES))
ARP_ICMP_ASM_OBJS := $(patsubst kernel/src/%.S,$(ARP_ICMP_OBJ_DIR)/%.o,$(ASM_SOURCES))
ARP_ICMP_LWIP_OBJS := $(patsubst third_party/lwip/src/%.c,$(ARP_ICMP_OBJ_DIR)/lwip/%.o,$(LWIP_PROTO_SOURCES))
ARP_ICMP_OBJS := $(ARP_ICMP_C_OBJS) $(ARP_ICMP_ASM_OBJS) $(ARP_ICMP_LWIP_OBJS)
ARP_ICMP_DEPS := $(patsubst kernel/src/%.c,$(ARP_ICMP_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(ARP_ICMP_OBJ_DIR)/%.d,$(ASM_SOURCES))
ARP_ICMP_KERNEL := build/arp-icmp/hfoc-kernel.elf
ARP_ICMP_ISO := build/arp-icmp/hfoc-os-$(VERSION)-arp-icmp.iso
ARP_ICMP_ISO_ROOT := build/arp-icmp/iso_root
ARP_ICMP_CFLAGS := $(CFLAGS) -DHFOC_ARP_ICMP_QEMU_TEST

TCP_LISTENER_OBJ_DIR := build/tcp-listener/obj
TCP_LISTENER_C_OBJS := $(patsubst kernel/src/%.c,$(TCP_LISTENER_OBJ_DIR)/%.o,$(C_SOURCES))
TCP_LISTENER_ASM_OBJS := $(patsubst kernel/src/%.S,$(TCP_LISTENER_OBJ_DIR)/%.o,$(ASM_SOURCES))
TCP_LISTENER_LWIP_OBJS := $(patsubst third_party/lwip/src/%.c,$(TCP_LISTENER_OBJ_DIR)/lwip/%.o,$(LWIP_TCP_SOURCES))
TCP_LISTENER_OBJS := $(TCP_LISTENER_C_OBJS) $(TCP_LISTENER_ASM_OBJS) $(TCP_LISTENER_LWIP_OBJS)
TCP_LISTENER_DEPS := $(patsubst kernel/src/%.c,$(TCP_LISTENER_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(TCP_LISTENER_OBJ_DIR)/%.d,$(ASM_SOURCES))
TCP_LISTENER_KERNEL := build/tcp-listener/hfoc-kernel.elf
TCP_LISTENER_ISO := build/tcp-listener/hfoc-os-$(VERSION)-tcp-listener.iso
TCP_LISTENER_ISO_ROOT := build/tcp-listener/iso_root
TCP_LISTENER_CFLAGS := $(CFLAGS) -DHFOC_TCP_LISTENER_QEMU_TEST -DHFOC_VIRTIO_NET_FRAME_QEMU_TEST

NETWORK_RESOURCE_OBJ_DIR := build/network-resource/obj
NETWORK_RESOURCE_C_OBJS := $(patsubst kernel/src/%.c,$(NETWORK_RESOURCE_OBJ_DIR)/%.o,$(C_SOURCES))
NETWORK_RESOURCE_ASM_OBJS := $(patsubst kernel/src/%.S,$(NETWORK_RESOURCE_OBJ_DIR)/%.o,$(ASM_SOURCES))
NETWORK_RESOURCE_LWIP_OBJS := $(patsubst third_party/lwip/src/%.c,$(NETWORK_RESOURCE_OBJ_DIR)/lwip/%.o,$(LWIP_TCP_SOURCES))
NETWORK_RESOURCE_OBJS := $(NETWORK_RESOURCE_C_OBJS) $(NETWORK_RESOURCE_ASM_OBJS) $(NETWORK_RESOURCE_LWIP_OBJS)
NETWORK_RESOURCE_DEPS := $(patsubst kernel/src/%.c,$(NETWORK_RESOURCE_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(NETWORK_RESOURCE_OBJ_DIR)/%.d,$(ASM_SOURCES))
NETWORK_RESOURCE_KERNEL := build/network-resource/hfoc-kernel.elf
NETWORK_RESOURCE_ISO := build/network-resource/hfoc-os-$(VERSION)-network-resource.iso
NETWORK_RESOURCE_ISO_ROOT := build/network-resource/iso_root
NETWORK_RESOURCE_CFLAGS := $(CFLAGS) -DHFOC_NETWORK_RESOURCE_QEMU_TEST -DHFOC_TCP_LISTENER_QEMU_TEST -DHFOC_VIRTIO_NET_FRAME_QEMU_TEST

TRUST_POLICY_OBJ_DIR := build/trust-policy/obj
TRUST_POLICY_C_OBJS := $(patsubst kernel/src/%.c,$(TRUST_POLICY_OBJ_DIR)/%.o,$(C_SOURCES))
TRUST_POLICY_ASM_OBJS := $(patsubst kernel/src/%.S,$(TRUST_POLICY_OBJ_DIR)/%.o,$(ASM_SOURCES))
TRUST_POLICY_OBJS := $(TRUST_POLICY_C_OBJS) $(TRUST_POLICY_ASM_OBJS)
TRUST_POLICY_DEPS := $(patsubst kernel/src/%.c,$(TRUST_POLICY_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(TRUST_POLICY_OBJ_DIR)/%.d,$(ASM_SOURCES))
TRUST_POLICY_KERNEL := build/trust-policy/hfoc-kernel.elf
TRUST_POLICY_ISO := build/trust-policy/hfoc-os-$(VERSION)-trust-policy.iso
TRUST_POLICY_ISO_ROOT := build/trust-policy/iso_root
TRUST_POLICY_CFLAGS := $(CFLAGS) -DHFOC_TRUST_POLICY_QEMU_TEST

UPDATE_LIFECYCLE_OBJ_DIR := build/update-lifecycle/obj
UPDATE_LIFECYCLE_C_OBJS := $(patsubst kernel/src/%.c,$(UPDATE_LIFECYCLE_OBJ_DIR)/%.o,$(C_SOURCES))
UPDATE_LIFECYCLE_ASM_OBJS := $(patsubst kernel/src/%.S,$(UPDATE_LIFECYCLE_OBJ_DIR)/%.o,$(ASM_SOURCES))
UPDATE_LIFECYCLE_OBJS := $(UPDATE_LIFECYCLE_C_OBJS) $(UPDATE_LIFECYCLE_ASM_OBJS)
UPDATE_LIFECYCLE_DEPS := $(patsubst kernel/src/%.c,$(UPDATE_LIFECYCLE_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(UPDATE_LIFECYCLE_OBJ_DIR)/%.d,$(ASM_SOURCES))
UPDATE_LIFECYCLE_KERNEL := build/update-lifecycle/hfoc-kernel.elf
UPDATE_LIFECYCLE_ISO := build/update-lifecycle/hfoc-os-$(VERSION)-update-lifecycle.iso
UPDATE_LIFECYCLE_ISO_ROOT := build/update-lifecycle/iso_root
UPDATE_LIFECYCLE_CFLAGS := $(CFLAGS) -DHFOC_UPDATE_LIFECYCLE_QEMU_TEST

SUPPORTABILITY_OBJ_DIR := build/supportability/obj
SUPPORTABILITY_C_OBJS := $(patsubst kernel/src/%.c,$(SUPPORTABILITY_OBJ_DIR)/%.o,$(C_SOURCES))
SUPPORTABILITY_ASM_OBJS := $(patsubst kernel/src/%.S,$(SUPPORTABILITY_OBJ_DIR)/%.o,$(ASM_SOURCES))
SUPPORTABILITY_OBJS := $(SUPPORTABILITY_C_OBJS) $(SUPPORTABILITY_ASM_OBJS)
SUPPORTABILITY_DEPS := $(patsubst kernel/src/%.c,$(SUPPORTABILITY_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(SUPPORTABILITY_OBJ_DIR)/%.d,$(ASM_SOURCES))
SUPPORTABILITY_KERNEL := build/supportability/hfoc-kernel.elf
SUPPORTABILITY_ISO := build/supportability/hfoc-os-$(VERSION)-supportability.iso
SUPPORTABILITY_ISO_ROOT := build/supportability/iso_root
SUPPORTABILITY_CFLAGS := $(CFLAGS) -DHFOC_SUPPORTABILITY_QEMU_TEST

ACCOUNTING_BOUNDARY_OBJ_DIR := build/accounting-boundary/obj
ACCOUNTING_BOUNDARY_C_OBJS := $(patsubst kernel/src/%.c,$(ACCOUNTING_BOUNDARY_OBJ_DIR)/%.o,$(C_SOURCES))
ACCOUNTING_BOUNDARY_ASM_OBJS := $(patsubst kernel/src/%.S,$(ACCOUNTING_BOUNDARY_OBJ_DIR)/%.o,$(ASM_SOURCES))
ACCOUNTING_BOUNDARY_OBJS := $(ACCOUNTING_BOUNDARY_C_OBJS) $(ACCOUNTING_BOUNDARY_ASM_OBJS)
ACCOUNTING_BOUNDARY_DEPS := $(patsubst kernel/src/%.c,$(ACCOUNTING_BOUNDARY_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(ACCOUNTING_BOUNDARY_OBJ_DIR)/%.d,$(ASM_SOURCES))
ACCOUNTING_BOUNDARY_KERNEL := build/accounting-boundary/hfoc-kernel.elf
ACCOUNTING_BOUNDARY_ISO := build/accounting-boundary/hfoc-os-$(VERSION)-accounting-boundary.iso
ACCOUNTING_BOUNDARY_ISO_ROOT := build/accounting-boundary/iso_root
ACCOUNTING_BOUNDARY_CFLAGS := $(CFLAGS) -DHFOC_ACCOUNTING_BOUNDARY_QEMU_TEST

STORAGE_INTERRUPTION_OBJ_DIR := build/storage-interruption/obj
STORAGE_INTERRUPTION_C_OBJS := $(patsubst kernel/src/%.c,$(STORAGE_INTERRUPTION_OBJ_DIR)/%.o,$(C_SOURCES))
STORAGE_INTERRUPTION_ASM_OBJS := $(patsubst kernel/src/%.S,$(STORAGE_INTERRUPTION_OBJ_DIR)/%.o,$(ASM_SOURCES))
STORAGE_INTERRUPTION_OBJS := $(STORAGE_INTERRUPTION_C_OBJS) $(STORAGE_INTERRUPTION_ASM_OBJS)
STORAGE_INTERRUPTION_DEPS := $(patsubst kernel/src/%.c,$(STORAGE_INTERRUPTION_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(STORAGE_INTERRUPTION_OBJ_DIR)/%.d,$(ASM_SOURCES))
STORAGE_INTERRUPTION_KERNEL := build/storage-interruption/hfoc-kernel.elf
STORAGE_INTERRUPTION_ISO := build/storage-interruption/hfoc-os-$(VERSION)-storage-interruption.iso
STORAGE_INTERRUPTION_ISO_ROOT := build/storage-interruption/iso_root
STORAGE_INTERRUPTION_CFLAGS := $(CFLAGS) -DHFOC_STORAGE_INTERRUPTION_QEMU_TEST

SERVICE_CASCADE_DRILL_OBJ_DIR := build/service-cascade-drill/obj
SERVICE_CASCADE_DRILL_C_OBJS := $(patsubst kernel/src/%.c,$(SERVICE_CASCADE_DRILL_OBJ_DIR)/%.o,$(C_SOURCES))
SERVICE_CASCADE_DRILL_ASM_OBJS := $(patsubst kernel/src/%.S,$(SERVICE_CASCADE_DRILL_OBJ_DIR)/%.o,$(ASM_SOURCES))
SERVICE_CASCADE_DRILL_OBJS := $(SERVICE_CASCADE_DRILL_C_OBJS) $(SERVICE_CASCADE_DRILL_ASM_OBJS)
SERVICE_CASCADE_DRILL_DEPS := $(patsubst kernel/src/%.c,$(SERVICE_CASCADE_DRILL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(SERVICE_CASCADE_DRILL_OBJ_DIR)/%.d,$(ASM_SOURCES))
SERVICE_CASCADE_DRILL_KERNEL := build/service-cascade-drill/hfoc-kernel.elf
SERVICE_CASCADE_DRILL_ISO := build/service-cascade-drill/hfoc-os-$(VERSION)-service-cascade-drill.iso
SERVICE_CASCADE_DRILL_ISO_ROOT := build/service-cascade-drill/iso_root
SERVICE_CASCADE_DRILL_CFLAGS := $(CFLAGS) -DHFOC_SERVICE_CASCADE_DRILL_QEMU_TEST

LOCAL_CONTROL_OUTAGE_DRILL_OBJ_DIR := build/local-control-outage-drill/obj
LOCAL_CONTROL_OUTAGE_DRILL_C_OBJS := $(patsubst kernel/src/%.c,$(LOCAL_CONTROL_OUTAGE_DRILL_OBJ_DIR)/%.o,$(C_SOURCES))
LOCAL_CONTROL_OUTAGE_DRILL_ASM_OBJS := $(patsubst kernel/src/%.S,$(LOCAL_CONTROL_OUTAGE_DRILL_OBJ_DIR)/%.o,$(ASM_SOURCES))
LOCAL_CONTROL_OUTAGE_DRILL_OBJS := $(LOCAL_CONTROL_OUTAGE_DRILL_C_OBJS) $(LOCAL_CONTROL_OUTAGE_DRILL_ASM_OBJS)
LOCAL_CONTROL_OUTAGE_DRILL_DEPS := $(patsubst kernel/src/%.c,$(LOCAL_CONTROL_OUTAGE_DRILL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(LOCAL_CONTROL_OUTAGE_DRILL_OBJ_DIR)/%.d,$(ASM_SOURCES))
LOCAL_CONTROL_OUTAGE_DRILL_KERNEL := build/local-control-outage-drill/hfoc-kernel.elf
LOCAL_CONTROL_OUTAGE_DRILL_ISO := build/local-control-outage-drill/hfoc-os-$(VERSION)-local-control-outage-drill.iso
LOCAL_CONTROL_OUTAGE_DRILL_ISO_ROOT := build/local-control-outage-drill/iso_root
LOCAL_CONTROL_OUTAGE_DRILL_CFLAGS := $(CFLAGS) -DHFOC_LOCAL_CONTROL_OUTAGE_DRILL_QEMU_TEST

NETWORK_PARTITION_DRILL_OBJ_DIR := build/network-partition-drill/obj
NETWORK_PARTITION_DRILL_C_OBJS := $(patsubst kernel/src/%.c,$(NETWORK_PARTITION_DRILL_OBJ_DIR)/%.o,$(C_SOURCES))
NETWORK_PARTITION_DRILL_ASM_OBJS := $(patsubst kernel/src/%.S,$(NETWORK_PARTITION_DRILL_OBJ_DIR)/%.o,$(ASM_SOURCES))
NETWORK_PARTITION_DRILL_OBJS := $(NETWORK_PARTITION_DRILL_C_OBJS) $(NETWORK_PARTITION_DRILL_ASM_OBJS)
NETWORK_PARTITION_DRILL_DEPS := $(patsubst kernel/src/%.c,$(NETWORK_PARTITION_DRILL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(NETWORK_PARTITION_DRILL_OBJ_DIR)/%.d,$(ASM_SOURCES))
NETWORK_PARTITION_DRILL_KERNEL := build/network-partition-drill/hfoc-kernel.elf
NETWORK_PARTITION_DRILL_ISO := build/network-partition-drill/hfoc-os-$(VERSION)-network-partition-drill.iso
NETWORK_PARTITION_DRILL_ISO_ROOT := build/network-partition-drill/iso_root
NETWORK_PARTITION_DRILL_CFLAGS := $(CFLAGS) -DHFOC_NETWORK_PARTITION_DRILL_QEMU_TEST

TRUST_FAILURE_DRILL_OBJ_DIR := build/trust-failure-drill/obj
TRUST_FAILURE_DRILL_C_OBJS := $(patsubst kernel/src/%.c,$(TRUST_FAILURE_DRILL_OBJ_DIR)/%.o,$(C_SOURCES))
TRUST_FAILURE_DRILL_ASM_OBJS := $(patsubst kernel/src/%.S,$(TRUST_FAILURE_DRILL_OBJ_DIR)/%.o,$(ASM_SOURCES))
TRUST_FAILURE_DRILL_OBJS := $(TRUST_FAILURE_DRILL_C_OBJS) $(TRUST_FAILURE_DRILL_ASM_OBJS)
TRUST_FAILURE_DRILL_DEPS := $(patsubst kernel/src/%.c,$(TRUST_FAILURE_DRILL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(TRUST_FAILURE_DRILL_OBJ_DIR)/%.d,$(ASM_SOURCES))
TRUST_FAILURE_DRILL_KERNEL := build/trust-failure-drill/hfoc-kernel.elf
TRUST_FAILURE_DRILL_ISO := build/trust-failure-drill/hfoc-os-$(VERSION)-trust-failure-drill.iso
TRUST_FAILURE_DRILL_ISO_ROOT := build/trust-failure-drill/iso_root
TRUST_FAILURE_DRILL_CFLAGS := $(CFLAGS) -DHFOC_TRUST_FAILURE_DRILL_QEMU_TEST

UPDATE_FAILURE_DRILL_OBJ_DIR := build/update-failure-drill/obj
UPDATE_FAILURE_DRILL_C_OBJS := $(patsubst kernel/src/%.c,$(UPDATE_FAILURE_DRILL_OBJ_DIR)/%.o,$(C_SOURCES))
UPDATE_FAILURE_DRILL_ASM_OBJS := $(patsubst kernel/src/%.S,$(UPDATE_FAILURE_DRILL_OBJ_DIR)/%.o,$(ASM_SOURCES))
UPDATE_FAILURE_DRILL_OBJS := $(UPDATE_FAILURE_DRILL_C_OBJS) $(UPDATE_FAILURE_DRILL_ASM_OBJS)
UPDATE_FAILURE_DRILL_DEPS := $(patsubst kernel/src/%.c,$(UPDATE_FAILURE_DRILL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(UPDATE_FAILURE_DRILL_OBJ_DIR)/%.d,$(ASM_SOURCES))
UPDATE_FAILURE_DRILL_KERNEL := build/update-failure-drill/hfoc-kernel.elf
UPDATE_FAILURE_DRILL_ISO := build/update-failure-drill/hfoc-os-$(VERSION)-update-failure-drill.iso
UPDATE_FAILURE_DRILL_ISO_ROOT := build/update-failure-drill/iso_root
UPDATE_FAILURE_DRILL_CFLAGS := $(CFLAGS) -DHFOC_UPDATE_FAILURE_DRILL_QEMU_TEST

INTEGRATION_REPLAY_OUTAGE_DRILL_OBJ_DIR := build/integration-replay-outage-drill/obj
INTEGRATION_REPLAY_OUTAGE_DRILL_C_OBJS := $(patsubst kernel/src/%.c,$(INTEGRATION_REPLAY_OUTAGE_DRILL_OBJ_DIR)/%.o,$(C_SOURCES))
INTEGRATION_REPLAY_OUTAGE_DRILL_ASM_OBJS := $(patsubst kernel/src/%.S,$(INTEGRATION_REPLAY_OUTAGE_DRILL_OBJ_DIR)/%.o,$(ASM_SOURCES))
INTEGRATION_REPLAY_OUTAGE_DRILL_OBJS := $(INTEGRATION_REPLAY_OUTAGE_DRILL_C_OBJS) $(INTEGRATION_REPLAY_OUTAGE_DRILL_ASM_OBJS)
INTEGRATION_REPLAY_OUTAGE_DRILL_DEPS := $(patsubst kernel/src/%.c,$(INTEGRATION_REPLAY_OUTAGE_DRILL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(INTEGRATION_REPLAY_OUTAGE_DRILL_OBJ_DIR)/%.d,$(ASM_SOURCES))
INTEGRATION_REPLAY_OUTAGE_DRILL_KERNEL := build/integration-replay-outage-drill/hfoc-kernel.elf
INTEGRATION_REPLAY_OUTAGE_DRILL_ISO := build/integration-replay-outage-drill/hfoc-os-$(VERSION)-integration-replay-outage-drill.iso
INTEGRATION_REPLAY_OUTAGE_DRILL_ISO_ROOT := build/integration-replay-outage-drill/iso_root
INTEGRATION_REPLAY_OUTAGE_DRILL_CFLAGS := $(CFLAGS) -DHFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_QEMU_TEST

POWER_CYCLE_CONTINUITY_DRILL_OBJ_DIR := build/power-cycle-continuity-drill/obj
POWER_CYCLE_CONTINUITY_DRILL_C_OBJS := $(patsubst kernel/src/%.c,$(POWER_CYCLE_CONTINUITY_DRILL_OBJ_DIR)/%.o,$(C_SOURCES))
POWER_CYCLE_CONTINUITY_DRILL_ASM_OBJS := $(patsubst kernel/src/%.S,$(POWER_CYCLE_CONTINUITY_DRILL_OBJ_DIR)/%.o,$(ASM_SOURCES))
POWER_CYCLE_CONTINUITY_DRILL_OBJS := $(POWER_CYCLE_CONTINUITY_DRILL_C_OBJS) $(POWER_CYCLE_CONTINUITY_DRILL_ASM_OBJS)
POWER_CYCLE_CONTINUITY_DRILL_DEPS := $(patsubst kernel/src/%.c,$(POWER_CYCLE_CONTINUITY_DRILL_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(POWER_CYCLE_CONTINUITY_DRILL_OBJ_DIR)/%.d,$(ASM_SOURCES))
POWER_CYCLE_CONTINUITY_DRILL_KERNEL := build/power-cycle-continuity-drill/hfoc-kernel.elf
POWER_CYCLE_CONTINUITY_DRILL_ISO := build/power-cycle-continuity-drill/hfoc-os-$(VERSION)-power-cycle-continuity-drill.iso
POWER_CYCLE_CONTINUITY_DRILL_ISO_ROOT := build/power-cycle-continuity-drill/iso_root
POWER_CYCLE_CONTINUITY_DRILL_CFLAGS := $(CFLAGS) -DHFOC_POWER_CYCLE_CONTINUITY_DRILL_QEMU_TEST

ENTROPY_OBJ_DIR := build/entropy/obj
ENTROPY_C_OBJS := $(patsubst kernel/src/%.c,$(ENTROPY_OBJ_DIR)/%.o,$(C_SOURCES))
ENTROPY_ASM_OBJS := $(patsubst kernel/src/%.S,$(ENTROPY_OBJ_DIR)/%.o,$(ASM_SOURCES))
ENTROPY_OBJS := $(ENTROPY_C_OBJS) $(ENTROPY_ASM_OBJS)
ENTROPY_DEPS := $(patsubst kernel/src/%.c,$(ENTROPY_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(ENTROPY_OBJ_DIR)/%.d,$(ASM_SOURCES))
ENTROPY_KERNEL := build/entropy/hfoc-kernel.elf
ENTROPY_ISO := build/entropy/hfoc-os-$(VERSION)-entropy.iso
ENTROPY_ISO_ROOT := build/entropy/iso_root
ENTROPY_CFLAGS := $(CFLAGS) -DHFOC_ENTROPY_QEMU_TEST

MBEDTLS_TEST_SECRET_OBJ_DIR := build/mbedtls-test-secret/obj
MBEDTLS_TEST_SECRET_C_OBJS := $(patsubst kernel/src/%.c,$(MBEDTLS_TEST_SECRET_OBJ_DIR)/%.o,$(C_SOURCES))
MBEDTLS_TEST_SECRET_ASM_OBJS := $(patsubst kernel/src/%.S,$(MBEDTLS_TEST_SECRET_OBJ_DIR)/%.o,$(ASM_SOURCES))
MBEDTLS_TEST_SECRET_OBJS := $(MBEDTLS_TEST_SECRET_C_OBJS) $(MBEDTLS_TEST_SECRET_ASM_OBJS)
MBEDTLS_TEST_SECRET_DEPS := $(patsubst kernel/src/%.c,$(MBEDTLS_TEST_SECRET_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(MBEDTLS_TEST_SECRET_OBJ_DIR)/%.d,$(ASM_SOURCES))
MBEDTLS_TEST_SECRET_KERNEL := build/mbedtls-test-secret/hfoc-kernel.elf
MBEDTLS_TEST_SECRET_ISO := build/mbedtls-test-secret/hfoc-os-$(VERSION)-mbedtls-test-secret.iso
MBEDTLS_TEST_SECRET_ISO_ROOT := build/mbedtls-test-secret/iso_root
MBEDTLS_TEST_SECRET_CFLAGS := $(CFLAGS) -DHFOC_MBEDTLS_TEST_SECRET_QEMU_TEST

MBEDTLS_TLS_QEMU_OBJ_DIR := build/mbedtls-tls-qemu/obj
MBEDTLS_TLS_QEMU_C_OBJS := $(patsubst kernel/src/%.c,$(MBEDTLS_TLS_QEMU_OBJ_DIR)/%.o,$(C_SOURCES))
MBEDTLS_TLS_QEMU_ASM_OBJS := $(patsubst kernel/src/%.S,$(MBEDTLS_TLS_QEMU_OBJ_DIR)/%.o,$(ASM_SOURCES))
MBEDTLS_TLS_QEMU_LWIP_OBJS := $(patsubst third_party/lwip/src/%.c,$(MBEDTLS_TLS_QEMU_OBJ_DIR)/lwip/%.o,$(LWIP_TCP_SOURCES))
MBEDTLS_TLS_QEMU_MBEDTLS_SOURCES := $(shell find third_party/mbedtls/library -type f -name '*.c' -print | sort)
MBEDTLS_TLS_QEMU_MBEDTLS_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(MBEDTLS_TLS_QEMU_OBJ_DIR)/mbedtls/%.o,$(MBEDTLS_TLS_QEMU_MBEDTLS_SOURCES))
MBEDTLS_TLS_QEMU_OBJS := $(MBEDTLS_TLS_QEMU_C_OBJS) $(MBEDTLS_TLS_QEMU_ASM_OBJS) $(MBEDTLS_TLS_QEMU_LWIP_OBJS) $(MBEDTLS_TLS_QEMU_MBEDTLS_OBJS)
MBEDTLS_TLS_QEMU_DEPS := $(patsubst kernel/src/%.c,$(MBEDTLS_TLS_QEMU_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(MBEDTLS_TLS_QEMU_OBJ_DIR)/%.d,$(ASM_SOURCES))
MBEDTLS_TLS_QEMU_LWIP_DEPS := $(patsubst third_party/lwip/src/%.c,$(MBEDTLS_TLS_QEMU_OBJ_DIR)/lwip/%.d,$(LWIP_TCP_SOURCES))
MBEDTLS_TLS_QEMU_MBEDTLS_DEPS := $(patsubst third_party/mbedtls/library/%.c,$(MBEDTLS_TLS_QEMU_OBJ_DIR)/mbedtls/%.d,$(MBEDTLS_TLS_QEMU_MBEDTLS_SOURCES))
MBEDTLS_TLS_QEMU_KERNEL := build/mbedtls-tls-qemu/hfoc-kernel.elf
MBEDTLS_TLS_QEMU_ISO := build/mbedtls-tls-qemu/hfoc-os-$(VERSION)-tls-qemu.iso
MBEDTLS_TLS_QEMU_ISO_ROOT := build/mbedtls-tls-qemu/iso_root
MBEDTLS_TLS_QEMU_CFLAGS := $(CFLAGS) \
	-DHFOC_MBEDTLS_TLS_QEMU_TEST \
	-DHFOC_TCP_LISTENER_QEMU_TEST \
	-DHFOC_VIRTIO_NET_FRAME_QEMU_TEST \
	-DHFOC_MBEDTLS_TEST_SECRET_QEMU_TEST \
	-DMBEDTLS_CONFIG_FILE='"hfoc-tls-qemu-config.h"' \
	-Ithird_party/mbedtls/include -Itests

HTTPS_SERVICE_QEMU_OBJ_DIR := build/https-service-qemu/obj
HTTPS_SERVICE_QEMU_C_OBJS := $(patsubst kernel/src/%.c,$(HTTPS_SERVICE_QEMU_OBJ_DIR)/%.o,$(C_SOURCES))
HTTPS_SERVICE_QEMU_ASM_OBJS := $(patsubst kernel/src/%.S,$(HTTPS_SERVICE_QEMU_OBJ_DIR)/%.o,$(ASM_SOURCES))
HTTPS_SERVICE_QEMU_LWIP_OBJS := $(patsubst third_party/lwip/src/%.c,$(HTTPS_SERVICE_QEMU_OBJ_DIR)/lwip/%.o,$(LWIP_TCP_SOURCES))
HTTPS_SERVICE_QEMU_MBEDTLS_SOURCES := $(shell find third_party/mbedtls/library -type f -name '*.c' -print | sort)
HTTPS_SERVICE_QEMU_MBEDTLS_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(HTTPS_SERVICE_QEMU_OBJ_DIR)/mbedtls/%.o,$(HTTPS_SERVICE_QEMU_MBEDTLS_SOURCES))
HTTPS_SERVICE_QEMU_OBJS := $(HTTPS_SERVICE_QEMU_C_OBJS) $(HTTPS_SERVICE_QEMU_ASM_OBJS) $(HTTPS_SERVICE_QEMU_LWIP_OBJS) $(HTTPS_SERVICE_QEMU_MBEDTLS_OBJS)
HTTPS_SERVICE_QEMU_DEPS := $(patsubst kernel/src/%.c,$(HTTPS_SERVICE_QEMU_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(HTTPS_SERVICE_QEMU_OBJ_DIR)/%.d,$(ASM_SOURCES))
HTTPS_SERVICE_QEMU_LWIP_DEPS := $(patsubst third_party/lwip/src/%.c,$(HTTPS_SERVICE_QEMU_OBJ_DIR)/lwip/%.d,$(LWIP_TCP_SOURCES))
HTTPS_SERVICE_QEMU_MBEDTLS_DEPS := $(patsubst third_party/mbedtls/library/%.c,$(HTTPS_SERVICE_QEMU_OBJ_DIR)/mbedtls/%.d,$(HTTPS_SERVICE_QEMU_MBEDTLS_SOURCES))
HTTPS_SERVICE_QEMU_KERNEL := build/https-service-qemu/hfoc-kernel.elf
HTTPS_SERVICE_QEMU_ISO := build/https-service-qemu/hfoc-os-$(VERSION)-https-service.iso
HTTPS_SERVICE_QEMU_ISO_ROOT := build/https-service-qemu/iso_root
HTTPS_SERVICE_QEMU_CFLAGS := $(MBEDTLS_TLS_QEMU_CFLAGS) \
	-DHFOC_HTTPS_SERVICE_QEMU_TEST \
	-DHFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST \
	-DHTTP_PARSER_HOST_TEST \
	-DHTTP_REQUEST_LINE_HOST_TEST \
	-DHTTP_HEADER_HOST_TEST \
	-DHTTP_POLICY_HOST_TEST \
	-DHTTP_FRAMING_HOST_TEST \
	-DHTTP_ROUTES_HOST_TEST \
	-DHTTP_RESPONSE_HOST_TEST \
	-DHTTP_ERROR_MATRIX_HOST_TEST \
	-DHTTPS_HANDLER_HOST_TEST \
	-DHTTPS_HEALTH_HOST_TEST \
	-DHTTPS_PEER_THROTTLE_HOST_TEST \
	-DHTTPS_SERVICE_LIFECYCLE_HOST_TEST \
	-DAPPLIANCE_MANAGEMENT_PANEL_HOST_TEST

HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR := build/https-entropy-fail-closed-qemu/obj
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_C_OBJS := $(patsubst kernel/src/%.c,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/%.o,$(C_SOURCES))
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ASM_OBJS := $(patsubst kernel/src/%.S,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/%.o,$(ASM_SOURCES))
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_LWIP_OBJS := $(patsubst third_party/lwip/src/%.c,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/lwip/%.o,$(LWIP_TCP_SOURCES))
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_MBEDTLS_SOURCES := $(shell find third_party/mbedtls/library -type f -name '*.c' -print | sort)
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_MBEDTLS_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/mbedtls/%.o,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_MBEDTLS_SOURCES))
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJS := $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_C_OBJS) $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ASM_OBJS) $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_LWIP_OBJS) $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_MBEDTLS_OBJS)
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_DEPS := $(patsubst kernel/src/%.c,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/%.d,$(ASM_SOURCES))
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_LWIP_DEPS := $(patsubst third_party/lwip/src/%.c,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/lwip/%.d,$(LWIP_TCP_SOURCES))
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_MBEDTLS_DEPS := $(patsubst third_party/mbedtls/library/%.c,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/mbedtls/%.d,$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_MBEDTLS_SOURCES))
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_KERNEL := build/https-entropy-fail-closed-qemu/hfoc-kernel.elf
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO := build/https-entropy-fail-closed-qemu/hfoc-os-$(VERSION)-https-entropy-fail-closed.iso
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT := build/https-entropy-fail-closed-qemu/iso_root
HTTPS_ENTROPY_FAIL_CLOSED_QEMU_CFLAGS := $(HTTPS_SERVICE_QEMU_CFLAGS) \
	-DHFOC_HTTPS_ENTROPY_FAIL_CLOSED_QEMU_TEST

HTTPS_RESTART_SOAK_QEMU_OBJ_DIR := build/https-restart-soak-qemu/obj
HTTPS_RESTART_SOAK_QEMU_C_OBJS := $(patsubst kernel/src/%.c,$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/%.o,$(C_SOURCES))
HTTPS_RESTART_SOAK_QEMU_ASM_OBJS := $(patsubst kernel/src/%.S,$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/%.o,$(ASM_SOURCES))
HTTPS_RESTART_SOAK_QEMU_LWIP_OBJS := $(patsubst third_party/lwip/src/%.c,$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/lwip/%.o,$(LWIP_TCP_SOURCES))
HTTPS_RESTART_SOAK_QEMU_MBEDTLS_SOURCES := $(shell find third_party/mbedtls/library -type f -name '*.c' -print | sort)
HTTPS_RESTART_SOAK_QEMU_MBEDTLS_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/mbedtls/%.o,$(HTTPS_RESTART_SOAK_QEMU_MBEDTLS_SOURCES))
HTTPS_RESTART_SOAK_QEMU_OBJS := $(HTTPS_RESTART_SOAK_QEMU_C_OBJS) $(HTTPS_RESTART_SOAK_QEMU_ASM_OBJS) $(HTTPS_RESTART_SOAK_QEMU_LWIP_OBJS) $(HTTPS_RESTART_SOAK_QEMU_MBEDTLS_OBJS)
HTTPS_RESTART_SOAK_QEMU_DEPS := $(patsubst kernel/src/%.c,$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/%.d,$(ASM_SOURCES))
HTTPS_RESTART_SOAK_QEMU_LWIP_DEPS := $(patsubst third_party/lwip/src/%.c,$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/lwip/%.d,$(LWIP_TCP_SOURCES))
HTTPS_RESTART_SOAK_QEMU_MBEDTLS_DEPS := $(patsubst third_party/mbedtls/library/%.c,$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/mbedtls/%.d,$(HTTPS_RESTART_SOAK_QEMU_MBEDTLS_SOURCES))
HTTPS_RESTART_SOAK_QEMU_KERNEL := build/https-restart-soak-qemu/hfoc-kernel.elf
HTTPS_RESTART_SOAK_QEMU_ISO := build/https-restart-soak-qemu/hfoc-os-$(VERSION)-https-restart-soak.iso
HTTPS_RESTART_SOAK_QEMU_ISO_ROOT := build/https-restart-soak-qemu/iso_root
HTTPS_RESTART_SOAK_QEMU_CFLAGS := $(HTTPS_SERVICE_QEMU_CFLAGS) \
	-DHFOC_HTTPS_RESTART_SOAK_QEMU_TEST

.PHONY: all kernel deps iso run run-uefi debug run-trace check test-irqs test-panic test-pagefault test-doublefault test-nmi test-acpi test-acpi-host test-pmm-host test-pmm-pci-hole-host test-pmm test-drill-stack-budget-host test-heap-host test-heap test-vmm-host test-vmm-user-host test-vmm test-vmm-as test-cpu-host test-context-host test-user-iretq-host test-user-iretq test-syscall-host test-syscall-origin-host test-syscall-gate-host test-syscall test-user-init-process-host test-user-init test-boot-policy-regression test-block-device-host test-block-device test-data-region-host test-data-region test-block-cache-host test-block-cache test-hfocfs-host test-hfocfs-workspace-host test-hfocfs-qemu test-hfocfs-workspace-qemu test-config-journal-host test-config-journal test-config-kv-host test-state-schema-host test-state-migration-host test-journal-compaction-host test-backup-stream-host test-backup-manifest-host test-backup-producer-host test-restore-validator-host test-restore-planner-host test-restore-activation-host test-service-descriptor-host test-https-service-descriptor-host test-https-admission-host test-https-connection-host test-https-data-flow-host test-https-peer-throttle-host test-https-service-lifecycle-host test-local-principal-host test-auth-throttle-host test-local-session-host test-management-command-host test-authorization-host test-appliance-status-host test-controlled-configuration-host test-audit-correlation-host test-local-control-qemu test-network-device-host test-link-state-host test-virtio-pci-identity-host test-ipv4-config-host test-network-config-transaction-host test-packet-buffer-host test-icmp-reachability-host test-management-transport-host test-network-admission-host test-network-event-host test-key-use-host test-trust-anchor-host test-verification-policy-host test-signed-metadata-host test-trust-rotation-host test-trust-revocation-host test-image-policy-host test-boot-evidence-host test-recovery-trust-host test-update-compatibility-host test-candidate-slot-inventory-host test-candidate-preflight-host test-candidate-staged-write-host test-activation-intent-host test-first-boot-health-host test-storage-health-host test-service-health-host test-appliance-health-host test-network-health-host test-audit-ring-health-host test-metrics-host test-fault-injection-host test-storage-interruption-host test-support-bundle-manifest-host test-support-bundle-exporter-host test-appliance-support-bundle-host test-appliance-support-session-host test-appliance-support-break-glass-host test-appliance-production-support-host test-hardware-acceptance-host test-product-release-gate-host test-boot-chain-determinism-host test-diagnostic-privacy-host test-integration-boundary-host test-integration-principal-host test-integration-session-host test-integration-envelope-host test-integration-ledger-host test-integration-event-cursor-host test-integration-config-intent-host test-integration-failure-isolation-host test-integration-compatibility-host test-integration-replay-outage-drill-host test-integration-replay-outage-drill-qemu test-automatic-rollback-host test-update-acceptance-host test-update-retention-host test-service-dependency-host test-service-graph-host test-service-restart-policy-host test-service-supervisor-host test-service-supervisor-qemu test-scheduler-host test-scheduler-rsp0-host test-scheduler-thread-host test-scheduler-yield-host test-scheduler-tick-host test-scheduler-wait-host test-scheduler-accounting-host test-scheduler-discipline-host test-scheduler-exit-host test-scheduler-stress-host test-process-host test-process-create-host test-process-limit-host test-descriptor-host test-vfs-host test-initramfs-host test-vfs-fd-host test-capability-host test-event-ring-host test-audit-host test-audit-snapshot-host test-audit-observability-host test-config-schema-host test-config-snapshot-host test-boot-policy-host test-system-image-host test-sha256-host test-image-integrity-host test-recovery-image-host test-process-scheduler-host test-scheduler-yield test-scheduler-tick test-scheduler-wait test-scheduler-exit test-scheduler-stress test-process-scheduler test-smp-host test-smp test-lapic-host test-lapic-timer-host test-lapic-timer-runtime-host test-lapic-timer-runtime test-lapic-timer-calibration test-lapic-timer-oneshot test-pic-disable test-x2apic test-lapic test-ioapic test-ioapic-host test-ioapic-irq1 test-ioapic-irq0 test-ioapic-irq3-7 test-ioapic-irq8-15 test-isa-irq3-7-host test-isa-irq8-15-host test-keyboard-host test-keyboard-line-editing test-console-history test-console-host test-pic-host test-pic-irq0-masking test-pic-irq0-masking-fallback test-backup-restore-qemu test-trust-policy-qemu test-update-lifecycle-qemu test-supportability-qemu test-accounting-boundary-qemu test-storage-interruption-qemu test-service-cascade-drill-host test-service-cascade-drill-qemu test-local-control-outage-drill-host test-network-partition-drill-host test-trust-failure-drill-host test-local-control-outage-drill-qemu test-network-partition-drill-qemu test-trust-failure-drill-qemu test-update-failure-drill-host test-update-failure-drill-qemu test-power-cycle-continuity-drill-host test-power-cycle-continuity-drill-qemu test clean distclean
.PHONY: test-virtio-pci-capability-host test-virtio-pci-device-status-host test-virtio-pci-feature-host test-virtio-queue-layout-host test-virtio-descriptor-ownership-host test-virtio-available-publication-host test-virtio-used-consumption-host test-virtio-dma-region-host test-virtio-net-feature-profile-host test-virtio-rng-feature-profile-host test-virtio-rng-queue-composition-host test-virtio-rng-request-host test-virtio-rng-completion-host test-entropy-health-host test-entropy-accumulator-host test-entropy-seed-fill-host test-entropy-diagnostics-host test-entropy-restart-host test-virtio-net-queue-composition-host test-virtio-net-rx-host test-virtio-net-tx-host test-virtio-net-interrupt-host test-virtio-net-link-state-host test-virtio-net-lifecycle-host test-virtio-foundation-qemu test-resilience-matrix-host test-resilience-matrix-qemu
.PHONY: test-pci-host test-ahci-host test-nvme-host test-appliance-gpt-discovery-host test-nvme-state-integration-host
.PHONY: test-entropy-qemu
.PHONY: test-virtio-net-frame-qemu
.PHONY: test-production-network-qemu
# ITEM-601: the production SSH endpoint on the fixture kernel, driven by real
# stock OpenSSH.  The fixture seeds an in-memory host key and the committed
# test-only client key; the production ELF must contain neither marker.
.PHONY: test-appliance-ssh-qemu
test-appliance-ssh-qemu: $(SSH_QEMU_ISO) $(KERNEL) \
		scripts/hfoc-ssh-collect.sh
	@strings $(SSH_QEMU_KERNEL) | grep -Fq '[SSH-QEMU]' || \
		{ echo "FAIL: SSH fixture kernel missing its test marker" >&2; exit 1; }
	@strings $(SSH_QEMU_ISO) | grep -Fq '[SSH-QEMU]' || \
		{ echo "FAIL: SSH fixture ISO missing its test marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SSH-QEMU]'; then \
		echo "FAIL: the production kernel contains the SSH fixture marker" >&2; \
		exit 1; fi
	IMAGE='$(SSH_QEMU_ISO)' ./scripts/test-qemu-ssh.sh

.PHONY: test-production-dhcp-dns-qemu
.PHONY: test-production-first-boot-qemu
.PHONY: test-production-service-https-qemu
.PHONY: test-production-service-https-main-qemu
.PHONY: test-production-service-https-noentropy-qemu
.PHONY: qemu-iso-ready
.PHONY: test-production-sni-selection-qemu
.PHONY: test-production-forward-qemu
.PHONY: test-packet-boundary-qemu
.PHONY: test-arp-icmp-qemu
.PHONY: test-tcp-listener-qemu test-network-resource-qemu
.PHONY: test-tls-qemu
.PHONY: test-https-service-qemu
.PHONY: test-management-plane-qemu
.PHONY: test-https-client-qemu
.PHONY: test-https-response-matrix-qemu
.PHONY: test-https-malformed-request-matrix-qemu
.PHONY: test-https-size-boundary-matrix-qemu
.PHONY: test-https-exhaustion-matrix-qemu
.PHONY: test-entropy-fail-closed-matrix-qemu
.PHONY: test-https-restart-soak-qemu
.PHONY: test-https-vertical-slice-acceptance-host test-https-vertical-slice-acceptance
.PHONY: test-release-profile-host
.PHONY: test-build-input-manifest-host build-input-manifest
.PHONY: test-reproducible-image-comparison-host test-reproducible-image-host
.PHONY: test-release-artifact-manifest-host test-release-artifact-host
.PHONY: test-release-upgrade-matrix-host test-release-upgrade-matrix
.PHONY: test-release-backup-restore-matrix-host \
	test-release-backup-restore-matrix
.PHONY: test-release-security-baseline-host test-release-security-baseline
.PHONY: test-release-operational-baseline-host test-release-operational-baseline
.PHONY: test-release-operational-qemu-liveness
.PHONY: test-release-disaster-recovery-rehearsal-host test-release-disaster-recovery-rehearsal
.PHONY: test-release-appliance-v1-acceptance-host test-release-appliance-v1-acceptance
.PHONY: test-hfoc-appliance-platform-release-acceptance-host test-hfoc-appliance-platform-release-acceptance
.PHONY: test-hfoc-integrated-installer-dual-boot-management-acceptance-host test-hfoc-integrated-installer-dual-boot-management-acceptance
.PHONY: test-hfoc-release-documentation-host test-hfoc-release-documentation
all: iso

build/obj/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCTION_CFLAGS) -MMD -MP -c $< -o $@

build/obj/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(PRODUCTION_LWIP_OBJ_DIR)/%.o: third_party/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(FALLBACK_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(FALLBACK_CFLAGS) -MMD -MP -c $< -o $@

$(FALLBACK_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ONESHOT_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(ONESHOT_CFLAGS) -MMD -MP -c $< -o $@

$(ONESHOT_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(LAPIC_RUNTIME_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(LAPIC_RUNTIME_CFLAGS) -MMD -MP -c $< -o $@

$(LAPIC_RUNTIME_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(IRQ3_7_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(IRQ3_7_CFLAGS) -MMD -MP -c $< -o $@

$(IRQ3_7_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(IRQ8_15_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(IRQ8_15_CFLAGS) -MMD -MP -c $< -o $@

$(IRQ8_15_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(X2APIC_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(X2APIC_CFLAGS) -MMD -MP -c $< -o $@

$(X2APIC_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(VMM_AS_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(VMM_AS_CFLAGS) -MMD -MP -c $< -o $@

$(VMM_AS_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(SCHED_YIELD_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(SCHED_YIELD_CFLAGS) -MMD -MP -c $< -o $@

$(SCHED_YIELD_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(SCHED_TICK_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(SCHED_TICK_CFLAGS) -MMD -MP -c $< -o $@

$(SCHED_TICK_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(SCHED_WAIT_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(SCHED_WAIT_CFLAGS) -MMD -MP -c $< -o $@

$(SCHED_WAIT_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(SCHED_EXIT_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(SCHED_EXIT_CFLAGS) -MMD -MP -c $< -o $@

$(SCHED_EXIT_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(SCHED_STRESS_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(SCHED_STRESS_CFLAGS) -MMD -MP -c $< -o $@

$(SCHED_STRESS_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(PROCESS_SCHED_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(PROCESS_SCHED_CFLAGS) -MMD -MP -c $< -o $@

$(PROCESS_SCHED_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(USER_IRETQ_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(USER_IRETQ_CFLAGS) -MMD -MP -c $< -o $@

$(USER_IRETQ_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(USER_IRETQ_CFLAGS) -MMD -MP -c $< -o $@

$(SYSCALL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(SYSCALL_CFLAGS) -MMD -MP -c $< -o $@

$(SYSCALL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(SYSCALL_CFLAGS) -MMD -MP -c $< -o $@

$(USER_INIT_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(USER_INIT_CFLAGS) -MMD -MP -c $< -o $@

$(USER_INIT_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(USER_INIT_CFLAGS) -MMD -MP -c $< -o $@

$(BOOT_POLICY_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(BOOT_POLICY_CFLAGS) -MMD -MP -c $< -o $@

$(BOOT_POLICY_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(BOOT_POLICY_CFLAGS) -MMD -MP -c $< -o $@

$(BLOCK_DEVICE_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(BLOCK_DEVICE_CFLAGS) -MMD -MP -c $< -o $@

$(BLOCK_DEVICE_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(BLOCK_DEVICE_CFLAGS) -MMD -MP -c $< -o $@

$(DATA_REGION_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(DATA_REGION_CFLAGS) -MMD -MP -c $< -o $@

$(DATA_REGION_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(DATA_REGION_CFLAGS) -MMD -MP -c $< -o $@

$(BLOCK_CACHE_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(BLOCK_CACHE_CFLAGS) -MMD -MP -c $< -o $@

$(BLOCK_CACHE_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(BLOCK_CACHE_CFLAGS) -MMD -MP -c $< -o $@

$(HFOCFS_QEMU_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(HFOCFS_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HFOCFS_QEMU_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(HFOCFS_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HFOCFS_WORKSPACE_QEMU_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(HFOCFS_WORKSPACE_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HFOCFS_WORKSPACE_QEMU_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(HFOCFS_WORKSPACE_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(DIAG_EXPORT_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(DIAG_EXPORT_CFLAGS) -MMD -MP -c $< -o $@

$(DIAG_EXPORT_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(DIAG_EXPORT_CFLAGS) -MMD -MP -c $< -o $@

$(CONFIG_JOURNAL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CONFIG_JOURNAL_CFLAGS) -MMD -MP -c $< -o $@

$(CONFIG_JOURNAL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(CONFIG_JOURNAL_CFLAGS) -MMD -MP -c $< -o $@

$(PHYSICAL_STORAGE_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(PHYSICAL_STORAGE_CFLAGS) -MMD -MP -c $< -o $@

$(PHYSICAL_STORAGE_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(PHYSICAL_STORAGE_CFLAGS) -MMD -MP -c $< -o $@

$(PHYSICAL_SATA_ACCEPTANCE_AHCI_OBJ): kernel/src/ahci_qemu.c
	@mkdir -p $(@D)
	$(CC) $(PHYSICAL_SATA_ACCEPTANCE_CFLAGS) -MMD -MP -c $< -o $@

$(PRODUCTION_INSTALLER_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(PRODUCTION_INSTALLER_CFLAGS) -MMD -MP -c $< -o $@

# ITEM-598: the installer seals the host seed under the account password before
# writing the persistence partition, so this object gets the same bounded
# state-crypto profile as the installed kernel.
$(PRODUCTION_INSTALLER_OBJ_DIR)/appliance_state_crypto.o: \
		kernel/src/appliance_state_crypto.c \
		kernel/include/hfoc_state_crypto_config.h \
		Makefile
	@mkdir -p $(@D)
	$(CC) $(PRODUCTION_INSTALLER_CFLAGS) \
		-DHFOC_PRODUCTION_STATE_ENCRYPTION \
		$(HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

$(PRODUCTION_INSTALLER_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(PRODUCTION_INSTALLER_CFLAGS) -MMD -MP -c $< -o $@

$(HFOC_EFI_HELPER_OBJ_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(HFOC_EFI_HELPER_CFLAGS) -MMD -MP -c $< -o $@

$(PERSISTENT_CONFIG_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(PERSISTENT_CONFIG_CFLAGS) -MMD -MP -c $< -o $@

$(PERSISTENT_CONFIG_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(PERSISTENT_CONFIG_CFLAGS) -MMD -MP -c $< -o $@

$(NETWORK_CONFIG_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(NETWORK_CONFIG_CFLAGS) -MMD -MP -c $< -o $@

$(NETWORK_CONFIG_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(NETWORK_CONFIG_CFLAGS) -MMD -MP -c $< -o $@

# ITEM-507 increment 6: the two objects the fixture kernel compiles for itself.
$(SNI_SELECTION_OBJ_DIR)/kernel.o: kernel/src/kernel.c
	@mkdir -p $(@D)
	$(CC) $(SNI_SELECTION_CFLAGS) -MMD -MP -c $< -o $@

# The fixture object sees the TLS-serving mbedTLS profile: it generates the two
# pairs with the same library the appliance generates its own certificate with.
$(SNI_SELECTION_OBJ_DIR)/appliance_certificate_name_fixture_qemu.o: \
		kernel/src/appliance_certificate_name_fixture_qemu.c \
		kernel/include/hfoc_mbedtls_tls_config.h \
		kernel/include/hfoc_mbedtls_config.h \
		kernel/include/hfoc_mbedtls_platform.h
	@mkdir -p $(@D)
	$(CC) $(SNI_SELECTION_CFLAGS) $(HFOC_TLS_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

# ITEM-508 increment 15: the two objects the directory proof compiles for
# itself.  The anchor fixture is the only object that needs the fixture header
# include path; `kernel.c` is the production entry point plus the test macro
# that reaches the driver's tick.
$(ACME_DIRECTORY_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(ACME_DIRECTORY_CFLAGS) -MMD -MP -c $< -o $@

# The driver includes the binding header, so it sees the TLS-serving mbedTLS
# profile exactly like the binding it drives.
$(ACME_DIRECTORY_OBJ_DIR)/appliance_acme_directory_qemu.o: \
		kernel/src/appliance_acme_directory_qemu.c \
		kernel/include/appliance_acme_transport_lwip.h \
		kernel/include/hfoc_mbedtls_tls_config.h \
		kernel/include/hfoc_mbedtls_config.h
	@mkdir -p $(@D)
	$(CC) $(ACME_DIRECTORY_CFLAGS) $(HFOC_TLS_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

$(ACME_DIRECTORY_OBJ_DIR)/appliance_acme_directory_anchor_fixture.o: \
		kernel/src/appliance_acme_directory_anchor_fixture.c \
		kernel/include/appliance_acme_trust.h \
		tests/appliance-acme-directory-fixtures.h
	@mkdir -p $(@D)
	$(CC) $(ACME_DIRECTORY_CFLAGS) -Itests -MMD -MP -c $< -o $@

# ITEM-508 increment 17: the order family's two objects.  The driver includes
# the binding and the flow headers, so it sees the TLS-serving mbedTLS profile
# exactly like the modules it drives.
$(ACME_ORDER_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(ACME_ORDER_CFLAGS) -MMD -MP -c $< -o $@

$(ACME_ORDER_OBJ_DIR)/appliance_acme_order_qemu.o: \
		kernel/src/appliance_acme_order_qemu.c \
		kernel/include/appliance_acme_account.h \
		kernel/include/appliance_acme_flow.h \
		kernel/include/appliance_acme_jws.h \
		kernel/include/appliance_acme_renewal.h \
		kernel/include/appliance_acme_transport_lwip.h \
		kernel/include/appliance_production_certificate.h \
		kernel/include/hfoc_mbedtls_tls_config.h \
		kernel/include/hfoc_mbedtls_config.h \
		tests/appliance-acme-directory-fixtures.h
	@mkdir -p $(@D)
	$(CC) $(ACME_ORDER_CFLAGS) $(HFOC_TLS_MBEDTLS_CFLAGS) -Itests -MMD -MP -c $< -o $@

$(ACME_ORDER_OBJ_DIR)/appliance_acme_directory_anchor_fixture.o: \
		kernel/src/appliance_acme_directory_anchor_fixture.c \
		kernel/include/appliance_acme_trust.h \
		tests/appliance-acme-directory-fixtures.h
	@mkdir -p $(@D)
	$(CC) $(ACME_ORDER_CFLAGS) -Itests -MMD -MP -c $< -o $@

$(ACME_BOOT_ORDER_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(ACME_BOOT_ORDER_CFLAGS) -MMD -MP -c $< -o $@

$(ACME_BOOT_ORDER_OBJ_DIR)/appliance_acme_directory_anchor_fixture.o: \
		kernel/src/appliance_acme_directory_anchor_fixture.c \
		kernel/include/appliance_acme_trust.h \
		tests/appliance-acme-directory-fixtures.h
	@mkdir -p $(@D)
	$(CC) $(ACME_BOOT_ORDER_CFLAGS) -Itests -MMD -MP -c $< -o $@

$(ACME_BOOT_ORDER_OBJ_DIR)/appliance_acme_fixture_clock_qemu.o: \
		kernel/src/appliance_acme_fixture_clock_qemu.c \
		kernel/include/appliance_acme_fixture_clock_qemu.h
	@mkdir -p $(@D)
	$(CC) $(ACME_BOOT_ORDER_CFLAGS) -MMD -MP -c $< -o $@

# ITEM-508 increment 36: the boot composition binds the mbedTLS transport link,
# so its family object is compiled with the TLS profile flags like the
# production TLS-profile objects.
$(ACME_BOOT_ORDER_OBJ_DIR)/appliance_production_acme_order_boot.o: \
		kernel/src/appliance_production_acme_order_boot.c \
		kernel/include/appliance_production_acme_order_boot.h \
		kernel/include/appliance_production_acme_order.h \
		kernel/include/appliance_acme_transport_lwip.h \
		kernel/include/hfoc_mbedtls_tls_config.h \
		kernel/include/hfoc_mbedtls_config.h
	@mkdir -p $(@D)
	$(CC) $(ACME_BOOT_ORDER_CFLAGS) $(HFOC_TLS_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

# ITEM-508 increment 29: the wire proof's arming hook.  It includes the
# production listener header only, so it cannot reach around the module it
# arms through.
$(ACME_HTTP01_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(ACME_HTTP01_CFLAGS) -MMD -MP -c $< -o $@

$(ACME_HTTP01_OBJ_DIR)/appliance_acme_http01_fixture_qemu.o: \
		kernel/src/appliance_acme_http01_fixture_qemu.c \
		kernel/include/appliance_production_acme_http01.h \
		kernel/include/appliance_acme_challenge.h \
		kernel/include/serial.h
	@mkdir -p $(@D)
	$(CC) $(ACME_HTTP01_CFLAGS) -MMD -MP -c $< -o $@

$(PERSISTENT_NETWORK_CONFIG_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(PERSISTENT_NETWORK_CONFIG_CFLAGS) -MMD -MP -c $< -o $@

$(PERSISTENT_NETWORK_CONFIG_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(PERSISTENT_NETWORK_CONFIG_CFLAGS) -MMD -MP -c $< -o $@

$(BACKUP_RESTORE_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(BACKUP_RESTORE_CFLAGS) -MMD -MP -c $< -o $@

$(BACKUP_RESTORE_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(BACKUP_RESTORE_CFLAGS) -MMD -MP -c $< -o $@

$(SUPERVISOR_COMPOSITION_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(SUPERVISOR_COMPOSITION_CFLAGS) -MMD -MP -c $< -o $@

$(SUPERVISOR_COMPOSITION_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(SUPERVISOR_COMPOSITION_CFLAGS) -MMD -MP -c $< -o $@

$(LOCAL_CONTROL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(LOCAL_CONTROL_CFLAGS) -MMD -MP -c $< -o $@

$(LOCAL_CONTROL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(RECOVERY_EXECUTOR_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(RECOVERY_EXECUTOR_CFLAGS) -MMD -MP -c $< -o $@

$(RECOVERY_EXECUTOR_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(NETWORK_FOUNDATION_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(NETWORK_FOUNDATION_CFLAGS) -MMD -MP -c $< -o $@

$(NETWORK_FOUNDATION_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(VIRTIO_FOUNDATION_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(VIRTIO_FOUNDATION_CFLAGS) -MMD -MP -c $< -o $@

$(VIRTIO_FOUNDATION_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(VIRTIO_NET_FRAME_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(VIRTIO_NET_FRAME_CFLAGS) -MMD -MP -c $< -o $@

$(VIRTIO_NET_FRAME_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(PACKET_BOUNDARY_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(PACKET_BOUNDARY_CFLAGS) -MMD -MP -c $< -o $@

$(PACKET_BOUNDARY_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ARP_ICMP_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(ARP_ICMP_CFLAGS) -MMD -MP -c $< -o $@

$(ARP_ICMP_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ARP_ICMP_OBJ_DIR)/lwip/%.o: third_party/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(ARP_ICMP_CFLAGS) -MMD -MP -c $< -o $@

$(TCP_LISTENER_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(TCP_LISTENER_CFLAGS) -MMD -MP -c $< -o $@

$(TCP_LISTENER_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(TCP_LISTENER_OBJ_DIR)/lwip/%.o: third_party/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(TCP_LISTENER_CFLAGS) -MMD -MP -c $< -o $@

$(NETWORK_RESOURCE_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(NETWORK_RESOURCE_CFLAGS) -MMD -MP -c $< -o $@

$(NETWORK_RESOURCE_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(NETWORK_RESOURCE_OBJ_DIR)/lwip/%.o: third_party/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(NETWORK_RESOURCE_CFLAGS) -MMD -MP -c $< -o $@

$(MBEDTLS_TLS_QEMU_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(MBEDTLS_TLS_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(MBEDTLS_TLS_QEMU_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(MBEDTLS_TLS_QEMU_OBJ_DIR)/lwip/%.o: third_party/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(MBEDTLS_TLS_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(MBEDTLS_TLS_QEMU_OBJ_DIR)/mbedtls/%.o: third_party/mbedtls/library/%.c
	@mkdir -p $(@D)
	$(CC) $(MBEDTLS_TLS_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_SERVICE_QEMU_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(HTTPS_SERVICE_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_SERVICE_QEMU_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_SERVICE_QEMU_OBJ_DIR)/lwip/%.o: third_party/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(HTTPS_SERVICE_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_SERVICE_QEMU_OBJ_DIR)/mbedtls/%.o: third_party/mbedtls/library/%.c
	@mkdir -p $(@D)
	$(CC) $(HTTPS_SERVICE_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/lwip/%.o: third_party/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJ_DIR)/mbedtls/%.o: third_party/mbedtls/library/%.c
	@mkdir -p $(@D)
	$(CC) $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(HTTPS_RESTART_SOAK_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/lwip/%.o: third_party/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(HTTPS_RESTART_SOAK_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(HTTPS_RESTART_SOAK_QEMU_OBJ_DIR)/mbedtls/%.o: third_party/mbedtls/library/%.c
	@mkdir -p $(@D)
	$(CC) $(HTTPS_RESTART_SOAK_QEMU_CFLAGS) -MMD -MP -c $< -o $@

$(TRUST_POLICY_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(TRUST_POLICY_CFLAGS) -MMD -MP -c $< -o $@

$(TRUST_POLICY_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(UPDATE_LIFECYCLE_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(UPDATE_LIFECYCLE_CFLAGS) -MMD -MP -c $< -o $@

$(UPDATE_LIFECYCLE_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(SUPPORTABILITY_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(SUPPORTABILITY_CFLAGS) -MMD -MP -c $< -o $@

$(SUPPORTABILITY_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ACCOUNTING_BOUNDARY_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(ACCOUNTING_BOUNDARY_CFLAGS) -MMD -MP -c $< -o $@

$(ACCOUNTING_BOUNDARY_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(STORAGE_INTERRUPTION_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(STORAGE_INTERRUPTION_CFLAGS) -MMD -MP -c $< -o $@

$(STORAGE_INTERRUPTION_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(SERVICE_CASCADE_DRILL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(SERVICE_CASCADE_DRILL_CFLAGS) -MMD -MP -c $< -o $@

$(SERVICE_CASCADE_DRILL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(LOCAL_CONTROL_OUTAGE_DRILL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(LOCAL_CONTROL_OUTAGE_DRILL_CFLAGS) -MMD -MP -c $< -o $@

$(LOCAL_CONTROL_OUTAGE_DRILL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(NETWORK_PARTITION_DRILL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(NETWORK_PARTITION_DRILL_CFLAGS) -MMD -MP -c $< -o $@

$(NETWORK_PARTITION_DRILL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(TRUST_FAILURE_DRILL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(TRUST_FAILURE_DRILL_CFLAGS) -MMD -MP -c $< -o $@

$(TRUST_FAILURE_DRILL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(UPDATE_FAILURE_DRILL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(UPDATE_FAILURE_DRILL_CFLAGS) -MMD -MP -c $< -o $@

$(UPDATE_FAILURE_DRILL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(INTEGRATION_REPLAY_OUTAGE_DRILL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(INTEGRATION_REPLAY_OUTAGE_DRILL_CFLAGS) -MMD -MP -c $< -o $@

$(INTEGRATION_REPLAY_OUTAGE_DRILL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(POWER_CYCLE_CONTINUITY_DRILL_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(POWER_CYCLE_CONTINUITY_DRILL_CFLAGS) -MMD -MP -c $< -o $@

$(POWER_CYCLE_CONTINUITY_DRILL_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ENTROPY_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(ENTROPY_CFLAGS) -MMD -MP -c $< -o $@

$(ENTROPY_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(MBEDTLS_TEST_SECRET_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(MBEDTLS_TEST_SECRET_CFLAGS) -MMD -MP -c $< -o $@

$(MBEDTLS_TEST_SECRET_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(KERNEL): $(OBJECTS) $(PRODUCTION_LWIP_OBJS) $(HFOC_VERIFY_KERNEL_OBJS) \
		$(HFOC_STATE_CRYPTO_KERNEL_OBJS) $(HFOC_TLS_KERNEL_OBJS) \
		kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) -o $@

# ITEM-601: the QEMU-only SSH fixture kernel.  It is the production kernel
# plus HFOC_APPLIANCE_SSH_QEMU_TEST, which seeds an in-memory host key and the
# committed test-only client key so the proof can run on a machine with no
# installed state volume.  The production ELF never carries the fixture marker.
$(SSH_QEMU_OBJ): kernel/src/appliance_production_ssh.c \
		kernel/include/appliance_production_ssh.h \
		tests/fixtures/ssh/qemu-test-key.h
	@mkdir -p $(@D)
	$(CC) $(PRODUCTION_CFLAGS) $(HFOC_TLS_MBEDTLS_CFLAGS) \
		-DHFOC_APPLIANCE_SSH_QEMU_TEST -Itests/fixtures/ssh \
		-MMD -MP -c $< -o $@
$(SSH_QEMU_KERNEL): $(SSH_QEMU_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SSH_QEMU_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) -o $@

$(SSH_QEMU_ISO): deps $(SSH_QEMU_KERNEL) $(USER_ROOTFS_STAGE)
	rm -rf $(SSH_QEMU_ISO_ROOT)
	mkdir -p $(SSH_QEMU_ISO_ROOT)/boot/limine $(SSH_QEMU_ISO_ROOT)/EFI/BOOT
	cp $(SSH_QEMU_KERNEL) $(SSH_QEMU_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SSH_QEMU_ISO_ROOT)/boot/limine/limine.conf
	printf '\nmodule_path: boot():/boot/hfoc-boot.man\n' \
	   >> $(SSH_QEMU_ISO_ROOT)/boot/limine/limine.conf
	printf 'module_path: boot():/boot/limine/limine.conf\n' \
	   >> $(SSH_QEMU_ISO_ROOT)/boot/limine/limine.conf
	python3 scripts/hfoc-rootfs-pack.py $(USER_ROOTFS_STAGE) \
	   $(SSH_QEMU_ISO_ROOT)/boot/hfoc-rootfs.arc
	printf 'module_path: boot():/boot/hfoc-rootfs.arc\n' \
	   >> $(SSH_QEMU_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SSH_QEMU_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SSH_QEMU_ISO_ROOT)/EFI/BOOT/
	python3 scripts/hfoc-boot-chain-manifest.py \
		--kernel $(SSH_QEMU_ISO_ROOT)/boot/hfoc-kernel.elf \
		--config $(SSH_QEMU_ISO_ROOT)/boot/limine/limine.conf \
		--release-id 1 --signer-id 1 \
		--key boot/keys/hfoc-development-signing-key.pem \
		--output $(SSH_QEMU_ISO_ROOT)/boot/hfoc-boot.man
	bash scripts/install-license-payload.sh $(SSH_QEMU_ISO_ROOT)
	bash scripts/verify-license-payload.sh $(SSH_QEMU_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SSH_QEMU_ISO_ROOT) -o $(SSH_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(SSH_QEMU_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SSH_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(SSH_QEMU_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SSH_QEMU_ISO) > $(SSH_QEMU_ISO).sha256
	@echo "ISO готов: $(SSH_QEMU_ISO)"

# ITEM-373: the state-encryption module is the one production-kernel object
# compiled with the state crypto profile (AES-256-CTR only).
build/obj/appliance_state_crypto.o: kernel/src/appliance_state_crypto.c \
		kernel/include/hfoc_state_crypto_config.h
	@mkdir -p $(@D)
	$(CC) $(PRODUCTION_CFLAGS) $(HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

# ITEM-386: the production serving composition, the entropy owner and the
# first-boot certificate generator are the production-kernel objects compiled
# with the TLS-serving mbedTLS profile.  ITEM-503 adds the BYO certificate
# validation gate: it parses an operator's certificate and key with the same
# library.  ITEM-508 increment 13 adds the ACME link binding, which is the one
# client-role translation unit outside the listener.  They are the only kernel
# objects that see the TLS profile header, so no unrelated translation unit can
# acquire a TLS, PSA or X.509 dependency by accident.
HFOC_TLS_PROFILE_KERNEL_SOURCES := \
	kernel/src/appliance_tls_platform.c \
	kernel/src/kernel.c \
	kernel/src/console.c \
	kernel/src/appliance_production_ssh.c \
	kernel/src/appliance_ssh_identity.c \
	kernel/src/hfoc_ssh_crypto.c \
	kernel/src/hfoc_ssh_transport.c \
	kernel/src/appliance_production_entropy.c \
	kernel/src/appliance_production_certificate.c \
	kernel/src/appliance_production_serving.c \
	kernel/src/appliance_certificate_import.c \
	kernel/src/appliance_acme_trust.c \
	kernel/src/appliance_acme_jws.c \
	kernel/src/appliance_acme_flow.c \
	kernel/src/appliance_acme_transport_lwip.c \
	kernel/src/appliance_production_acme_order_boot.c \
	kernel/src/hfoc_mbedtls_bio.c \
	kernel/src/hfoc_mbedtls_drbg.c

define hfoc-tls-profile-object-rule
build/obj/$(notdir $(basename $(1))).o: $(1) \
		kernel/include/hfoc_mbedtls_tls_config.h \
		kernel/include/hfoc_mbedtls_config.h \
		kernel/include/hfoc_mbedtls_platform.h
	@mkdir -p $$(@D)
	$$(CC) $$(PRODUCTION_CFLAGS) $$(HFOC_TLS_MBEDTLS_CFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach source,$(HFOC_TLS_PROFILE_KERNEL_SOURCES),$(eval $(call hfoc-tls-profile-object-rule,$(source))))

# ITEM-372: the ECDSA backend is the one production-kernel object compiled
# with the verification profile; every other kernel object stays free of the
# mbedTLS configuration.
build/obj/image_signature_ecdsa.o: kernel/src/image_signature_ecdsa.c \
		kernel/include/hfoc_verify_config.h \
		kernel/include/hfoc_verify_symbols.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(HFOC_VERIFY_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

$(FALLBACK_KERNEL): $(FALLBACK_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(FALLBACK_OBJS) -o $@

$(ONESHOT_KERNEL): $(ONESHOT_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(ONESHOT_OBJS) -o $@

$(LAPIC_RUNTIME_KERNEL): $(LAPIC_RUNTIME_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(LAPIC_RUNTIME_OBJS) -o $@

$(IRQ3_7_KERNEL): $(IRQ3_7_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(IRQ3_7_OBJS) -o $@

$(IRQ8_15_KERNEL): $(IRQ8_15_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(IRQ8_15_OBJS) -o $@

$(X2APIC_KERNEL): $(X2APIC_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(X2APIC_OBJS) -o $@

$(VMM_AS_KERNEL): $(VMM_AS_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(VMM_AS_OBJS) -o $@

$(SCHED_YIELD_KERNEL): $(SCHED_YIELD_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SCHED_YIELD_OBJS) -o $@

$(SCHED_TICK_KERNEL): $(SCHED_TICK_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SCHED_TICK_OBJS) -o $@

$(SCHED_WAIT_KERNEL): $(SCHED_WAIT_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SCHED_WAIT_OBJS) -o $@

$(SCHED_EXIT_KERNEL): $(SCHED_EXIT_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SCHED_EXIT_OBJS) -o $@

$(SCHED_STRESS_KERNEL): $(SCHED_STRESS_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SCHED_STRESS_OBJS) -o $@

$(PROCESS_SCHED_KERNEL): $(PROCESS_SCHED_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(PROCESS_SCHED_OBJS) -o $@

$(USER_IRETQ_KERNEL): $(USER_IRETQ_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(USER_IRETQ_OBJS) -o $@

$(SYSCALL_KERNEL): $(SYSCALL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SYSCALL_OBJS) -o $@

$(USER_INIT_KERNEL): $(USER_INIT_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(USER_INIT_OBJS) -o $@

$(BOOT_POLICY_KERNEL): $(BOOT_POLICY_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(BOOT_POLICY_OBJS) -o $@

$(BLOCK_DEVICE_KERNEL): $(BLOCK_DEVICE_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(BLOCK_DEVICE_OBJS) -o $@

$(DATA_REGION_KERNEL): $(DATA_REGION_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(DATA_REGION_OBJS) -o $@

$(BLOCK_CACHE_KERNEL): $(BLOCK_CACHE_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(BLOCK_CACHE_OBJS) -o $@

$(HFOCFS_QEMU_KERNEL): $(HFOCFS_QEMU_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(HFOCFS_QEMU_OBJS) -o $@

$(HFOCFS_WORKSPACE_QEMU_KERNEL): $(HFOCFS_WORKSPACE_QEMU_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(HFOCFS_WORKSPACE_QEMU_OBJS) -o $@

$(CONFIG_JOURNAL_KERNEL): $(CONFIG_JOURNAL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(CONFIG_JOURNAL_OBJS) -o $@

$(DIAG_EXPORT_KERNEL): $(DIAG_EXPORT_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(DIAG_EXPORT_OBJS) -o $@

$(PHYSICAL_STORAGE_KERNEL): $(PHYSICAL_STORAGE_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(PHYSICAL_STORAGE_OBJS) -o $@

$(PHYSICAL_SATA_ACCEPTANCE_KERNEL): $(PHYSICAL_SATA_ACCEPTANCE_OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(PHYSICAL_SATA_ACCEPTANCE_OBJS) -o $@

# ITEM-394: the installer medium verifies its own signed installer payloads
# before it writes anything, so this profile links the same ECDSA backend the
# production kernel links.  Without this object the verifier is the weak
# default that reports UNAVAILABLE and every install would refuse.
$(PRODUCTION_INSTALLER_OBJ_DIR)/image_signature_ecdsa.o: \
		kernel/src/image_signature_ecdsa.c \
		kernel/include/hfoc_verify_config.h \
		kernel/include/hfoc_verify_symbols.h
	@mkdir -p $(@D)
	$(CC) $(PRODUCTION_INSTALLER_CFLAGS) $(HFOC_VERIFY_MBEDTLS_CFLAGS) \
		-MMD -MP -c $< -o $@

# ITEM-517: the binary loader blob the installer kernel stages onto the
# installed FAT boot volume.  The rule is installer-only: the object is a
# member of PRODUCTION_INSTALLER_OBJS, never of OBJECTS.
$(PRODUCTION_INSTALLER_LIMINE_BIOS_SYS_OBJ): deps/limine/limine-bios.sys
	@mkdir -p $(@D)
	$(LD) -r -b binary deps/limine/limine-bios.sys -o $@

$(PRODUCTION_INSTALLER_KERNEL): $(PRODUCTION_INSTALLER_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) kernel/linker.ld Makefile
	$(LD) $(LDFLAGS) $(PRODUCTION_INSTALLER_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) -o $@

$(BOOT_CHAIN_OBJ_DIR)/%.o: kernel/src/%.c
	@mkdir -p $(@D)
	$(CC) $(BOOT_CHAIN_CFLAGS) -MMD -MP -c $< -o $@

$(BOOT_CHAIN_OBJ_DIR)/%.o: kernel/src/%.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BOOT_CHAIN_OBJ_DIR)/mbedtls/%.o: third_party/mbedtls/library/%.c \
		kernel/include/hfoc_verify_config.h \
		kernel/include/hfoc_verify_symbols.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(HFOC_VERIFY_MBEDTLS_CFLAGS) -MMD -MP -c $< -o $@

$(BOOT_CHAIN_VECTOR_OBJ): tests/boot-chain-vector.c \
		tests/boot-chain-vector.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Itests -c $< -o $@

$(BOOT_CHAIN_KERNEL): $(BOOT_CHAIN_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(BOOT_CHAIN_OBJS) -o $@

$(HFOC_EFI_HELPER): $(HFOC_EFI_HELPER_OBJS) \
		boot/efi/hfoc_efi_installer_uefi.lds
	@mkdir -p $(@D)
	$(LD) -m i386pep --subsystem 10 --image-base 0 \
		--entry efi_main --enable-reloc-section \
		--disable-high-entropy-va --disable-dynamicbase \
		--disable-nxcompat --file-alignment 0x1000 \
		--section-alignment 0x1000 \
		-T boot/efi/hfoc_efi_installer_uefi.lds \
		$(HFOC_EFI_HELPER_OBJS) -o $@

$(PERSISTENT_CONFIG_KERNEL): $(PERSISTENT_CONFIG_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(PERSISTENT_CONFIG_OBJS) -o $@

$(NETWORK_CONFIG_KERNEL): $(NETWORK_CONFIG_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(NETWORK_CONFIG_OBJS) -o $@

# ITEM-507 increment 6: the fixture kernel is the production kernel plus the one
# seeding object, so it links the production libraries it shares with it.
$(SNI_SELECTION_KERNEL): $(SNI_SELECTION_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SNI_SELECTION_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) -o $@

# ITEM-508 increment 15: the directory proof kernel, linked exactly like the
# production kernel apart from the two fixture objects.
$(ACME_DIRECTORY_KERNEL): $(ACME_DIRECTORY_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(ACME_DIRECTORY_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) -o $@

# ITEM-508 increment 17: the order proof kernel, linked exactly like the
# production kernel apart from the two fixture objects.
$(ACME_ORDER_KERNEL): $(ACME_ORDER_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(ACME_ORDER_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) -o $@

# ITEM-508 increment 36: the wire proof kernel, linked exactly like the
# production kernel apart from the fixture anchor and the shared fixture clock.
$(ACME_BOOT_ORDER_KERNEL): $(ACME_BOOT_ORDER_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(ACME_BOOT_ORDER_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) -o $@

# ITEM-508 increment 29: the wire proof kernel, linked exactly like the
# production kernel apart from the one arming hook.
$(ACME_HTTP01_KERNEL): $(ACME_HTTP01_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(ACME_HTTP01_OBJECTS) $(PRODUCTION_LWIP_OBJS) \
		$(HFOC_VERIFY_KERNEL_OBJS) $(HFOC_STATE_CRYPTO_KERNEL_OBJS) \
		$(HFOC_TLS_KERNEL_OBJS) -o $@

$(PERSISTENT_NETWORK_CONFIG_KERNEL): $(PERSISTENT_NETWORK_CONFIG_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(PERSISTENT_NETWORK_CONFIG_OBJS) -o $@

$(BACKUP_RESTORE_KERNEL): $(BACKUP_RESTORE_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(BACKUP_RESTORE_OBJS) -o $@

$(SUPERVISOR_COMPOSITION_KERNEL): $(SUPERVISOR_COMPOSITION_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SUPERVISOR_COMPOSITION_OBJS) -o $@

$(LOCAL_CONTROL_KERNEL): $(LOCAL_CONTROL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(LOCAL_CONTROL_OBJS) -o $@

$(RECOVERY_EXECUTOR_KERNEL): $(RECOVERY_EXECUTOR_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(RECOVERY_EXECUTOR_OBJS) -o $@

$(NETWORK_FOUNDATION_KERNEL): $(NETWORK_FOUNDATION_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(NETWORK_FOUNDATION_OBJS) -o $@

$(VIRTIO_FOUNDATION_KERNEL): $(VIRTIO_FOUNDATION_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(VIRTIO_FOUNDATION_OBJS) -o $@

$(VIRTIO_NET_FRAME_KERNEL): $(VIRTIO_NET_FRAME_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(VIRTIO_NET_FRAME_OBJS) -o $@

$(PACKET_BOUNDARY_KERNEL): $(PACKET_BOUNDARY_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(PACKET_BOUNDARY_OBJS) -o $@

$(ARP_ICMP_KERNEL): $(ARP_ICMP_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(ARP_ICMP_OBJS) -o $@

$(TCP_LISTENER_KERNEL): $(TCP_LISTENER_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(TCP_LISTENER_OBJS) -o $@

$(NETWORK_RESOURCE_KERNEL): $(NETWORK_RESOURCE_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(NETWORK_RESOURCE_OBJS) -o $@

$(TRUST_POLICY_KERNEL): $(TRUST_POLICY_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(TRUST_POLICY_OBJS) -o $@

$(UPDATE_LIFECYCLE_KERNEL): $(UPDATE_LIFECYCLE_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(UPDATE_LIFECYCLE_OBJS) -o $@

$(SUPPORTABILITY_KERNEL): $(SUPPORTABILITY_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SUPPORTABILITY_OBJS) -o $@

$(ACCOUNTING_BOUNDARY_KERNEL): $(ACCOUNTING_BOUNDARY_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(ACCOUNTING_BOUNDARY_OBJS) -o $@

$(STORAGE_INTERRUPTION_KERNEL): $(STORAGE_INTERRUPTION_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(STORAGE_INTERRUPTION_OBJS) -o $@

$(SERVICE_CASCADE_DRILL_KERNEL): $(SERVICE_CASCADE_DRILL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(SERVICE_CASCADE_DRILL_OBJS) -o $@

$(LOCAL_CONTROL_OUTAGE_DRILL_KERNEL): $(LOCAL_CONTROL_OUTAGE_DRILL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(LOCAL_CONTROL_OUTAGE_DRILL_OBJS) -o $@

$(NETWORK_PARTITION_DRILL_KERNEL): $(NETWORK_PARTITION_DRILL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(NETWORK_PARTITION_DRILL_OBJS) -o $@

$(TRUST_FAILURE_DRILL_KERNEL): $(TRUST_FAILURE_DRILL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(TRUST_FAILURE_DRILL_OBJS) -o $@

$(UPDATE_FAILURE_DRILL_KERNEL): $(UPDATE_FAILURE_DRILL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(UPDATE_FAILURE_DRILL_OBJS) -o $@

$(INTEGRATION_REPLAY_OUTAGE_DRILL_KERNEL): $(INTEGRATION_REPLAY_OUTAGE_DRILL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(INTEGRATION_REPLAY_OUTAGE_DRILL_OBJS) -o $@

$(POWER_CYCLE_CONTINUITY_DRILL_KERNEL): $(POWER_CYCLE_CONTINUITY_DRILL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(POWER_CYCLE_CONTINUITY_DRILL_OBJS) -o $@

$(ENTROPY_KERNEL): $(ENTROPY_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(ENTROPY_OBJS) -o $@

$(MBEDTLS_TEST_SECRET_KERNEL): $(MBEDTLS_TEST_SECRET_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(MBEDTLS_TEST_SECRET_OBJS) -o $@

$(MBEDTLS_TLS_QEMU_KERNEL): $(MBEDTLS_TLS_QEMU_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(MBEDTLS_TLS_QEMU_OBJS) -o $@

$(HTTPS_SERVICE_QEMU_KERNEL): $(HTTPS_SERVICE_QEMU_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(HTTPS_SERVICE_QEMU_OBJS) -o $@

$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_KERNEL): $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_OBJS) -o $@

$(HTTPS_RESTART_SOAK_QEMU_KERNEL): $(HTTPS_RESTART_SOAK_QEMU_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(HTTPS_RESTART_SOAK_QEMU_OBJS) -o $@

kernel: build-input-manifest $(KERNEL)

build-input-manifest:
	mkdir -p build
	HFOC_CC='$(CC)' HFOC_LD='$(LD)' \
		HFOC_CFLAGS='$(CFLAGS)' HFOC_ASFLAGS='$(ASFLAGS)' \
		HFOC_INPUT_FILES='$(BUILD_INPUT_FILES)' \
		bash scripts/build-input-manifest.sh $(BUILD_INPUT_MANIFEST)
	HFOC_CC='$(CC)' HFOC_LD='$(LD)' \
		HFOC_CFLAGS='$(CFLAGS)' HFOC_ASFLAGS='$(ASFLAGS)' \
		HFOC_INPUT_FILES='$(BUILD_INPUT_FILES)' \
		bash scripts/verify-build-input-manifest.sh $(BUILD_INPUT_MANIFEST)

deps:
	./scripts/fetch-limine.sh

iso: deps kernel $(USER_ROOTFS_STAGE)
	rm -rf build/iso_root
	mkdir -p build/iso_root/boot/limine build/iso_root/EFI/BOOT
	cp $(KERNEL) build/iso_root/boot/hfoc-kernel.elf
	cp boot/limine.conf build/iso_root/boot/limine/limine.conf
	printf '\nmodule_path: boot():/boot/hfoc-boot.man\n' \
	   >> build/iso_root/boot/limine/limine.conf
	printf 'module_path: boot():/boot/limine/limine.conf\n' \
	   >> build/iso_root/boot/limine/limine.conf
	python3 scripts/hfoc-rootfs-pack.py $(USER_ROOTFS_STAGE) \
		build/iso_root/boot/hfoc-rootfs.arc
	printf 'module_path: boot():/boot/hfoc-rootfs.arc\n' \
	   >> build/iso_root/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin build/iso_root/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI build/iso_root/EFI/BOOT/
	python3 scripts/hfoc-boot-chain-manifest.py \
		--kernel build/iso_root/boot/hfoc-kernel.elf \
		--config build/iso_root/boot/limine/limine.conf \
		--release-id 1 --signer-id 1 \
		--key boot/keys/hfoc-development-signing-key.pem \
		--output build/iso_root/boot/hfoc-boot.man
	bash scripts/install-license-payload.sh build/iso_root
	bash scripts/verify-license-payload.sh build/iso_root/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		build/iso_root -o $(IMAGE)
	python3 scripts/normalize-iso-gpt.py $(IMAGE) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(IMAGE)
	python3 scripts/normalize-iso-gpt.py $(IMAGE) "$(ISO_GPT_SEED)"
	sha256sum $(IMAGE) > $(IMAGE).sha256
	@echo "ISO готов: $(IMAGE)"

$(FALLBACK_ISO): deps $(FALLBACK_KERNEL)
	rm -rf $(FALLBACK_ISO_ROOT)
	mkdir -p $(FALLBACK_ISO_ROOT)/boot/limine $(FALLBACK_ISO_ROOT)/EFI/BOOT
	cp $(FALLBACK_KERNEL) $(FALLBACK_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(FALLBACK_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(FALLBACK_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(FALLBACK_ISO_ROOT)/EFI/BOOT/
	bash scripts/install-license-payload.sh $(FALLBACK_ISO_ROOT)
	bash scripts/verify-license-payload.sh $(FALLBACK_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(FALLBACK_ISO_ROOT) -o $(FALLBACK_ISO)
	python3 scripts/normalize-iso-gpt.py $(FALLBACK_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(FALLBACK_ISO)
	python3 scripts/normalize-iso-gpt.py $(FALLBACK_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(FALLBACK_ISO) > $(FALLBACK_ISO).sha256
	@echo "ISO готов: $(FALLBACK_ISO)"

.PHONY: release-license-payload
release-license-payload:
	bash scripts/install-license-payload.sh build/release
	bash scripts/verify-license-payload.sh build/release/licenses

$(ONESHOT_ISO): deps $(ONESHOT_KERNEL)
	rm -rf $(ONESHOT_ISO_ROOT)
	mkdir -p $(ONESHOT_ISO_ROOT)/boot/limine $(ONESHOT_ISO_ROOT)/EFI/BOOT
	cp $(ONESHOT_KERNEL) $(ONESHOT_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(ONESHOT_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(ONESHOT_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ONESHOT_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ONESHOT_ISO_ROOT) -o $(ONESHOT_ISO)
	python3 scripts/normalize-iso-gpt.py $(ONESHOT_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(ONESHOT_ISO)
	python3 scripts/normalize-iso-gpt.py $(ONESHOT_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(ONESHOT_ISO) > $(ONESHOT_ISO).sha256
	@echo "ISO готов: $(ONESHOT_ISO)"

$(LAPIC_RUNTIME_ISO): deps $(LAPIC_RUNTIME_KERNEL)
	rm -rf $(LAPIC_RUNTIME_ISO_ROOT)
	mkdir -p $(LAPIC_RUNTIME_ISO_ROOT)/boot/limine $(LAPIC_RUNTIME_ISO_ROOT)/EFI/BOOT
	cp $(LAPIC_RUNTIME_KERNEL) $(LAPIC_RUNTIME_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(LAPIC_RUNTIME_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(LAPIC_RUNTIME_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(LAPIC_RUNTIME_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(LAPIC_RUNTIME_ISO_ROOT) -o $(LAPIC_RUNTIME_ISO)
	python3 scripts/normalize-iso-gpt.py $(LAPIC_RUNTIME_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(LAPIC_RUNTIME_ISO)
	python3 scripts/normalize-iso-gpt.py $(LAPIC_RUNTIME_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(LAPIC_RUNTIME_ISO) > $(LAPIC_RUNTIME_ISO).sha256
	@echo "ISO готов: $(LAPIC_RUNTIME_ISO)"

$(IRQ3_7_ISO): deps $(IRQ3_7_KERNEL)
	rm -rf $(IRQ3_7_ISO_ROOT)
	mkdir -p $(IRQ3_7_ISO_ROOT)/boot/limine $(IRQ3_7_ISO_ROOT)/EFI/BOOT
	cp $(IRQ3_7_KERNEL) $(IRQ3_7_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(IRQ3_7_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(IRQ3_7_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(IRQ3_7_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(IRQ3_7_ISO_ROOT) -o $(IRQ3_7_ISO)
	python3 scripts/normalize-iso-gpt.py $(IRQ3_7_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(IRQ3_7_ISO)
	python3 scripts/normalize-iso-gpt.py $(IRQ3_7_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(IRQ3_7_ISO) > $(IRQ3_7_ISO).sha256
	@echo "ISO готов: $(IRQ3_7_ISO)"

$(IRQ8_15_ISO): deps $(IRQ8_15_KERNEL)
	rm -rf $(IRQ8_15_ISO_ROOT)
	mkdir -p $(IRQ8_15_ISO_ROOT)/boot/limine $(IRQ8_15_ISO_ROOT)/EFI/BOOT
	cp $(IRQ8_15_KERNEL) $(IRQ8_15_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(IRQ8_15_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(IRQ8_15_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(IRQ8_15_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(IRQ8_15_ISO_ROOT) -o $(IRQ8_15_ISO)
	python3 scripts/normalize-iso-gpt.py $(IRQ8_15_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(IRQ8_15_ISO)
	python3 scripts/normalize-iso-gpt.py $(IRQ8_15_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(IRQ8_15_ISO) > $(IRQ8_15_ISO).sha256
	@echo "ISO готов: $(IRQ8_15_ISO)"

$(X2APIC_ISO): deps $(X2APIC_KERNEL)
	rm -rf $(X2APIC_ISO_ROOT)
	mkdir -p $(X2APIC_ISO_ROOT)/boot/limine $(X2APIC_ISO_ROOT)/EFI/BOOT
	cp $(X2APIC_KERNEL) $(X2APIC_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(X2APIC_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(X2APIC_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(X2APIC_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(X2APIC_ISO_ROOT) -o $(X2APIC_ISO)
	python3 scripts/normalize-iso-gpt.py $(X2APIC_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(X2APIC_ISO)
	python3 scripts/normalize-iso-gpt.py $(X2APIC_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(X2APIC_ISO) > $(X2APIC_ISO).sha256
	@echo "ISO готов: $(X2APIC_ISO)"

$(VMM_AS_ISO): deps $(VMM_AS_KERNEL)
	rm -rf $(VMM_AS_ISO_ROOT)
	mkdir -p $(VMM_AS_ISO_ROOT)/boot/limine $(VMM_AS_ISO_ROOT)/EFI/BOOT
	cp $(VMM_AS_KERNEL) $(VMM_AS_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(VMM_AS_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(VMM_AS_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(VMM_AS_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(VMM_AS_ISO_ROOT) -o $(VMM_AS_ISO)
	python3 scripts/normalize-iso-gpt.py $(VMM_AS_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(VMM_AS_ISO)
	python3 scripts/normalize-iso-gpt.py $(VMM_AS_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(VMM_AS_ISO) > $(VMM_AS_ISO).sha256
	@echo "ISO готов: $(VMM_AS_ISO)"

$(SCHED_YIELD_ISO): deps $(SCHED_YIELD_KERNEL)
	rm -rf $(SCHED_YIELD_ISO_ROOT)
	mkdir -p $(SCHED_YIELD_ISO_ROOT)/boot/limine $(SCHED_YIELD_ISO_ROOT)/EFI/BOOT
	cp $(SCHED_YIELD_KERNEL) $(SCHED_YIELD_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SCHED_YIELD_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SCHED_YIELD_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SCHED_YIELD_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SCHED_YIELD_ISO_ROOT) -o $(SCHED_YIELD_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_YIELD_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SCHED_YIELD_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_YIELD_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SCHED_YIELD_ISO) > $(SCHED_YIELD_ISO).sha256
	@echo "ISO готов: $(SCHED_YIELD_ISO)"

$(SCHED_TICK_ISO): deps $(SCHED_TICK_KERNEL)
	rm -rf $(SCHED_TICK_ISO_ROOT)
	mkdir -p $(SCHED_TICK_ISO_ROOT)/boot/limine $(SCHED_TICK_ISO_ROOT)/EFI/BOOT
	cp $(SCHED_TICK_KERNEL) $(SCHED_TICK_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SCHED_TICK_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SCHED_TICK_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SCHED_TICK_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SCHED_TICK_ISO_ROOT) -o $(SCHED_TICK_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_TICK_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SCHED_TICK_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_TICK_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SCHED_TICK_ISO) > $(SCHED_TICK_ISO).sha256
	@echo "ISO готов: $(SCHED_TICK_ISO)"

$(SCHED_WAIT_ISO): deps $(SCHED_WAIT_KERNEL)
	rm -rf $(SCHED_WAIT_ISO_ROOT)
	mkdir -p $(SCHED_WAIT_ISO_ROOT)/boot/limine $(SCHED_WAIT_ISO_ROOT)/EFI/BOOT
	cp $(SCHED_WAIT_KERNEL) $(SCHED_WAIT_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SCHED_WAIT_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SCHED_WAIT_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SCHED_WAIT_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SCHED_WAIT_ISO_ROOT) -o $(SCHED_WAIT_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_WAIT_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SCHED_WAIT_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_WAIT_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SCHED_WAIT_ISO) > $(SCHED_WAIT_ISO).sha256
	@echo "ISO готов: $(SCHED_WAIT_ISO)"

$(SCHED_EXIT_ISO): deps $(SCHED_EXIT_KERNEL)
	rm -rf $(SCHED_EXIT_ISO_ROOT)
	mkdir -p $(SCHED_EXIT_ISO_ROOT)/boot/limine $(SCHED_EXIT_ISO_ROOT)/EFI/BOOT
	cp $(SCHED_EXIT_KERNEL) $(SCHED_EXIT_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SCHED_EXIT_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SCHED_EXIT_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SCHED_EXIT_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SCHED_EXIT_ISO_ROOT) -o $(SCHED_EXIT_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_EXIT_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SCHED_EXIT_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_EXIT_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SCHED_EXIT_ISO) > $(SCHED_EXIT_ISO).sha256
	@echo "ISO готов: $(SCHED_EXIT_ISO)"

$(SCHED_STRESS_ISO): deps $(SCHED_STRESS_KERNEL)
	rm -rf $(SCHED_STRESS_ISO_ROOT)
	mkdir -p $(SCHED_STRESS_ISO_ROOT)/boot/limine $(SCHED_STRESS_ISO_ROOT)/EFI/BOOT
	cp $(SCHED_STRESS_KERNEL) $(SCHED_STRESS_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SCHED_STRESS_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SCHED_STRESS_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SCHED_STRESS_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SCHED_STRESS_ISO_ROOT) -o $(SCHED_STRESS_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_STRESS_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SCHED_STRESS_ISO)
	python3 scripts/normalize-iso-gpt.py $(SCHED_STRESS_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SCHED_STRESS_ISO) > $(SCHED_STRESS_ISO).sha256
	@echo "ISO готов: $(SCHED_STRESS_ISO)"

$(PROCESS_SCHED_ISO): deps $(PROCESS_SCHED_KERNEL)
	rm -rf $(PROCESS_SCHED_ISO_ROOT)
	mkdir -p $(PROCESS_SCHED_ISO_ROOT)/boot/limine $(PROCESS_SCHED_ISO_ROOT)/EFI/BOOT
	cp $(PROCESS_SCHED_KERNEL) $(PROCESS_SCHED_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(PROCESS_SCHED_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(PROCESS_SCHED_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(PROCESS_SCHED_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PROCESS_SCHED_ISO_ROOT) -o $(PROCESS_SCHED_ISO)
	python3 scripts/normalize-iso-gpt.py $(PROCESS_SCHED_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PROCESS_SCHED_ISO)
	python3 scripts/normalize-iso-gpt.py $(PROCESS_SCHED_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(PROCESS_SCHED_ISO) > $(PROCESS_SCHED_ISO).sha256
	@echo "ISO готов: $(PROCESS_SCHED_ISO)"

$(USER_IRETQ_ISO): deps $(USER_IRETQ_KERNEL)
	rm -rf $(USER_IRETQ_ISO_ROOT)
	mkdir -p $(USER_IRETQ_ISO_ROOT)/boot/limine $(USER_IRETQ_ISO_ROOT)/EFI/BOOT
	cp $(USER_IRETQ_KERNEL) $(USER_IRETQ_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(USER_IRETQ_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(USER_IRETQ_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(USER_IRETQ_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(USER_IRETQ_ISO_ROOT) -o $(USER_IRETQ_ISO)
	python3 scripts/normalize-iso-gpt.py $(USER_IRETQ_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(USER_IRETQ_ISO)
	python3 scripts/normalize-iso-gpt.py $(USER_IRETQ_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(USER_IRETQ_ISO) > $(USER_IRETQ_ISO).sha256
	@echo "ISO готов: $(USER_IRETQ_ISO)"

$(SYSCALL_ISO): deps $(SYSCALL_KERNEL)
	rm -rf $(SYSCALL_ISO_ROOT)
	mkdir -p $(SYSCALL_ISO_ROOT)/boot/limine $(SYSCALL_ISO_ROOT)/EFI/BOOT
	cp $(SYSCALL_KERNEL) $(SYSCALL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SYSCALL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SYSCALL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SYSCALL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SYSCALL_ISO_ROOT) -o $(SYSCALL_ISO)
	python3 scripts/normalize-iso-gpt.py $(SYSCALL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SYSCALL_ISO)
	python3 scripts/normalize-iso-gpt.py $(SYSCALL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SYSCALL_ISO) > $(SYSCALL_ISO).sha256
	@echo "ISO готов: $(SYSCALL_ISO)"

$(USER_EXEC_HELLO_OBJ): tests/user-elf-hello.S
	mkdir -p $(dir $@)
	$(CC) -c -nostdlib -fno-pic -fno-pie -m64 $< -o $@

$(USER_EXEC_HELLO): $(USER_EXEC_HELLO_OBJ) scripts/hfoc-user-elf-wrap.py
	objcopy -O binary -j .text $< $(USER_EXEC_HELLO_BIN)
	python3 scripts/hfoc-user-elf-wrap.py $(USER_EXEC_HELLO_BIN) $@ 0x400000

$(USER_EXEC_CRASH_OBJ): tests/user-elf-crash.S
	mkdir -p $(dir $@)
	$(CC) -c -nostdlib -fno-pic -fno-pie -m64 $< -o $@

$(USER_EXEC_CRASH): $(USER_EXEC_CRASH_OBJ) scripts/hfoc-user-elf-wrap.py
	objcopy -O binary -j .text $< $(USER_EXEC_CRASH_BIN)
	python3 scripts/hfoc-user-elf-wrap.py $(USER_EXEC_CRASH_BIN) $@ 0x400000

$(USER_EXEC_EXEC_OBJ): tests/user-elf-exec.S
	mkdir -p $(dir $@)
	$(CC) -c -nostdlib -fno-pic -fno-pie -m64 $< -o $@

$(USER_EXEC_EXEC): $(USER_EXEC_EXEC_OBJ) scripts/hfoc-user-elf-wrap.py
	objcopy -O binary -j .text $< $(USER_EXEC_EXEC_BIN)
	python3 scripts/hfoc-user-elf-wrap.py $(USER_EXEC_EXEC_BIN) $@ 0x400000

$(USER_EXEC_MISSING_OBJ): tests/user-elf-exec-missing.S
	mkdir -p $(dir $@)
	$(CC) -c -nostdlib -fno-pic -fno-pie -m64 $< -o $@

$(USER_EXEC_MISSING): $(USER_EXEC_MISSING_OBJ) scripts/hfoc-user-elf-wrap.py
	objcopy -O binary -j .text $< $(USER_EXEC_MISSING_BIN)
	python3 scripts/hfoc-user-elf-wrap.py $(USER_EXEC_MISSING_BIN) $@ 0x400000

$(USER_EXEC_SPIN_OBJ): tests/user-elf-spin.S
	mkdir -p $(dir $@)
	$(CC) -c -nostdlib -fno-pic -fno-pie -m64 $< -o $@

$(USER_EXEC_SPIN): $(USER_EXEC_SPIN_OBJ) scripts/hfoc-user-elf-wrap.py
	objcopy -O binary -j .text $< $(USER_EXEC_SPIN_BIN)
	python3 scripts/hfoc-user-elf-wrap.py $(USER_EXEC_SPIN_BIN) $@ 0x400000

build/user/lib/%.o: user/libhfoc/%.c
	@mkdir -p $(@D)
	$(CC) $(USER_PROGRAM_CFLAGS) -MMD -MP -c $< -o $@

build/user/lib/crt0.o: user/crt0.S
	@mkdir -p $(@D)
	$(CC) $(USER_PROGRAM_CFLAGS) -c $< -o $@

build/user/bin/%.o: user/bin/%.c
	@mkdir -p $(@D)
	$(CC) $(USER_PROGRAM_CFLAGS) -MMD -MP -c $< -o $@

$(USER_PROGRAM_ELFS): build/user/bin/%.elf: build/user/bin/%.o \
		$(USER_LIB_OBJECTS) $(USER_CRT_OBJECT) user/linker.ld
	$(LD) -nostdlib -static -s -no-pie -z max-page-size=0x1000 \
		--build-id=none -T user/linker.ld -o $@ \
		$(USER_CRT_OBJECT) $< $(USER_LIB_OBJECTS)

$(USER_TEST_PIPETEST): build/user/bin/pipetest.o \
		$(USER_LIB_OBJECTS) $(USER_CRT_OBJECT) user/linker.ld
	$(LD) -nostdlib -static -s -no-pie -z max-page-size=0x1000 \
		--build-id=none -T user/linker.ld -o $@ \
		$(USER_CRT_OBJECT) $< $(USER_LIB_OBJECTS)

$(USER_ROOTFS_STAGE): $(USER_PROGRAM_ELFS) $(ROOTFS_INPUTS)
	rm -rf $@
	cp -R rootfs $@
	mkdir -p $@/bin
	for program in $(USER_PROGRAM_NAMES); do \
		cp build/user/bin/$$program.elf $@/bin/$$program; \
	done

$(USER_INIT_ISO): deps $(USER_INIT_KERNEL) $(USER_EXEC_HELLO) $(USER_EXEC_CRASH) \
		$(USER_EXEC_EXEC) $(USER_EXEC_MISSING) $(USER_EXEC_SPIN) \
		$(USER_PROGRAM_ELFS) $(USER_TEST_PIPETEST) $(ROOTFS_INPUTS)
	rm -rf $(USER_INIT_ISO_ROOT)
	mkdir -p $(USER_INIT_ISO_ROOT)/boot/limine $(USER_INIT_ISO_ROOT)/EFI/BOOT
	cp $(USER_INIT_KERNEL) $(USER_INIT_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(USER_INIT_ISO_ROOT)/boot/limine/limine.conf
	rm -rf $(USER_EXEC_ROOTFS)
	mkdir -p $(USER_EXEC_ROOTFS)/bin
	cp -R rootfs/etc $(USER_EXEC_ROOTFS)/
	for program in $(USER_PROGRAM_NAMES); do \
		if [ "$$program" = sleep ]; then continue; fi; \
		cp build/user/bin/$$program.elf $(USER_EXEC_ROOTFS)/bin/$$program; \
	done
	python3 -c "open('$(USER_EXEC_ROOTFS)/etc/big','wb').write(b'x' * 65536)"
	cp tests/fixtures/sh-test.sh $(USER_EXEC_ROOTFS)/etc/sh-test.sh
	cp tests/fixtures/sh-632-red.sh $(USER_EXEC_ROOTFS)/etc/sh-632-red.sh
	cp $(USER_TEST_PIPETEST) $(USER_EXEC_ROOTFS)/bin/pipetest
	cp $(USER_EXEC_HELLO) $(USER_EXEC_ROOTFS)/bin/hello
	cp $(USER_EXEC_CRASH) $(USER_EXEC_ROOTFS)/bin/crash
	cp $(USER_EXEC_EXEC) $(USER_EXEC_ROOTFS)/bin/exec-ok
	cp $(USER_EXEC_MISSING) $(USER_EXEC_ROOTFS)/bin/exec-missing
	cp $(USER_EXEC_SPIN) $(USER_EXEC_ROOTFS)/bin/spin
	python3 scripts/hfoc-rootfs-pack.py $(USER_EXEC_ROOTFS) \
		$(USER_INIT_ISO_ROOT)/boot/hfoc-rootfs.arc
	printf 'module_path: boot():/boot/hfoc-rootfs.arc\n' \
		>> $(USER_INIT_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(USER_INIT_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(USER_INIT_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(USER_INIT_ISO_ROOT) -o $(USER_INIT_ISO)
	python3 scripts/normalize-iso-gpt.py $(USER_INIT_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(USER_INIT_ISO)
	python3 scripts/normalize-iso-gpt.py $(USER_INIT_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(USER_INIT_ISO) > $(USER_INIT_ISO).sha256
	@echo "ISO готов: $(USER_INIT_ISO)"

$(BOOT_POLICY_ISO): deps $(BOOT_POLICY_KERNEL)
	rm -rf $(BOOT_POLICY_ISO_ROOT)
	mkdir -p $(BOOT_POLICY_ISO_ROOT)/boot/limine $(BOOT_POLICY_ISO_ROOT)/EFI/BOOT
	cp $(BOOT_POLICY_KERNEL) $(BOOT_POLICY_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(BOOT_POLICY_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(BOOT_POLICY_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(BOOT_POLICY_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BOOT_POLICY_ISO_ROOT) -o $(BOOT_POLICY_ISO)
	python3 scripts/normalize-iso-gpt.py $(BOOT_POLICY_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(BOOT_POLICY_ISO)
	python3 scripts/normalize-iso-gpt.py $(BOOT_POLICY_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(BOOT_POLICY_ISO) > $(BOOT_POLICY_ISO).sha256
	@echo "ISO готов: $(BOOT_POLICY_ISO)"

$(BLOCK_DEVICE_ISO): deps $(BLOCK_DEVICE_KERNEL)
	rm -rf $(BLOCK_DEVICE_ISO_ROOT)
	mkdir -p $(BLOCK_DEVICE_ISO_ROOT)/boot/limine $(BLOCK_DEVICE_ISO_ROOT)/EFI/BOOT
	cp $(BLOCK_DEVICE_KERNEL) $(BLOCK_DEVICE_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(BLOCK_DEVICE_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(BLOCK_DEVICE_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(BLOCK_DEVICE_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BLOCK_DEVICE_ISO_ROOT) -o $(BLOCK_DEVICE_ISO)
	python3 scripts/normalize-iso-gpt.py $(BLOCK_DEVICE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(BLOCK_DEVICE_ISO)
	python3 scripts/normalize-iso-gpt.py $(BLOCK_DEVICE_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(BLOCK_DEVICE_ISO) > $(BLOCK_DEVICE_ISO).sha256
	@echo "ISO готов: $(BLOCK_DEVICE_ISO)"

$(DATA_REGION_ISO): deps $(DATA_REGION_KERNEL)
	rm -rf $(DATA_REGION_ISO_ROOT)
	mkdir -p $(DATA_REGION_ISO_ROOT)/boot/limine $(DATA_REGION_ISO_ROOT)/EFI/BOOT
	cp $(DATA_REGION_KERNEL) $(DATA_REGION_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(DATA_REGION_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(DATA_REGION_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(DATA_REGION_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(DATA_REGION_ISO_ROOT) -o $(DATA_REGION_ISO)
	python3 scripts/normalize-iso-gpt.py $(DATA_REGION_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(DATA_REGION_ISO)
	python3 scripts/normalize-iso-gpt.py $(DATA_REGION_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(DATA_REGION_ISO) > $(DATA_REGION_ISO).sha256
	@echo "ISO готов: $(DATA_REGION_ISO)"

$(BLOCK_CACHE_ISO): deps $(BLOCK_CACHE_KERNEL)
	rm -rf $(BLOCK_CACHE_ISO_ROOT)
	mkdir -p $(BLOCK_CACHE_ISO_ROOT)/boot/limine $(BLOCK_CACHE_ISO_ROOT)/EFI/BOOT
	cp $(BLOCK_CACHE_KERNEL) $(BLOCK_CACHE_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(BLOCK_CACHE_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(BLOCK_CACHE_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(BLOCK_CACHE_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BLOCK_CACHE_ISO_ROOT) -o $(BLOCK_CACHE_ISO)
	python3 scripts/normalize-iso-gpt.py $(BLOCK_CACHE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(BLOCK_CACHE_ISO)
	python3 scripts/normalize-iso-gpt.py $(BLOCK_CACHE_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(BLOCK_CACHE_ISO) > $(BLOCK_CACHE_ISO).sha256
	@echo "ISO готов: $(BLOCK_CACHE_ISO)"

$(HFOCFS_QEMU_ISO): deps $(HFOCFS_QEMU_KERNEL)
	rm -rf $(HFOCFS_QEMU_ISO_ROOT)
	mkdir -p $(HFOCFS_QEMU_ISO_ROOT)/boot/limine $(HFOCFS_QEMU_ISO_ROOT)/EFI/BOOT
	cp $(HFOCFS_QEMU_KERNEL) $(HFOCFS_QEMU_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(HFOCFS_QEMU_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(HFOCFS_QEMU_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(HFOCFS_QEMU_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(HFOCFS_QEMU_ISO_ROOT) -o $(HFOCFS_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HFOCFS_QEMU_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(HFOCFS_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HFOCFS_QEMU_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(HFOCFS_QEMU_ISO) > $(HFOCFS_QEMU_ISO).sha256
	@echo "ISO готов: $(HFOCFS_QEMU_ISO)"

$(HFOCFS_WORKSPACE_QEMU_ISO): deps $(HFOCFS_WORKSPACE_QEMU_KERNEL)
	rm -rf $(HFOCFS_WORKSPACE_QEMU_ISO_ROOT)
	mkdir -p $(HFOCFS_WORKSPACE_QEMU_ISO_ROOT)/boot/limine $(HFOCFS_WORKSPACE_QEMU_ISO_ROOT)/EFI/BOOT
	cp $(HFOCFS_WORKSPACE_QEMU_KERNEL) $(HFOCFS_WORKSPACE_QEMU_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(HFOCFS_WORKSPACE_QEMU_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(HFOCFS_WORKSPACE_QEMU_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(HFOCFS_WORKSPACE_QEMU_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(HFOCFS_WORKSPACE_QEMU_ISO_ROOT) -o $(HFOCFS_WORKSPACE_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HFOCFS_WORKSPACE_QEMU_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(HFOCFS_WORKSPACE_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HFOCFS_WORKSPACE_QEMU_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(HFOCFS_WORKSPACE_QEMU_ISO) > $(HFOCFS_WORKSPACE_QEMU_ISO).sha256
	@echo "ISO готов: $(HFOCFS_WORKSPACE_QEMU_ISO)"

$(CONFIG_JOURNAL_ISO): deps $(CONFIG_JOURNAL_KERNEL)
	rm -rf $(CONFIG_JOURNAL_ISO_ROOT)
	mkdir -p $(CONFIG_JOURNAL_ISO_ROOT)/boot/limine $(CONFIG_JOURNAL_ISO_ROOT)/EFI/BOOT
	cp $(CONFIG_JOURNAL_KERNEL) $(CONFIG_JOURNAL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(CONFIG_JOURNAL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(CONFIG_JOURNAL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(CONFIG_JOURNAL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(CONFIG_JOURNAL_ISO_ROOT) -o $(CONFIG_JOURNAL_ISO)
	python3 scripts/normalize-iso-gpt.py $(CONFIG_JOURNAL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(CONFIG_JOURNAL_ISO)
	python3 scripts/normalize-iso-gpt.py $(CONFIG_JOURNAL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(CONFIG_JOURNAL_ISO) > $(CONFIG_JOURNAL_ISO).sha256
	@echo "ISO готов: $(CONFIG_JOURNAL_ISO)"

$(PHYSICAL_STORAGE_ISO): deps $(PHYSICAL_STORAGE_KERNEL)
	rm -rf $(PHYSICAL_STORAGE_ISO_ROOT)
	mkdir -p $(PHYSICAL_STORAGE_ISO_ROOT)/boot/limine $(PHYSICAL_STORAGE_ISO_ROOT)/EFI/BOOT
	cp $(PHYSICAL_STORAGE_KERNEL) $(PHYSICAL_STORAGE_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(PHYSICAL_STORAGE_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(PHYSICAL_STORAGE_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(PHYSICAL_STORAGE_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PHYSICAL_STORAGE_ISO_ROOT) -o $(PHYSICAL_STORAGE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PHYSICAL_STORAGE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PHYSICAL_STORAGE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PHYSICAL_STORAGE_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(PHYSICAL_STORAGE_ISO) > $(PHYSICAL_STORAGE_ISO).sha256
	@echo "ISO готов: $(PHYSICAL_STORAGE_ISO)"

$(PHYSICAL_SATA_ACCEPTANCE_ISO): deps $(PHYSICAL_SATA_ACCEPTANCE_KERNEL)
	rm -rf $(PHYSICAL_SATA_ACCEPTANCE_ISO_ROOT)
	mkdir -p $(PHYSICAL_SATA_ACCEPTANCE_ISO_ROOT)/boot/limine \
		$(PHYSICAL_SATA_ACCEPTANCE_ISO_ROOT)/EFI/BOOT
	cp $(PHYSICAL_SATA_ACCEPTANCE_KERNEL) \
		$(PHYSICAL_SATA_ACCEPTANCE_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf \
		$(PHYSICAL_SATA_ACCEPTANCE_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin \
		$(PHYSICAL_SATA_ACCEPTANCE_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI \
		$(PHYSICAL_SATA_ACCEPTANCE_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PHYSICAL_SATA_ACCEPTANCE_ISO_ROOT) \
		-o $(PHYSICAL_SATA_ACCEPTANCE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PHYSICAL_SATA_ACCEPTANCE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PHYSICAL_SATA_ACCEPTANCE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PHYSICAL_SATA_ACCEPTANCE_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(PHYSICAL_SATA_ACCEPTANCE_ISO) > \
		$(PHYSICAL_SATA_ACCEPTANCE_ISO).sha256
	@echo "ISO готов: $(PHYSICAL_SATA_ACCEPTANCE_ISO)"

# CI-SPEEDUP-5: the installer media are files with prerequisites, not phony
# recipes, so a run that prepared them (the CI phase's image step, or
# `make qemu-test-images`) reuses them instead of rebuilding them inside the
# QEMU suite's slots.  `make test-production-installer-qemu` still builds
# whatever is missing or stale - the media aliases below are the same names the
# target and the standalone runs always used - and the target's own marker and
# medium checks still run on every proof.
#
# `deps` is an order-only prerequisite: its recipe fetches third-party sources
# and has no file of its own, so as a normal prerequisite it would make every
# medium permanently out of date.  The fetched limine binary is wildcarded for
# the same reason - it is a real input when the fetch has already happened, and
# the order-only `deps` brings it in when it has not.  `Makefile` is a normal
# prerequisite so a changed declaration rebuilds the medium it changed.
PRODUCTION_INSTALLER_MEDIA_PREREQUISITES := \
	$(PRODUCTION_INSTALLER_KERNEL) $(KERNEL) Makefile \
	$(wildcard $(LIMINE)/limine) \
	$(USER_ROOTFS_STAGE) \
	scripts/hfoc-rootfs-pack.py \
	scripts/hfoc-host-seed-provision.py \
	scripts/hfoc-install-media-stage.sh \
	scripts/hfoc-installed-config.py \
	scripts/hfoc-install-media-manifest.py \
	scripts/hfoc-install-media-prealloc.py \
	scripts/normalize-iso-gpt.py

# The media the QEMU installer proof reads: preparing them before the queue
# takes them off the QEMU suite's critical path.
PRODUCTION_INSTALLER_QEMU_IMAGES := \
	$(PRODUCTION_INSTALLER_ISO) \
	$(PRODUCTION_INSTALLER_AUTHORISED_ISO) \
	$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO) \
	$(PRODUCTION_INSTALLER_MANAGEMENT_ISO) \
	$(PRODUCTION_INSTALLER_NETWORK_ISO) \
	$(PRODUCTION_INSTALLER_FORWARD_ISO) \
	$(PRODUCTION_INSTALLER_LANGUAGE_ISO) \
	$(PRODUCTION_INSTALLER_CONSOLE_ISO)

$(PRODUCTION_INSTALLER_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_ISO_ROOT) $(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) '' '' '$(HFOC_INSTALL_MEDIA_MANAGEMENT)' '' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)' \
		'$(HFOC_INSTALL_MEDIA_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_DNS)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_ISO_ROOT) -o $(PRODUCTION_INSTALLER_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_ISO)
	sha256sum $(PRODUCTION_INSTALLER_ISO) > $(PRODUCTION_INSTALLER_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_ISO)"

# ITEM-394: the operator-authorised installer medium.  The shipped installer
# ISO carries no install declaration, so an unattended boot refuses; a real
# install run needs a medium that names the one disk it may write, which is
# what this target builds from explicit variables.  It is phony on purpose:
# the ISO content depends on the variables, not only on the file times.
.PHONY: production-installer-authorised-media
production-installer-authorised-media: $(PRODUCTION_INSTALLER_AUTHORISED_ISO)
$(PRODUCTION_INSTALLER_AUTHORISED_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	HFOC_INSTALL_MEDIA_ENTRY='$(HFOC_INSTALL_MEDIA_ENTRY)' \
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'$(HFOC_INSTALL_MEDIA_MANAGEMENT)' \
		'$(HFOC_INSTALL_MEDIA_LAYOUT)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)' \
		'$(HFOC_INSTALL_MEDIA_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_DNS)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_AUTHORISED_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_AUTHORISED_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_AUTHORISED_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_AUTHORISED_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_AUTHORISED_ISO)
	sha256sum $(PRODUCTION_INSTALLER_AUTHORISED_ISO) > \
		$(PRODUCTION_INSTALLER_AUTHORISED_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_AUTHORISED_ISO)"

# ITEM-509 increment 27 (509.7c-2): the same authorised declaration with one
# directive more - the interface language.  The stage script reads it from the
# environment, so every medium that declares no language keeps its exact token
# bytes, and this one medium is what the language proof installs from.  Phony
# for the same reason the targets around it are.
.PHONY: production-installer-language-media
production-installer-language-media: $(PRODUCTION_INSTALLER_LANGUAGE_ISO)
$(PRODUCTION_INSTALLER_LANGUAGE_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	HFOC_INSTALL_MEDIA_LANGUAGE='ru' \
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_LANGUAGE_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'$(HFOC_INSTALL_MEDIA_MANAGEMENT)' \
		'$(HFOC_INSTALL_MEDIA_LAYOUT)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)' \
		'$(HFOC_INSTALL_MEDIA_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_DNS)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_LANGUAGE_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_LANGUAGE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_LANGUAGE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_LANGUAGE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_LANGUAGE_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_LANGUAGE_ISO)
	sha256sum $(PRODUCTION_INSTALLER_LANGUAGE_ISO) > \
		$(PRODUCTION_INSTALLER_LANGUAGE_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_LANGUAGE_ISO)"

# ITEM-490: the medium a person boots.  It carries the same declaration as the
# authorised medium and one difference: the operator entry is first, so the
# machine lands on the storage screen and waits for the operator to name the
# disk.  Phony for the same reason the targets around it are.
.PHONY: production-installer-console-media
production-installer-console-media: $(PRODUCTION_INSTALLER_CONSOLE_ISO)
$(PRODUCTION_INSTALLER_CONSOLE_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	HFOC_INSTALL_MEDIA_ENTRY='console' \
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_CONSOLE_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'$(HFOC_INSTALL_MEDIA_MANAGEMENT)' \
		'$(HFOC_INSTALL_MEDIA_LAYOUT)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)' \
		'$(HFOC_INSTALL_MEDIA_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_DNS)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_CONSOLE_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_CONSOLE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_CONSOLE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_CONSOLE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_CONSOLE_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_CONSOLE_ISO)
	sha256sum $(PRODUCTION_INSTALLER_CONSOLE_ISO) > \
		$(PRODUCTION_INSTALLER_CONSOLE_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_CONSOLE_ISO)"

# ITEM-598: a personalised operator medium.  The host seed is generated outside
# the repository by scripts/hfoc-host-seed-provision.py and passed explicitly:
# `make production-installer-host-seeded-media
# HFOC_INSTALL_MEDIA_HOST_SEED=/path/outside/repo/hfoc-seed.bin
# HFOC_INSTALL_MEDIA_TARGET=<target-serial>`.
.PHONY: production-installer-host-seeded-media
production-installer-host-seeded-media: $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	@set -Eeuo pipefail; \
	umask 077; \
	trap 'status=$$?; \
		rm -rf $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO_ROOT); \
		if ((status != 0)); then \
			rm -f $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO) \
				$(PRODUCTION_INSTALLER_HOST_SEEDED_ISO).sha256; \
		fi; \
		exit $$status' EXIT; \
	test -n "$(HFOC_INSTALL_MEDIA_HOST_SEED)" || { \
		echo "HFOC_INSTALL_MEDIA_HOST_SEED must name a host-generated record" >&2; \
		exit 2; \
	}; \
	test -n "$(HFOC_INSTALL_MEDIA_TARGET)" || { \
		echo "HFOC_INSTALL_MEDIA_TARGET must name the target serial" >&2; \
		exit 2; \
	}; \
	HFOC_INSTALL_MEDIA_ENTRY='console' \
	HFOC_INSTALL_MEDIA_HOST_SEED='$(HFOC_INSTALL_MEDIA_HOST_SEED)' \
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_HOST_SEEDED_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'$(HFOC_INSTALL_MEDIA_MANAGEMENT)' \
		'$(HFOC_INSTALL_MEDIA_LAYOUT)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)' \
		'$(HFOC_INSTALL_MEDIA_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_DNS)'; \
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_HOST_SEEDED_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO); \
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO) "$(ISO_GPT_SEED)"; \
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO); \
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO) "$(ISO_GPT_SEED)"; \
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO); \
	chmod 600 $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO); \
	sha256sum $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO) > \
		$(PRODUCTION_INSTALLER_HOST_SEEDED_ISO).sha256; \
	chmod 600 $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO).sha256; \
	echo "ISO готов: $(PRODUCTION_INSTALLER_HOST_SEEDED_ISO)"

# ITEM-588: the update medium - the state-preserving reinstall of an appliance
# that is already installed.  It is the unattended entry with the update layout
# declared, so the run reaches the preflight that proves the persistence range
# and the installed reader before it writes anything; the preflight's record
# names both ranges, the loader's verdict and whether the state held a
# configuration and an account.
.PHONY: production-installer-update-media
production-installer-update-media: $(PRODUCTION_INSTALLER_UPDATE_ISO)
$(PRODUCTION_INSTALLER_UPDATE_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	HFOC_INSTALL_MEDIA_ENTRY='unattended' \
	HFOC_INSTALL_MEDIA_LAYOUT='update' \
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_UPDATE_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'$(HFOC_INSTALL_MEDIA_MANAGEMENT)' \
		'update' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_UPDATE_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_UPDATE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_UPDATE_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_UPDATE_ISO)
	sha256sum $(PRODUCTION_INSTALLER_UPDATE_ISO) > \
		$(PRODUCTION_INSTALLER_UPDATE_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_UPDATE_ISO)"

# ITEM-606: the test-only update medium that carries the committed SSH client
# public key.  The flow writes it into the installed state and the proof boots
# the installed system and reads the key back with `ssh-key show`.
$(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) \
		$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE) | deps
	HFOC_INSTALL_MEDIA_ENTRY='unattended' \
	HFOC_INSTALL_MEDIA_LAYOUT='update' \
	HFOC_INSTALL_MEDIA_SSH_KEY='$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE)' \
	HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM='$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE_CONFIRM)' \
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'$(HFOC_INSTALL_MEDIA_MANAGEMENT)' \
		'update' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO)
	sha256sum $(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO) > \
		$(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO)"

# ITEM-637: the management-scope test-key update.  It carries the committed
# test-only client key and the same `update` declaration as the owner medium,
# so the installed-persistence proof exercises the production update shape.
.PHONY: production-installer-management-ssh-key-update-media
production-installer-management-ssh-key-update-media: $(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO)
$(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) \
		$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE) | deps
	HFOC_INSTALL_MEDIA_ENTRY='unattended' \
	HFOC_INSTALL_MEDIA_LAYOUT='update' \
	HFOC_INSTALL_MEDIA_SSH_KEY='$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE)' \
	HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM='$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE_CONFIRM)' \
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'management-interface' \
		'update' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO)
	sha256sum $(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO) > \
		$(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO)"

# ITEM-638: the >512-byte standard update medium.  The declaration carries
# every standard directive at once, including four forward clients; the
# kernel's finite bound (not the old 512) is the one the stage script reads.
.PHONY: production-installer-capacity-update-media
production-installer-capacity-update-media: $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO)
$(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) \
		$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE) | deps
	HFOC_INSTALL_MEDIA_ENTRY='unattended' \
	HFOC_INSTALL_MEDIA_LAYOUT='update' \
	HFOC_INSTALL_MEDIA_SSH_KEY='$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE)' \
	HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM='$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE_CONFIRM)' \
	HFOC_INSTALL_MEDIA_FORWARD='on' \
	HFOC_INSTALL_MEDIA_FORWARD_TARGET='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET)' \
	HFOC_INSTALL_MEDIA_FORWARD_TARGET_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT)' \
	HFOC_INSTALL_MEDIA_FORWARD_LISTEN_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT)' \
	HFOC_INSTALL_MEDIA_FORWARD_CLIENTS='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS),10.0.2.3,10.0.2.4,10.0.2.5' \
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'management-interface' \
		'update' \
		'static' \
		'$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_DNS)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO)
	sha256sum $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO) > \
		$(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO)"

# ITEM-638: the oversized negative control.  Staging succeeds at the format
# bound; 600 comment bytes are appended after it, before the ISO is sealed, so
# the kernel sees a declaration over 1024 bytes and must refuse with
# TOKEN_TOO_LARGE before any write.
.PHONY: production-installer-oversized-update-media
production-installer-oversized-update-media: $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO)
$(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO): $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO)
	rm -rf $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO_ROOT)
	cp -a $(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO_ROOT)
	head -c 600 /dev/zero | tr '\0' '#' >> \
		$(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO_ROOT)/boot/hfoc-install.token
	printf '\n' >> $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO_ROOT)/boot/hfoc-install.token
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO)
	sha256sum $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO) > \
		$(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO)"

# ITEM-552: the export proof's console medium - the shipped recipe with the
# fault-injection kernel, so the proof can make the xHCI fail where the shipped
# medium must not.
.PHONY: diag-export-qemu-media
diag-export-qemu-media: deps $(DIAG_EXPORT_KERNEL) $(KERNEL)
	HFOC_INSTALL_MEDIA_ENTRY='console' \
	bash scripts/hfoc-install-media-stage.sh \
		$(DIAG_EXPORT_ISO_ROOT) \
		$(DIAG_EXPORT_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'$(HFOC_INSTALL_MEDIA_MANAGEMENT)' \
		'$(HFOC_INSTALL_MEDIA_LAYOUT)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)' \
		'$(HFOC_INSTALL_MEDIA_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_DNS)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(DIAG_EXPORT_ISO_ROOT) -o $(DIAG_EXPORT_ISO)
	python3 scripts/normalize-iso-gpt.py $(DIAG_EXPORT_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(DIAG_EXPORT_ISO)
	python3 scripts/normalize-iso-gpt.py $(DIAG_EXPORT_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(DIAG_EXPORT_ISO)
	@echo "ISO готов: $(DIAG_EXPORT_ISO)"

# ITEM-398: the same authorised medium with the destructive layout declared.
# The preserving proof boots it against a machine that already carries a system
# to reproduce, through the product flow, the run the flow used to hardcode -
# and boots the plain authorised medium against the same machine to prove the
# run that replaces it.  Phony for the same reason the target above is: the
# ISO content is the declaration, not the file times.
.PHONY: production-installer-whole-disk-media
production-installer-whole-disk-media: $(PRODUCTION_INSTALLER_WHOLE_DISK_ISO)
$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'$(HFOC_INSTALL_MEDIA_MANAGEMENT)' \
		'whole-disk' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)' \
		'$(HFOC_INSTALL_MEDIA_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_DNS)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_WHOLE_DISK_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_WHOLE_DISK_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_WHOLE_DISK_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_WHOLE_DISK_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_WHOLE_DISK_ISO)
	sha256sum $(PRODUCTION_INSTALLER_WHOLE_DISK_ISO) > \
		$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_WHOLE_DISK_ISO)"

# ITEM-399: the authorised medium with the management interface declared.  The
# scope is a property of the media declaration, so proving that an installed
# appliance publishes MANAGEMENT_INTERFACE needs a medium that declared it; the
# flow proof installs from it and boots the installed disk.  Phony for the same
# reason the two targets above are: the ISO content is the declaration.
.PHONY: production-installer-management-media
production-installer-management-media: $(PRODUCTION_INSTALLER_MANAGEMENT_ISO)
$(PRODUCTION_INSTALLER_MANAGEMENT_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_MANAGEMENT_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'management-interface' \
		'$(HFOC_INSTALL_MEDIA_LAYOUT)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK)' \
		'$(HFOC_INSTALL_MEDIA_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_DNS)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_MANAGEMENT_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_MANAGEMENT_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_MANAGEMENT_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_MANAGEMENT_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_MANAGEMENT_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_MANAGEMENT_ISO)
	sha256sum $(PRODUCTION_INSTALLER_MANAGEMENT_ISO) > \
		$(PRODUCTION_INSTALLER_MANAGEMENT_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_MANAGEMENT_ISO)"

.PHONY: production-installer-network-media
production-installer-network-media: $(PRODUCTION_INSTALLER_NETWORK_ISO)
$(PRODUCTION_INSTALLER_NETWORK_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_NETWORK_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'management-interface' \
		'$(HFOC_INSTALL_MEDIA_LAYOUT)' \
		'static' \
		'$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_ADDRESS)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_GATEWAY)' \
		'$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_DNS)'
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_NETWORK_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_NETWORK_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_NETWORK_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_NETWORK_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_NETWORK_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_NETWORK_ISO)
	sha256sum $(PRODUCTION_INSTALLER_NETWORK_ISO) > \
		$(PRODUCTION_INSTALLER_NETWORK_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_NETWORK_ISO)"

# ITEM-449: the same authorised declaration with one forward declared.  The
# stage script reads the forward from the environment, so the media that
# declare none keep their exact command lines.
.PHONY: production-installer-forward-media
production-installer-forward-media: $(PRODUCTION_INSTALLER_FORWARD_ISO)
$(PRODUCTION_INSTALLER_FORWARD_ISO): private FORWARD_MEDIA_ISO_ROOT := $(PRODUCTION_INSTALLER_FORWARD_ISO_ROOT)
# The published forward medium uses the operator's key settings. Only the
# coexistence proof installs the repository's test key.
$(PRODUCTION_INSTALLER_FORWARD_SSH_ISO): private FORWARD_MEDIA_ISO_ROOT := $(PRODUCTION_INSTALLER_FORWARD_SSH_ISO_ROOT)
$(PRODUCTION_INSTALLER_FORWARD_SSH_ISO): private override HFOC_INSTALL_MEDIA_SSH_KEY := $(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE)
$(PRODUCTION_INSTALLER_FORWARD_SSH_ISO): private override HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM := $(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE_CONFIRM)
$(PRODUCTION_INSTALLER_FORWARD_SSH_ISO): $(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE)
$(PRODUCTION_INSTALLER_FORWARD_ISO) $(PRODUCTION_INSTALLER_FORWARD_SSH_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	HFOC_INSTALL_MEDIA_FORWARD='on' \
	HFOC_INSTALL_MEDIA_FORWARD_TARGET='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET)' \
	HFOC_INSTALL_MEDIA_FORWARD_TARGET_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT)' \
	HFOC_INSTALL_MEDIA_FORWARD_LISTEN_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT)' \
	HFOC_INSTALL_MEDIA_FORWARD_CLIENTS='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS)' \
	bash scripts/hfoc-install-media-stage.sh \
		$(FORWARD_MEDIA_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'management-interface' \
		'$(HFOC_INSTALL_MEDIA_LAYOUT)' \
		'' '' '' ''
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(FORWARD_MEDIA_ISO_ROOT) \
		-o $@
	python3 scripts/normalize-iso-gpt.py $@ "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $@
	python3 scripts/normalize-iso-gpt.py $@ "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $@
	sha256sum $@ > $@.sha256
	@echo "ISO готов: $@"

# ITEM-449: the same forward medium with a client list the proof's source is
# not on; the relay must refuse the connection before dialling the target.
.PHONY: production-installer-forward-denied-media
production-installer-forward-denied-media: $(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO)
$(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO): $(PRODUCTION_INSTALLER_MEDIA_PREREQUISITES) | deps
	HFOC_INSTALL_MEDIA_FORWARD='on' \
	HFOC_INSTALL_MEDIA_FORWARD_TARGET='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET)' \
	HFOC_INSTALL_MEDIA_FORWARD_TARGET_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT)' \
	HFOC_INSTALL_MEDIA_FORWARD_LISTEN_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT)' \
	HFOC_INSTALL_MEDIA_FORWARD_CLIENTS='$(HFOC_INSTALL_MEDIA_FORWARD_DENIED_CLIENTS)' \
	bash scripts/hfoc-install-media-stage.sh \
		$(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO_ROOT) \
		$(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) \
		'$(HFOC_INSTALL_MEDIA_CONFIRM)' \
		'$(HFOC_INSTALL_MEDIA_TARGET)' \
		'management-interface' \
		'$(HFOC_INSTALL_MEDIA_LAYOUT)' \
		'' '' '' ''
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO_ROOT) \
		-o $(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO)
	python3 scripts/normalize-iso-gpt.py $(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO) "$(ISO_GPT_SEED)"
	python3 scripts/hfoc-install-media-prealloc.py $(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO)
	sha256sum $(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO) > \
		$(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO).sha256
	@echo "ISO готов: $(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO)"

$(BOOT_CHAIN_ISO): deps $(BOOT_CHAIN_KERNEL)
	rm -rf $(BOOT_CHAIN_ISO_ROOT)
	mkdir -p $(BOOT_CHAIN_ISO_ROOT)/boot/limine $(BOOT_CHAIN_ISO_ROOT)/EFI/BOOT
	cp $(BOOT_CHAIN_KERNEL) $(BOOT_CHAIN_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(BOOT_CHAIN_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(BOOT_CHAIN_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(BOOT_CHAIN_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BOOT_CHAIN_ISO_ROOT) -o $(BOOT_CHAIN_ISO)
	python3 scripts/normalize-iso-gpt.py $(BOOT_CHAIN_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(BOOT_CHAIN_ISO)
	python3 scripts/normalize-iso-gpt.py $(BOOT_CHAIN_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(BOOT_CHAIN_ISO) > $(BOOT_CHAIN_ISO).sha256
	@echo "ISO готов: $(BOOT_CHAIN_ISO)"

$(HFOC_EFI_HELPER_EFI_IMAGE): deps
	@mkdir -p $(@D)
	cp $(LIMINE)/limine-uefi-cd.bin $@
	mmd -i $@ ::EFI/HFOC
	mcopy -i $@ $(LIMINE)/BOOTX64.EFI ::EFI/HFOC/PAYLOAD.EFI

$(HFOC_EFI_HELPER_ISO): deps $(HFOC_EFI_HELPER) \
		$(HFOC_EFI_HELPER_EFI_IMAGE) kernel
	rm -rf $(HFOC_EFI_HELPER_ISO_ROOT)
	mkdir -p $(HFOC_EFI_HELPER_ISO_ROOT)/boot/limine \
		$(HFOC_EFI_HELPER_ISO_ROOT)/EFI/BOOT \
		$(HFOC_EFI_HELPER_ISO_ROOT)/EFI/HFOC
	cp $(KERNEL) $(HFOC_EFI_HELPER_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/efi/limine-helper.conf \
		$(HFOC_EFI_HELPER_ISO_ROOT)/boot/limine/limine.conf
	cp $(HFOC_EFI_HELPER) \
		$(HFOC_EFI_HELPER_ISO_ROOT)/EFI/HFOC/HFOCINST.EFI
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
		$(HFOC_EFI_HELPER_ISO_ROOT)/boot/limine/
	cp $(HFOC_EFI_HELPER_EFI_IMAGE) \
		$(HFOC_EFI_HELPER_ISO_ROOT)/boot/limine/limine-uefi-cd.bin
	cp $(LIMINE)/BOOTX64.EFI \
		$(HFOC_EFI_HELPER_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(HFOC_EFI_HELPER_ISO_ROOT) -o $(HFOC_EFI_HELPER_ISO)
	python3 scripts/normalize-iso-gpt.py $(HFOC_EFI_HELPER_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(HFOC_EFI_HELPER_ISO)
	python3 scripts/normalize-iso-gpt.py $(HFOC_EFI_HELPER_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(HFOC_EFI_HELPER_ISO) > $(HFOC_EFI_HELPER_ISO).sha256
	@echo "ISO готов: $(HFOC_EFI_HELPER_ISO)"

$(PERSISTENT_CONFIG_ISO): deps $(PERSISTENT_CONFIG_KERNEL)
	rm -rf $(PERSISTENT_CONFIG_ISO_ROOT)
	mkdir -p $(PERSISTENT_CONFIG_ISO_ROOT)/boot/limine $(PERSISTENT_CONFIG_ISO_ROOT)/EFI/BOOT
	cp $(PERSISTENT_CONFIG_KERNEL) $(PERSISTENT_CONFIG_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(PERSISTENT_CONFIG_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(PERSISTENT_CONFIG_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(PERSISTENT_CONFIG_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PERSISTENT_CONFIG_ISO_ROOT) -o $(PERSISTENT_CONFIG_ISO)
	python3 scripts/normalize-iso-gpt.py $(PERSISTENT_CONFIG_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PERSISTENT_CONFIG_ISO)
	python3 scripts/normalize-iso-gpt.py $(PERSISTENT_CONFIG_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(PERSISTENT_CONFIG_ISO) > $(PERSISTENT_CONFIG_ISO).sha256
	@echo "ISO готов: $(PERSISTENT_CONFIG_ISO)"

$(NETWORK_CONFIG_ISO): deps $(NETWORK_CONFIG_KERNEL)
	rm -rf $(NETWORK_CONFIG_ISO_ROOT)
	mkdir -p $(NETWORK_CONFIG_ISO_ROOT)/boot/limine $(NETWORK_CONFIG_ISO_ROOT)/EFI/BOOT
	cp $(NETWORK_CONFIG_KERNEL) $(NETWORK_CONFIG_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(NETWORK_CONFIG_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(NETWORK_CONFIG_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(NETWORK_CONFIG_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(NETWORK_CONFIG_ISO_ROOT) -o $(NETWORK_CONFIG_ISO)
	python3 scripts/normalize-iso-gpt.py $(NETWORK_CONFIG_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(NETWORK_CONFIG_ISO)
	python3 scripts/normalize-iso-gpt.py $(NETWORK_CONFIG_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(NETWORK_CONFIG_ISO) > $(NETWORK_CONFIG_ISO).sha256
	@echo "ISO готов: $(NETWORK_CONFIG_ISO)"

# ITEM-507 increment 6: the fixture medium mirrors the production recipe - the
# boot-chain manifest and the license payload the production ISO carries - so
# the only difference between the two boots is the fixture define.
$(SNI_SELECTION_ISO): deps $(SNI_SELECTION_KERNEL)
	rm -rf $(SNI_SELECTION_ISO_ROOT)
	mkdir -p $(SNI_SELECTION_ISO_ROOT)/boot/limine $(SNI_SELECTION_ISO_ROOT)/EFI/BOOT
	cp $(SNI_SELECTION_KERNEL) $(SNI_SELECTION_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SNI_SELECTION_ISO_ROOT)/boot/limine/limine.conf
	printf '\nmodule_path: boot():/boot/hfoc-boot.man\n' \
	   >> $(SNI_SELECTION_ISO_ROOT)/boot/limine/limine.conf
	printf 'module_path: boot():/boot/limine/limine.conf\n' \
	   >> $(SNI_SELECTION_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SNI_SELECTION_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SNI_SELECTION_ISO_ROOT)/EFI/BOOT/
	python3 scripts/hfoc-boot-chain-manifest.py \
		--kernel $(SNI_SELECTION_ISO_ROOT)/boot/hfoc-kernel.elf \
		--config $(SNI_SELECTION_ISO_ROOT)/boot/limine/limine.conf \
		--release-id 1 --signer-id 1 \
		--key boot/keys/hfoc-development-signing-key.pem \
		--output $(SNI_SELECTION_ISO_ROOT)/boot/hfoc-boot.man
	bash scripts/install-license-payload.sh $(SNI_SELECTION_ISO_ROOT)
	bash scripts/verify-license-payload.sh $(SNI_SELECTION_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SNI_SELECTION_ISO_ROOT) -o $(SNI_SELECTION_ISO)
	python3 scripts/normalize-iso-gpt.py $(SNI_SELECTION_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SNI_SELECTION_ISO)
	python3 scripts/normalize-iso-gpt.py $(SNI_SELECTION_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SNI_SELECTION_ISO) > $(SNI_SELECTION_ISO).sha256
	@echo "ISO готов: $(SNI_SELECTION_ISO)"

# ITEM-508 increment 15: the bootable medium of the directory proof.
$(ACME_DIRECTORY_ISO): deps $(ACME_DIRECTORY_KERNEL)
	rm -rf $(ACME_DIRECTORY_ISO_ROOT)
	mkdir -p $(ACME_DIRECTORY_ISO_ROOT)/boot/limine $(ACME_DIRECTORY_ISO_ROOT)/EFI/BOOT
	cp $(ACME_DIRECTORY_KERNEL) $(ACME_DIRECTORY_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(ACME_DIRECTORY_ISO_ROOT)/boot/limine/limine.conf
	printf '\nmodule_path: boot():/boot/hfoc-boot.man\n' \
	   >> $(ACME_DIRECTORY_ISO_ROOT)/boot/limine/limine.conf
	printf 'module_path: boot():/boot/limine/limine.conf\n' \
	   >> $(ACME_DIRECTORY_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(ACME_DIRECTORY_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ACME_DIRECTORY_ISO_ROOT)/EFI/BOOT/
	python3 scripts/hfoc-boot-chain-manifest.py \
		--kernel $(ACME_DIRECTORY_ISO_ROOT)/boot/hfoc-kernel.elf \
		--config $(ACME_DIRECTORY_ISO_ROOT)/boot/limine/limine.conf \
		--release-id 1 --signer-id 1 \
		--key boot/keys/hfoc-development-signing-key.pem \
		--output $(ACME_DIRECTORY_ISO_ROOT)/boot/hfoc-boot.man
	bash scripts/install-license-payload.sh $(ACME_DIRECTORY_ISO_ROOT)
	bash scripts/verify-license-payload.sh $(ACME_DIRECTORY_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ACME_DIRECTORY_ISO_ROOT) -o $(ACME_DIRECTORY_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACME_DIRECTORY_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(ACME_DIRECTORY_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACME_DIRECTORY_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(ACME_DIRECTORY_ISO) > $(ACME_DIRECTORY_ISO).sha256
	@echo "ISO готов: $(ACME_DIRECTORY_ISO)"

# ITEM-508 increment 17: the bootable medium of the order proof.
$(ACME_ORDER_ISO): deps $(ACME_ORDER_KERNEL)
	rm -rf $(ACME_ORDER_ISO_ROOT)
	mkdir -p $(ACME_ORDER_ISO_ROOT)/boot/limine $(ACME_ORDER_ISO_ROOT)/EFI/BOOT
	cp $(ACME_ORDER_KERNEL) $(ACME_ORDER_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(ACME_ORDER_ISO_ROOT)/boot/limine/limine.conf
	printf '\nmodule_path: boot():/boot/hfoc-boot.man\n' \
	   >> $(ACME_ORDER_ISO_ROOT)/boot/limine/limine.conf
	printf 'module_path: boot():/boot/limine/limine.conf\n' \
	   >> $(ACME_ORDER_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(ACME_ORDER_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ACME_ORDER_ISO_ROOT)/EFI/BOOT/
	python3 scripts/hfoc-boot-chain-manifest.py \
		--kernel $(ACME_ORDER_ISO_ROOT)/boot/hfoc-kernel.elf \
		--config $(ACME_ORDER_ISO_ROOT)/boot/limine/limine.conf \
		--release-id 1 --signer-id 1 \
		--key boot/keys/hfoc-development-signing-key.pem \
		--output $(ACME_ORDER_ISO_ROOT)/boot/hfoc-boot.man
	bash scripts/install-license-payload.sh $(ACME_ORDER_ISO_ROOT)
	bash scripts/verify-license-payload.sh $(ACME_ORDER_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ACME_ORDER_ISO_ROOT) -o $(ACME_ORDER_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACME_ORDER_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(ACME_ORDER_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACME_ORDER_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(ACME_ORDER_ISO) > $(ACME_ORDER_ISO).sha256
	@echo "ISO готов: $(ACME_ORDER_ISO)"

# ITEM-508 increment 36: the bootable medium of the production order-path wire
# proof.
$(ACME_BOOT_ORDER_ISO): deps $(ACME_BOOT_ORDER_KERNEL)
	rm -rf $(ACME_BOOT_ORDER_ISO_ROOT)
	mkdir -p $(ACME_BOOT_ORDER_ISO_ROOT)/boot/limine $(ACME_BOOT_ORDER_ISO_ROOT)/EFI/BOOT
	cp $(ACME_BOOT_ORDER_KERNEL) $(ACME_BOOT_ORDER_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(ACME_BOOT_ORDER_ISO_ROOT)/boot/limine/limine.conf
	printf '\nmodule_path: boot():/boot/hfoc-boot.man\n' \
	   >> $(ACME_BOOT_ORDER_ISO_ROOT)/boot/limine/limine.conf
	printf 'module_path: boot():/boot/limine/limine.conf\n' \
	   >> $(ACME_BOOT_ORDER_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(ACME_BOOT_ORDER_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ACME_BOOT_ORDER_ISO_ROOT)/EFI/BOOT/
	python3 scripts/hfoc-boot-chain-manifest.py \
		--kernel $(ACME_BOOT_ORDER_ISO_ROOT)/boot/hfoc-kernel.elf \
		--config $(ACME_BOOT_ORDER_ISO_ROOT)/boot/limine/limine.conf \
		--release-id 1 --signer-id 1 \
		--key boot/keys/hfoc-development-signing-key.pem \
		--output $(ACME_BOOT_ORDER_ISO_ROOT)/boot/hfoc-boot.man
	bash scripts/install-license-payload.sh $(ACME_BOOT_ORDER_ISO_ROOT)
	bash scripts/verify-license-payload.sh $(ACME_BOOT_ORDER_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ACME_BOOT_ORDER_ISO_ROOT) -o $(ACME_BOOT_ORDER_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACME_BOOT_ORDER_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(ACME_BOOT_ORDER_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACME_BOOT_ORDER_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(ACME_BOOT_ORDER_ISO) > $(ACME_BOOT_ORDER_ISO).sha256
	@echo "ISO готов: $(ACME_BOOT_ORDER_ISO)"

# ITEM-508 increment 29: the bootable medium of the HTTP-01 wire proof.
$(ACME_HTTP01_ISO): deps $(ACME_HTTP01_KERNEL)
	rm -rf $(ACME_HTTP01_ISO_ROOT)
	mkdir -p $(ACME_HTTP01_ISO_ROOT)/boot/limine $(ACME_HTTP01_ISO_ROOT)/EFI/BOOT
	cp $(ACME_HTTP01_KERNEL) $(ACME_HTTP01_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(ACME_HTTP01_ISO_ROOT)/boot/limine/limine.conf
	printf '\nmodule_path: boot():/boot/hfoc-boot.man\n' \
	   >> $(ACME_HTTP01_ISO_ROOT)/boot/limine/limine.conf
	printf 'module_path: boot():/boot/limine/limine.conf\n' \
	   >> $(ACME_HTTP01_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(ACME_HTTP01_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ACME_HTTP01_ISO_ROOT)/EFI/BOOT/
	python3 scripts/hfoc-boot-chain-manifest.py \
		--kernel $(ACME_HTTP01_ISO_ROOT)/boot/hfoc-kernel.elf \
		--config $(ACME_HTTP01_ISO_ROOT)/boot/limine/limine.conf \
		--release-id 1 --signer-id 1 \
		--key boot/keys/hfoc-development-signing-key.pem \
		--output $(ACME_HTTP01_ISO_ROOT)/boot/hfoc-boot.man
	bash scripts/install-license-payload.sh $(ACME_HTTP01_ISO_ROOT)
	bash scripts/verify-license-payload.sh $(ACME_HTTP01_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ACME_HTTP01_ISO_ROOT) -o $(ACME_HTTP01_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACME_HTTP01_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(ACME_HTTP01_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACME_HTTP01_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(ACME_HTTP01_ISO) > $(ACME_HTTP01_ISO).sha256
	@echo "ISO готов: $(ACME_HTTP01_ISO)"

$(PERSISTENT_NETWORK_CONFIG_ISO): deps $(PERSISTENT_NETWORK_CONFIG_KERNEL)
	rm -rf $(PERSISTENT_NETWORK_CONFIG_ISO_ROOT)
	mkdir -p $(PERSISTENT_NETWORK_CONFIG_ISO_ROOT)/boot/limine \
		$(PERSISTENT_NETWORK_CONFIG_ISO_ROOT)/EFI/BOOT
	cp $(PERSISTENT_NETWORK_CONFIG_KERNEL) \
		$(PERSISTENT_NETWORK_CONFIG_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf \
		$(PERSISTENT_NETWORK_CONFIG_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin \
		$(PERSISTENT_NETWORK_CONFIG_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI \
		$(PERSISTENT_NETWORK_CONFIG_ISO_ROOT)/EFI/BOOT/
	bash scripts/install-license-payload.sh \
		$(PERSISTENT_NETWORK_CONFIG_ISO_ROOT)
	bash scripts/verify-license-payload.sh \
		$(PERSISTENT_NETWORK_CONFIG_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PERSISTENT_NETWORK_CONFIG_ISO_ROOT) \
		-o $(PERSISTENT_NETWORK_CONFIG_ISO)
	python3 scripts/normalize-iso-gpt.py $(PERSISTENT_NETWORK_CONFIG_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PERSISTENT_NETWORK_CONFIG_ISO)
	python3 scripts/normalize-iso-gpt.py $(PERSISTENT_NETWORK_CONFIG_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(PERSISTENT_NETWORK_CONFIG_ISO) > \
		$(PERSISTENT_NETWORK_CONFIG_ISO).sha256
	@echo "ISO готов: $(PERSISTENT_NETWORK_CONFIG_ISO)"

$(BACKUP_RESTORE_ISO): deps $(BACKUP_RESTORE_KERNEL)
	rm -rf $(BACKUP_RESTORE_ISO_ROOT)
	mkdir -p $(BACKUP_RESTORE_ISO_ROOT)/boot/limine $(BACKUP_RESTORE_ISO_ROOT)/EFI/BOOT
	cp $(BACKUP_RESTORE_KERNEL) $(BACKUP_RESTORE_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(BACKUP_RESTORE_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(BACKUP_RESTORE_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(BACKUP_RESTORE_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BACKUP_RESTORE_ISO_ROOT) -o $(BACKUP_RESTORE_ISO)
	python3 scripts/normalize-iso-gpt.py $(BACKUP_RESTORE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(BACKUP_RESTORE_ISO)
	python3 scripts/normalize-iso-gpt.py $(BACKUP_RESTORE_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(BACKUP_RESTORE_ISO) > $(BACKUP_RESTORE_ISO).sha256
	@echo "ISO готов: $(BACKUP_RESTORE_ISO)"

$(SUPERVISOR_COMPOSITION_ISO): deps $(SUPERVISOR_COMPOSITION_KERNEL)
	rm -rf $(SUPERVISOR_COMPOSITION_ISO_ROOT)
	mkdir -p $(SUPERVISOR_COMPOSITION_ISO_ROOT)/boot/limine $(SUPERVISOR_COMPOSITION_ISO_ROOT)/EFI/BOOT
	cp $(SUPERVISOR_COMPOSITION_KERNEL) $(SUPERVISOR_COMPOSITION_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SUPERVISOR_COMPOSITION_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SUPERVISOR_COMPOSITION_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SUPERVISOR_COMPOSITION_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SUPERVISOR_COMPOSITION_ISO_ROOT) -o $(SUPERVISOR_COMPOSITION_ISO)
	python3 scripts/normalize-iso-gpt.py $(SUPERVISOR_COMPOSITION_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SUPERVISOR_COMPOSITION_ISO)
	python3 scripts/normalize-iso-gpt.py $(SUPERVISOR_COMPOSITION_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SUPERVISOR_COMPOSITION_ISO) > $(SUPERVISOR_COMPOSITION_ISO).sha256
	@echo "ISO готов: $(SUPERVISOR_COMPOSITION_ISO)"

$(LOCAL_CONTROL_ISO): deps $(LOCAL_CONTROL_KERNEL)
	rm -rf $(LOCAL_CONTROL_ISO_ROOT)
	mkdir -p $(LOCAL_CONTROL_ISO_ROOT)/boot/limine $(LOCAL_CONTROL_ISO_ROOT)/EFI/BOOT
	cp $(LOCAL_CONTROL_KERNEL) $(LOCAL_CONTROL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(LOCAL_CONTROL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(LOCAL_CONTROL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(LOCAL_CONTROL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(LOCAL_CONTROL_ISO_ROOT) -o $(LOCAL_CONTROL_ISO)
	python3 scripts/normalize-iso-gpt.py $(LOCAL_CONTROL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(LOCAL_CONTROL_ISO)
	python3 scripts/normalize-iso-gpt.py $(LOCAL_CONTROL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(LOCAL_CONTROL_ISO) > $(LOCAL_CONTROL_ISO).sha256
	@echo "ISO готов: $(LOCAL_CONTROL_ISO)"

$(RECOVERY_EXECUTOR_ISO): deps $(RECOVERY_EXECUTOR_KERNEL)
	rm -rf $(RECOVERY_EXECUTOR_ISO_ROOT)
	mkdir -p $(RECOVERY_EXECUTOR_ISO_ROOT)/boot/limine $(RECOVERY_EXECUTOR_ISO_ROOT)/EFI/BOOT
	cp $(RECOVERY_EXECUTOR_KERNEL) $(RECOVERY_EXECUTOR_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(RECOVERY_EXECUTOR_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(RECOVERY_EXECUTOR_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(RECOVERY_EXECUTOR_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(RECOVERY_EXECUTOR_ISO_ROOT) -o $(RECOVERY_EXECUTOR_ISO)
	python3 scripts/normalize-iso-gpt.py $(RECOVERY_EXECUTOR_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(RECOVERY_EXECUTOR_ISO)
	python3 scripts/normalize-iso-gpt.py $(RECOVERY_EXECUTOR_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(RECOVERY_EXECUTOR_ISO) > $(RECOVERY_EXECUTOR_ISO).sha256
	@echo "ISO готов: $(RECOVERY_EXECUTOR_ISO)"

$(NETWORK_FOUNDATION_ISO): deps $(NETWORK_FOUNDATION_KERNEL)
	rm -rf $(NETWORK_FOUNDATION_ISO_ROOT)
	mkdir -p $(NETWORK_FOUNDATION_ISO_ROOT)/boot/limine $(NETWORK_FOUNDATION_ISO_ROOT)/EFI/BOOT
	cp $(NETWORK_FOUNDATION_KERNEL) $(NETWORK_FOUNDATION_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(NETWORK_FOUNDATION_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(NETWORK_FOUNDATION_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(NETWORK_FOUNDATION_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(NETWORK_FOUNDATION_ISO_ROOT) -o $(NETWORK_FOUNDATION_ISO)
	python3 scripts/normalize-iso-gpt.py $(NETWORK_FOUNDATION_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(NETWORK_FOUNDATION_ISO)
	python3 scripts/normalize-iso-gpt.py $(NETWORK_FOUNDATION_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(NETWORK_FOUNDATION_ISO) > $(NETWORK_FOUNDATION_ISO).sha256
	@echo "ISO готов: $(NETWORK_FOUNDATION_ISO)"

$(VIRTIO_FOUNDATION_ISO): deps $(VIRTIO_FOUNDATION_KERNEL)
	rm -rf $(VIRTIO_FOUNDATION_ISO_ROOT)
	mkdir -p $(VIRTIO_FOUNDATION_ISO_ROOT)/boot/limine $(VIRTIO_FOUNDATION_ISO_ROOT)/EFI/BOOT
	cp $(VIRTIO_FOUNDATION_KERNEL) $(VIRTIO_FOUNDATION_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(VIRTIO_FOUNDATION_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(VIRTIO_FOUNDATION_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(VIRTIO_FOUNDATION_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(VIRTIO_FOUNDATION_ISO_ROOT) -o $(VIRTIO_FOUNDATION_ISO)
	python3 scripts/normalize-iso-gpt.py $(VIRTIO_FOUNDATION_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(VIRTIO_FOUNDATION_ISO)
	python3 scripts/normalize-iso-gpt.py $(VIRTIO_FOUNDATION_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(VIRTIO_FOUNDATION_ISO) > $(VIRTIO_FOUNDATION_ISO).sha256
	@echo "ISO готов: $(VIRTIO_FOUNDATION_ISO)"

$(VIRTIO_NET_FRAME_ISO): deps $(VIRTIO_NET_FRAME_KERNEL)
	rm -rf $(VIRTIO_NET_FRAME_ISO_ROOT)
	mkdir -p $(VIRTIO_NET_FRAME_ISO_ROOT)/boot/limine $(VIRTIO_NET_FRAME_ISO_ROOT)/EFI/BOOT
	cp $(VIRTIO_NET_FRAME_KERNEL) $(VIRTIO_NET_FRAME_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(VIRTIO_NET_FRAME_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(VIRTIO_NET_FRAME_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(VIRTIO_NET_FRAME_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(VIRTIO_NET_FRAME_ISO_ROOT) -o $(VIRTIO_NET_FRAME_ISO)
	python3 scripts/normalize-iso-gpt.py $(VIRTIO_NET_FRAME_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(VIRTIO_NET_FRAME_ISO)
	python3 scripts/normalize-iso-gpt.py $(VIRTIO_NET_FRAME_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(VIRTIO_NET_FRAME_ISO) > $(VIRTIO_NET_FRAME_ISO).sha256
	@echo "ISO готов: $(VIRTIO_NET_FRAME_ISO)"

$(PACKET_BOUNDARY_ISO): deps $(PACKET_BOUNDARY_KERNEL)
	rm -rf $(PACKET_BOUNDARY_ISO_ROOT)
	mkdir -p $(PACKET_BOUNDARY_ISO_ROOT)/boot/limine $(PACKET_BOUNDARY_ISO_ROOT)/EFI/BOOT
	cp $(PACKET_BOUNDARY_KERNEL) $(PACKET_BOUNDARY_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(PACKET_BOUNDARY_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(PACKET_BOUNDARY_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(PACKET_BOUNDARY_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(PACKET_BOUNDARY_ISO_ROOT) -o $(PACKET_BOUNDARY_ISO)
	python3 scripts/normalize-iso-gpt.py $(PACKET_BOUNDARY_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(PACKET_BOUNDARY_ISO)
	python3 scripts/normalize-iso-gpt.py $(PACKET_BOUNDARY_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(PACKET_BOUNDARY_ISO) > $(PACKET_BOUNDARY_ISO).sha256
	@echo "ISO готов: $(PACKET_BOUNDARY_ISO)"

$(ARP_ICMP_ISO): deps $(ARP_ICMP_KERNEL)
	rm -rf $(ARP_ICMP_ISO_ROOT)
	mkdir -p $(ARP_ICMP_ISO_ROOT)/boot/limine $(ARP_ICMP_ISO_ROOT)/EFI/BOOT
	cp $(ARP_ICMP_KERNEL) $(ARP_ICMP_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(ARP_ICMP_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(ARP_ICMP_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ARP_ICMP_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ARP_ICMP_ISO_ROOT) -o $(ARP_ICMP_ISO)
	python3 scripts/normalize-iso-gpt.py $(ARP_ICMP_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(ARP_ICMP_ISO)
	python3 scripts/normalize-iso-gpt.py $(ARP_ICMP_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(ARP_ICMP_ISO) > $(ARP_ICMP_ISO).sha256
	@echo "ISO готов: $(ARP_ICMP_ISO)"

$(TCP_LISTENER_ISO): deps $(TCP_LISTENER_KERNEL)
	rm -rf $(TCP_LISTENER_ISO_ROOT)
	mkdir -p $(TCP_LISTENER_ISO_ROOT)/boot/limine $(TCP_LISTENER_ISO_ROOT)/EFI/BOOT
	cp $(TCP_LISTENER_KERNEL) $(TCP_LISTENER_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(TCP_LISTENER_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(TCP_LISTENER_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(TCP_LISTENER_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(TCP_LISTENER_ISO_ROOT) -o $(TCP_LISTENER_ISO)
	python3 scripts/normalize-iso-gpt.py $(TCP_LISTENER_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(TCP_LISTENER_ISO)
	python3 scripts/normalize-iso-gpt.py $(TCP_LISTENER_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(TCP_LISTENER_ISO) > $(TCP_LISTENER_ISO).sha256
	@echo "ISO готов: $(TCP_LISTENER_ISO)"

$(NETWORK_RESOURCE_ISO): deps $(NETWORK_RESOURCE_KERNEL)
	rm -rf $(NETWORK_RESOURCE_ISO_ROOT)
	mkdir -p $(NETWORK_RESOURCE_ISO_ROOT)/boot/limine $(NETWORK_RESOURCE_ISO_ROOT)/EFI/BOOT
	cp $(NETWORK_RESOURCE_KERNEL) $(NETWORK_RESOURCE_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(NETWORK_RESOURCE_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(NETWORK_RESOURCE_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(NETWORK_RESOURCE_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(NETWORK_RESOURCE_ISO_ROOT) -o $(NETWORK_RESOURCE_ISO)
	python3 scripts/normalize-iso-gpt.py $(NETWORK_RESOURCE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(NETWORK_RESOURCE_ISO)
	python3 scripts/normalize-iso-gpt.py $(NETWORK_RESOURCE_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(NETWORK_RESOURCE_ISO) > $(NETWORK_RESOURCE_ISO).sha256
	@echo "ISO готов: $(NETWORK_RESOURCE_ISO)"

$(TRUST_POLICY_ISO): deps $(TRUST_POLICY_KERNEL)
	rm -rf $(TRUST_POLICY_ISO_ROOT)
	mkdir -p $(TRUST_POLICY_ISO_ROOT)/boot/limine $(TRUST_POLICY_ISO_ROOT)/EFI/BOOT
	cp $(TRUST_POLICY_KERNEL) $(TRUST_POLICY_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(TRUST_POLICY_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(TRUST_POLICY_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(TRUST_POLICY_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(TRUST_POLICY_ISO_ROOT) -o $(TRUST_POLICY_ISO)
	python3 scripts/normalize-iso-gpt.py $(TRUST_POLICY_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(TRUST_POLICY_ISO)
	python3 scripts/normalize-iso-gpt.py $(TRUST_POLICY_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(TRUST_POLICY_ISO) > $(TRUST_POLICY_ISO).sha256
	@echo "ISO готов: $(TRUST_POLICY_ISO)"

$(UPDATE_LIFECYCLE_ISO): deps $(UPDATE_LIFECYCLE_KERNEL)
	rm -rf $(UPDATE_LIFECYCLE_ISO_ROOT)
	mkdir -p $(UPDATE_LIFECYCLE_ISO_ROOT)/boot/limine $(UPDATE_LIFECYCLE_ISO_ROOT)/EFI/BOOT
	cp $(UPDATE_LIFECYCLE_KERNEL) $(UPDATE_LIFECYCLE_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(UPDATE_LIFECYCLE_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(UPDATE_LIFECYCLE_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(UPDATE_LIFECYCLE_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(UPDATE_LIFECYCLE_ISO_ROOT) -o $(UPDATE_LIFECYCLE_ISO)
	python3 scripts/normalize-iso-gpt.py $(UPDATE_LIFECYCLE_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(UPDATE_LIFECYCLE_ISO)
	python3 scripts/normalize-iso-gpt.py $(UPDATE_LIFECYCLE_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(UPDATE_LIFECYCLE_ISO) > $(UPDATE_LIFECYCLE_ISO).sha256
	@echo "ISO готов: $(UPDATE_LIFECYCLE_ISO)"

$(SUPPORTABILITY_ISO): deps $(SUPPORTABILITY_KERNEL)
	rm -rf $(SUPPORTABILITY_ISO_ROOT)
	mkdir -p $(SUPPORTABILITY_ISO_ROOT)/boot/limine $(SUPPORTABILITY_ISO_ROOT)/EFI/BOOT
	cp $(SUPPORTABILITY_KERNEL) $(SUPPORTABILITY_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SUPPORTABILITY_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SUPPORTABILITY_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SUPPORTABILITY_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SUPPORTABILITY_ISO_ROOT) -o $(SUPPORTABILITY_ISO)
	python3 scripts/normalize-iso-gpt.py $(SUPPORTABILITY_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SUPPORTABILITY_ISO)
	python3 scripts/normalize-iso-gpt.py $(SUPPORTABILITY_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SUPPORTABILITY_ISO) > $(SUPPORTABILITY_ISO).sha256
	@echo "ISO готов: $(SUPPORTABILITY_ISO)"

$(ACCOUNTING_BOUNDARY_ISO): deps $(ACCOUNTING_BOUNDARY_KERNEL)
	rm -rf $(ACCOUNTING_BOUNDARY_ISO_ROOT)
	mkdir -p $(ACCOUNTING_BOUNDARY_ISO_ROOT)/boot/limine $(ACCOUNTING_BOUNDARY_ISO_ROOT)/EFI/BOOT
	cp $(ACCOUNTING_BOUNDARY_KERNEL) $(ACCOUNTING_BOUNDARY_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(ACCOUNTING_BOUNDARY_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(ACCOUNTING_BOUNDARY_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ACCOUNTING_BOUNDARY_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ACCOUNTING_BOUNDARY_ISO_ROOT) -o $(ACCOUNTING_BOUNDARY_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACCOUNTING_BOUNDARY_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(ACCOUNTING_BOUNDARY_ISO)
	python3 scripts/normalize-iso-gpt.py $(ACCOUNTING_BOUNDARY_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(ACCOUNTING_BOUNDARY_ISO) > $(ACCOUNTING_BOUNDARY_ISO).sha256
	@echo "ISO готов: $(ACCOUNTING_BOUNDARY_ISO)"

$(STORAGE_INTERRUPTION_ISO): deps $(STORAGE_INTERRUPTION_KERNEL)
	rm -rf $(STORAGE_INTERRUPTION_ISO_ROOT)
	mkdir -p $(STORAGE_INTERRUPTION_ISO_ROOT)/boot/limine $(STORAGE_INTERRUPTION_ISO_ROOT)/EFI/BOOT
	cp $(STORAGE_INTERRUPTION_KERNEL) $(STORAGE_INTERRUPTION_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(STORAGE_INTERRUPTION_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(STORAGE_INTERRUPTION_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(STORAGE_INTERRUPTION_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(STORAGE_INTERRUPTION_ISO_ROOT) -o $(STORAGE_INTERRUPTION_ISO)
	python3 scripts/normalize-iso-gpt.py $(STORAGE_INTERRUPTION_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(STORAGE_INTERRUPTION_ISO)
	python3 scripts/normalize-iso-gpt.py $(STORAGE_INTERRUPTION_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(STORAGE_INTERRUPTION_ISO) > $(STORAGE_INTERRUPTION_ISO).sha256
	@echo "ISO готов: $(STORAGE_INTERRUPTION_ISO)"

$(SERVICE_CASCADE_DRILL_ISO): deps $(SERVICE_CASCADE_DRILL_KERNEL)
	rm -rf $(SERVICE_CASCADE_DRILL_ISO_ROOT)
	mkdir -p $(SERVICE_CASCADE_DRILL_ISO_ROOT)/boot/limine $(SERVICE_CASCADE_DRILL_ISO_ROOT)/EFI/BOOT
	cp $(SERVICE_CASCADE_DRILL_KERNEL) $(SERVICE_CASCADE_DRILL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(SERVICE_CASCADE_DRILL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(SERVICE_CASCADE_DRILL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(SERVICE_CASCADE_DRILL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(SERVICE_CASCADE_DRILL_ISO_ROOT) -o $(SERVICE_CASCADE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(SERVICE_CASCADE_DRILL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(SERVICE_CASCADE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(SERVICE_CASCADE_DRILL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(SERVICE_CASCADE_DRILL_ISO) > $(SERVICE_CASCADE_DRILL_ISO).sha256
	@echo "ISO готов: $(SERVICE_CASCADE_DRILL_ISO)"

$(LOCAL_CONTROL_OUTAGE_DRILL_ISO): deps $(LOCAL_CONTROL_OUTAGE_DRILL_KERNEL)
	rm -rf $(LOCAL_CONTROL_OUTAGE_DRILL_ISO_ROOT)
	mkdir -p $(LOCAL_CONTROL_OUTAGE_DRILL_ISO_ROOT)/boot/limine $(LOCAL_CONTROL_OUTAGE_DRILL_ISO_ROOT)/EFI/BOOT
	cp $(LOCAL_CONTROL_OUTAGE_DRILL_KERNEL) $(LOCAL_CONTROL_OUTAGE_DRILL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(LOCAL_CONTROL_OUTAGE_DRILL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(LOCAL_CONTROL_OUTAGE_DRILL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(LOCAL_CONTROL_OUTAGE_DRILL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(LOCAL_CONTROL_OUTAGE_DRILL_ISO_ROOT) -o $(LOCAL_CONTROL_OUTAGE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(LOCAL_CONTROL_OUTAGE_DRILL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(LOCAL_CONTROL_OUTAGE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(LOCAL_CONTROL_OUTAGE_DRILL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(LOCAL_CONTROL_OUTAGE_DRILL_ISO) > $(LOCAL_CONTROL_OUTAGE_DRILL_ISO).sha256
	@echo "ISO готов: $(LOCAL_CONTROL_OUTAGE_DRILL_ISO)"

$(NETWORK_PARTITION_DRILL_ISO): deps $(NETWORK_PARTITION_DRILL_KERNEL)
	rm -rf $(NETWORK_PARTITION_DRILL_ISO_ROOT)
	mkdir -p $(NETWORK_PARTITION_DRILL_ISO_ROOT)/boot/limine $(NETWORK_PARTITION_DRILL_ISO_ROOT)/EFI/BOOT
	cp $(NETWORK_PARTITION_DRILL_KERNEL) $(NETWORK_PARTITION_DRILL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(NETWORK_PARTITION_DRILL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(NETWORK_PARTITION_DRILL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(NETWORK_PARTITION_DRILL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(NETWORK_PARTITION_DRILL_ISO_ROOT) -o $(NETWORK_PARTITION_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(NETWORK_PARTITION_DRILL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(NETWORK_PARTITION_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(NETWORK_PARTITION_DRILL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(NETWORK_PARTITION_DRILL_ISO) > $(NETWORK_PARTITION_DRILL_ISO).sha256
	@echo "ISO готов: $(NETWORK_PARTITION_DRILL_ISO)"

$(TRUST_FAILURE_DRILL_ISO): deps $(TRUST_FAILURE_DRILL_KERNEL)
	rm -rf $(TRUST_FAILURE_DRILL_ISO_ROOT)
	mkdir -p $(TRUST_FAILURE_DRILL_ISO_ROOT)/boot/limine $(TRUST_FAILURE_DRILL_ISO_ROOT)/EFI/BOOT
	cp $(TRUST_FAILURE_DRILL_KERNEL) $(TRUST_FAILURE_DRILL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(TRUST_FAILURE_DRILL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(TRUST_FAILURE_DRILL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(TRUST_FAILURE_DRILL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(TRUST_FAILURE_DRILL_ISO_ROOT) -o $(TRUST_FAILURE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(TRUST_FAILURE_DRILL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(TRUST_FAILURE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(TRUST_FAILURE_DRILL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(TRUST_FAILURE_DRILL_ISO) > $(TRUST_FAILURE_DRILL_ISO).sha256
	@echo "ISO готов: $(TRUST_FAILURE_DRILL_ISO)"

$(UPDATE_FAILURE_DRILL_ISO): deps $(UPDATE_FAILURE_DRILL_KERNEL)
	rm -rf $(UPDATE_FAILURE_DRILL_ISO_ROOT)
	mkdir -p $(UPDATE_FAILURE_DRILL_ISO_ROOT)/boot/limine $(UPDATE_FAILURE_DRILL_ISO_ROOT)/EFI/BOOT
	cp $(UPDATE_FAILURE_DRILL_KERNEL) $(UPDATE_FAILURE_DRILL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(UPDATE_FAILURE_DRILL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(UPDATE_FAILURE_DRILL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(UPDATE_FAILURE_DRILL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(UPDATE_FAILURE_DRILL_ISO_ROOT) -o $(UPDATE_FAILURE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(UPDATE_FAILURE_DRILL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(UPDATE_FAILURE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(UPDATE_FAILURE_DRILL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(UPDATE_FAILURE_DRILL_ISO) > $(UPDATE_FAILURE_DRILL_ISO).sha256
	@echo "ISO готов: $(UPDATE_FAILURE_DRILL_ISO)"

$(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO): deps $(INTEGRATION_REPLAY_OUTAGE_DRILL_KERNEL)
	rm -rf $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO_ROOT)
	mkdir -p $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO_ROOT)/boot/limine $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO_ROOT)/EFI/BOOT
	cp $(INTEGRATION_REPLAY_OUTAGE_DRILL_KERNEL) $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO_ROOT) -o $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO) > $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO).sha256
	@echo "ISO готов: $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO)"

$(POWER_CYCLE_CONTINUITY_DRILL_ISO): deps $(POWER_CYCLE_CONTINUITY_DRILL_KERNEL)
	rm -rf $(POWER_CYCLE_CONTINUITY_DRILL_ISO_ROOT)
	mkdir -p $(POWER_CYCLE_CONTINUITY_DRILL_ISO_ROOT)/boot/limine $(POWER_CYCLE_CONTINUITY_DRILL_ISO_ROOT)/EFI/BOOT
	cp $(POWER_CYCLE_CONTINUITY_DRILL_KERNEL) $(POWER_CYCLE_CONTINUITY_DRILL_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(POWER_CYCLE_CONTINUITY_DRILL_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(POWER_CYCLE_CONTINUITY_DRILL_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(POWER_CYCLE_CONTINUITY_DRILL_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(POWER_CYCLE_CONTINUITY_DRILL_ISO_ROOT) -o $(POWER_CYCLE_CONTINUITY_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(POWER_CYCLE_CONTINUITY_DRILL_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(POWER_CYCLE_CONTINUITY_DRILL_ISO)
	python3 scripts/normalize-iso-gpt.py $(POWER_CYCLE_CONTINUITY_DRILL_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(POWER_CYCLE_CONTINUITY_DRILL_ISO) > $(POWER_CYCLE_CONTINUITY_DRILL_ISO).sha256
	@echo "ISO готов: $(POWER_CYCLE_CONTINUITY_DRILL_ISO)"

$(ENTROPY_ISO): deps $(ENTROPY_KERNEL)
	rm -rf $(ENTROPY_ISO_ROOT)
	mkdir -p $(ENTROPY_ISO_ROOT)/boot/limine $(ENTROPY_ISO_ROOT)/EFI/BOOT
	cp $(ENTROPY_KERNEL) $(ENTROPY_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf $(ENTROPY_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(ENTROPY_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(ENTROPY_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ENTROPY_ISO_ROOT) -o $(ENTROPY_ISO)
	python3 scripts/normalize-iso-gpt.py $(ENTROPY_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(ENTROPY_ISO)
	python3 scripts/normalize-iso-gpt.py $(ENTROPY_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(ENTROPY_ISO) > $(ENTROPY_ISO).sha256
	@echo "ISO готов: $(ENTROPY_ISO)"

$(MBEDTLS_TEST_SECRET_ISO): deps $(MBEDTLS_TEST_SECRET_KERNEL)
	rm -rf $(MBEDTLS_TEST_SECRET_ISO_ROOT)
	mkdir -p $(MBEDTLS_TEST_SECRET_ISO_ROOT)/boot/limine \
		$(MBEDTLS_TEST_SECRET_ISO_ROOT)/EFI/BOOT \
		$(MBEDTLS_TEST_SECRET_ISO_ROOT)/test-secrets
	cp $(MBEDTLS_TEST_SECRET_KERNEL) \
		$(MBEDTLS_TEST_SECRET_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf \
		$(MBEDTLS_TEST_SECRET_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin \
		$(MBEDTLS_TEST_SECRET_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(MBEDTLS_TEST_SECRET_ISO_ROOT)/EFI/BOOT/
	printf '%s\n' \
		'HFOC-MBEDTLS-TEST-SECRET 1552a388cd8bbd3e9028214d14fdd0f135b1f8b71ea7147010d35df7ddfb2c92 04a7df80aa46753c466eb4e9df8891161d4aaf9d2145deae79c27a640c7989f1' \
		>$(MBEDTLS_TEST_SECRET_ISO_ROOT)/test-secrets/TEST-SECRET-MANIFEST.txt
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(MBEDTLS_TEST_SECRET_ISO_ROOT) -o $(MBEDTLS_TEST_SECRET_ISO)
	python3 scripts/normalize-iso-gpt.py $(MBEDTLS_TEST_SECRET_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(MBEDTLS_TEST_SECRET_ISO)
	python3 scripts/normalize-iso-gpt.py $(MBEDTLS_TEST_SECRET_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(MBEDTLS_TEST_SECRET_ISO) > $(MBEDTLS_TEST_SECRET_ISO).sha256
	@echo "ISO готов: $(MBEDTLS_TEST_SECRET_ISO)"

$(MBEDTLS_TLS_QEMU_ISO): deps $(MBEDTLS_TLS_QEMU_KERNEL)
	rm -rf $(MBEDTLS_TLS_QEMU_ISO_ROOT)
	mkdir -p $(MBEDTLS_TLS_QEMU_ISO_ROOT)/boot/limine \
		$(MBEDTLS_TLS_QEMU_ISO_ROOT)/EFI/BOOT
	cp $(MBEDTLS_TLS_QEMU_KERNEL) \
		$(MBEDTLS_TLS_QEMU_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf \
		$(MBEDTLS_TLS_QEMU_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin \
		$(MBEDTLS_TLS_QEMU_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(MBEDTLS_TLS_QEMU_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(MBEDTLS_TLS_QEMU_ISO_ROOT) -o $(MBEDTLS_TLS_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(MBEDTLS_TLS_QEMU_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(MBEDTLS_TLS_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(MBEDTLS_TLS_QEMU_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(MBEDTLS_TLS_QEMU_ISO) > $(MBEDTLS_TLS_QEMU_ISO).sha256
	@echo "ISO готов: $(MBEDTLS_TLS_QEMU_ISO)"

$(HTTPS_SERVICE_QEMU_ISO): deps $(HTTPS_SERVICE_QEMU_KERNEL)
	rm -rf $(HTTPS_SERVICE_QEMU_ISO_ROOT)
	mkdir -p $(HTTPS_SERVICE_QEMU_ISO_ROOT)/boot/limine \
		$(HTTPS_SERVICE_QEMU_ISO_ROOT)/EFI/BOOT
	cp $(HTTPS_SERVICE_QEMU_KERNEL) \
		$(HTTPS_SERVICE_QEMU_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf \
		$(HTTPS_SERVICE_QEMU_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin \
		$(HTTPS_SERVICE_QEMU_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(HTTPS_SERVICE_QEMU_ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(HTTPS_SERVICE_QEMU_ISO_ROOT) -o $(HTTPS_SERVICE_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HTTPS_SERVICE_QEMU_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(HTTPS_SERVICE_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HTTPS_SERVICE_QEMU_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(HTTPS_SERVICE_QEMU_ISO) > $(HTTPS_SERVICE_QEMU_ISO).sha256
	@echo "ISO готов: $(HTTPS_SERVICE_QEMU_ISO)"

$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO): deps $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_KERNEL)
	rm -rf $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT)
	mkdir -p $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT)/boot/limine \
		$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT)/EFI/BOOT
	cp $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_KERNEL) \
		$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf \
		$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin \
		$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI \
		$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT)/EFI/BOOT/
	bash scripts/install-license-payload.sh \
		$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT)
	bash scripts/verify-license-payload.sh \
		$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO_ROOT) \
		-o $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO) > \
		$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO).sha256
	@echo "ISO готов: $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO)"

$(HTTPS_RESTART_SOAK_QEMU_ISO): deps $(HTTPS_RESTART_SOAK_QEMU_KERNEL)
	rm -rf $(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT)
	mkdir -p $(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT)/boot/limine \
		$(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT)/EFI/BOOT
	cp $(HTTPS_RESTART_SOAK_QEMU_KERNEL) \
		$(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT)/boot/hfoc-kernel.elf
	cp boot/limine.conf \
		$(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin \
		$(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI \
		$(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT)/EFI/BOOT/
	bash scripts/install-license-payload.sh \
		$(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT)
	bash scripts/verify-license-payload.sh \
		$(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT)/licenses
	xorriso -as mkisofs $(ISO_REPRODUCIBLE_FLAGS) -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(HTTPS_RESTART_SOAK_QEMU_ISO_ROOT) \
		-o $(HTTPS_RESTART_SOAK_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HTTPS_RESTART_SOAK_QEMU_ISO) "$(ISO_GPT_SEED)"
	$(LIMINE)/limine bios-install $(HTTPS_RESTART_SOAK_QEMU_ISO)
	python3 scripts/normalize-iso-gpt.py $(HTTPS_RESTART_SOAK_QEMU_ISO) "$(ISO_GPT_SEED)"
	sha256sum $(HTTPS_RESTART_SOAK_QEMU_ISO) > \
		$(HTTPS_RESTART_SOAK_QEMU_ISO).sha256
	@echo "ISO готов: $(HTTPS_RESTART_SOAK_QEMU_ISO)"

run: iso
	qemu-system-x86_64 $(QEMU_COMMON)

run-uefi: iso
	./scripts/run-uefi.sh

debug: iso
	qemu-system-x86_64 $(QEMU_COMMON) -S -s

run-trace: iso
	qemu-system-x86_64 $(QEMU_COMMON) \
		-d int,guest_errors -D build/qemu-interrupts.log

check:
	./scripts/check.sh

test-irqs: iso
	QEMU_MACHINE='$(QEMU_MACHINE)' QEMU_CPU='$(QEMU_CPU)' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-irqs.sh

test-panic: iso
	QEMU_MACHINE='$(QEMU_MACHINE)' QEMU_CPU='$(QEMU_CPU)' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-panic.sh

test-pagefault: iso
	QEMU_MACHINE='$(QEMU_MACHINE)' QEMU_CPU='$(QEMU_CPU)' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-pagefault.sh

test-doublefault: iso
	QEMU_MACHINE='$(QEMU_MACHINE)' QEMU_CPU='$(QEMU_CPU)' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-doublefault.sh

test-nmi: iso
	QEMU_MACHINE='$(QEMU_MACHINE)' QEMU_CPU='$(QEMU_CPU)' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-nmi.sh

test-acpi: iso
	QEMU_MACHINE='q35' QEMU_CPU='max' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-acpi.sh

test-vmm: iso
	QEMU_MACHINE='q35' QEMU_CPU='max,-apic' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-vmm.sh

test-vmm-as: $(VMM_AS_ISO) iso
	@echo "VMM-AS marker checks: before QEMU proof"
	@strings $(VMM_AS_KERNEL) | grep -Fq '[VMM-AS] address space map/activate/restore: OK' || { echo "FAIL: VMM-AS kernel missing success marker" >&2; exit 1; }
	@strings $(VMM_AS_ISO) | grep -Fq '[VMM-AS] address space map/activate/restore: OK' || { echo "FAIL: VMM-AS ISO missing success marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[VMM-AS] address space map/activate/restore: OK'; then echo "FAIL: production kernel contains VMM-AS marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[VMM-AS] address space map/activate/restore: OK'; then echo "FAIL: production ISO contains VMM-AS marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(VMM_AS_ISO)' \
		./scripts/test-qemu-vmm-as.sh
	@echo "VMM-AS marker checks: after QEMU proof"
	@strings $(VMM_AS_KERNEL) | grep -Fq '[VMM-AS] address space map/activate/restore: OK' || { echo "FAIL: VMM-AS kernel missing success marker after QEMU proof" >&2; exit 1; }
	@strings $(VMM_AS_ISO) | grep -Fq '[VMM-AS] address space map/activate/restore: OK' || { echo "FAIL: VMM-AS ISO missing success marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[VMM-AS] address space map/activate/restore: OK'; then echo "FAIL: production kernel contains VMM-AS marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[VMM-AS] address space map/activate/restore: OK'; then echo "FAIL: production ISO contains VMM-AS marker after QEMU proof" >&2; exit 1; fi
test-acpi-host: kernel/include/acpi.h kernel/src/acpi.c tests/acpi-madt.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ACPI_HOST_TEST \
		-Ikernel/include \
		kernel/src/acpi.c tests/acpi-madt.c \
		-o build/test-acpi-host -no-pie
	build/test-acpi-host
# ITEM-411: which reset mechanism the machine is rebooted through.  The decision
# is a pure function of the FADT's reset register, the PCI host bridge and the
# i8042 observation, so the acceptance host's shape is pinned here without a
# chipset, a keyboard controller or a reboot.
.PHONY: test-platform-reset-host
test-platform-reset-host: kernel/include/platform_reset.h \
		kernel/include/acpi.h \
		kernel/src/platform_reset.c \
		tests/platform-reset-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PLATFORM_RESET_HOST_TEST \
		-Ikernel/include \
		kernel/src/platform_reset.c tests/platform-reset-host.c \
		-o build/test-platform-reset-host -no-pie
	build/test-platform-reset-host
# ITEM-418: what the machine is asked to do when an operator reboots or powers
# it off.  The decision is a pure function of the FADT's PM1 control block and
# the DSDT's \_S5 object, and the walk over that object is host-testable too, so
# the acceptance host's shape is pinned without a chipset or a power button.
.PHONY: test-platform-power-host
test-platform-power-host: kernel/include/platform_power.h \
		kernel/include/record_line.h \
		kernel/src/platform_power.c \
		kernel/src/record_line.c \
		tests/platform-power-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PLATFORM_POWER_HOST_TEST \
		-Ikernel/include \
		kernel/src/platform_power.c kernel/src/record_line.c \
		tests/platform-power-host.c \
		-o build/test-platform-power-host -no-pie
	build/test-platform-power-host
test-vmm-host: kernel/include/vmm.h kernel/src/vmm.c tests/vmm-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VMM_HOST_TEST \
		-Ikernel/include \
		kernel/src/vmm.c tests/vmm-host.c \
		-o build/test-vmm-host -no-pie
	build/test-vmm-host

test-vmm-user-host: kernel/include/vmm.h kernel/src/vmm.c tests/vmm-user-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VMM_HOST_TEST \
		-Ikernel/include \
		kernel/src/vmm.c tests/vmm-user-host.c \
		-o build/test-vmm-user-host -no-pie
	build/test-vmm-user-host

test-pmm-host: kernel/include/pmm.h kernel/src/pmm.c tests/pmm-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PMM_HOST_TEST \
		-Ikernel/include \
		kernel/src/pmm.c tests/pmm-host.c \
		-o build/test-pmm-host -no-pie
	build/test-pmm-host

# ITEM-390: the shipped page cap is 1 TiB, so the over-cap path is driven with
# a reduced compile-time cap (256 MiB) against a synthetic map above it.
test-pmm-limited-host: kernel/include/pmm.h kernel/src/pmm.c \
		tests/pmm-limited-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PMM_LIMITED_HOST_TEST -DHFOC_PMM_MAX_PAGES='(1U << 16U)' \
		-Ikernel/include \
		kernel/src/pmm.c tests/pmm-limited-host.c \
		-o build/test-pmm-limited-host -no-pie
	build/test-pmm-limited-host

# ITEM-390: a synthetic fragmented machine above the legacy 16 GiB cap, driven
# at the shipped page cap.
test-pmm-highmem-host: kernel/include/pmm.h kernel/src/pmm.c \
		tests/pmm-highmem-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PMM_HIGHMEM_HOST_TEST \
		-Ikernel/include \
		kernel/src/pmm.c tests/pmm-highmem-host.c \
		-o build/test-pmm-highmem-host -no-pie
	build/test-pmm-highmem-host

# ITEM-393: the acceptance host's firmware map, driven with a guard over every
# span the map does not describe as usable RAM. The proof covers the bitmap
# placement, a whole tracked-set exhaustion and the region-boundary cases a
# contiguous allocation used to straddle.
test-pmm-pci-hole-host: kernel/include/pmm.h kernel/src/pmm.c \
		tests/pmm-pci-hole-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PMM_PCI_HOLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/pmm.c tests/pmm-pci-hole-host.c \
		-o build/test-pmm-pci-hole-host -no-pie
	build/test-pmm-pci-hole-host

# ITEM-393: the proofs kmain calls run on the stack the bootloader handed over,
# so their entry points must not allocate the boot stack. This measures each
# one's frame with the flags its QEMU kernel uses and fails above the budget:
# the storage-interruption target used to keep 63 KiB of fixtures in one frame
# and triple-faulted into a silent reboot when that frame reached the
# bootloader's page tables.
DRILL_STACK_BUDGET_BYTES := 4096

test-drill-stack-budget-host:
	bash tests/drill-stack-budget-host.sh '$(CC)' '$(CFLAGS)' $(DRILL_STACK_BUDGET_BYTES)

# ITEM-437: the budget proof measures a disassembly file, so a reader that
# stops early cannot close the pipe on its writer; this drill proves the race
# the old shape lost on an idle host is really gone.
.PHONY: test-drill-stack-budget-race-host
test-drill-stack-budget-race-host:
	bash tests/drill-stack-budget-race-host.sh

test-heap-host: kernel/include/heap.h kernel/src/heap.c tests/heap-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_HEAP_HOST_TEST \
		-Ikernel/include \
		kernel/src/heap.c tests/heap-host.c \
		-o build/test-heap-host -no-pie
	build/test-heap-host

test-pmm: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-pmm.sh

test-heap: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-heap.sh

test-cpu-host: kernel/include/cpu.h kernel/src/cpu.c \
		kernel/include/arch/x86_64/gdt.h kernel/src/arch/x86_64/gdt.c \
		kernel/include/arch/x86_64/tss.h kernel/src/arch/x86_64/tss.c \
		tests/cpu-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CPU_HOST_TEST \
		-Ikernel/include \
		kernel/src/cpu.c kernel/src/arch/x86_64/gdt.c \
		kernel/src/arch/x86_64/tss.c tests/cpu-host.c \
		-o build/test-cpu-host -no-pie
	build/test-cpu-host

test-context-host: kernel/include/context.h \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S tests/context-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CONTEXT_HOST_TEST \
		-Ikernel/include \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S tests/context-host.c \
		-o build/test-context-host -no-pie
	build/test-context-host

test-user-iretq-host: kernel/include/user.h kernel/src/user_entry.c \
		kernel/src/arch/x86_64/user_entry.S tests/user-iretq-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_USER_IRETQ_HOST_TEST \
		-Ikernel/include \
		kernel/src/user_entry.c \
		kernel/src/arch/x86_64/user_entry.S \
		tests/user-iretq-host.c \
		-o build/test-user-iretq-host -no-pie
	build/test-user-iretq-host

test-syscall-host: kernel/include/syscall.h kernel/include/user.h \
		kernel/include/capability.h \
		kernel/src/capability.c \
		kernel/src/syscall.c kernel/src/syscall_entry.S tests/syscall-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SYSCALL_HOST_TEST \
		-Ikernel/include \
		kernel/src/capability.c \
		kernel/src/syscall.c \
		kernel/src/syscall_entry.S \
		tests/syscall-host.c \
		-o build/test-syscall-host -no-pie
	build/test-syscall-host

test-user-init-process-host: kernel/include/process.h kernel/include/user_init.h \
		kernel/include/descriptor.h kernel/include/context.h \
		kernel/include/capability.h \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c \
		kernel/src/user_init_qemu.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/user-init-process-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PROCESS_HOST_TEST -DHFOC_USER_INIT_HOST_TEST \
		-Ikernel/include \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c \
		kernel/src/user_init_qemu.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/user-init-process-host.c \
		-o build/test-user-init-process-host -no-pie
	build/test-user-init-process-host

test-syscall: $(SYSCALL_ISO) iso
	@echo "Syscall ABI marker checks: before QEMU proof"
	@strings $(SYSCALL_KERNEL) | grep -Fq '[SYSCALL] proof init' || { echo "FAIL: syscall kernel missing proof marker" >&2; exit 1; }
	@strings $(SYSCALL_ISO) | grep -Fq '[SYSCALL] proof init' || { echo "FAIL: syscall ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SYSCALL] proof init'; then echo "FAIL: production kernel contains syscall marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SYSCALL] proof init'; then echo "FAIL: production ISO contains syscall marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SYSCALL_ISO)' \
		./scripts/test-qemu-syscall.sh
	@echo "Syscall ABI marker checks: after QEMU proof"
	@strings $(SYSCALL_KERNEL) | grep -Fq '[SYSCALL] proof OK' || { echo "FAIL: syscall kernel missing proof OK marker" >&2; exit 1; }
	@strings $(SYSCALL_ISO) | grep -Fq '[SYSCALL] proof OK' || { echo "FAIL: syscall ISO missing proof OK marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SYSCALL] proof init'; then echo "FAIL: production kernel contains syscall marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SYSCALL] proof init'; then echo "FAIL: production ISO contains syscall marker after proof" >&2; exit 1; fi

test-user-init: $(USER_INIT_ISO) iso
	@echo "User init marker checks: before QEMU proof"
	@strings $(USER_INIT_KERNEL) | grep -Fq '[USER-INIT] proof init' || { echo "FAIL: user init kernel missing proof marker" >&2; exit 1; }
	@strings $(USER_INIT_ISO) | grep -Fq '[USER-INIT] proof init' || { echo "FAIL: user init ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[USER-INIT] proof init'; then echo "FAIL: production kernel contains user init marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[USER-INIT] proof init'; then echo "FAIL: production ISO contains user init marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(USER_INIT_ISO)' \
		./scripts/test-qemu-user-init.sh
	@echo "User init marker checks: after QEMU proof"
	@strings $(USER_INIT_KERNEL) | grep -Fq '[USER-INIT] proof OK' || { echo "FAIL: user init kernel missing proof OK marker" >&2; exit 1; }
	@strings $(USER_INIT_ISO) | grep -Fq '[USER-INIT] proof OK' || { echo "FAIL: user init ISO missing proof OK marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[USER-INIT] proof init'; then echo "FAIL: production kernel contains user init marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[USER-INIT] proof init'; then echo "FAIL: production ISO contains user init marker after proof" >&2; exit 1; fi

test-boot-policy-regression: $(BOOT_POLICY_ISO) iso
	@echo "Boot policy marker checks: before QEMU proof"
	@strings $(BOOT_POLICY_KERNEL) | grep -Fq '[BOOT-POLICY] proof init' || { echo "FAIL: boot policy kernel missing proof marker" >&2; exit 1; }
	@strings $(BOOT_POLICY_ISO) | grep -Fq '[BOOT-POLICY] proof init' || { echo "FAIL: boot policy ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BOOT-POLICY]'; then echo "FAIL: production kernel contains boot policy marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BOOT-POLICY]'; then echo "FAIL: production ISO contains boot policy marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(BOOT_POLICY_ISO)' \
		./scripts/test-qemu-boot-policy-regression.sh
	@echo "Boot policy marker checks: after QEMU proof"
	@strings $(BOOT_POLICY_KERNEL) | grep -Fq '[BOOT-POLICY] proof OK' || { echo "FAIL: boot policy kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(BOOT_POLICY_ISO) | grep -Fq '[BOOT-POLICY] proof OK' || { echo "FAIL: boot policy ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BOOT-POLICY]'; then echo "FAIL: production kernel contains boot policy marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BOOT-POLICY]'; then echo "FAIL: production ISO contains boot policy marker after proof" >&2; exit 1; fi

test-block-device: $(BLOCK_DEVICE_ISO) iso
	@echo "Block-device marker checks: before QEMU proof"
	@strings $(BLOCK_DEVICE_KERNEL) | grep -Fq '[BLOCK-DEVICE] proof init' || { echo "FAIL: block-device kernel missing proof marker" >&2; exit 1; }
	@strings $(BLOCK_DEVICE_ISO) | grep -Fq '[BLOCK-DEVICE] proof init' || { echo "FAIL: block-device ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BLOCK-DEVICE]'; then echo "FAIL: production kernel contains block-device marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BLOCK-DEVICE]'; then echo "FAIL: production ISO contains block-device marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(BLOCK_DEVICE_ISO)' \
		./scripts/test-qemu-block-device.sh
	@echo "Block-device marker checks: after QEMU proof"
	@strings $(BLOCK_DEVICE_KERNEL) | grep -Fq '[BLOCK-DEVICE] proof OK' || { echo "FAIL: block-device kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(BLOCK_DEVICE_ISO) | grep -Fq '[BLOCK-DEVICE] proof OK' || { echo "FAIL: block-device ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BLOCK-DEVICE]'; then echo "FAIL: production kernel contains block-device marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BLOCK-DEVICE]'; then echo "FAIL: production ISO contains block-device marker after proof" >&2; exit 1; fi

test-data-region: $(DATA_REGION_ISO) iso
	@echo "Data-region marker checks: before QEMU proof"
	@strings $(DATA_REGION_KERNEL) | grep -Fq '[DATA-REGION] proof init' || { echo "FAIL: data-region kernel missing proof marker" >&2; exit 1; }
	@strings $(DATA_REGION_ISO) | grep -Fq '[DATA-REGION] proof init' || { echo "FAIL: data-region ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[DATA-REGION]'; then echo "FAIL: production kernel contains data-region marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[DATA-REGION]'; then echo "FAIL: production ISO contains data-region marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(DATA_REGION_ISO)' \
		./scripts/test-qemu-data-region.sh
	@echo "Data-region marker checks: after QEMU proof"
	@strings $(DATA_REGION_KERNEL) | grep -Fq '[DATA-REGION] proof OK' || { echo "FAIL: data-region kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(DATA_REGION_ISO) | grep -Fq '[DATA-REGION] proof OK' || { echo "FAIL: data-region ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[DATA-REGION]'; then echo "FAIL: production kernel contains data-region marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[DATA-REGION]'; then echo "FAIL: production ISO contains data-region marker after proof" >&2; exit 1; fi

test-block-cache: $(BLOCK_CACHE_ISO) iso
	@echo "Block-cache marker checks: before QEMU proof"
	@strings $(BLOCK_CACHE_KERNEL) | grep -Fq '[BLOCK-CACHE] proof init' || { echo "FAIL: block-cache kernel missing proof marker" >&2; exit 1; }
	@strings $(BLOCK_CACHE_ISO) | grep -Fq '[BLOCK-CACHE] proof init' || { echo "FAIL: block-cache ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BLOCK-CACHE]'; then echo "FAIL: production kernel contains block-cache marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BLOCK-CACHE]'; then echo "FAIL: production ISO contains block-cache marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(BLOCK_CACHE_ISO)' \
		./scripts/test-qemu-block-cache.sh
	@echo "Block-cache marker checks: after QEMU proof"
	@strings $(BLOCK_CACHE_KERNEL) | grep -Fq '[BLOCK-CACHE] proof OK' || { echo "FAIL: block-cache kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(BLOCK_CACHE_ISO) | grep -Fq '[BLOCK-CACHE] proof OK' || { echo "FAIL: block-cache ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BLOCK-CACHE]'; then echo "FAIL: production kernel contains block-cache marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BLOCK-CACHE]'; then echo "FAIL: production ISO contains block-cache marker after proof" >&2; exit 1; fi

test-hfocfs-qemu: $(HFOCFS_QEMU_ISO) iso
	@echo "HFOCFS marker checks: before QEMU proof"
	@strings $(HFOCFS_QEMU_KERNEL) | grep -Fq '[HFOCFS] proof init' || { echo "FAIL: hfocfs kernel missing proof marker" >&2; exit 1; }
	@strings $(HFOCFS_QEMU_ISO) | grep -Fq '[HFOCFS] proof init' || { echo "FAIL: hfocfs ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HFOCFS]'; then echo "FAIL: production kernel contains hfocfs QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HFOCFS]'; then echo "FAIL: production ISO contains hfocfs QEMU marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HFOCFS_QEMU_ISO)' \
		./scripts/test-qemu-hfocfs.sh
	@strings $(HFOCFS_QEMU_KERNEL) | grep -Fq '[HFOCFS] proof OK' || { echo "FAIL: hfocfs kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(HFOCFS_QEMU_ISO) | grep -Fq '[HFOCFS] proof OK' || { echo "FAIL: hfocfs ISO missing proof OK marker after QEMU proof" >&2; exit 1; }

test-hfocfs-workspace-qemu: $(HFOCFS_WORKSPACE_QEMU_ISO) iso
	@echo "HFOCFS workspace marker checks: before QEMU proof"
	@strings $(HFOCFS_WORKSPACE_QEMU_KERNEL) | grep -Fq '[WORKSPACE-QEMU] proof init' || { echo "FAIL: workspace kernel missing proof marker" >&2; exit 1; }
	@strings $(HFOCFS_WORKSPACE_QEMU_ISO) | grep -Fq '[WORKSPACE-QEMU] proof init' || { echo "FAIL: workspace ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[WORKSPACE-QEMU]'; then echo "FAIL: production kernel contains workspace QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[WORKSPACE-QEMU]'; then echo "FAIL: production ISO contains workspace QEMU marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HFOCFS_WORKSPACE_QEMU_ISO)' \
		./scripts/test-qemu-hfocfs-workspace.sh
	@strings $(HFOCFS_WORKSPACE_QEMU_KERNEL) | grep -Fq '[WORKSPACE-QEMU] proof OK' || { echo "FAIL: workspace kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(HFOCFS_WORKSPACE_QEMU_ISO) | grep -Fq '[WORKSPACE-QEMU] proof OK' || { echo "FAIL: workspace ISO missing proof OK marker after QEMU proof" >&2; exit 1; }

test-config-journal: $(CONFIG_JOURNAL_ISO) iso
	@echo "Config-journal marker checks: before QEMU proof"
	@strings $(CONFIG_JOURNAL_KERNEL) | grep -Fq '[CONFIG-JOURNAL] proof init' || { echo "FAIL: config-journal kernel missing proof marker" >&2; exit 1; }
	@strings $(CONFIG_JOURNAL_ISO) | grep -Fq '[CONFIG-JOURNAL] proof init' || { echo "FAIL: config-journal ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[CONFIG-JOURNAL]'; then echo "FAIL: production kernel contains config-journal marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[CONFIG-JOURNAL]'; then echo "FAIL: production ISO contains config-journal marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(CONFIG_JOURNAL_ISO)' \
		./scripts/test-qemu-config-journal.sh
	@echo "Config-journal marker checks: after QEMU proof"
	@strings $(CONFIG_JOURNAL_KERNEL) | grep -Fq '[CONFIG-JOURNAL] proof OK' || { echo "FAIL: config-journal kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(CONFIG_JOURNAL_ISO) | grep -Fq '[CONFIG-JOURNAL] proof OK' || { echo "FAIL: config-journal ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[CONFIG-JOURNAL]'; then echo "FAIL: production kernel contains config-journal marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[CONFIG-JOURNAL]'; then echo "FAIL: production ISO contains config-journal marker after proof" >&2; exit 1; fi

.PHONY: test-physical-storage-qemu
test-physical-storage-qemu: $(PHYSICAL_STORAGE_ISO) iso
	@echo "Physical-storage marker checks: before QEMU proof"
	@strings $(PHYSICAL_STORAGE_KERNEL) | grep -Fq '[PHYSICAL-STORAGE] proof init' || { echo "FAIL: physical-storage kernel missing proof marker" >&2; exit 1; }
	@strings $(PHYSICAL_STORAGE_ISO) | grep -Fq '[PHYSICAL-STORAGE] proof init' || { echo "FAIL: physical-storage ISO missing proof marker" >&2; exit 1; }
	@strings $(PHYSICAL_STORAGE_KERNEL) | grep -Fq '[AHCI] proof init' || { echo "FAIL: physical-storage kernel missing AHCI proof marker" >&2; exit 1; }
	@strings $(PHYSICAL_STORAGE_ISO) | grep -Fq '[AHCI] proof init' || { echo "FAIL: physical-storage ISO missing AHCI proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PHYSICAL-STORAGE]'; then echo "FAIL: production kernel contains physical-storage marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PHYSICAL-STORAGE]'; then echo "FAIL: production ISO contains physical-storage marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[AHCI]'; then echo "FAIL: production kernel contains AHCI proof marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[AHCI]'; then echo "FAIL: production ISO contains AHCI proof marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(PHYSICAL_STORAGE_ISO)' \
		./scripts/test-qemu-physical-storage.sh
	@echo "Physical-storage marker checks: after QEMU proof"
	@strings $(PHYSICAL_STORAGE_KERNEL) | grep -Fq '[PHYSICAL-STORAGE] proof OK' || { echo "FAIL: physical-storage kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(PHYSICAL_STORAGE_ISO) | grep -Fq '[PHYSICAL-STORAGE] proof OK' || { echo "FAIL: physical-storage ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(PHYSICAL_STORAGE_KERNEL) | grep -Fq '[AHCI] proof OK' || { echo "FAIL: physical-storage kernel missing AHCI proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(PHYSICAL_STORAGE_ISO) | grep -Fq '[AHCI] proof OK' || { echo "FAIL: physical-storage ISO missing AHCI proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PHYSICAL-STORAGE]'; then echo "FAIL: production kernel contains physical-storage marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PHYSICAL-STORAGE]'; then echo "FAIL: production ISO contains physical-storage marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[AHCI]'; then echo "FAIL: production kernel contains AHCI proof marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[AHCI]'; then echo "FAIL: production ISO contains AHCI proof marker after proof" >&2; exit 1; fi

.PHONY: test-physical-sata-acceptance-qemu
test-physical-sata-acceptance-qemu: $(PHYSICAL_SATA_ACCEPTANCE_ISO) iso
	@echo "Physical SATA acceptance marker checks: before QEMU proof"
	@strings $(PHYSICAL_SATA_ACCEPTANCE_KERNEL) | grep -Fq '[AHCI] physical evidence proof OK' || { echo "FAIL: physical SATA kernel missing physical evidence marker" >&2; exit 1; }
	@strings $(PHYSICAL_SATA_ACCEPTANCE_ISO) | grep -Fq '[AHCI] physical evidence proof OK' || { echo "FAIL: physical SATA ISO missing physical evidence marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[AHCI] physical evidence proof OK'; then echo "FAIL: production kernel contains physical SATA marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[AHCI] physical evidence proof OK'; then echo "FAIL: production ISO contains physical SATA marker" >&2; exit 1; fi
	@echo "Physical SATA acceptance regression: baseline q35"
	QEMU_MACHINE='q35' QEMU_SMP='1' QEMU_LABEL='baseline' \
		IMAGE='$(PHYSICAL_SATA_ACCEPTANCE_ISO)' \
		./scripts/test-qemu-physical-sata-acceptance.sh
	@echo "Physical SATA acceptance regression: no PS/2 controller"
	QEMU_MACHINE='q35' QEMU_SMP='1' QEMU_LABEL='no-ps2' \
		QEMU_MACHINE_OPTIONS='i8042=off' \
		IMAGE='$(PHYSICAL_SATA_ACCEPTANCE_ISO)' \
		./scripts/test-qemu-physical-sata-acceptance.sh
	@echo "Physical SATA acceptance regression: no PS/2 plus PCIe bridge"
	QEMU_MACHINE='q35' QEMU_SMP='1' QEMU_LABEL='no-ps2-bridge' \
		QEMU_MACHINE_OPTIONS='i8042=off' \
		QEMU_PCIE_ROOT_PORT='1' \
		IMAGE='$(PHYSICAL_SATA_ACCEPTANCE_ISO)' \
		./scripts/test-qemu-physical-sata-acceptance.sh
	@echo "Physical SATA acceptance warm reset proof: no PS/2"
	IMAGE='$(PHYSICAL_SATA_ACCEPTANCE_ISO)' \
		./scripts/test-qemu-physical-sata-warm-reset.sh
	@echo "Physical SATA acceptance marker checks: after QEMU proof"
	@strings $(PHYSICAL_SATA_ACCEPTANCE_KERNEL) | grep -Fq '[AHCI] physical evidence proof OK' || { echo "FAIL: physical SATA kernel missing physical evidence marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[AHCI] physical evidence proof OK'; then echo "FAIL: production kernel contains physical SATA marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[AHCI] physical evidence proof OK'; then echo "FAIL: production ISO contains physical SATA marker after proof" >&2; exit 1; fi

.PHONY: test-production-installer-qemu
# ITEM-488: the installer's display through the real UEFI path.  The proof
# boots the installer medium twice through OVMF - once with a display device,
# where the firmware offers a GOP framebuffer, and once with none, where it does
# not - and reads the `[DISPLAY]` reading and the foundation screen from the
# serial stream.  It is a firmware proof, not a physical one: physical GPU
# acceptance stays NOT TESTED.
# ITEM-588: the update proof on a legacy-BIOS install.  Four runs: an installed
# appliance with no settings is updated and keeps its (default) state, one with
# the installer's language keeps it, a moved persistence range is refused with
# writes=0, and a store the reader cannot load is refused the same way.
.PHONY: test-production-installer-update-qemu
test-production-installer-update-qemu: $(PRODUCTION_INSTALLER_UPDATE_ISO) \
		$(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO) \
		$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO) \
		$(PRODUCTION_INSTALLER_LANGUAGE_ISO) \
		$(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO) \
		$(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO) \
		$(KERNEL)
	INSTALL_IMAGE='$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO)' \
	LANGUAGE_IMAGE='$(PRODUCTION_INSTALLER_LANGUAGE_ISO)' \
	UPDATE_IMAGE='$(PRODUCTION_INSTALLER_UPDATE_ISO)' \
	SSH_KEY_UPDATE_IMAGE='$(PRODUCTION_INSTALLER_SSH_KEY_UPDATE_ISO)' \
	CAPACITY_UPDATE_IMAGE='$(PRODUCTION_INSTALLER_CAPACITY_UPDATE_ISO)' \
	OVERSIZED_UPDATE_IMAGE='$(PRODUCTION_INSTALLER_OVERSIZED_UPDATE_ISO)' \
	CAPACITY_CONFIRM='$(HFOC_INSTALL_MEDIA_CONFIRM)' \
	CAPACITY_TARGET='$(HFOC_INSTALL_MEDIA_TARGET)' \
	CAPACITY_ADDRESS='$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_ADDRESS)' \
	CAPACITY_FORWARD_TARGET='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET)' \
	CAPACITY_FORWARD_CLIENTS='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS),10.0.2.3,10.0.2.4,10.0.2.5' \
	SSH_KEY_FINGERPRINT='$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE_CONFIRM)' \
	PRODUCT_KERNEL='$(KERNEL)' EXPECT_BUILD='$(BUILD_ID)' \
	./scripts/test-qemu-production-install-flow-update.sh

# ITEM-637: the installed disk keeps an owner file across a reboot and the
# state-preserving update.  The proof installs from the management medium,
# applies the management-scope test-key update, and drives the real production
# SSH listener on the installed image with a pinned host key.  It is a focused
# QEMU proof, not a Toshiba hardware proof.
.PHONY: test-installed-persistence-qemu
test-installed-persistence-qemu: $(PRODUCTION_INSTALLER_MANAGEMENT_ISO) \
		$(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO) \
		$(KERNEL)
	INSTALL_IMAGE='$(PRODUCTION_INSTALLER_MANAGEMENT_ISO)' \
	UPDATE_IMAGE='$(PRODUCTION_INSTALLER_MANAGEMENT_SSH_KEY_UPDATE_ISO)' \
	SSH_KEY_FINGERPRINT='$(PRODUCTION_INSTALLER_SSH_KEY_FIXTURE_CONFIRM)' \
	PRODUCT_KERNEL='$(KERNEL)' EXPECT_BUILD='$(BUILD_ID)' \
	./scripts/test-qemu-installed-persistence.sh

.PHONY: test-installer-display-qemu
test-installer-display-qemu: $(PRODUCTION_INSTALLER_ISO)
	./scripts/test-qemu-installer-display.sh

# ITEM-492: the machine's NICs, discovered and described on machines that differ
# only in their network hardware - an unsupported controller, a supported one
# beside it, and no controller at all.
.PHONY: test-nic-discovery-qemu
test-nic-discovery-qemu: production-installer-authorised-media
	./scripts/test-qemu-nic-discovery.sh
# ITEM-490: the operator entry's mandatory storage screen - the screen a person
# reads, the disk they name, the confirmation they type, and the disk that is
# written because of it.
.PHONY: test-installer-disk-screen-qemu
test-installer-disk-screen-qemu: production-installer-console-media
	./scripts/test-qemu-installer-disk-screen.sh

# ITEM-598: a real UEFI install and two installed boots on a CPU profile with
# neither RDRAND nor RDSEED.  The proof creates its personalised media and seed
# locally, never writes them to CI artifacts, and requires the normal account
# login instead of the recovery-only `entropy seed` command.
.PHONY: test-host-provisioned-entropy-qemu
test-host-provisioned-entropy-qemu: $(PRODUCTION_INSTALLER_KERNEL) \
		$(KERNEL) | deps
	./scripts/test-qemu-host-provisioned-entropy.sh

test-production-installer-qemu: $(PRODUCTION_INSTALLER_ISO) iso \
		test-installed-boot-disk-host \
		production-installer-authorised-media \
		production-installer-whole-disk-media \
		production-installer-management-media \
		production-installer-network-media \
		production-installer-forward-media \
		production-installer-language-media \
		production-installer-console-media
	@echo "Production-installer marker checks: before QEMU proof"
	@strings $(PRODUCTION_INSTALLER_KERNEL) | grep -Fq '[PRODUCTION-INSTALLER] proof init' || { echo "FAIL: production-installer kernel missing proof marker" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_ISO) | grep -Fq '[PRODUCTION-INSTALLER] proof init' || { echo "FAIL: production-installer ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PRODUCTION-INSTALLER] proof init'; then echo "FAIL: production kernel contains production-installer proof marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PRODUCTION-INSTALLER] proof init'; then echo "FAIL: production ISO contains production-installer proof marker" >&2; exit 1; fi
	@echo "ITEM-399 two-kernel medium checks: the ISO boots the installer, the ESP gets the product kernel"
	@cmp -s $(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT)/boot/hfoc-installer-kernel.elf $(PRODUCTION_INSTALLER_KERNEL) || { echo "FAIL: the authorised medium does not carry the installer kernel" >&2; exit 1; }
	@cmp -s $(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT)/boot/hfoc-kernel.elf $(KERNEL) || { echo "FAIL: the authorised medium does not carry the product kernel the installer stages" >&2; exit 1; }
	@if strings $(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT)/boot/hfoc-kernel.elf | grep -Fq '[PRODUCTION-INSTALLER] proof init'; then echo "FAIL: the medium's installed kernel carries the installer proof marker" >&2; exit 1; fi
	@grep -Fq 'path: boot():/boot/hfoc-installer-kernel.elf' $(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT)/boot/limine/limine.conf || { echo "FAIL: the medium's own entries do not boot the installer kernel" >&2; exit 1; }
	@grep -Fq 'module_path: boot():/boot/hfoc-kernel.elf' $(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT)/boot/limine/limine.conf || { echo "FAIL: the medium does not pass the installed kernel as a module" >&2; exit 1; }
	@grep -Fq 'cmdline: hfoc.management=local-only' $(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT)/boot/hfoc-installed.conf || { echo "FAIL: the authorised medium's installed configuration does not state the local scope" >&2; exit 1; }
	@grep -Fq 'cmdline: hfoc.management=management-interface' $(PRODUCTION_INSTALLER_MANAGEMENT_ISO_ROOT)/boot/hfoc-installed.conf || { echo "FAIL: the management medium's installed configuration does not state the management scope" >&2; exit 1; }
	@grep -Fq 'cmdline: hfoc.management=management-interface hfoc.network=static hfoc.address=$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_ADDRESS) hfoc.gateway=$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_GATEWAY) hfoc.dns=$(HFOC_INSTALL_MEDIA_NETWORK_PROOF_DNS)' $(PRODUCTION_INSTALLER_NETWORK_ISO_ROOT)/boot/hfoc-installed.conf || { echo "FAIL: the network medium's installed configuration does not state the declared address" >&2; exit 1; }
	@grep -Fq 'cmdline: hfoc.management=management-interface hfoc.forward=on hfoc.forward.target=$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET) hfoc.forward.target-port=$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT) hfoc.forward.listen-port=$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT) hfoc.forward.clients=$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS)' $(PRODUCTION_INSTALLER_FORWARD_ISO_ROOT)/boot/hfoc-installed.conf || { echo "FAIL: the forward medium's installed configuration does not state the declared forward" >&2; exit 1; }
	@grep -Fq 'HFOC-INSTALL-FORWARD-CLIENTS=$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS)' $(PRODUCTION_INSTALLER_FORWARD_ISO_ROOT)/boot/hfoc-install.token || { echo "FAIL: the forward medium's token does not carry the declared clients" >&2; exit 1; }
	@grep -Fq 'HFOC-INSTALL-LANGUAGE=ru' $(PRODUCTION_INSTALLER_LANGUAGE_ISO_ROOT)/boot/hfoc-install.token || { echo "FAIL: the language medium's token does not carry the declared language" >&2; exit 1; }
	@if grep -Fq 'HFOC-INSTALL-LANGUAGE' $(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT)/boot/hfoc-install.token; then echo "FAIL: a medium without a language declaration carries the directive" >&2; exit 1; fi
	@echo "ITEM-394 install-flow marker checks: before QEMU proof"
	@strings $(PRODUCTION_INSTALLER_KERNEL) | grep -Fq '[PRODUCTION-INSTALL] media sealed=VERIFIED' || { echo "FAIL: production-installer kernel missing install-flow engine" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_ISO) | grep -Fq 'hfoc.install=unattended' || { echo "FAIL: production-installer ISO missing the install entry" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_ISO) | grep -Fq 'hfoc-install.man' || { echo "FAIL: production-installer ISO missing the sealed media manifest" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PRODUCTION-INSTALL] media sealed=VERIFIED'; then echo "FAIL: production kernel contains the install-flow engine" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PRODUCTION-INSTALL] media sealed=VERIFIED'; then echo "FAIL: production ISO contains the install-flow engine" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(PRODUCTION_INSTALLER_ISO)' \
		./scripts/test-qemu-production-installer.sh
	# ITEM-590: the installed system's own telemetry fills the reserved journal
	# before the operator's command (512 slots), and the proof then saves twice
	# on the recovered journal.  That is one bounded wait more than the proof
	# used to need, so its own deadline rises with it instead of turning a
	# loaded runner into a false timing failure.
	@echo "ITEM-394 install-flow QEMU proof: ordinary boot of the installer medium"
	QEMU_MACHINE='q35' QEMU_SMP='1' \
		HFOC_INSTALL_FLOW_TEST_TIMEOUT='900' \
		PLAIN_IMAGE='$(PRODUCTION_INSTALLER_ISO)' \
		AUTHORISED_IMAGE='$(PRODUCTION_INSTALLER_AUTHORISED_ISO)' \
		AUTHORISED_ISO_ROOT='$(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT)' \
		MANAGEMENT_IMAGE='$(PRODUCTION_INSTALLER_MANAGEMENT_ISO)' \
		MANAGEMENT_ISO_ROOT='$(PRODUCTION_INSTALLER_MANAGEMENT_ISO_ROOT)' \
		NETWORK_IMAGE='$(PRODUCTION_INSTALLER_NETWORK_ISO)' \
		NETWORK_ISO_ROOT='$(PRODUCTION_INSTALLER_NETWORK_ISO_ROOT)' \
		FORWARD_IMAGE='$(PRODUCTION_INSTALLER_FORWARD_ISO)' \
		FORWARD_ISO_ROOT='$(PRODUCTION_INSTALLER_FORWARD_ISO_ROOT)' \
		LANGUAGE_IMAGE='$(PRODUCTION_INSTALLER_LANGUAGE_ISO)' \
		LANGUAGE_ISO_ROOT='$(PRODUCTION_INSTALLER_LANGUAGE_ISO_ROOT)' \
		HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET)' \
		HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT)' \
		HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT)' \
		HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS)' \
		WHOLE_DISK_IMAGE='$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO)' \
		WHOLE_DISK_ISO_ROOT='$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO_ROOT)' \
		./scripts/test-qemu-production-install-flow.sh
	@echo "ITEM-398 preserve-flow QEMU proof: RED, GREEN and the installed boot"
	QEMU_MACHINE='q35' QEMU_SMP='1' \
		PRESERVE_IMAGE='$(PRODUCTION_INSTALLER_AUTHORISED_ISO)' \
		WHOLE_DISK_IMAGE='$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO)' \
		CONSOLE_IMAGE='$(PRODUCTION_INSTALLER_CONSOLE_ISO)' \
		./scripts/test-qemu-production-install-flow-preserve.sh
	@echo "Production-installer marker checks: after QEMU proof"
	@strings $(PRODUCTION_INSTALLER_KERNEL) | grep -Fq '[PRODUCTION-INSTALLER] proof OK' || { echo "FAIL: production-installer kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_ISO) | grep -Fq '[PRODUCTION-INSTALLER] proof OK' || { echo "FAIL: production-installer ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@for marker in \
		'unverified media no-write proof OK' \
		'unsupported hardware no-write proof OK' \
		'layout contract proof OK' \
		'uninstall dry-run preview proof OK' \
		'uninstall previous-OS restoration proof OK' \
		'foreign disk prepared' \
		'preserved table proof OK' \
		'dual-boot refusal proof OK' \
		'shared ESP reuse proof OK' \
		'boot menu proof OK'; do \
		strings $(PRODUCTION_INSTALLER_KERNEL) | grep -Fq "[PRODUCTION-INSTALLER] $$marker" || { echo "FAIL: production-installer kernel missing marker: $$marker" >&2; exit 1; }; \
		strings $(PRODUCTION_INSTALLER_ISO) | grep -Fq "[PRODUCTION-INSTALLER] $$marker" || { echo "FAIL: production-installer ISO missing marker: $$marker" >&2; exit 1; }; \
	done
	@strings $(PRODUCTION_INSTALLER_KERNEL) | grep -Fq '[INSTALLER-LAYOUT] plan mode=' || { echo "FAIL: production-installer kernel missing layout record" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_ISO) | grep -Fq '[INSTALLER-LAYOUT] plan mode=' || { echo "FAIL: production-installer ISO missing layout record" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_KERNEL) | grep -Fq '[INSTALLER-PRESERVE] plan mode=' || { echo "FAIL: production-installer kernel missing preservation record" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_ISO) | grep -Fq '[INSTALLER-PRESERVE] plan mode=' || { echo "FAIL: production-installer ISO missing preservation record" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_KERNEL) | grep -Fq '[INSTALLER-BOOT-MENU] config-begin' || { echo "FAIL: production-installer kernel missing boot menu record" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_ISO) | grep -Fq '[INSTALLER-BOOT-MENU] config-begin' || { echo "FAIL: production-installer ISO missing boot menu record" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PRODUCTION-INSTALLER] proof init'; then echo "FAIL: production kernel contains production-installer proof marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PRODUCTION-INSTALLER] proof init'; then echo "FAIL: production ISO contains production-installer proof marker after proof" >&2; exit 1; fi
	@echo "Installed-disk boot proof: production install, then boot from the disk"
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-INSTALL] boot-entry ' || { echo "FAIL: production kernel missing the installed boot-entry record" >&2; exit 1; }
	@strings $(PRODUCTION_INSTALLER_KERNEL) | grep -Fq '[PRODUCTION-INSTALL] boot-entry ' || { echo "FAIL: production-installer kernel missing the installed boot-entry record" >&2; exit 1; }
	@strings $(KERNEL) | grep -Fq 'EFI_FAT32_ESP_PATH_CONFLICT' || { echo "FAIL: production kernel missing the ESP payload-set bounds" >&2; exit 1; }
	KERNEL='$(KERNEL)' ISO_ROOT='build/iso_root' \
		BUILDER='build/test-installed-boot-disk-host' \
		./scripts/test-qemu-installed-boot.sh
	@echo "Installed-disk boot proof: after QEMU boot from the disk"
	@strings $(KERNEL) | grep -Fq 'REMOVABLE_MEDIA_PATH' || { echo "FAIL: production kernel missing the removable-media mechanism record" >&2; exit 1; }

.PHONY: test-production-boot-chain-qemu
test-production-boot-chain-qemu: $(BOOT_CHAIN_ISO) iso
	@echo "Boot-chain marker checks: before QEMU proof"
	@strings $(BOOT_CHAIN_KERNEL) | grep -Fq '[BOOT-CHAIN] proof init' || { echo "FAIL: boot-chain kernel missing proof init marker" >&2; exit 1; }
	@strings $(BOOT_CHAIN_ISO) | grep -Fq '[BOOT-CHAIN] proof init' || { echo "FAIL: boot-chain ISO missing proof init marker" >&2; exit 1; }
	@strings $(KERNEL) | grep -Fq '[BOOT-CHAIN] verdict=' || { echo "FAIL: production kernel missing boot-chain verdict marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[BOOT-CHAIN] verdict=' || { echo "FAIL: production ISO missing boot-chain verdict marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BOOT-CHAIN] proof init'; then echo "FAIL: production kernel contains boot-chain proof marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BOOT-CHAIN] proof init'; then echo "FAIL: production ISO contains boot-chain proof marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' PRODUCTION_IMAGE='$(IMAGE)' \
		PROOF_IMAGE='$(BOOT_CHAIN_ISO)' \
		./scripts/test-qemu-boot-chain.sh
	@echo "Boot-chain marker checks: after QEMU proof"
	@strings $(BOOT_CHAIN_KERNEL) | grep -Fq '[BOOT-CHAIN] proof OK' || { echo "FAIL: boot-chain kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(BOOT_CHAIN_ISO) | grep -Fq '[BOOT-CHAIN] proof OK' || { echo "FAIL: boot-chain ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BOOT-CHAIN] proof OK'; then echo "FAIL: production kernel contains boot-chain proof OK marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BOOT-CHAIN] proof OK'; then echo "FAIL: production ISO contains boot-chain proof OK marker after proof" >&2; exit 1; fi

.PHONY: test-appliance-state-loader-qemu
test-appliance-state-loader-qemu: test-physical-storage-qemu
	@strings $(PHYSICAL_STORAGE_KERNEL) | grep -Fq '[STATE-LOADER] proof OK' || { echo "FAIL: physical-storage kernel missing state-loader proof OK marker" >&2; exit 1; }
	@strings $(PHYSICAL_STORAGE_ISO) | grep -Fq '[STATE-LOADER] proof OK' || { echo "FAIL: physical-storage ISO missing state-loader proof OK marker" >&2; exit 1; }
	@echo "State-loader QEMU marker checks: OK"

.PHONY: test-production-storage-boot-qemu
test-production-storage-boot-qemu: iso
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-BOOT] status=' || { echo "FAIL: production kernel missing boot coordinator marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[PRODUCTION-BOOT] status=' || { echo "FAIL: production ISO missing boot coordinator marker" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-production-storage-boot.sh

.PHONY: test-production-recovery-reset-qemu
test-production-recovery-reset-qemu: $(RECOVERY_EXECUTOR_ISO) iso
	@echo "Production recovery reset marker checks: before QEMU proof"
	@strings $(RECOVERY_EXECUTOR_KERNEL) | grep -Fq '[PRODUCTION-RECOVERY] proof init' || { echo "FAIL: recovery-executor kernel missing proof init marker" >&2; exit 1; }
	@strings $(RECOVERY_EXECUTOR_ISO) | grep -Fq '[PRODUCTION-RECOVERY] proof init' || { echo "FAIL: recovery-executor ISO missing proof init marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PRODUCTION-RECOVERY]'; then echo "FAIL: production kernel contains recovery-executor marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PRODUCTION-RECOVERY]'; then echo "FAIL: production ISO contains recovery-executor marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(RECOVERY_EXECUTOR_ISO)' \
		./scripts/test-qemu-production-recovery-reset.sh
	@echo "Production recovery reset marker checks: after QEMU proof"
	@strings $(RECOVERY_EXECUTOR_KERNEL) | grep -Fq '[PRODUCTION-RECOVERY] proof OK' || { echo "FAIL: recovery-executor kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(RECOVERY_EXECUTOR_ISO) | grep -Fq '[PRODUCTION-RECOVERY] proof OK' || { echo "FAIL: recovery-executor ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PRODUCTION-RECOVERY]'; then echo "FAIL: production kernel contains recovery-executor marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PRODUCTION-RECOVERY]'; then echo "FAIL: production ISO contains recovery-executor marker after proof" >&2; exit 1; fi

.PHONY: test-appliance-persistent-config-qemu
test-appliance-persistent-config-qemu: $(PERSISTENT_CONFIG_ISO) iso
	@echo "Persistent-config marker checks: before QEMU proof"
	@strings $(PERSISTENT_CONFIG_KERNEL) | grep -Fq '[PERSISTENT-CONFIG] proof init' || { echo "FAIL: persistent-config kernel missing proof marker" >&2; exit 1; }
	@strings $(PERSISTENT_CONFIG_ISO) | grep -Fq '[PERSISTENT-CONFIG] proof init' || { echo "FAIL: persistent-config ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PERSISTENT-CONFIG]'; then echo "FAIL: production kernel contains persistent-config marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PERSISTENT-CONFIG]'; then echo "FAIL: production ISO contains persistent-config marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(PERSISTENT_CONFIG_ISO)' \
		./scripts/test-qemu-persistent-config.sh
	@echo "Persistent-config marker checks: after QEMU proof"
	@strings $(PERSISTENT_CONFIG_KERNEL) | grep -Fq '[PERSISTENT-CONFIG] proof OK' || { echo "FAIL: persistent-config kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(PERSISTENT_CONFIG_ISO) | grep -Fq '[PERSISTENT-CONFIG] proof OK' || { echo "FAIL: persistent-config ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PERSISTENT-CONFIG]'; then echo "FAIL: production kernel contains persistent-config marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PERSISTENT-CONFIG]'; then echo "FAIL: production ISO contains persistent-config marker after proof" >&2; exit 1; fi

.PHONY: test-persistent-config-reboot-acceptance-qemu
test-persistent-config-reboot-acceptance-qemu: $(PERSISTENT_CONFIG_ISO) iso
	@echo "Persistent-config reboot marker checks: before QEMU proof"
	@strings $(PERSISTENT_CONFIG_KERNEL) | grep -Fq '[REBOOT-CONFIG] phase1 committed' || { echo "FAIL: persistent-config kernel missing reboot phase marker" >&2; exit 1; }
	@strings $(PERSISTENT_CONFIG_ISO) | grep -Fq '[REBOOT-CONFIG] phase1 committed' || { echo "FAIL: persistent-config ISO missing reboot phase marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[REBOOT-CONFIG]'; then echo "FAIL: production kernel contains persistent-config reboot marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[REBOOT-CONFIG]'; then echo "FAIL: production ISO contains persistent-config reboot marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(PERSISTENT_CONFIG_ISO)' \
		./scripts/test-qemu-persistent-config-reboot-acceptance.sh
	@echo "Persistent-config reboot marker checks: after QEMU proof"
	@strings $(PERSISTENT_CONFIG_KERNEL) | grep -Fq '[REBOOT-CONFIG] phase3 recovery OK' || { echo "FAIL: persistent-config kernel missing reboot recovery marker after QEMU proof" >&2; exit 1; }
	@strings $(PERSISTENT_CONFIG_ISO) | grep -Fq '[REBOOT-CONFIG] phase3 recovery OK' || { echo "FAIL: persistent-config ISO missing reboot recovery marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[REBOOT-CONFIG]'; then echo "FAIL: production kernel contains persistent-config reboot marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[REBOOT-CONFIG]'; then echo "FAIL: production ISO contains persistent-config reboot marker after proof" >&2; exit 1; fi

.PHONY: test-appliance-network-config-qemu
test-appliance-network-config-qemu: $(NETWORK_CONFIG_ISO) iso
	@echo "Network-config marker checks: before QEMU proof"
	@strings $(NETWORK_CONFIG_KERNEL) | grep -Fq '[NET-CONFIG] proof init' || { echo "FAIL: network-config kernel missing proof marker" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_ISO) | grep -Fq '[NET-CONFIG] proof init' || { echo "FAIL: network-config ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NET-CONFIG]'; then echo "FAIL: production kernel contains network-config marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NET-CONFIG]'; then echo "FAIL: production ISO contains network-config marker" >&2; exit 1; fi
	@strings $(NETWORK_CONFIG_KERNEL) | grep -Fq '[NET-DIAG] proof init' || { echo "FAIL: network-config kernel missing network diagnostics proof marker" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_ISO) | grep -Fq '[NET-DIAG] proof init' || { echo "FAIL: network-config ISO missing network diagnostics proof marker" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_KERNEL) | grep -Fq '[DHCP-CLIENT] proof init' || { echo "FAIL: network-config kernel missing DHCP client proof marker" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_ISO) | grep -Fq '[DHCP-CLIENT] proof init' || { echo "FAIL: network-config ISO missing DHCP client proof marker" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_KERNEL) | grep -Fq '[DNS-RESOLVER] proof init' || { echo "FAIL: network-config kernel missing DNS resolver proof marker" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_ISO) | grep -Fq '[DNS-RESOLVER] proof init' || { echo "FAIL: network-config ISO missing DNS resolver proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NET-DIAG]'; then echo "FAIL: production kernel contains network diagnostics marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NET-DIAG]'; then echo "FAIL: production ISO contains network diagnostics marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[DHCP-CLIENT]'; then echo "FAIL: production kernel contains DHCP client marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[DHCP-CLIENT]'; then echo "FAIL: production ISO contains DHCP client marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[DNS-RESOLVER]'; then echo "FAIL: production kernel contains DNS resolver marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[DNS-RESOLVER]'; then echo "FAIL: production ISO contains DNS resolver marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(NETWORK_CONFIG_ISO)' \
		./scripts/test-qemu-network-config.sh
	@echo "Network-config marker checks: after QEMU proof"

.PHONY: test-appliance-network-lifecycle-qemu
test-appliance-network-lifecycle-qemu: test-appliance-network-config-qemu
	@echo "Network lifecycle QEMU proof target OK"
	@strings $(NETWORK_CONFIG_KERNEL) | grep -Fq '[NET-CONFIG] proof OK' || { echo "FAIL: network-config kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_ISO) | grep -Fq '[NET-CONFIG] proof OK' || { echo "FAIL: network-config ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NET-CONFIG]'; then echo "FAIL: production kernel contains network-config marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NET-CONFIG]'; then echo "FAIL: production ISO contains network-config marker after proof" >&2; exit 1; fi
	@strings $(NETWORK_CONFIG_KERNEL) | grep -Fq '[NET-DIAG] proof OK' || { echo "FAIL: network-config kernel missing network diagnostics proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_ISO) | grep -Fq '[NET-DIAG] proof OK' || { echo "FAIL: network-config ISO missing network diagnostics proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_KERNEL) | grep -Fq '[DHCP-CLIENT] proof OK' || { echo "FAIL: network-config kernel missing DHCP client proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_ISO) | grep -Fq '[DHCP-CLIENT] proof OK' || { echo "FAIL: network-config ISO missing DHCP client proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_KERNEL) | grep -Fq '[DNS-RESOLVER] proof OK' || { echo "FAIL: network-config kernel missing DNS resolver proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(NETWORK_CONFIG_ISO) | grep -Fq '[DNS-RESOLVER] proof OK' || { echo "FAIL: network-config ISO missing DNS resolver proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NET-DIAG]'; then echo "FAIL: production kernel contains network diagnostics marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NET-DIAG]'; then echo "FAIL: production ISO contains network diagnostics marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[DHCP-CLIENT]'; then echo "FAIL: production kernel contains DHCP client marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[DHCP-CLIENT]'; then echo "FAIL: production ISO contains DHCP client marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[DNS-RESOLVER]'; then echo "FAIL: production kernel contains DNS resolver marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[DNS-RESOLVER]'; then echo "FAIL: production ISO contains DNS resolver marker after proof" >&2; exit 1; fi

.PHONY: test-appliance-persistent-network-config-qemu
test-appliance-persistent-network-config-qemu: \
		$(PERSISTENT_NETWORK_CONFIG_ISO) iso
	@echo "Persistent-network-config marker checks: before QEMU proof"
	@strings $(PERSISTENT_NETWORK_CONFIG_KERNEL) | \
		grep -Fq '[NETWORK-PERSISTENT] proof init' || \
		{ echo "FAIL: persistent-network-config kernel missing proof marker" >&2; exit 1; }
	@strings $(PERSISTENT_NETWORK_CONFIG_ISO) | \
		grep -Fq '[NETWORK-PERSISTENT] proof init' || \
		{ echo "FAIL: persistent-network-config ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NETWORK-PERSISTENT]'; then \
		echo "FAIL: production kernel contains persistent-network-config marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NETWORK-PERSISTENT]'; then \
		echo "FAIL: production ISO contains persistent-network-config marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(PERSISTENT_NETWORK_CONFIG_ISO)' \
		./scripts/test-qemu-persistent-network-config.sh
	@echo "Persistent-network-config marker checks: after QEMU proof"
	@strings $(PERSISTENT_NETWORK_CONFIG_KERNEL) | \
		grep -Fq '[NETWORK-PERSISTENT] proof OK' || \
		{ echo "FAIL: persistent-network-config kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(PERSISTENT_NETWORK_CONFIG_ISO) | \
		grep -Fq '[NETWORK-PERSISTENT] proof OK' || \
		{ echo "FAIL: persistent-network-config ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NETWORK-PERSISTENT]'; then \
		echo "FAIL: production kernel contains persistent-network-config marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NETWORK-PERSISTENT]'; then \
		echo "FAIL: production ISO contains persistent-network-config marker after proof" >&2; exit 1; fi

test-backup-restore-qemu: $(BACKUP_RESTORE_ISO) iso
	@echo "Backup-restore marker checks: before QEMU proof"
	@strings $(BACKUP_RESTORE_KERNEL) | grep -Fq '[BACKUP-RESTORE] proof init' || { echo "FAIL: backup-restore kernel missing proof marker" >&2; exit 1; }
	@strings $(BACKUP_RESTORE_ISO) | grep -Fq '[BACKUP-RESTORE] proof init' || { echo "FAIL: backup-restore ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BACKUP-RESTORE]'; then echo "FAIL: production kernel contains backup-restore marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BACKUP-RESTORE]'; then echo "FAIL: production ISO contains backup-restore marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(BACKUP_RESTORE_ISO)' \
		./scripts/test-qemu-backup-restore.sh
	@echo "Backup-restore marker checks: after QEMU proof"
	@strings $(BACKUP_RESTORE_KERNEL) | grep -Fq '[BACKUP-RESTORE] proof OK' || { echo "FAIL: backup-restore kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(BACKUP_RESTORE_ISO) | grep -Fq '[BACKUP-RESTORE] proof OK' || { echo "FAIL: backup-restore ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[BACKUP-RESTORE]'; then echo "FAIL: production kernel contains backup-restore marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[BACKUP-RESTORE]'; then echo "FAIL: production ISO contains backup-restore marker after proof" >&2; exit 1; fi

test-service-supervisor-qemu: $(SUPERVISOR_COMPOSITION_ISO) iso
	@echo "Supervisor composition marker checks: before QEMU proof"
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[SUPERVISOR] composition init' || { echo "FAIL: supervisor kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[SUPERVISOR] composition init' || { echo "FAIL: supervisor ISO missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[APP-SVC] manager init' || { echo "FAIL: appliance service manager kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[APP-SVC] manager init' || { echo "FAIL: appliance service manager ISO missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[APP-HTTPS] dependency-gated start' || { echo "FAIL: appliance HTTPS kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[APP-HTTPS] dependency-gated start' || { echo "FAIL: appliance HTTPS ISO missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[SVC-PERSIST] proof init' || { echo "FAIL: persistent service kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[SVC-PERSIST] proof init' || { echo "FAIL: persistent service ISO missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[FAIL-HANDLER] proof init' || { echo "FAIL: service failure handler kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[FAIL-HANDLER] proof init' || { echo "FAIL: service failure handler ISO missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[PROD-SVC] proof init' || { echo "FAIL: production service HTTPS kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[PROD-SVC] proof init' || { echo "FAIL: production service HTTPS ISO missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[PROD-MGMT] proof init' || { echo "FAIL: production management kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[PROD-MGMT] proof init' || { echo "FAIL: production management ISO missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[FIRST-BOOT] proof init' || { echo "FAIL: first boot integration kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[FIRST-BOOT] proof init' || { echo "FAIL: first boot integration ISO missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[RECOVERY-RESET] proof init' || { echo "FAIL: recovery factory reset integration kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[RECOVERY-RESET] proof init' || { echo "FAIL: recovery factory reset integration ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SUPERVISOR]'; then echo "FAIL: production kernel contains supervisor marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SUPERVISOR]'; then echo "FAIL: production ISO contains supervisor marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[APP-SVC]'; then echo "FAIL: production kernel contains appliance service manager marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[APP-SVC]'; then echo "FAIL: production ISO contains appliance service manager marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[APP-HTTPS]'; then echo "FAIL: production kernel contains appliance HTTPS marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[APP-HTTPS]'; then echo "FAIL: production ISO contains appliance HTTPS marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[SVC-PERSIST]'; then echo "FAIL: production kernel contains persistent service marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SVC-PERSIST]'; then echo "FAIL: production ISO contains persistent service marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[FAIL-HANDLER]'; then echo "FAIL: production kernel contains service failure handler marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[FAIL-HANDLER]'; then echo "FAIL: production ISO contains service failure handler marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[PROD-SVC]'; then echo "FAIL: production kernel contains production service HTTPS marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PROD-SVC]'; then echo "FAIL: production ISO contains production service HTTPS marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[PROD-MGMT]'; then echo "FAIL: production kernel contains production management marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PROD-MGMT]'; then echo "FAIL: production ISO contains production management marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[FIRST-BOOT]'; then echo "FAIL: production kernel contains first boot integration marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[FIRST-BOOT]'; then echo "FAIL: production ISO contains first boot integration marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[RECOVERY-RESET]'; then echo "FAIL: production kernel contains recovery factory reset integration marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[RECOVERY-RESET]'; then echo "FAIL: production ISO contains recovery factory reset integration marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SUPERVISOR_COMPOSITION_ISO)' \
		./scripts/test-qemu-supervisor-composition.sh
	@echo "Supervisor composition marker checks: after QEMU proof"
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[SUPERVISOR] proof OK' || { echo "FAIL: supervisor kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[SUPERVISOR] proof OK' || { echo "FAIL: supervisor ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[APP-SVC] proof OK' || { echo "FAIL: appliance service manager kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[APP-SVC] proof OK' || { echo "FAIL: appliance service manager ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[APP-HTTPS] proof OK' || { echo "FAIL: appliance HTTPS kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[APP-HTTPS] proof OK' || { echo "FAIL: appliance HTTPS ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[SVC-PERSIST] proof OK' || { echo "FAIL: persistent service kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[SVC-PERSIST] proof OK' || { echo "FAIL: persistent service ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[FAIL-HANDLER] proof OK' || { echo "FAIL: service failure handler kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[FAIL-HANDLER] proof OK' || { echo "FAIL: service failure handler ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[PROD-SVC] proof OK' || { echo "FAIL: production service HTTPS kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[PROD-SVC] proof OK' || { echo "FAIL: production service HTTPS ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[PROD-MGMT] proof OK' || { echo "FAIL: production management kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[PROD-MGMT] proof OK' || { echo "FAIL: production management ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[FIRST-BOOT] proof OK' || { echo "FAIL: first boot integration kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[FIRST-BOOT] proof OK' || { echo "FAIL: first boot integration ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_KERNEL) | grep -Fq '[RECOVERY-RESET] proof OK' || { echo "FAIL: recovery factory reset integration kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPERVISOR_COMPOSITION_ISO) | grep -Fq '[RECOVERY-RESET] proof OK' || { echo "FAIL: recovery factory reset integration ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SUPERVISOR]'; then echo "FAIL: production kernel contains supervisor marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SUPERVISOR]'; then echo "FAIL: production ISO contains supervisor marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[APP-SVC]'; then echo "FAIL: production kernel contains appliance service manager marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[APP-SVC]'; then echo "FAIL: production ISO contains appliance service manager marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[APP-HTTPS]'; then echo "FAIL: production kernel contains appliance HTTPS marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[APP-HTTPS]'; then echo "FAIL: production ISO contains appliance HTTPS marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[SVC-PERSIST]'; then echo "FAIL: production kernel contains persistent service marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SVC-PERSIST]'; then echo "FAIL: production ISO contains persistent service marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[FAIL-HANDLER]'; then echo "FAIL: production kernel contains service failure handler marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[FAIL-HANDLER]'; then echo "FAIL: production ISO contains service failure handler marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[PROD-SVC]'; then echo "FAIL: production kernel contains production service HTTPS marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PROD-SVC]'; then echo "FAIL: production ISO contains production service HTTPS marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[PROD-MGMT]'; then echo "FAIL: production kernel contains production management marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PROD-MGMT]'; then echo "FAIL: production ISO contains production management marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[FIRST-BOOT]'; then echo "FAIL: production kernel contains first boot integration marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[FIRST-BOOT]'; then echo "FAIL: production ISO contains first boot integration marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[RECOVERY-RESET]'; then echo "FAIL: production kernel contains recovery factory reset integration marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[RECOVERY-RESET]'; then echo "FAIL: production ISO contains recovery factory reset integration marker after proof" >&2; exit 1; fi

.PHONY: test-appliance-production-service-https-qemu
test-appliance-production-service-https-qemu: test-service-supervisor-qemu
	@echo "Production service HTTPS QEMU proof target OK"

.PHONY: test-appliance-production-management-qemu
test-appliance-production-management-qemu: test-service-supervisor-qemu
	@echo "Production management QEMU proof target OK"

.PHONY: test-appliance-first-boot-integration-qemu
test-appliance-first-boot-integration-qemu: test-service-supervisor-qemu
	@echo "First boot integration QEMU proof target OK"

.PHONY: test-appliance-recovery-factory-reset-integration-qemu
test-appliance-recovery-factory-reset-integration-qemu: test-service-supervisor-qemu
	@echo "Recovery factory reset integration QEMU proof target OK"

test-local-control-qemu: $(LOCAL_CONTROL_ISO) iso
	@echo "Local-control marker checks: before QEMU proof"
	@strings $(LOCAL_CONTROL_KERNEL) | grep -Fq '[LOCAL-CONTROL] proof init' || { echo "FAIL: local-control kernel missing proof marker" >&2; exit 1; }
	@strings $(LOCAL_CONTROL_ISO) | grep -Fq '[LOCAL-CONTROL] proof init' || { echo "FAIL: local-control ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[LOCAL-CONTROL]'; then echo "FAIL: production kernel contains local-control marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[LOCAL-CONTROL]'; then echo "FAIL: production ISO contains local-control marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(LOCAL_CONTROL_ISO)' \
		./scripts/test-qemu-local-control.sh
	@echo "Local-control marker checks: after QEMU proof"
	@strings $(LOCAL_CONTROL_KERNEL) | grep -Fq '[LOCAL-CONTROL] proof OK' || { echo "FAIL: local-control kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(LOCAL_CONTROL_ISO) | grep -Fq '[LOCAL-CONTROL] proof OK' || { echo "FAIL: local-control ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[LOCAL-CONTROL]'; then echo "FAIL: production kernel contains local-control marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[LOCAL-CONTROL]'; then echo "FAIL: production ISO contains local-control marker after proof" >&2; exit 1; fi

test-network-foundation-qemu: $(NETWORK_FOUNDATION_ISO) iso
	@echo "Network-foundation marker checks: before QEMU proof"
	@strings $(NETWORK_FOUNDATION_KERNEL) | grep -Fq '[NETWORK-FOUNDATION] proof init' || { echo "FAIL: network-foundation kernel missing proof marker" >&2; exit 1; }
	@strings $(NETWORK_FOUNDATION_ISO) | grep -Fq '[NETWORK-FOUNDATION] proof init' || { echo "FAIL: network-foundation ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NETWORK-FOUNDATION]'; then echo "FAIL: production kernel contains network-foundation marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NETWORK-FOUNDATION]'; then echo "FAIL: production ISO contains network-foundation marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(NETWORK_FOUNDATION_ISO)' \
		./scripts/test-qemu-network-foundation.sh
	@echo "Network-foundation marker checks: after QEMU proof"
	@strings $(NETWORK_FOUNDATION_KERNEL) | grep -Fq '[NETWORK-FOUNDATION] proof OK' || { echo "FAIL: network-foundation kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(NETWORK_FOUNDATION_ISO) | grep -Fq '[NETWORK-FOUNDATION] proof OK' || { echo "FAIL: network-foundation ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NETWORK-FOUNDATION]'; then echo "FAIL: production kernel contains network-foundation marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NETWORK-FOUNDATION]'; then echo "FAIL: production ISO contains network-foundation marker after proof" >&2; exit 1; fi

test-production-network-qemu: $(KERNEL) iso
	@echo "Production-network marker checks: before QEMU proof"
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-NET]' || { echo "FAIL: production kernel missing production-network marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[PRODUCTION-NET]' || { echo "FAIL: production ISO missing production-network marker" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-production-network.sh

test-production-dhcp-dns-qemu: $(KERNEL) iso
	@echo "Production DHCP/DNS marker checks: before QEMU proof"
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-DHCP]' || { echo "FAIL: production kernel missing production DHCP marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[PRODUCTION-DHCP]' || { echo "FAIL: production ISO missing production DHCP marker" >&2; exit 1; }
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-DNS]' || { echo "FAIL: production kernel missing production DNS marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[PRODUCTION-DNS]' || { echo "FAIL: production ISO missing production DNS marker" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-production-dhcp-dns.sh

# ITEM-494: the same ordinary-boot DHCP/DNS proof on the second vendor.  The
# machine has one emulated Intel 82540EM and no virtio-net function, so the
# lease, the name and the clock can only come from the Intel driver, and the
# proof requires the records to name that path (`driver=E1000`,
# `state=AVAILABLE` with the part's address, `backend=E1000`).  This is a
# QEMU/OVMF proof, not physical acceptance: the acceptance host carries no
# Intel controller.
.PHONY: test-production-dhcp-dns-e1000-qemu
test-production-dhcp-dns-e1000-qemu: $(KERNEL) iso
	@echo "Production DHCP/DNS (Intel e1000) marker checks: before QEMU proof"
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-DHCP]' || { echo "FAIL: production kernel missing production DHCP marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[PRODUCTION-DHCP]' || { echo "FAIL: production ISO missing production DHCP marker" >&2; exit 1; }
	@strings $(KERNEL) | grep -Fq 'E1000' || { echo "FAIL: production kernel missing the Intel claim name" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq 'E1000' || { echo "FAIL: production ISO missing the Intel claim name" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		HFOC_PRODUCTION_DHCP_DNS_NIC='e1000' \
		./scripts/test-qemu-production-dhcp-dns.sh

test-production-first-boot-qemu: $(KERNEL) iso
	@echo "Production first-boot marker checks: before QEMU proof"
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-FIRST-BOOT]' || { echo "FAIL: production kernel missing first-boot marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[PRODUCTION-FIRST-BOOT]' || { echo "FAIL: production ISO missing first-boot marker" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-production-first-boot.sh

# ITEM-587: the HTTPS proof is one public target made of two children.  The
# public target prepares the shared ISO once and then runs both children in
# order, printing the canonical marker only after both succeeded.  In CI the
# two children are scheduled separately by the QEMU suite's external queue
# (QEMU_PARALLEL_PHASE_1), so neither the marker nor the wrapper is duplicated.
test-production-service-https-qemu: iso
	$(MAKE) test-production-service-https-main-qemu
	$(MAKE) test-production-service-https-noentropy-qemu
	./scripts/test-production-service-https-summary.sh

# The shared ISO is prepared by the caller (the public target above, or
# qemu-test-images in CI) before either child starts: both children read the
# same build/iso_root, so building it inside each child raced when the suite
# ran them in parallel.  The guard fails loudly instead of rebuilding.
qemu-iso-ready:
	@test -f '$(IMAGE)' || { echo "FAIL: $(IMAGE) is missing; run make iso (or make qemu-test-images) before the HTTPS children" >&2; exit 1; }

test-production-service-https-main-qemu: $(KERNEL) qemu-iso-ready
	@echo "Production service/HTTPS marker checks: before QEMU proof"
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-SERVICE]' || { echo "FAIL: production kernel missing production-service marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[PRODUCTION-SERVICE]' || { echo "FAIL: production ISO missing production-service marker" >&2; exit 1; }
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-SERVING]' || { echo "FAIL: production kernel missing production-serving marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[PRODUCTION-SERVING]' || { echo "FAIL: production ISO missing production-serving marker" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-production-service-https.sh

test-production-service-https-noentropy-qemu: $(KERNEL) qemu-iso-ready
	@echo "no-RNG entropy marker checks: before QEMU proof"
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-SERVING]' || { echo "FAIL: production kernel missing production-serving marker" >&2; exit 1; }
	@strings $(IMAGE) | grep -Fq '[PRODUCTION-SERVING]' || { echo "FAIL: production ISO missing production-serving marker" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-production-service-https-noentropy.sh

# ITEM-507 increment 6: the item's own two-certificate evidence.  The fixture
# ISO carries two test-only pairs declared for two names and a test-only
# account; the proof then requires a real TLS 1.3 client to receive the two
# different certificates those names declare, an undeclared name to be refused,
# a client without SNI to receive the appliance identity, and the committed
# record to survive a reboot.
test-production-sni-selection-qemu: $(SNI_SELECTION_ISO) iso
	@echo "Production SNI selection marker checks: before QEMU proof"
	@strings $(SNI_SELECTION_KERNEL) | grep -Fq '[CERTIFICATE-FIXTURE]' || { echo "FAIL: fixture kernel missing the certificate fixture record" >&2; exit 1; }
	@strings $(SNI_SELECTION_ISO) | grep -Fq '[CERTIFICATE-FIXTURE]' || { echo "FAIL: fixture ISO missing the certificate fixture record" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SNI_SELECTION_ISO)' \
		./scripts/test-qemu-production-sni-selection.sh

# ITEM-508 increment 15: the offline directory exchange over the real binding.
# The harness serves the increment-15 stub on the host and the fixture kernel
# dials it as the slirp gateway at the fixed port both sides carry.
test-acme-directory-qemu: $(ACME_DIRECTORY_ISO) iso
	@echo "ACME directory QEMU marker checks: before harness"
	@strings $(ACME_DIRECTORY_KERNEL) | grep -Fq '[ACME-DIRECTORY] proof init' || { echo "FAIL: fixture kernel missing the directory proof marker" >&2; exit 1; }
	@strings $(ACME_DIRECTORY_ISO) | grep -Fq '[ACME-DIRECTORY] proof init' || { echo "FAIL: fixture ISO missing the directory proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ACME-DIRECTORY]'; then echo "FAIL: production kernel contains the directory proof marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ACME-DIRECTORY]'; then echo "FAIL: production ISO contains the directory proof marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(ACME_DIRECTORY_ISO)' \
		HFOC_ACME_DIRECTORY_QEMU_TEST_PORT='18445' \
		./scripts/test-qemu-acme-directory.sh
	@if strings $(KERNEL) | grep -Fq '[ACME-DIRECTORY]'; then echo "FAIL: production kernel contains the directory proof marker after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ACME-DIRECTORY]'; then echo "FAIL: production ISO contains the directory proof marker after harness" >&2; exit 1; fi
	@echo "ACME directory QEMU marker checks: after harness"

# ITEM-508 increment 17: one complete ACME order through the real binding.
# The grown stub answers the order's endpoints on the host and the fixture
# kernel dials it as the slirp gateway at the fixed port both sides carry.
test-acme-order-qemu: $(ACME_ORDER_ISO) iso
	@echo "ACME order QEMU marker checks: before harness"
	@strings $(ACME_ORDER_KERNEL) | grep -Fq '[ACME-ORDER] proof init' || { echo "FAIL: fixture kernel missing the order proof marker" >&2; exit 1; }
	@strings $(ACME_ORDER_ISO) | grep -Fq '[ACME-ORDER] proof init' || { echo "FAIL: fixture ISO missing the order proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ACME-ORDER]'; then echo "FAIL: production kernel contains the order proof marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ACME-ORDER]'; then echo "FAIL: production ISO contains the order proof marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(ACME_ORDER_ISO)' \
		HFOC_ACME_ORDER_QEMU_TEST_PORT='18446' \
		HFOC_ACME_ORDER_QEMU_HTTP01_PORT='18448' \
		./scripts/test-qemu-acme-order.sh
	@if strings $(KERNEL) | grep -Fq '[ACME-ORDER]'; then echo "FAIL: production kernel contains the order proof marker after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ACME-ORDER]'; then echo "FAIL: production ISO contains the order proof marker after harness" >&2; exit 1; fi
	@echo "ACME order QEMU marker checks: after harness"

# ITEM-508 increment 36: the production order path over the real binding.  The
# guest is the production kernel plus the fixture anchor and the shared fixture
# clock; the ordinary-boot composition dials the offline stub and runs one
# complete order, and the fixture entry asks for it.  The record is production
# code, so unlike the fixture families the production kernel carries it too.
test-acme-boot-order-qemu: $(ACME_BOOT_ORDER_ISO) iso
	@echo "ACME boot-order QEMU marker checks: before harness"
	@strings $(ACME_BOOT_ORDER_KERNEL) | grep -Fq '[PRODUCTION-ACME-ORDER] state=' || { echo "FAIL: boot-order kernel missing the order record" >&2; exit 1; }
	@strings $(ACME_BOOT_ORDER_ISO) | grep -Fq '[PRODUCTION-ACME-ORDER] state=' || { echo "FAIL: boot-order ISO missing the order record" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(ACME_BOOT_ORDER_ISO)' \
		HFOC_ACME_BOOT_ORDER_QEMU_TEST_PORT='18449' \
		./scripts/test-qemu-acme-boot-order.sh
	@echo "ACME boot-order QEMU marker checks: after harness"

# ITEM-508 increment 29: the HTTP-01 answer on the real wire.  The guest is the
# production listener with one fixture token armed; the host forwards a local
# port to guest port 80 and speaks HTTP/1.1 to it, so the proof judges the
# bytes the production listener wrote, not the decision the host proof pins.
# The production kernel and ISO must not carry the fixture record: the arming
# hook is a fixture-family object only.
test-acme-http01-qemu: $(ACME_HTTP01_ISO) iso
	@echo "ACME HTTP-01 QEMU marker checks: before harness"
	@strings $(ACME_HTTP01_KERNEL) | grep -Fq '[ACME-HTTP01-FIXTURE] v=1 armed=1 token=hfoc-fixture-token' || { echo "FAIL: fixture kernel missing the HTTP-01 fixture arming record" >&2; exit 1; }
	@strings $(ACME_HTTP01_ISO) | grep -Fq '[ACME-HTTP01-FIXTURE] v=1 armed=1 token=hfoc-fixture-token' || { echo "FAIL: fixture ISO missing the HTTP-01 fixture arming record" >&2; exit 1; }
	@strings $(ACME_HTTP01_KERNEL) | grep -Fq '[ACME-HTTP01] v=1' || { echo "FAIL: fixture kernel missing the production HTTP-01 record" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ACME-HTTP01-FIXTURE]'; then echo "FAIL: production kernel contains the HTTP-01 fixture record" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ACME-HTTP01-FIXTURE]'; then echo "FAIL: production ISO contains the HTTP-01 fixture record" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(ACME_HTTP01_ISO)' \
		HFOC_ACME_HTTP01_QEMU_TEST_PORT='18447' \
		./scripts/test-qemu-acme-http01.sh
	@if strings $(KERNEL) | grep -Fq '[ACME-HTTP01-FIXTURE]'; then echo "FAIL: production kernel contains the HTTP-01 fixture record after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ACME-HTTP01-FIXTURE]'; then echo "FAIL: production ISO contains the HTTP-01 fixture record after harness" >&2; exit 1; fi
	@echo "ACME HTTP-01 QEMU marker checks: after harness"

# ITEM-449: the declared forward really carries bytes.  The proof installs the
# forward medium, boots the installed disk with the declared listen port
# forwarded, verifies HTTPS with the owner SSH key installed,
# and then repeats the run with a medium whose client list does not include the
# source, where the connection must be refused before the target is dialled.
test-production-forward-qemu: $(PRODUCTION_INSTALLER_FORWARD_SSH_ISO) \
		production-installer-forward-denied-media iso
	@echo "ITEM-449 forward marker checks: before QEMU proof"
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-FORWARD]' || { echo "FAIL: production kernel missing the forward relay record" >&2; exit 1; }
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-FORWARD] state=LISTENING' || { echo "FAIL: production kernel cannot publish a live listener" >&2; exit 1; }
	@strings $(KERNEL) | grep -Fq '[PRODUCTION-FORWARD] state=SESSION' || { echo "FAIL: production kernel cannot publish a session record" >&2; exit 1; }
	@strings $(KERNEL) | grep -Fq 'reason=SOURCE_NOT_ALLOWED' || { echo "FAIL: production kernel cannot publish a source refusal" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='1' \
		FORWARD_IMAGE='$(PRODUCTION_INSTALLER_FORWARD_SSH_ISO)' \
		FORWARD_DENIED_IMAGE='$(PRODUCTION_INSTALLER_FORWARD_DENIED_ISO)' \
		HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET)' \
		HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT)' \
		HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT)' \
		HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS='$(HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS)' \
		HFOC_INSTALL_MEDIA_FORWARD_DENIED_CLIENTS='$(HFOC_INSTALL_MEDIA_FORWARD_DENIED_CLIENTS)' \
		./scripts/test-qemu-production-forward.sh

test-virtio-foundation-qemu: $(VIRTIO_FOUNDATION_ISO) iso
	@echo "Virtio-foundation marker checks: before QEMU proof"
	@strings $(VIRTIO_FOUNDATION_KERNEL) | grep -Fq '[VIRTIO-FOUNDATION] proof init' || { echo "FAIL: virtio-foundation kernel missing proof marker" >&2; exit 1; }
	@strings $(VIRTIO_FOUNDATION_ISO) | grep -Fq '[VIRTIO-FOUNDATION] proof init' || { echo "FAIL: virtio-foundation ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[VIRTIO-FOUNDATION]'; then echo "FAIL: production kernel contains virtio-foundation marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[VIRTIO-FOUNDATION]'; then echo "FAIL: production ISO contains virtio-foundation marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(VIRTIO_FOUNDATION_ISO)' \
		./scripts/test-qemu-virtio-foundation.sh
	@echo "Virtio-foundation marker checks: after QEMU proof"
	@strings $(VIRTIO_FOUNDATION_KERNEL) | grep -Fq '[VIRTIO-FOUNDATION] proof OK' || { echo "FAIL: virtio-foundation kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(VIRTIO_FOUNDATION_ISO) | grep -Fq '[VIRTIO-FOUNDATION] proof OK' || { echo "FAIL: virtio-foundation ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[VIRTIO-FOUNDATION]'; then echo "FAIL: production kernel contains virtio-foundation marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[VIRTIO-FOUNDATION]'; then echo "FAIL: production ISO contains virtio-foundation marker after proof" >&2; exit 1; fi

test-virtio-net-frame-qemu: $(VIRTIO_NET_FRAME_ISO) iso
	@echo "Virtio-net-frame marker checks: before QEMU proof"
	@strings $(VIRTIO_NET_FRAME_KERNEL) | grep -Fq '[VIRTIO-NET-FRAME] proof init' || { echo "FAIL: virtio-net-frame kernel missing proof marker" >&2; exit 1; }
	@strings $(VIRTIO_NET_FRAME_ISO) | grep -Fq '[VIRTIO-NET-FRAME] proof init' || { echo "FAIL: virtio-net-frame ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[VIRTIO-NET-FRAME]'; then echo "FAIL: production kernel contains virtio-net-frame marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[VIRTIO-NET-FRAME]'; then echo "FAIL: production ISO contains virtio-net-frame marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(VIRTIO_NET_FRAME_ISO)' \
		./scripts/test-qemu-virtio-net-frame.sh
	@echo "Virtio-net-frame marker checks: after QEMU proof"
	@strings $(VIRTIO_NET_FRAME_KERNEL) | grep -Fq '[VIRTIO-NET-FRAME] proof OK' || { echo "FAIL: virtio-net-frame kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(VIRTIO_NET_FRAME_ISO) | grep -Fq '[VIRTIO-NET-FRAME] proof OK' || { echo "FAIL: virtio-net-frame ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[VIRTIO-NET-FRAME]'; then echo "FAIL: production kernel contains virtio-net-frame marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[VIRTIO-NET-FRAME]'; then echo "FAIL: production ISO contains virtio-net-frame marker after proof" >&2; exit 1; fi

test-packet-boundary-qemu: $(PACKET_BOUNDARY_ISO) iso
	@echo "Packet-boundary marker checks: before QEMU proof"
	@strings $(PACKET_BOUNDARY_KERNEL) | grep -Fq '[PACKET-BOUNDARY] proof init' || { echo "FAIL: packet-boundary kernel missing proof marker" >&2; exit 1; }
	@strings $(PACKET_BOUNDARY_ISO) | grep -Fq '[PACKET-BOUNDARY] proof init' || { echo "FAIL: packet-boundary ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PACKET-BOUNDARY]'; then echo "FAIL: production kernel contains packet-boundary marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PACKET-BOUNDARY]'; then echo "FAIL: production ISO contains packet-boundary marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(PACKET_BOUNDARY_ISO)' \
		./scripts/test-qemu-packet-boundary.sh
	@echo "Packet-boundary marker checks: after QEMU proof"
	@strings $(PACKET_BOUNDARY_KERNEL) | grep -Fq '[PACKET-BOUNDARY] proof OK' || { echo "FAIL: packet-boundary kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(PACKET_BOUNDARY_ISO) | grep -Fq '[PACKET-BOUNDARY] proof OK' || { echo "FAIL: packet-boundary ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PACKET-BOUNDARY]'; then echo "FAIL: production kernel contains packet-boundary marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PACKET-BOUNDARY]'; then echo "FAIL: production ISO contains packet-boundary marker after proof" >&2; exit 1; fi

test-arp-icmp-qemu: $(ARP_ICMP_ISO) iso
	@echo "ARP/ICMP marker checks: before QEMU proof"
	@strings $(ARP_ICMP_KERNEL) | grep -Fq '[ARP-ICMP] proof init' || { echo "FAIL: arp-icmp kernel missing proof marker" >&2; exit 1; }
	@strings $(ARP_ICMP_ISO) | grep -Fq '[ARP-ICMP] proof init' || { echo "FAIL: arp-icmp ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ARP-ICMP]'; then echo "FAIL: production kernel contains arp-icmp marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ARP-ICMP]'; then echo "FAIL: production ISO contains arp-icmp marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(ARP_ICMP_ISO)' \
		./scripts/test-qemu-arp-icmp.sh
	@echo "ARP/ICMP marker checks: after QEMU proof"
	@strings $(ARP_ICMP_KERNEL) | grep -Fq '[ARP-ICMP] proof OK' || { echo "FAIL: arp-icmp kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(ARP_ICMP_ISO) | grep -Fq '[ARP-ICMP] proof OK' || { echo "FAIL: arp-icmp ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ARP-ICMP]'; then echo "FAIL: production kernel contains arp-icmp marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ARP-ICMP]'; then echo "FAIL: production ISO contains arp-icmp marker after proof" >&2; exit 1; fi

test-tcp-listener-qemu: $(TCP_LISTENER_ISO) iso
	@echo "TCP listener marker checks: before QEMU proof"
	@strings $(TCP_LISTENER_KERNEL) | grep -Fq '[TCP-LISTENER] proof init' || { echo "FAIL: tcp-listener kernel missing proof marker" >&2; exit 1; }
	@strings $(TCP_LISTENER_ISO) | grep -Fq '[TCP-LISTENER] proof init' || { echo "FAIL: tcp-listener ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[TCP-LISTENER]'; then echo "FAIL: production kernel contains tcp-listener marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TCP-LISTENER]'; then echo "FAIL: production ISO contains tcp-listener marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(TCP_LISTENER_ISO)' \
		./scripts/test-qemu-tcp-listener.sh
	@echo "TCP listener marker checks: after QEMU proof"
	@strings $(TCP_LISTENER_KERNEL) | grep -Fq '[TCP-LISTENER] proof OK' || { echo "FAIL: tcp-listener kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(TCP_LISTENER_ISO) | grep -Fq '[TCP-LISTENER] proof OK' || { echo "FAIL: tcp-listener ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[TCP-LISTENER]'; then echo "FAIL: production kernel contains tcp-listener marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TCP-LISTENER]'; then echo "FAIL: production ISO contains tcp-listener marker after proof" >&2; exit 1; fi

test-network-resource-qemu: $(NETWORK_RESOURCE_ISO) iso
	@echo "Network resource marker checks: before QEMU proof"
	@strings $(NETWORK_RESOURCE_KERNEL) | grep -Fq '[NET-RESOURCE] proof init' || { echo "FAIL: network-resource kernel missing proof marker" >&2; exit 1; }
	@strings $(NETWORK_RESOURCE_ISO) | grep -Fq '[NET-RESOURCE] proof init' || { echo "FAIL: network-resource ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NET-RESOURCE]'; then echo "FAIL: production kernel contains network-resource marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NET-RESOURCE]'; then echo "FAIL: production ISO contains network-resource marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(NETWORK_RESOURCE_ISO)' \
		./scripts/test-qemu-network-resource.sh
	@echo "Network resource marker checks: after QEMU proof"
	@strings $(NETWORK_RESOURCE_KERNEL) | grep -Fq '[NET-RESOURCE] proof OK' || { echo "FAIL: network-resource kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(NETWORK_RESOURCE_ISO) | grep -Fq '[NET-RESOURCE] proof OK' || { echo "FAIL: network-resource ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NET-RESOURCE]'; then echo "FAIL: production kernel contains network-resource marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NET-RESOURCE]'; then echo "FAIL: production ISO contains network-resource marker after proof" >&2; exit 1; fi

test-tls-qemu: $(MBEDTLS_TLS_QEMU_ISO) iso
	@echo "TLS QEMU marker checks: before QEMU proof"
	@strings $(MBEDTLS_TLS_QEMU_KERNEL) | grep -Fq '[TLS-QEMU] proof init' || { echo "FAIL: TLS QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(MBEDTLS_TLS_QEMU_ISO) | grep -Fq '[TLS-QEMU] proof init' || { echo "FAIL: TLS QEMU ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[TLS-QEMU]'; then echo "FAIL: production kernel contains TLS QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TLS-QEMU]'; then echo "FAIL: production ISO contains TLS QEMU marker" >&2; exit 1; fi
# ITEM-386: the ordinary production boot terminates TLS 1.3 itself, so the
# production kernel links the serving mbedTLS SSL/PSA closure on purpose. The
# pre-ITEM-386 blanket prohibition on `mbedtls_ssl_*`, `psa_crypto_*` and
# `mbedtls_psa_*` asserted that old isolation rule and is re-pointed here, not
# deleted: the serving closure MUST be present, the ITEM-372 verification
# namespace MUST still be present beside it (the TLS link must never displace
# the verification profile), and the fixture-only surface MUST stay absent —
# its proof markers and test secret above, its secret-boundary and host-test
# symbols here. A missing serving closure, a lost verification profile or a
# leaked fixture symbol all still fail this target.
	@echo "Production TLS-serving symbol checks"
	@for tls_serving_symbol in \
		mbedtls_ssl_setup \
		mbedtls_ssl_handshake \
		mbedtls_ssl_close_notify \
		mbedtls_ssl_tls13_crypto_init \
		psa_crypto_init \
		mbedtls_x509_crt_parse_der \
		mbedtls_pk_parse_key \
		appliance_production_serving_init \
		appliance_production_entropy_init \
		appliance_production_certificate_prepare; do \
		nm -g $(KERNEL) | grep -Eq " [Tt] $$tls_serving_symbol$$" || \
			{ echo "FAIL: production kernel missing TLS-serving symbol $$tls_serving_symbol" >&2; exit 1; }; \
	done
	@for tls_verify_symbol in \
		hfoc_verify_mbedtls_asn1_get_len \
		hfoc_verify_mbedtls_ecdsa_verify \
		hfoc_verify_mbedtls_sha256_starts; do \
		nm -g $(KERNEL) | grep -Eq " [Tt] $$tls_verify_symbol$$" || \
			{ echo "FAIL: production kernel missing ITEM-372 verification symbol $$tls_verify_symbol" >&2; exit 1; }; \
	done
	@for tls_fixture_symbol in \
		hfoc_mbedtls_test_secret_ \
		hfoc_mbedtls_alloc_host_test_ \
		hfoc_mbedtls_manifest_ \
		hfoc_tls_handshake_ \
		hfoc_tls_close_ \
		hfoc_lwip_host_test_ \
		hfoc_lwip_time_host_test_ \
		hfoc_lwip_pbuf_host_test_ \
		hfoc_lwip_netif_host_test_; do \
		if nm -g $(KERNEL) | grep -F " $$tls_fixture_symbol" >/dev/null; then \
			echo "FAIL: production kernel contains fixture-only symbol $$tls_fixture_symbol" >&2; exit 1; fi; \
	done
	@if strings $(KERNEL) | grep -Fq 'HFOC-MBEDTLS-TEST-SECRET'; then echo "FAIL: production kernel contains mbedTLS test-secret marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq 'HFOC-MBEDTLS-TEST-SECRET'; then echo "FAIL: production ISO contains mbedTLS test-secret marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(MBEDTLS_TLS_QEMU_ISO)' \
		./scripts/test-qemu-tls.sh
	@echo "TLS QEMU marker checks: after QEMU proof"
	@strings $(MBEDTLS_TLS_QEMU_KERNEL) | grep -Fq '[TLS-QEMU] proof OK' || { echo "FAIL: TLS QEMU kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(MBEDTLS_TLS_QEMU_ISO) | grep -Fq '[TLS-QEMU] proof OK' || { echo "FAIL: TLS QEMU ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[TLS-QEMU]'; then echo "FAIL: production kernel contains TLS QEMU marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TLS-QEMU]'; then echo "FAIL: production ISO contains TLS QEMU marker after proof" >&2; exit 1; fi

test-https-service-qemu: $(HTTPS_SERVICE_QEMU_ISO) iso
	@echo "HTTPS service QEMU marker checks: before QEMU proof"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS service QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS service QEMU ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[TLS-QEMU]'; then echo "FAIL: production kernel contains TLS QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TLS-QEMU]'; then echo "FAIL: production ISO contains TLS QEMU marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq 'HFOC-MBEDTLS-TEST-SECRET'; then echo "FAIL: production kernel contains mbedTLS test-secret marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq 'HFOC-MBEDTLS-TEST-SECRET'; then echo "FAIL: production ISO contains mbedTLS test-secret marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HTTPS_SERVICE_QEMU_ISO)' \
		./scripts/test-qemu-https-service.sh
	@echo "HTTPS service QEMU marker checks: after QEMU proof"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof OK' || { echo "FAIL: HTTPS service QEMU kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof OK' || { echo "FAIL: HTTPS service QEMU ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[TLS-QEMU]'; then echo "FAIL: production kernel contains TLS QEMU marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TLS-QEMU]'; then echo "FAIL: production ISO contains TLS QEMU marker after proof" >&2; exit 1; fi

test-management-plane-qemu: $(HTTPS_SERVICE_QEMU_ISO) iso
	@echo "Management plane QEMU marker checks: before QEMU proof"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[MGMT-PLANE] proof init' || { echo "FAIL: management plane QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[MGMT-PLANE] proof init' || { echo "FAIL: management plane QEMU ISO missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: management plane QEMU kernel missing HTTPS proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: management plane QEMU ISO missing HTTPS proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[MGMT-REACH] proof init' || { echo "FAIL: management plane QEMU kernel missing reachability proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[MGMT-REACH] proof init' || { echo "FAIL: management plane QEMU ISO missing reachability proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[MGMT-PLANE]'; then echo "FAIL: production kernel contains management plane marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[MGMT-PLANE]'; then echo "FAIL: production ISO contains management plane marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[MGMT-REACH]'; then echo "FAIL: production kernel contains reachability marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[MGMT-REACH]'; then echo "FAIL: production ISO contains reachability marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HTTPS_SERVICE_QEMU_ISO)' \
		./scripts/test-qemu-management-plane-acceptance.sh
	@echo "Management plane QEMU marker checks: after QEMU proof"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[MGMT-PLANE] proof OK' || { echo "FAIL: management plane QEMU kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[MGMT-PLANE] proof OK' || { echo "FAIL: management plane QEMU ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof OK' || { echo "FAIL: management plane QEMU kernel missing HTTPS proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof OK' || { echo "FAIL: management plane QEMU ISO missing HTTPS proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[MGMT-REACH] proof OK' || { echo "FAIL: management plane QEMU kernel missing reachability proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[MGMT-REACH] proof OK' || { echo "FAIL: management plane QEMU ISO missing reachability proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[MGMT-PLANE]'; then echo "FAIL: production kernel contains management plane marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[MGMT-PLANE]'; then echo "FAIL: production ISO contains management plane marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[MGMT-REACH]'; then echo "FAIL: production kernel contains reachability marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[MGMT-REACH]'; then echo "FAIL: production ISO contains reachability marker after proof" >&2; exit 1; fi

test-https-client-qemu: $(HTTPS_SERVICE_QEMU_ISO) iso
	@echo "HTTPS client QEMU marker checks: before harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS client QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS client QEMU ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HTTPS_SERVICE_QEMU_ISO)' \
		HFOC_HTTPS_CLIENT_QEMU_TEST_PORT='18443' \
		./scripts/test-qemu-https-client.sh
	@echo "HTTPS client QEMU marker checks: after harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS client QEMU kernel missing proof marker after harness" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS client QEMU ISO missing proof marker after harness" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker after harness" >&2; exit 1; fi

test-https-response-matrix-qemu: $(HTTPS_SERVICE_QEMU_ISO) iso
	@echo "HTTPS response matrix QEMU marker checks: before harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS response matrix QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS response matrix QEMU ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HTTPS_SERVICE_QEMU_ISO)' \
		./scripts/test-qemu-https-response-matrix.sh
	@echo "HTTPS response matrix QEMU marker checks: after harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS response matrix QEMU kernel missing proof marker after harness" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS response matrix QEMU ISO missing proof marker after harness" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker after harness" >&2; exit 1; fi

test-https-malformed-request-matrix-qemu: $(HTTPS_SERVICE_QEMU_ISO) iso
	@echo "HTTPS malformed-request matrix QEMU marker checks: before harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS malformed-request matrix QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS malformed-request matrix QEMU ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HTTPS_SERVICE_QEMU_ISO)' \
		./scripts/test-qemu-https-malformed-request-matrix.sh
	@echo "HTTPS malformed-request matrix QEMU marker checks: after harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS malformed-request matrix QEMU kernel missing proof marker after harness" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS malformed-request matrix QEMU ISO missing proof marker after harness" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker after harness" >&2; exit 1; fi

test-https-size-boundary-matrix-qemu: $(HTTPS_SERVICE_QEMU_ISO) iso
	@echo "HTTPS size-boundary matrix QEMU marker checks: before harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS size-boundary matrix QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS size-boundary matrix QEMU ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HTTPS_SERVICE_QEMU_ISO)' \
		./scripts/test-qemu-https-size-boundary-matrix.sh
	@echo "HTTPS size-boundary matrix QEMU marker checks: after harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS size-boundary matrix QEMU kernel missing proof marker after harness" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS size-boundary matrix QEMU ISO missing proof marker after harness" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker after harness" >&2; exit 1; fi

test-https-exhaustion-matrix-qemu: $(HTTPS_SERVICE_QEMU_ISO) iso
	@echo "HTTPS exhaustion matrix QEMU marker checks: before harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS exhaustion matrix QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS exhaustion matrix QEMU ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HTTPS_SERVICE_QEMU_ISO)' \
		./scripts/test-qemu-https-exhaustion-matrix.sh
	@echo "HTTPS exhaustion matrix QEMU marker checks: after harness"
	@strings $(HTTPS_SERVICE_QEMU_KERNEL) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS exhaustion matrix QEMU kernel missing proof marker after harness" >&2; exit 1; }
	@strings $(HTTPS_SERVICE_QEMU_ISO) | grep -Fq '[HTTPS-QEMU] proof init' || { echo "FAIL: HTTPS exhaustion matrix QEMU ISO missing proof marker after harness" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker after harness" >&2; exit 1; fi

test-entropy-fail-closed-matrix-qemu: $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO) iso
	@echo "Entropy fail-closed matrix QEMU marker checks: before harness"
	@strings $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_KERNEL) | grep -Fq '[ENTROPY-FAIL-CLOSED] proof init' || { echo "FAIL: entropy fail-closed matrix QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO) | grep -Fq '[ENTROPY-FAIL-CLOSED] proof init' || { echo "FAIL: entropy fail-closed matrix QEMU ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ENTROPY-FAIL-CLOSED]'; then echo "FAIL: production kernel contains entropy fail-closed marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ENTROPY-FAIL-CLOSED]'; then echo "FAIL: production ISO contains entropy fail-closed marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO)' \
		./scripts/test-qemu-entropy-fail-closed-matrix.sh
	@echo "Entropy fail-closed matrix QEMU marker checks: after harness"
	@strings $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_KERNEL) | grep -Fq '[ENTROPY-FAIL-CLOSED] matrix ok' || { echo "FAIL: entropy fail-closed matrix QEMU kernel missing matrix OK marker after harness" >&2; exit 1; }
	@strings $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO) | grep -Fq '[ENTROPY-FAIL-CLOSED] matrix ok' || { echo "FAIL: entropy fail-closed matrix QEMU ISO missing matrix OK marker after harness" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ENTROPY-FAIL-CLOSED]'; then echo "FAIL: production kernel contains entropy fail-closed marker after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ENTROPY-FAIL-CLOSED]'; then echo "FAIL: production ISO contains entropy fail-closed marker after harness" >&2; exit 1; fi

test-https-restart-soak-qemu: $(HTTPS_RESTART_SOAK_QEMU_ISO) iso
	@echo "HTTPS restart/soak QEMU marker checks: before harness"
	@strings $(HTTPS_RESTART_SOAK_QEMU_KERNEL) | grep -Fq '[HTTPS-RESTART-SOAK] proof init' || { echo "FAIL: HTTPS restart/soak QEMU kernel missing proof marker" >&2; exit 1; }
	@strings $(HTTPS_RESTART_SOAK_QEMU_ISO) | grep -Fq '[HTTPS-RESTART-SOAK] proof init' || { echo "FAIL: HTTPS restart/soak QEMU ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-RESTART-SOAK]'; then echo "FAIL: production kernel contains HTTPS restart/soak marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-RESTART-SOAK]'; then echo "FAIL: production ISO contains HTTPS restart/soak marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production kernel contains HTTPS QEMU marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-QEMU]'; then echo "FAIL: production ISO contains HTTPS QEMU marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(HTTPS_RESTART_SOAK_QEMU_ISO)' \
		./scripts/test-qemu-https-restart-soak.sh
	@echo "HTTPS restart/soak QEMU marker checks: after harness"
	@strings $(HTTPS_RESTART_SOAK_QEMU_KERNEL) | grep -Fq '[HTTPS-RESTART-SOAK] proof OK' || { echo "FAIL: HTTPS restart/soak QEMU kernel missing proof OK marker after harness" >&2; exit 1; }
	@strings $(HTTPS_RESTART_SOAK_QEMU_ISO) | grep -Fq '[HTTPS-RESTART-SOAK] proof OK' || { echo "FAIL: HTTPS restart/soak QEMU ISO missing proof OK marker after harness" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HTTPS-RESTART-SOAK]'; then echo "FAIL: production kernel contains HTTPS restart/soak marker after harness" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HTTPS-RESTART-SOAK]'; then echo "FAIL: production ISO contains HTTPS restart/soak marker after harness" >&2; exit 1; fi

test-trust-policy-qemu: $(TRUST_POLICY_ISO) iso
	@echo "Trust-policy marker checks: before QEMU proof"
	@strings $(TRUST_POLICY_KERNEL) | grep -Fq '[TRUST-POLICY] proof init' || { echo "FAIL: trust-policy kernel missing proof marker" >&2; exit 1; }
	@strings $(TRUST_POLICY_ISO) | grep -Fq '[TRUST-POLICY] proof init' || { echo "FAIL: trust-policy ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[TRUST-POLICY]'; then echo "FAIL: production kernel contains trust-policy marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TRUST-POLICY]'; then echo "FAIL: production ISO contains trust-policy marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(TRUST_POLICY_ISO)' \
		./scripts/test-qemu-trust-policy.sh
	@echo "Trust-policy marker checks: after QEMU proof"
	@strings $(TRUST_POLICY_KERNEL) | grep -Fq '[TRUST-POLICY] proof OK' || { echo "FAIL: trust-policy kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(TRUST_POLICY_ISO) | grep -Fq '[TRUST-POLICY] proof OK' || { echo "FAIL: trust-policy ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[TRUST-POLICY]'; then echo "FAIL: production kernel contains trust-policy marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TRUST-POLICY]'; then echo "FAIL: production ISO contains trust-policy marker after proof" >&2; exit 1; fi

test-update-lifecycle-qemu: $(UPDATE_LIFECYCLE_ISO) iso
	@echo "Update-lifecycle marker checks: before QEMU proof"
	@strings $(UPDATE_LIFECYCLE_KERNEL) | grep -Fq '[UPDATE-LIFECYCLE] proof init' || { echo "FAIL: update-lifecycle kernel missing proof marker" >&2; exit 1; }
	@strings $(UPDATE_LIFECYCLE_ISO) | grep -Fq '[UPDATE-LIFECYCLE] proof init' || { echo "FAIL: update-lifecycle ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[UPDATE-LIFECYCLE]'; then echo "FAIL: production kernel contains update-lifecycle marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[UPDATE-LIFECYCLE]'; then echo "FAIL: production ISO contains update-lifecycle marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(UPDATE_LIFECYCLE_ISO)' \
		./scripts/test-qemu-update-lifecycle.sh
	@echo "Update-lifecycle marker checks: after QEMU proof"
	@strings $(UPDATE_LIFECYCLE_KERNEL) | grep -Fq '[UPDATE-LIFECYCLE] proof OK' || { echo "FAIL: update-lifecycle kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(UPDATE_LIFECYCLE_ISO) | grep -Fq '[UPDATE-LIFECYCLE] proof OK' || { echo "FAIL: update-lifecycle ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[UPDATE-LIFECYCLE]'; then echo "FAIL: production kernel contains update-lifecycle marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[UPDATE-LIFECYCLE]'; then echo "FAIL: production ISO contains update-lifecycle marker after proof" >&2; exit 1; fi

test-supportability-qemu: $(SUPPORTABILITY_ISO) iso
	@echo "Supportability marker checks: before QEMU proof"
	@strings $(SUPPORTABILITY_KERNEL) | grep -Fq '[SUPPORTABILITY] proof init' || { echo "FAIL: supportability kernel missing proof marker" >&2; exit 1; }
	@strings $(SUPPORTABILITY_ISO) | grep -Fq '[SUPPORTABILITY] proof init' || { echo "FAIL: supportability ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SUPPORTABILITY]'; then echo "FAIL: production kernel contains supportability marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SUPPORTABILITY]'; then echo "FAIL: production ISO contains supportability marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SUPPORTABILITY_ISO)' \
		./scripts/test-qemu-supportability.sh
	@echo "Supportability marker checks: after QEMU proof"
	@strings $(SUPPORTABILITY_KERNEL) | grep -Fq '[SUPPORTABILITY] proof OK' || { echo "FAIL: supportability kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SUPPORTABILITY_ISO) | grep -Fq '[SUPPORTABILITY] proof OK' || { echo "FAIL: supportability ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SUPPORTABILITY]'; then echo "FAIL: production kernel contains supportability marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SUPPORTABILITY]'; then echo "FAIL: production ISO contains supportability marker after proof" >&2; exit 1; fi

test-accounting-boundary-qemu: $(ACCOUNTING_BOUNDARY_ISO) iso
	@echo "Accounting-boundary marker checks: before QEMU proof"
	@strings $(ACCOUNTING_BOUNDARY_KERNEL) | grep -Fq '[ACCOUNTING-BOUNDARY] proof init' || { echo "FAIL: accounting-boundary kernel missing proof marker" >&2; exit 1; }
	@strings $(ACCOUNTING_BOUNDARY_ISO) | grep -Fq '[ACCOUNTING-BOUNDARY] proof init' || { echo "FAIL: accounting-boundary ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ACCOUNTING-BOUNDARY]'; then echo "FAIL: production kernel contains accounting-boundary marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ACCOUNTING-BOUNDARY]'; then echo "FAIL: production ISO contains accounting-boundary marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(ACCOUNTING_BOUNDARY_ISO)' \
		./scripts/test-qemu-accounting-boundary.sh
	@echo "Accounting-boundary marker checks: after QEMU proof"
	@strings $(ACCOUNTING_BOUNDARY_KERNEL) | grep -Fq '[ACCOUNTING-BOUNDARY] proof OK' || { echo "FAIL: accounting-boundary kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(ACCOUNTING_BOUNDARY_ISO) | grep -Fq '[ACCOUNTING-BOUNDARY] proof OK' || { echo "FAIL: accounting-boundary ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ACCOUNTING-BOUNDARY]'; then echo "FAIL: production kernel contains accounting-boundary marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ACCOUNTING-BOUNDARY]'; then echo "FAIL: production ISO contains accounting-boundary marker after proof" >&2; exit 1; fi

test-storage-interruption-qemu: $(STORAGE_INTERRUPTION_ISO) iso
	@echo "Storage-interruption marker checks: before QEMU proof"
	@strings $(STORAGE_INTERRUPTION_KERNEL) | grep -Fq '[STORAGE-INTERRUPTION] proof init' || { echo "FAIL: storage-interruption kernel missing proof marker" >&2; exit 1; }
	@strings $(STORAGE_INTERRUPTION_ISO) | grep -Fq '[STORAGE-INTERRUPTION] proof init' || { echo "FAIL: storage-interruption ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[STORAGE-INTERRUPTION]'; then echo "FAIL: production kernel contains storage-interruption marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[STORAGE-INTERRUPTION]'; then echo "FAIL: production ISO contains storage-interruption marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(STORAGE_INTERRUPTION_ISO)' \
		./scripts/test-qemu-storage-interruption.sh
	@echo "Storage-interruption marker checks: after QEMU proof"
	@strings $(STORAGE_INTERRUPTION_KERNEL) | grep -Fq '[STORAGE-INTERRUPTION] proof OK' || { echo "FAIL: storage-interruption kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(STORAGE_INTERRUPTION_ISO) | grep -Fq '[STORAGE-INTERRUPTION] proof OK' || { echo "FAIL: storage-interruption ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[STORAGE-INTERRUPTION]'; then echo "FAIL: production kernel contains storage-interruption marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[STORAGE-INTERRUPTION]'; then echo "FAIL: production ISO contains storage-interruption marker after proof" >&2; exit 1; fi

test-service-cascade-drill-qemu: $(SERVICE_CASCADE_DRILL_ISO) iso
	@echo "Service-cascade-drill marker checks: before QEMU proof"
	@strings $(SERVICE_CASCADE_DRILL_KERNEL) | grep -Fq '[SERVICE-CASCADE-DRILL] proof init' || { echo "FAIL: service-cascade-drill kernel missing proof marker" >&2; exit 1; }
	@strings $(SERVICE_CASCADE_DRILL_ISO) | grep -Fq '[SERVICE-CASCADE-DRILL] proof init' || { echo "FAIL: service-cascade-drill ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SERVICE-CASCADE-DRILL]'; then echo "FAIL: production kernel contains service-cascade-drill marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SERVICE-CASCADE-DRILL]'; then echo "FAIL: production ISO contains service-cascade-drill marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SERVICE_CASCADE_DRILL_ISO)' \
		./scripts/test-qemu-service-cascade-drill.sh
	@echo "Service-cascade-drill marker checks: after QEMU proof"
	@strings $(SERVICE_CASCADE_DRILL_KERNEL) | grep -Fq '[SERVICE-CASCADE-DRILL] proof OK' || { echo "FAIL: service-cascade-drill kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(SERVICE_CASCADE_DRILL_ISO) | grep -Fq '[SERVICE-CASCADE-DRILL] proof OK' || { echo "FAIL: service-cascade-drill ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SERVICE-CASCADE-DRILL]'; then echo "FAIL: production kernel contains service-cascade-drill marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SERVICE-CASCADE-DRILL]'; then echo "FAIL: production ISO contains service-cascade-drill marker after proof" >&2; exit 1; fi

test-local-control-outage-drill-qemu: $(LOCAL_CONTROL_OUTAGE_DRILL_ISO) iso
	@echo "Local-control-outage-drill marker checks: before QEMU proof"
	@strings $(LOCAL_CONTROL_OUTAGE_DRILL_KERNEL) | grep -Fq '[LOCAL-CONTROL-OUTAGE-DRILL] proof init' || { echo "FAIL: local-control-outage-drill kernel missing proof marker" >&2; exit 1; }
	@strings $(LOCAL_CONTROL_OUTAGE_DRILL_ISO) | grep -Fq '[LOCAL-CONTROL-OUTAGE-DRILL] proof init' || { echo "FAIL: local-control-outage-drill ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[LOCAL-CONTROL-OUTAGE-DRILL]'; then echo "FAIL: production kernel contains local-control-outage-drill marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[LOCAL-CONTROL-OUTAGE-DRILL]'; then echo "FAIL: production ISO contains local-control-outage-drill marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(LOCAL_CONTROL_OUTAGE_DRILL_ISO)' \
		./scripts/test-qemu-local-control-outage-drill.sh
	@echo "Local-control-outage-drill marker checks: after QEMU proof"
	@strings $(LOCAL_CONTROL_OUTAGE_DRILL_KERNEL) | grep -Fq '[LOCAL-CONTROL-OUTAGE-DRILL] proof OK' || { echo "FAIL: local-control-outage-drill kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(LOCAL_CONTROL_OUTAGE_DRILL_ISO) | grep -Fq '[LOCAL-CONTROL-OUTAGE-DRILL] proof OK' || { echo "FAIL: local-control-outage-drill ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[LOCAL-CONTROL-OUTAGE-DRILL]'; then echo "FAIL: production kernel contains local-control-outage-drill marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[LOCAL-CONTROL-OUTAGE-DRILL]'; then echo "FAIL: production ISO contains local-control-outage-drill marker after proof" >&2; exit 1; fi

test-network-partition-drill-qemu: $(NETWORK_PARTITION_DRILL_ISO) iso
	@echo "Network-partition-drill marker checks: before QEMU proof"
	@strings $(NETWORK_PARTITION_DRILL_KERNEL) | grep -Fq '[NETWORK-PARTITION-DRILL] proof init' || { echo "FAIL: network-partition-drill kernel missing proof marker" >&2; exit 1; }
	@strings $(NETWORK_PARTITION_DRILL_ISO) | grep -Fq '[NETWORK-PARTITION-DRILL] proof init' || { echo "FAIL: network-partition-drill ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NETWORK-PARTITION-DRILL]'; then echo "FAIL: production kernel contains network-partition-drill marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NETWORK-PARTITION-DRILL]'; then echo "FAIL: production ISO contains network-partition-drill marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(NETWORK_PARTITION_DRILL_ISO)' \
		./scripts/test-qemu-network-partition-drill.sh
	@echo "Network-partition-drill marker checks: after QEMU proof"
	@strings $(NETWORK_PARTITION_DRILL_KERNEL) | grep -Fq '[NETWORK-PARTITION-DRILL] proof OK' || { echo "FAIL: network-partition-drill kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(NETWORK_PARTITION_DRILL_ISO) | grep -Fq '[NETWORK-PARTITION-DRILL] proof OK' || { echo "FAIL: network-partition-drill ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[NETWORK-PARTITION-DRILL]'; then echo "FAIL: production kernel contains network-partition-drill marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[NETWORK-PARTITION-DRILL]'; then echo "FAIL: production ISO contains network-partition-drill marker after proof" >&2; exit 1; fi

test-trust-failure-drill-qemu: $(TRUST_FAILURE_DRILL_ISO) iso
	@echo "Trust-failure-drill marker checks: before QEMU proof"
	@strings $(TRUST_FAILURE_DRILL_KERNEL) | grep -Fq '[TRUST-FAILURE-DRILL] proof init' || { echo "FAIL: trust-failure-drill kernel missing proof marker" >&2; exit 1; }
	@strings $(TRUST_FAILURE_DRILL_ISO) | grep -Fq '[TRUST-FAILURE-DRILL] proof init' || { echo "FAIL: trust-failure-drill ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[TRUST-FAILURE-DRILL]'; then echo "FAIL: production kernel contains trust-failure-drill marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TRUST-FAILURE-DRILL]'; then echo "FAIL: production ISO contains trust-failure-drill marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(TRUST_FAILURE_DRILL_ISO)' \
		./scripts/test-qemu-trust-failure-drill.sh
	@echo "Trust-failure-drill marker checks: after QEMU proof"
	@strings $(TRUST_FAILURE_DRILL_KERNEL) | grep -Fq '[TRUST-FAILURE-DRILL] proof OK' || { echo "FAIL: trust-failure-drill kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(TRUST_FAILURE_DRILL_ISO) | grep -Fq '[TRUST-FAILURE-DRILL] proof OK' || { echo "FAIL: trust-failure-drill ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[TRUST-FAILURE-DRILL]'; then echo "FAIL: production kernel contains trust-failure-drill marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[TRUST-FAILURE-DRILL]'; then echo "FAIL: production ISO contains trust-failure-drill marker after proof" >&2; exit 1; fi

test-update-failure-drill-qemu: $(UPDATE_FAILURE_DRILL_ISO) iso
	@echo "Update-failure-drill marker checks: before QEMU proof"
	@strings $(UPDATE_FAILURE_DRILL_KERNEL) | grep -Fq '[UPDATE-FAILURE-DRILL] proof init' || { echo "FAIL: update-failure-drill kernel missing proof marker" >&2; exit 1; }
	@strings $(UPDATE_FAILURE_DRILL_ISO) | grep -Fq '[UPDATE-FAILURE-DRILL] proof init' || { echo "FAIL: update-failure-drill ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[UPDATE-FAILURE-DRILL]'; then echo "FAIL: production kernel contains update-failure-drill marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[UPDATE-FAILURE-DRILL]'; then echo "FAIL: production ISO contains update-failure-drill marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(UPDATE_FAILURE_DRILL_ISO)' \
		./scripts/test-qemu-update-failure-drill.sh
	@echo "Update-failure-drill marker checks: after QEMU proof"
	@strings $(UPDATE_FAILURE_DRILL_KERNEL) | grep -Fq '[UPDATE-FAILURE-DRILL] proof OK' || { echo "FAIL: update-failure-drill kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(UPDATE_FAILURE_DRILL_ISO) | grep -Fq '[UPDATE-FAILURE-DRILL] proof OK' || { echo "FAIL: update-failure-drill ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[UPDATE-FAILURE-DRILL]'; then echo "FAIL: production kernel contains update-failure-drill marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[UPDATE-FAILURE-DRILL]'; then echo "FAIL: production ISO contains update-failure-drill marker after proof" >&2; exit 1; fi

test-integration-replay-outage-drill-qemu: $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO) iso
	@echo "Integration-replay-outage-drill marker checks: before QEMU proof"
	@strings $(INTEGRATION_REPLAY_OUTAGE_DRILL_KERNEL) | grep -Fq '[INTEGRATION-REPLAY-OUTAGE-DRILL] proof init' || { echo "FAIL: integration-replay-outage-drill kernel missing proof marker" >&2; exit 1; }
	@strings $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO) | grep -Fq '[INTEGRATION-REPLAY-OUTAGE-DRILL] proof init' || { echo "FAIL: integration-replay-outage-drill ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[INTEGRATION-REPLAY-OUTAGE-DRILL]'; then echo "FAIL: production kernel contains integration-replay-outage-drill marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[INTEGRATION-REPLAY-OUTAGE-DRILL]'; then echo "FAIL: production ISO contains integration-replay-outage-drill marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO)' \
		./scripts/test-qemu-integration-replay-outage-drill.sh
	@echo "Integration-replay-outage-drill marker checks: after QEMU proof"
	@strings $(INTEGRATION_REPLAY_OUTAGE_DRILL_KERNEL) | grep -Fq '[INTEGRATION-REPLAY-OUTAGE-DRILL] proof OK' || { echo "FAIL: integration-replay-outage-drill kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO) | grep -Fq '[INTEGRATION-REPLAY-OUTAGE-DRILL] proof OK' || { echo "FAIL: integration-replay-outage-drill ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[INTEGRATION-REPLAY-OUTAGE-DRILL]'; then echo "FAIL: production kernel contains integration-replay-outage-drill marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[INTEGRATION-REPLAY-OUTAGE-DRILL]'; then echo "FAIL: production ISO contains integration-replay-outage-drill marker after proof" >&2; exit 1; fi

test-power-cycle-continuity-drill-qemu: $(POWER_CYCLE_CONTINUITY_DRILL_ISO) iso
	@echo "Power-cycle-continuity-drill marker checks: before QEMU proof"
	@strings $(POWER_CYCLE_CONTINUITY_DRILL_KERNEL) | grep -Fq '[POWER-CYCLE-DRILL] proof init' || { echo "FAIL: power-cycle-continuity-drill kernel missing proof marker" >&2; exit 1; }
	@strings $(POWER_CYCLE_CONTINUITY_DRILL_ISO) | grep -Fq '[POWER-CYCLE-DRILL] proof init' || { echo "FAIL: power-cycle-continuity-drill ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[POWER-CYCLE-DRILL]'; then echo "FAIL: production kernel contains power-cycle-continuity-drill marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[POWER-CYCLE-DRILL]'; then echo "FAIL: production ISO contains power-cycle-continuity-drill marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(POWER_CYCLE_CONTINUITY_DRILL_ISO)' \
		./scripts/test-qemu-power-cycle-continuity-drill.sh
	@echo "Power-cycle-continuity-drill marker checks: after QEMU proof"
	@strings $(POWER_CYCLE_CONTINUITY_DRILL_KERNEL) | grep -Fq '[POWER-CYCLE-DRILL] proof OK' || { echo "FAIL: power-cycle-continuity-drill kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(POWER_CYCLE_CONTINUITY_DRILL_ISO) | grep -Fq '[POWER-CYCLE-DRILL] proof OK' || { echo "FAIL: power-cycle-continuity-drill ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[POWER-CYCLE-DRILL]'; then echo "FAIL: production kernel contains power-cycle-continuity-drill marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[POWER-CYCLE-DRILL]'; then echo "FAIL: production ISO contains power-cycle-continuity-drill marker after proof" >&2; exit 1; fi

test-entropy-qemu: $(ENTROPY_ISO) iso
	@echo "Entropy QEMU proof marker checks: before QEMU proof"
	@strings $(ENTROPY_KERNEL) | grep -Fq '[ENTROPY] proof init' || { echo "FAIL: entropy kernel missing proof marker" >&2; exit 1; }
	@strings $(ENTROPY_ISO) | grep -Fq '[ENTROPY] proof init' || { echo "FAIL: entropy ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ENTROPY]'; then echo "FAIL: production kernel contains entropy marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ENTROPY]'; then echo "FAIL: production ISO contains entropy marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq 'HFOC-ENTROPY-QEMU-SECRET'; then echo "FAIL: production kernel contains entropy leak sentinel" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq 'HFOC-ENTROPY-QEMU-SECRET'; then echo "FAIL: production ISO contains entropy leak sentinel" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(ENTROPY_ISO)' \
		./scripts/test-qemu-entropy.sh
	@echo "Entropy QEMU proof marker checks: after QEMU proof"
	@strings $(ENTROPY_KERNEL) | grep -Fq '[ENTROPY] proof OK' || { echo "FAIL: entropy kernel missing proof OK marker after QEMU proof" >&2; exit 1; }
	@strings $(ENTROPY_ISO) | grep -Fq '[ENTROPY] proof OK' || { echo "FAIL: entropy ISO missing proof OK marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[ENTROPY]'; then echo "FAIL: production kernel contains entropy marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[ENTROPY]'; then echo "FAIL: production ISO contains entropy marker after proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq 'HFOC-ENTROPY-QEMU-SECRET'; then echo "FAIL: production kernel contains entropy leak sentinel after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq 'HFOC-ENTROPY-QEMU-SECRET'; then echo "FAIL: production ISO contains entropy leak sentinel after proof" >&2; exit 1; fi

test-user-iretq: $(USER_IRETQ_ISO) iso
	@echo "User iretq marker checks: before QEMU proof"
	@strings $(USER_IRETQ_KERNEL) | grep -Fq '[USER] proof init' || { echo "FAIL: user iretq kernel missing proof marker" >&2; exit 1; }
	@strings $(USER_IRETQ_ISO) | grep -Fq '[USER] proof init' || { echo "FAIL: user iretq ISO missing proof marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[USER] proof init'; then echo "FAIL: production kernel contains user iretq marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[USER] proof init'; then echo "FAIL: production ISO contains user iretq marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(USER_IRETQ_ISO)' \
		./scripts/test-qemu-user-iretq.sh
	@echo "User iretq marker checks: after QEMU proof"
	@strings $(USER_IRETQ_KERNEL) | grep -Fq '[USER] proof OK' || { echo "FAIL: user iretq kernel missing proof OK marker" >&2; exit 1; }
	@strings $(USER_IRETQ_ISO) | grep -Fq '[USER] proof OK' || { echo "FAIL: user iretq ISO missing proof OK marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[USER] proof init'; then echo "FAIL: production kernel contains user iretq marker after proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[USER] proof init'; then echo "FAIL: production ISO contains user iretq marker after proof" >&2; exit 1; fi

test-scheduler-host: kernel/include/scheduler.h kernel/src/scheduler.c \
		kernel/include/context.h kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S tests/scheduler-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S tests/scheduler-host.c \
		-o build/test-scheduler-host -no-pie
	build/test-scheduler-host

test-scheduler-rsp0-host: kernel/include/scheduler.h \
		kernel/include/context.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-rsp0-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-rsp0-host.c \
		-o build/test-scheduler-rsp0-host -no-pie
	build/test-scheduler-rsp0-host

test-scheduler-thread-host: kernel/include/scheduler.h \
		kernel/include/context.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/sched_trampoline.S \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-thread-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST -DHFOC_SCHEDULER_THREAD_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c \
		kernel/src/arch/x86_64/sched_trampoline.S \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-thread-host.c \
		-o build/test-scheduler-thread-host -no-pie
	build/test-scheduler-thread-host

test-scheduler-yield-host: kernel/include/scheduler.h \
		kernel/include/context.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-yield-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-yield-host.c \
		-o build/test-scheduler-yield-host -no-pie
	build/test-scheduler-yield-host

test-scheduler-tick-host: kernel/include/scheduler.h \
		kernel/include/context.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-tick-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-tick-host.c \
		-o build/test-scheduler-tick-host -no-pie
	build/test-scheduler-tick-host

test-scheduler-wait-host: kernel/include/scheduler.h \
		kernel/include/context.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-wait-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-wait-host.c \
		-o build/test-scheduler-wait-host -no-pie
	build/test-scheduler-wait-host

test-scheduler-accounting-host: kernel/include/scheduler.h \
		kernel/include/context.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-accounting-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-accounting-host.c \
		-o build/test-scheduler-accounting-host -no-pie
	build/test-scheduler-accounting-host

test-scheduler-discipline-host: kernel/include/scheduler.h \
		kernel/include/context.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-discipline-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-discipline-host.c \
		-o build/test-scheduler-discipline-host -no-pie
	build/test-scheduler-discipline-host

test-scheduler-exit-host: kernel/include/scheduler.h \
		kernel/include/context.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-exit-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-exit-host.c \
		-o build/test-scheduler-exit-host -no-pie
	build/test-scheduler-exit-host

test-scheduler-stress-host: kernel/include/scheduler.h \
		kernel/include/context.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/sched_trampoline.S \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-stress-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SCHEDULER_HOST_TEST -DHFOC_SCHEDULER_THREAD_HOST_TEST \
		-Ikernel/include \
		kernel/src/scheduler.c \
		kernel/src/arch/x86_64/sched_trampoline.S \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/scheduler-stress-host.c \
		-o build/test-scheduler-stress-host -no-pie
	build/test-scheduler-stress-host

test-process-host: kernel/include/process.h kernel/include/descriptor.h \
		kernel/include/capability.h \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c tests/process-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PROCESS_HOST_TEST \
		-Ikernel/include \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c tests/process-host.c \
		-o build/test-process-host -no-pie
	build/test-process-host

test-syscall-origin-host: kernel/include/process.h kernel/include/descriptor.h \
		kernel/include/capability.h kernel/include/syscall.h \
		kernel/include/user.h \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c \
		kernel/src/syscall.c kernel/src/syscall_entry.S \
		tests/syscall-origin-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PROCESS_HOST_TEST -DHFOC_SYSCALL_ORIGIN_HOST_TEST \
		-Ikernel/include \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c \
		kernel/src/syscall.c kernel/src/syscall_entry.S \
		tests/syscall-origin-host.c \
		-o build/test-syscall-origin-host -no-pie
	build/test-syscall-origin-host

test-syscall-gate-host: kernel/include/process.h kernel/include/descriptor.h \
		kernel/include/capability.h kernel/include/syscall.h \
		kernel/include/user.h kernel/include/vmm.h \
		kernel/src/capability.c \
		kernel/src/syscall.c kernel/src/syscall_entry.S \
		tests/syscall-gate-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SYSCALL_HOST_TEST -DHFOC_SYSCALL_GATE_HOST_TEST \
		-Ikernel/include \
		kernel/src/capability.c \
		kernel/src/syscall.c kernel/src/syscall_entry.S \
		tests/syscall-gate-host.c \
		-o build/test-syscall-gate-host -no-pie
	build/test-syscall-gate-host

test-process-create-host: kernel/include/process.h kernel/include/context.h \
		kernel/include/descriptor.h kernel/include/capability.h \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/process-create-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PROCESS_HOST_TEST \
		-Ikernel/include \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/process-create-host.c \
		-o build/test-process-create-host -no-pie
	build/test-process-create-host

test-descriptor-host: kernel/include/descriptor.h \
		kernel/src/descriptor.c tests/descriptor-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_DESCRIPTOR_HOST_TEST \
		-Ikernel/include \
		kernel/src/descriptor.c tests/descriptor-host.c \
		-o build/test-descriptor-host -no-pie
	build/test-descriptor-host

test-vfs-host: kernel/include/vfs.h kernel/include/ramfs.h \
		kernel/include/descriptor.h kernel/include/vfs_fd.h \
		kernel/src/vfs.c kernel/src/ramfs.c kernel/src/descriptor.c \
		tests/vfs-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VFS_HOST_TEST \
		-Ikernel/include \
		kernel/src/vfs.c kernel/src/ramfs.c kernel/src/descriptor.c \
		tests/vfs-host.c -o build/test-vfs-host -no-pie
	build/test-vfs-host

test-hfocfs-host: kernel/include/hfocfs.h kernel/include/sha256.h \
		kernel/include/block_device.h kernel/src/hfocfs.c \
		kernel/src/sha256.c kernel/src/block_device.c tests/hfocfs-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_HFOCFS_HOST_TEST \
		-Ikernel/include \
		kernel/src/hfocfs.c kernel/src/sha256.c kernel/src/block_device.c \
		tests/hfocfs-host.c -o build/test-hfocfs-host -no-pie
	build/test-hfocfs-host

test-hfocfs-workspace-host: kernel/include/hfocfs_workspace.h \
		kernel/include/hfocfs.h kernel/include/vfs.h \
		kernel/include/ramfs.h kernel/include/vfs_fd.h \
		kernel/include/appliance_state_block_device.h \
		kernel/src/hfocfs.c kernel/src/hfocfs_workspace.c \
		kernel/src/appliance_state_block_device.c kernel/src/vfs.c \
		kernel/src/ramfs.c kernel/src/descriptor.c kernel/src/sha256.c \
		kernel/src/block_device.c tests/hfocfs-workspace-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_HFOCFS_HOST_TEST \
		-Ikernel/include \
		kernel/src/hfocfs.c kernel/src/hfocfs_workspace.c \
		kernel/src/appliance_state_block_device.c kernel/src/vfs.c \
		kernel/src/ramfs.c kernel/src/descriptor.c kernel/src/sha256.c \
		kernel/src/block_device.c tests/hfocfs-workspace-host.c \
		-o build/test-hfocfs-workspace-host -no-pie
	build/test-hfocfs-workspace-host

test-initramfs-host: kernel/include/initramfs.h kernel/include/vfs.h \
		kernel/include/vfs_mount.h kernel/include/ramfs.h \
		kernel/include/descriptor.h kernel/include/vfs_fd.h \
		kernel/src/initramfs.c kernel/src/vfs.c kernel/src/ramfs.c \
		kernel/src/vfs_mount.c kernel/src/descriptor.c \
		scripts/hfoc-rootfs-pack.py tests/initramfs-host.c
	rm -rf build/initramfs-fixture
	mkdir -p build/initramfs-fixture/bin build/initramfs-fixture/etc
	printf 'native shell placeholder\n' > build/initramfs-fixture/bin/sh
	printf 'HFOC OS\n' > build/initramfs-fixture/etc/motd
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INITRAMFS_HOST_TEST \
		-Ikernel/include \
		kernel/src/initramfs.c kernel/src/vfs.c kernel/src/ramfs.c \
		kernel/src/vfs_mount.c kernel/src/descriptor.c \
		tests/initramfs-host.c -o build/test-initramfs-host -no-pie
	python3 scripts/hfoc-rootfs-pack.py build/initramfs-fixture \
		build/initramfs.fixture
	build/test-initramfs-host build/initramfs.fixture

test-vfs-fd-host: kernel/include/descriptor.h kernel/include/vfs_fd.h \
		kernel/include/vfs.h kernel/include/ramfs.h \
		kernel/src/descriptor.c kernel/src/vfs.c kernel/src/ramfs.c \
		tests/vfs-fd-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VFS_FD_HOST_TEST \
		-Ikernel/include \
		kernel/src/descriptor.c kernel/src/vfs.c kernel/src/ramfs.c \
		tests/vfs-fd-host.c -o build/test-vfs-fd-host -no-pie
	build/test-vfs-fd-host
test-user-copy-host: kernel/include/user_copy.h kernel/include/vmm.h \
		kernel/src/user_copy.c kernel/src/vmm.c tests/user-copy-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VMM_HOST_TEST \
		-Ikernel/include \
		kernel/src/user_copy.c kernel/src/vmm.c \
		tests/user-copy-host.c -o build/test-user-copy-host -no-pie
	build/test-user-copy-host

test-pipe-host: kernel/include/pipe.h kernel/include/descriptor.h \
		kernel/include/vfs.h kernel/include/scheduler.h \
		kernel/src/pipe.c tests/pipe-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_PIPE_HOST_TEST \
		-Ikernel/include \
		kernel/src/pipe.c tests/pipe-host.c \
		-o build/test-pipe-host -no-pie
	build/test-pipe-host

test-user-runtime-host: kernel/include/syscall.h kernel/include/vfs.h \
		user/libhfoc/hfoc.h user/libhfoc/string.c user/libhfoc/format.c \
		user/libhfoc/alloc.c tests/user-runtime-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_USER_RUNTIME_HOST_TEST \
		-Iuser/libhfoc -Ikernel/include \
		tests/user-runtime-host.c user/libhfoc/string.c \
		user/libhfoc/format.c user/libhfoc/alloc.c \
		user/libhfoc/syscall.c \
		-o build/test-user-runtime-host -no-pie
	build/test-user-runtime-host

test-elf-user-host: kernel/include/elf_user.h kernel/include/vmm.h \
		kernel/src/elf_user.c tests/elf-user-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/elf_user.c tests/elf-user-host.c \
		-o build/test-elf-user-host -no-pie
	build/test-elf-user-host

test-syscall-file-host: kernel/include/syscall_file.h kernel/include/syscall.h \
		kernel/include/user_copy.h kernel/include/kernel_vfs.h \
		kernel/include/descriptor.h kernel/include/vfs.h \
		kernel/include/vfs_fd.h kernel/include/pipe.h \
		kernel/src/syscall_file.c kernel/src/pipe.c \
		kernel/src/user_copy.c kernel/src/descriptor.c kernel/src/vfs.c \
		kernel/src/ramfs.c tests/syscall-file-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_USER_COPY_HOST_TEST -DHFOC_PIPE_HOST_TEST \
		-Ikernel/include \
		kernel/src/syscall_file.c kernel/src/pipe.c \
		kernel/src/user_copy.c \
		kernel/src/descriptor.c kernel/src/vfs.c kernel/src/ramfs.c \
		tests/syscall-file-host.c -o build/test-syscall-file-host -no-pie
	build/test-syscall-file-host


test-capability-host: kernel/include/capability.h \
		kernel/src/capability.c tests/capability-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CAPABILITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/capability.c tests/capability-host.c \
		-o build/test-capability-host -no-pie
	build/test-capability-host

test-event-ring-host: kernel/include/event_ring.h \
		kernel/src/event_ring.c tests/event-ring-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_EVENT_RING_HOST_TEST \
		-Ikernel/include \
		kernel/src/event_ring.c tests/event-ring-host.c \
		-o build/test-event-ring-host -no-pie
	build/test-event-ring-host

test-audit-snapshot-host: kernel/include/event_ring.h kernel/include/audit.h \
		kernel/src/event_ring.c kernel/src/audit.c \
		tests/audit-snapshot-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_AUDIT_SNAPSHOT_HOST_TEST \
		-Ikernel/include \
		kernel/src/event_ring.c kernel/src/audit.c \
		tests/audit-snapshot-host.c \
		-o build/test-audit-snapshot-host -no-pie
	build/test-audit-snapshot-host

test-audit-host: kernel/include/event_ring.h kernel/include/audit.h \
		kernel/src/event_ring.c kernel/src/audit.c tests/audit-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_AUDIT_HOST_TEST \
		-Ikernel/include \
		kernel/src/event_ring.c kernel/src/audit.c tests/audit-host.c \
		-o build/test-audit-host -no-pie
	build/test-audit-host

test-audit-observability-host: kernel/include/event_ring.h kernel/include/audit.h \
		kernel/src/event_ring.c kernel/src/audit.c \
		tests/audit-observability-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_AUDIT_OBSERVABILITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/event_ring.c kernel/src/audit.c \
		tests/audit-observability-host.c \
		-o build/test-audit-observability-host -no-pie
	build/test-audit-observability-host

test-config-schema-host: kernel/include/config_schema.h \
		kernel/src/config_schema.c tests/config-schema-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CONFIG_SCHEMA_HOST_TEST \
		-Ikernel/include \
		kernel/src/config_schema.c tests/config-schema-host.c \
		-o build/test-config-schema-host -no-pie
	build/test-config-schema-host

test-config-snapshot-host: kernel/include/config_schema.h \
		kernel/src/config_schema.c tests/config-snapshot-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CONFIG_SNAPSHOT_HOST_TEST \
		-Ikernel/include \
		kernel/src/config_schema.c tests/config-snapshot-host.c \
		-o build/test-config-snapshot-host -no-pie
	build/test-config-snapshot-host

test-boot-policy-host: kernel/include/boot_policy.h \
		kernel/src/boot_policy.c tests/boot-policy-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_BOOT_POLICY_HOST_TEST \
		-Ikernel/include \
		kernel/src/boot_policy.c tests/boot-policy-host.c \
		-o build/test-boot-policy-host -no-pie
	build/test-boot-policy-host

test-system-image-host: kernel/include/system_image.h \
		kernel/src/system_image.c tests/system-image-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SYSTEM_IMAGE_HOST_TEST \
		-Ikernel/include \
		kernel/src/system_image.c tests/system-image-host.c \
		-o build/test-system-image-host -no-pie
	build/test-system-image-host

test-sha256-host: kernel/include/sha256.h kernel/src/sha256.c \
		tests/sha256-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SHA256_HOST_TEST \
		-Ikernel/include \
		kernel/src/sha256.c tests/sha256-host.c \
		-o build/test-sha256-host -no-pie
	build/test-sha256-host

test-image-integrity-host: kernel/include/image_integrity.h \
		kernel/include/system_image.h kernel/include/sha256.h \
		kernel/src/image_integrity.c kernel/src/sha256.c \
		tests/image-integrity-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_IMAGE_INTEGRITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/image_integrity.c kernel/src/sha256.c \
		tests/image-integrity-host.c \
		-o build/test-image-integrity-host -no-pie
	build/test-image-integrity-host

test-recovery-image-host: kernel/include/recovery_image.h \
		kernel/include/boot_policy.h kernel/include/image_integrity.h \
		kernel/include/system_image.h kernel/include/sha256.h \
		kernel/src/recovery_image.c kernel/src/boot_policy.c \
		kernel/src/image_integrity.c kernel/src/system_image.c \
		kernel/src/sha256.c tests/recovery-image-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_RECOVERY_IMAGE_HOST_TEST \
		-Ikernel/include \
		kernel/src/recovery_image.c kernel/src/boot_policy.c \
		kernel/src/image_integrity.c kernel/src/system_image.c \
		kernel/src/sha256.c tests/recovery-image-host.c \
		-o build/test-recovery-image-host -no-pie
	build/test-recovery-image-host

test-block-device-host: kernel/include/block_device.h \
		kernel/src/block_device.c tests/block-device-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_BLOCK_DEVICE_HOST_TEST \
		-Ikernel/include \
		kernel/src/block_device.c tests/block-device-host.c \
		-o build/test-block-device-host -no-pie
	build/test-block-device-host

test-data-region-host: kernel/include/block_device.h \
		kernel/include/data_region.h kernel/src/block_device.c \
		kernel/src/data_region.c tests/data-region-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_DATA_REGION_HOST_TEST \
		-Ikernel/include \
		kernel/src/data_region.c kernel/src/block_device.c \
		tests/data-region-host.c \
		-o build/test-data-region-host -no-pie
	build/test-data-region-host

test-block-cache-host: kernel/include/block_device.h \
		kernel/include/block_cache.h kernel/src/block_device.c \
		kernel/src/block_cache.c tests/block-cache-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_BLOCK_CACHE_HOST_TEST \
		-Ikernel/include \
		kernel/src/block_cache.c kernel/src/block_device.c \
		tests/block-cache-host.c \
		-o build/test-block-cache-host -no-pie
	build/test-block-cache-host

test-config-journal-host: kernel/include/block_device.h \
		kernel/include/config_journal.h kernel/include/sha256.h \
		kernel/src/block_device.c kernel/src/config_journal.c \
		kernel/src/sha256.c tests/config-journal-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CONFIG_JOURNAL_HOST_TEST \
		-Ikernel/include \
		kernel/src/config_journal.c kernel/src/block_device.c \
		kernel/src/sha256.c tests/config-journal-host.c \
		-o build/test-config-journal-host -no-pie
	build/test-config-journal-host

# ITEM-503: the bounded PEM decoder the BYO certificate import uses; the frozen
# mbedTLS profile has no PEM or base64, so the decoder is the product's own.
.PHONY: test-pem-decode-host
test-pem-decode-host: kernel/include/pem_decode.h kernel/src/pem_decode.c \
		tests/pem-decode-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/pem_decode.c tests/pem-decode-host.c \
		-o build/test-pem-decode-host -no-pie
	build/test-pem-decode-host

test-config-kv-host: kernel/include/config_kv.h tests/config-kv-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/config_kv.c tests/config-kv-host.c \
		-o build/test-config-kv-host -no-pie
	build/test-config-kv-host

test-state-schema-host: kernel/include/state_schema.h \
		kernel/include/config_journal.h kernel/include/config_kv.h \
		kernel/include/block_device.h kernel/include/sha256.h \
		kernel/src/state_schema.c kernel/src/config_journal.c \
		kernel/src/config_kv.c kernel/src/block_device.c \
		kernel/src/sha256.c tests/state-schema-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_STATE_SCHEMA_HOST_TEST \
		-Ikernel/include \
		kernel/src/state_schema.c kernel/src/config_journal.c \
		kernel/src/config_kv.c kernel/src/block_device.c \
		kernel/src/sha256.c tests/state-schema-host.c \
		-o build/test-state-schema-host -no-pie
	build/test-state-schema-host

test-state-migration-host: kernel/include/state_migration.h \
		kernel/include/state_schema.h kernel/include/config_kv.h \
		kernel/include/config_journal.h kernel/include/block_device.h \
		kernel/include/sha256.h kernel/src/state_migration.c \
		kernel/src/state_schema.c kernel/src/config_kv.c \
		kernel/src/config_journal.c kernel/src/block_device.c \
		kernel/src/sha256.c tests/state-migration-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_STATE_MIGRATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/state_migration.c kernel/src/state_schema.c \
		kernel/src/config_kv.c kernel/src/config_journal.c \
		kernel/src/block_device.c kernel/src/sha256.c \
		tests/state-migration-host.c \
		-o build/test-state-migration-host -no-pie
	build/test-state-migration-host

test-journal-compaction-host: kernel/include/config_kv.h \
		kernel/include/journal_compaction.h kernel/src/config_kv.c \
		kernel/src/journal_compaction.c tests/journal-compaction-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_JOURNAL_COMPACTION_HOST_TEST \
		-Ikernel/include \
		kernel/src/journal_compaction.c kernel/src/config_kv.c \
		tests/journal-compaction-host.c \
		-o build/test-journal-compaction-host -no-pie
	build/test-journal-compaction-host

test-backup-stream-host: kernel/include/config_kv.h \
		kernel/include/backup_stream.h kernel/include/sha256.h \
		kernel/src/config_kv.c kernel/src/backup_stream.c \
		kernel/src/sha256.c tests/backup-stream-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/backup_stream.c kernel/src/config_kv.c \
		kernel/src/sha256.c tests/backup-stream-host.c \
		-o build/test-backup-stream-host -no-pie
	build/test-backup-stream-host

test-backup-manifest-host: kernel/include/config_kv.h \
		kernel/include/backup_manifest.h \
		kernel/include/backup_stream.h kernel/include/state_schema.h \
		kernel/include/config_journal.h kernel/include/block_device.h \
		kernel/include/journal_compaction.h kernel/include/sha256.h \
		kernel/src/backup_manifest.c kernel/src/backup_stream.c \
		kernel/src/config_kv.c kernel/src/state_schema.c \
		kernel/src/config_journal.c kernel/src/block_device.c \
		kernel/src/journal_compaction.c kernel/src/sha256.c \
		tests/backup-manifest-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_BACKUP_MANIFEST_HOST_TEST \
		-Ikernel/include \
		kernel/src/backup_manifest.c kernel/src/backup_stream.c \
		kernel/src/config_kv.c kernel/src/state_schema.c \
		kernel/src/config_journal.c kernel/src/block_device.c \
		kernel/src/journal_compaction.c kernel/src/sha256.c \
		tests/backup-manifest-host.c \
		-o build/test-backup-manifest-host -no-pie
	build/test-backup-manifest-host

test-backup-producer-host: kernel/include/config_kv.h \
		kernel/include/backup_stream.h kernel/include/backup_producer.h \
		kernel/include/sha256.h kernel/src/config_kv.c \
		kernel/src/backup_stream.c kernel/src/sha256.c \
		kernel/src/backup_producer.c tests/backup-producer-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/backup_producer.c kernel/src/backup_stream.c \
		kernel/src/config_kv.c kernel/src/sha256.c \
		tests/backup-producer-host.c \
		-o build/test-backup-producer-host -no-pie
	build/test-backup-producer-host

test-restore-validator-host: kernel/include/config_kv.h \
		kernel/include/backup_stream.h kernel/include/restore_validator.h \
		kernel/include/sha256.h kernel/src/config_kv.c \
		kernel/src/backup_stream.c kernel/src/restore_validator.c \
		kernel/src/sha256.c tests/restore-validator-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/backup_stream.c kernel/src/config_kv.c \
		kernel/src/restore_validator.c kernel/src/sha256.c \
		tests/restore-validator-host.c \
		-o build/test-restore-validator-host -no-pie
	build/test-restore-validator-host

test-restore-planner-host: kernel/include/config_kv.h \
		kernel/include/backup_stream.h kernel/include/restore_validator.h \
		kernel/include/sha256.h kernel/src/config_kv.c \
		kernel/src/backup_stream.c kernel/src/restore_validator.c \
		kernel/src/sha256.c tests/restore-planner-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/backup_stream.c kernel/src/config_kv.c \
		kernel/src/restore_validator.c kernel/src/sha256.c \
		tests/restore-planner-host.c \
		-o build/test-restore-planner-host -no-pie
	build/test-restore-planner-host

test-restore-activation-host: kernel/include/config_kv.h \
		kernel/include/backup_stream.h kernel/include/restore_validator.h \
		kernel/include/sha256.h kernel/src/config_kv.c \
		kernel/src/backup_stream.c kernel/src/restore_validator.c \
		kernel/src/sha256.c tests/restore-activation-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/backup_stream.c kernel/src/config_kv.c \
		kernel/src/restore_validator.c kernel/src/sha256.c \
		tests/restore-activation-host.c \
		-o build/test-restore-activation-host -no-pie
	build/test-restore-activation-host

test-service-descriptor-host: kernel/include/service.h \
		kernel/src/service.c tests/service-descriptor-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SERVICE_DESCRIPTOR_HOST_TEST \
		-Ikernel/include \
		kernel/src/service.c tests/service-descriptor-host.c \
		-o build/test-service-descriptor-host -no-pie
	build/test-service-descriptor-host

test-https-service-descriptor-host: kernel/include/https_service.h \
		kernel/src/https_service.c tests/https-service-descriptor-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_HTTPS_SERVICE_HOST_TEST \
		-Ikernel/include \
		kernel/src/https_service.c tests/https-service-descriptor-host.c \
		-o build/test-https-service-descriptor-host -no-pie
	build/test-https-service-descriptor-host

test-https-admission-host: kernel/include/https_admission.h \
		kernel/src/https_admission.c tests/https-admission-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_HTTPS_ADMISSION_HOST_TEST \
		-Ikernel/include \
		kernel/src/https_admission.c tests/https-admission-host.c \
		-o build/test-https-admission-host -no-pie
	build/test-https-admission-host

test-https-connection-host: kernel/include/https_connection.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_tls_handshake.h \
		kernel/include/http_parser.h \
		kernel/include/http_response.h \
		kernel/src/hfoc_lwip.c \
		kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		kernel/src/hfoc_mbedtls_bio.c \
		kernel/src/hfoc_tls_handshake.c \
		kernel/src/http_parser.c \
		kernel/src/http_response.c \
		kernel/src/https_connection.c \
		tests/https-connection-host.c
	rm -rf build/https-connection-host
	mkdir -p build/https-connection-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/https-connection-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-DHFOC_TLS_HANDSHAKE_HOST_TEST \
		-DHFOC_MBEDTLS_BIO_HOST_TEST \
		-DHTTP_PARSER_HOST_TEST \
		-DHTTP_RESPONSE_HOST_TEST \
		-DHTTPS_CONNECTION_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		-Ithird_party/mbedtls/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_config.h"' \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		kernel/src/hfoc_mbedtls_bio.c \
		kernel/src/hfoc_tls_handshake.c \
		kernel/src/http_parser.c \
		kernel/src/http_response.c \
		kernel/src/https_connection.c tests/https-connection-host.c \
		build/https-connection-host/*.o \
		-o build/test-https-connection-host -no-pie
	build/test-https-connection-host

test-https-data-flow-host: kernel/include/https_data_flow.h \
		kernel/include/https_connection.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_tls_handshake.h \
		kernel/include/http_parser.h \
		kernel/include/http_response.h \
		kernel/src/hfoc_lwip.c \
		kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		kernel/src/hfoc_mbedtls_bio.c \
		kernel/src/hfoc_tls_handshake.c \
		kernel/src/http_parser.c \
		kernel/src/http_response.c \
		kernel/src/https_connection.c \
		kernel/src/https_data_flow.c \
		tests/https-data-flow-host.c
	rm -rf build/https-data-flow-host
	mkdir -p build/https-data-flow-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/https-data-flow-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-DHFOC_TLS_HANDSHAKE_HOST_TEST \
		-DHFOC_MBEDTLS_BIO_HOST_TEST \
		-DHTTP_PARSER_HOST_TEST \
		-DHTTP_REQUEST_LINE_HOST_TEST \
		-DHTTP_HEADER_HOST_TEST \
		-DHTTP_RESPONSE_HOST_TEST \
		-DHTTPS_CONNECTION_HOST_TEST \
		-DHTTPS_DATA_FLOW_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		-Ithird_party/mbedtls/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_config.h"' \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		kernel/src/hfoc_mbedtls_bio.c \
		kernel/src/hfoc_tls_handshake.c \
		kernel/src/http_parser.c \
		kernel/src/http_response.c \
		kernel/src/https_connection.c \
		kernel/src/https_data_flow.c tests/https-data-flow-host.c \
		build/https-data-flow-host/*.o \
		-o build/test-https-data-flow-host -no-pie
	build/test-https-data-flow-host

test-https-handler-host: kernel/include/https_handler.h \
		kernel/include/appliance_management_web_ui.h \
		kernel/include/http_parser.h \
		kernel/include/http_response.h \
		kernel/src/http_parser.c \
		kernel/src/http_response.c \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_license_inventory.c \
		kernel/src/https_handler.c \
		tests/https-handler-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHTTP_PARSER_HOST_TEST \
		-DHTTP_REQUEST_LINE_HOST_TEST \
		-DHTTP_HEADER_HOST_TEST \
		-DHTTP_POLICY_HOST_TEST \
		-DHTTP_FRAMING_HOST_TEST \
		-DHTTP_ROUTES_HOST_TEST \
		-DHTTP_RESPONSE_HOST_TEST \
		-DHTTPS_HANDLER_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_license_inventory.c \
		kernel/src/http_parser.c \
		kernel/src/http_response.c \
		kernel/src/https_handler.c \
		tests/https-handler-host.c \
		-o build/test-https-handler-host -no-pie
	build/test-https-handler-host

test-https-health-host: kernel/include/https_health.h \
		kernel/include/audit.h \
		kernel/include/event_ring.h \
		kernel/src/event_ring.c \
		kernel/src/audit.c \
		kernel/src/https_health.c \
		tests/https-health-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHTTPS_HEALTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/event_ring.c kernel/src/audit.c \
		kernel/src/https_health.c tests/https-health-host.c \
		-o build/test-https-health-host -no-pie
	build/test-https-health-host

test-https-peer-throttle-host: kernel/include/https_peer_throttle.h \
		kernel/src/https_peer_throttle.c \
		tests/https-peer-throttle-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHTTPS_PEER_THROTTLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/https_peer_throttle.c \
		tests/https-peer-throttle-host.c \
		-o build/test-https-peer-throttle-host -no-pie
	build/test-https-peer-throttle-host

test-https-service-lifecycle-host: kernel/include/https_service_lifecycle.h \
		kernel/src/https_service_lifecycle.c \
		tests/https-service-lifecycle-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHTTPS_SERVICE_LIFECYCLE_HOST_TEST \
	-DAPPLIANCE_MANAGEMENT_PANEL_HOST_TEST \
		-Ikernel/include \
		kernel/src/https_service_lifecycle.c \
		tests/https-service-lifecycle-host.c \
		-o build/test-https-service-lifecycle-host -no-pie
	build/test-https-service-lifecycle-host

test-local-principal-host: kernel/include/local_principal.h \
		kernel/src/local_principal.c tests/local-principal-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LOCAL_PRINCIPAL_HOST_TEST \
		-Ikernel/include \
		kernel/src/local_principal.c tests/local-principal-host.c \
		-o build/test-local-principal-host -no-pie
	build/test-local-principal-host

test-credential-verifier-host: kernel/include/credential_verifier.h \
		kernel/src/credential_verifier.c tests/credential-verifier-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CREDENTIAL_VERIFIER_HOST_TEST \
		-Ikernel/include \
		kernel/src/credential_verifier.c tests/credential-verifier-host.c \
		-o build/test-credential-verifier-host -no-pie
	build/test-credential-verifier-host

test-local-session-host: kernel/include/local_session.h \
		kernel/src/local_session.c tests/local-session-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LOCAL_SESSION_HOST_TEST \
		-Ikernel/include \
		kernel/src/local_session.c tests/local-session-host.c \
		-o build/test-local-session-host -no-pie
	build/test-local-session-host

test-auth-throttle-host: kernel/include/auth_throttle.h \
		kernel/src/auth_throttle.c tests/auth-throttle-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_AUTH_THROTTLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/auth_throttle.c tests/auth-throttle-host.c \
		-o build/test-auth-throttle-host -no-pie
	build/test-auth-throttle-host

.PHONY: test-appliance-admin-auth-host test-appliance-management-authorization-host test-appliance-secret-protection-host test-appliance-persistent-storage-namespace-host test-appliance-config-integrity-host test-appliance-secret-persistence-host
test-appliance-admin-auth-host: kernel/include/appliance_admin_auth.h \
		kernel/src/appliance_admin_auth.c tests/appliance-admin-auth-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_ADMIN_AUTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_admin_auth.c tests/appliance-admin-auth-host.c \
		-o build/test-appliance-admin-auth-host -no-pie
	build/test-appliance-admin-auth-host

test-appliance-management-authorization-host: \
		kernel/include/appliance_management_authorization.h \
		kernel/include/appliance_admin_auth.h \
		kernel/include/authorization.h \
		kernel/include/http_parser.h \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		tests/appliance-management-authorization-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_MANAGEMENT_AUTHORIZATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		tests/appliance-management-authorization-host.c \
		-o build/test-appliance-management-authorization-host -no-pie
	build/test-appliance-management-authorization-host

test-appliance-secret-protection-host: \
		kernel/include/appliance_secret_protection.h \
		kernel/include/sha256.h \
		kernel/src/appliance_secret_protection.c \
		kernel/src/sha256.c \
		tests/appliance-secret-protection-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_SECRET_PROTECTION_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_secret_protection.c \
		kernel/src/sha256.c \
		tests/appliance-secret-protection-host.c \
		-o build/test-appliance-secret-protection-host -no-pie
	build/test-appliance-secret-protection-host

test-appliance-persistent-storage-namespace-host: \
		kernel/include/appliance_persistent_storage_namespace.h \
		kernel/src/appliance_persistent_storage_namespace.c \
		tests/appliance-persistent-storage-namespace-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_PERSISTENT_STORAGE_NAMESPACE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_persistent_storage_namespace.c \
		tests/appliance-persistent-storage-namespace-host.c \
		-o build/test-appliance-persistent-storage-namespace-host -no-pie
	build/test-appliance-persistent-storage-namespace-host

test-appliance-config-integrity-host: \
		kernel/include/appliance_config_integrity.h \
		kernel/include/appliance_persistent_storage_namespace.h \
		kernel/include/config_journal.h \
		kernel/src/appliance_config_integrity.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		tests/appliance-config-integrity-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_CONFIG_INTEGRITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_config_integrity.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		tests/appliance-config-integrity-host.c \
		-o build/test-appliance-config-integrity-host -no-pie
	build/test-appliance-config-integrity-host

test-appliance-secret-persistence-host: \
		kernel/include/appliance_secret_persistence.h \
		kernel/include/appliance_persistent_storage_namespace.h \
		kernel/include/config_journal.h \
		kernel/include/https_service.h \
		kernel/src/appliance_secret_persistence.c \
		tests/appliance-secret-persistence-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_SECRET_PERSISTENCE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_secret_persistence.c \
		tests/appliance-secret-persistence-host.c \
		-o build/test-appliance-secret-persistence-host -no-pie
	build/test-appliance-secret-persistence-host

test-management-command-host: kernel/include/management_command.h \
		kernel/src/management_command.c tests/management-command-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_MANAGEMENT_COMMAND_HOST_TEST \
		-Ikernel/include \
		kernel/src/management_command.c tests/management-command-host.c \
		-o build/test-management-command-host -no-pie
	build/test-management-command-host

test-authorization-host: kernel/include/authorization.h \
		kernel/src/authorization.c tests/authorization-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_AUTHORIZATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/authorization.c tests/authorization-host.c \
		-o build/test-authorization-host -no-pie
	build/test-authorization-host

test-appliance-status-host: kernel/include/appliance_status.h \
		kernel/include/authorization.h kernel/include/management_command.h \
		kernel/src/appliance_status.c kernel/src/authorization.c \
		kernel/src/management_command.c tests/appliance-status-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_STATUS_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_status.c kernel/src/authorization.c \
		kernel/src/management_command.c tests/appliance-status-host.c \
		-o build/test-appliance-status-host -no-pie
	build/test-appliance-status-host

test-controlled-configuration-host: \
		kernel/include/controlled_configuration.h \
		kernel/include/authorization.h kernel/include/audit.h \
		kernel/include/audit_correlation.h \
		kernel/include/config_schema.h kernel/include/event_ring.h \
		kernel/include/management_command.h \
		kernel/src/controlled_configuration.c kernel/src/authorization.c \
		kernel/src/audit.c kernel/src/audit_correlation.c \
		kernel/src/config_schema.c \
		kernel/src/event_ring.c kernel/src/management_command.c \
		tests/controlled-configuration-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CONTROLLED_CONFIGURATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/controlled_configuration.c kernel/src/authorization.c \
		kernel/src/audit.c kernel/src/audit_correlation.c \
		kernel/src/config_schema.c \
		kernel/src/event_ring.c kernel/src/management_command.c \
		tests/controlled-configuration-host.c \
		-o build/test-controlled-configuration-host -no-pie
	build/test-controlled-configuration-host

test-audit-correlation-host: \
		kernel/include/audit_correlation.h \
		kernel/include/audit.h kernel/include/config_schema.h \
		kernel/include/controlled_configuration.h \
		kernel/include/event_ring.h \
		kernel/src/audit.c kernel/src/audit_correlation.c \
		kernel/src/config_schema.c \
		kernel/src/controlled_configuration.c kernel/src/event_ring.c \
		tests/audit-correlation-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_AUDIT_CORRELATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/audit.c kernel/src/audit_correlation.c \
		kernel/src/config_schema.c \
		kernel/src/controlled_configuration.c kernel/src/event_ring.c \
		tests/audit-correlation-host.c \
		-o build/test-audit-correlation-host -no-pie
	build/test-audit-correlation-host

test-network-device-host: kernel/include/network_device.h \
		kernel/src/network_device.c tests/network-device-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_DEVICE_HOST_TEST \
		-Ikernel/include \
		kernel/src/network_device.c tests/network-device-host.c \
		-o build/test-network-device-host -no-pie
	build/test-network-device-host

test-link-state-host: kernel/include/network_device.h \
		kernel/include/link_state.h \
		kernel/src/network_device.c kernel/src/link_state.c \
		tests/link-state-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LINK_STATE_HOST_TEST \
		-Ikernel/include \
		kernel/src/network_device.c kernel/src/link_state.c \
		tests/link-state-host.c \
		-o build/test-link-state-host -no-pie
	build/test-link-state-host

test-virtio-pci-identity-host: kernel/include/virtio_pci_identity.h \
		kernel/src/virtio_pci_identity.c tests/virtio-pci-identity-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_PCI_IDENTITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_pci_identity.c tests/virtio-pci-identity-host.c \
		-o build/test-virtio-pci-identity-host -no-pie
	build/test-virtio-pci-identity-host

test-pci-host: kernel/include/pci.h kernel/include/pci_dma.h \
		kernel/src/pci.c kernel/src/pci_dma.c tests/pci-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PCI_HOST_TEST \
		-Ikernel/include \
		kernel/src/pci.c kernel/src/pci_dma.c tests/pci-host.c \
		-o build/test-pci-host -no-pie
	build/test-pci-host

test-ahci-host: kernel/include/ahci.h kernel/include/pci.h \
		kernel/include/block_device.h \
		kernel/src/ahci.c kernel/src/block_device.c tests/ahci-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_AHCI_HOST_TEST \
		-Ikernel/include \
		kernel/src/ahci.c kernel/src/block_device.c \
		tests/ahci-host.c \
		-o build/test-ahci-host -no-pie
	build/test-ahci-host

.PHONY: test-kernel-status-format-host
test-kernel-status-format-host: kernel/include/kernel_status_format.h \
		kernel/src/kernel_status_format.c tests/kernel-status-format-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/kernel_status_format.c \
		tests/kernel-status-format-host.c \
		-o build/test-kernel-status-format-host -no-pie
	build/test-kernel-status-format-host

.PHONY: test-rtl8168-host
test-rtl8168-host: kernel/include/rtl8168.h kernel/include/pci.h \
		kernel/src/rtl8168.c tests/rtl8168-host.c tests/rtl8168-phy-model.h
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_RTL8168_HOST_TEST \
		-Ikernel/include \
		kernel/src/rtl8168.c \
		tests/rtl8168-host.c \
		-o build/test-rtl8168-host -no-pie
	build/test-rtl8168-host

# ITEM-590: the `[NET-PHY]` record reaches the boot journal.
#
# The installed Toshiba's 512-slot boot log carried no PHY record at all: the
# record was appended after the `[PRODUCTION-NET]` terminator, where the
# console's journal mirror ignores fragments.  This target links the driver's
# own RTL8168 register model, the record's one composition and emitter, and the
# real journal seam with the real preallocated FAT12 writer; it drives
# CONFIGURED (the laptop's raw 0x2C900000 TxConfig), REFUSED (a PHY that never
# answers, with the first error kept across a later poll) and SKIPPED (a
# classic part), then reads the finished medium with the operator's own Debian
# reader.
.PHONY: test-net-phy-journal-host
test-net-phy-journal-host: kernel/include/appliance_production_network_phy_record.h \
		kernel/include/appliance_production_network_boot.h \
		kernel/src/appliance_production_network_x86.c \
		kernel/include/rtl8168.h \
		kernel/include/install_journal_platform.h \
		kernel/include/install_journal_core.h \
		kernel/include/install_fat12_journal.h \
		kernel/include/block_device.h \
		kernel/src/appliance_production_network_phy_record.c \
		kernel/src/rtl8168.c \
		kernel/src/diag_export.c kernel/src/diag_session.c \
		kernel/src/diag_failure.c kernel/src/hfoc_build.c \
		kernel/src/install_journal_platform.c \
		kernel/src/install_journal_core.c \
		kernel/src/install_fat12_journal.c \
		kernel/src/install_medium.c kernel/src/block_device.c \
		tests/net-phy-journal-host.c tests/rtl8168-phy-model.h \
		tests/rtl8168-pit-reference-model.h kernel/src/platform_reset.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PRODUCTION_NETWORK -ffunction-sections -fdata-sections \
		-DHFOC_RTL8168_HOST_TEST -DHFOC_BUILD_ID='"host-593"' \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/appliance_production_network_phy_record.c \
		kernel/src/rtl8168.c \
		kernel/src/diag_export.c kernel/src/diag_session.c \
		kernel/src/diag_failure.c kernel/src/hfoc_build.c \
		kernel/src/install_journal_platform.c \
		kernel/src/install_journal_core.c \
		kernel/src/install_fat12_journal.c \
		kernel/src/install_medium.c kernel/src/block_device.c \
		tests/net-phy-journal-host.c \
		-o build/test-net-phy-journal-host -no-pie -Wl,--gc-sections
	build/test-net-phy-journal-host

# ITEM-494: the Intel 8254x ("e1000") driver core.  The second vendor's part is
# a fixture here - identity and claim, the register file the bring-up writes,
# the MAC read, the descriptor state machines and the ring arithmetic - so the
# 8254x path is proven without the hardware the acceptance host does not have.
.PHONY: test-network-io-record-host
test-network-io-record-host: \
		kernel/include/appliance_production_network_io_record.h \
		kernel/include/appliance_production_network_boot.h \
		kernel/src/appliance_production_network_io_record.c \
		tests/network-io-record-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_IO_RECORD_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/appliance_production_network_io_record.c \
		tests/network-io-record-host.c \
		-o build/test-network-io-record-host -no-pie
	build/test-network-io-record-host

# ITEM-617: the panel-login trace is bounded scalar data, and the receive
# counters follow the frame rather than the zero value of the lwIP enum.
.PHONY: test-panel-auth-trace-host
test-panel-auth-trace-host: \
		kernel/include/appliance_panel_auth_trace.h \
		kernel/src/appliance_panel_auth_trace.c \
		tests/panel-auth-trace-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_panel_auth_trace.c \
		tests/panel-auth-trace-host.c \
		-o build/test-panel-auth-trace-host -no-pie
	build/test-panel-auth-trace-host

.PHONY: test-network-rx-accounting-host
test-network-rx-accounting-host: \
		kernel/include/appliance_production_network_rx_accounting.h \
		tests/network-rx-accounting-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		tests/network-rx-accounting-host.c \
		-o build/test-network-rx-accounting-host -no-pie
	build/test-network-rx-accounting-host

.PHONY: test-e1000-host
test-e1000-host: kernel/include/e1000.h kernel/include/pci.h \
		kernel/src/e1000.c tests/e1000-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_E1000_HOST_TEST \
		-Ikernel/include \
		kernel/src/e1000.c \
		tests/e1000-host.c \
		-o build/test-e1000-host -no-pie
	build/test-e1000-host

.PHONY: test-network-backend-host
test-network-backend-host: kernel/include/network_backend.h \
		kernel/include/network_device.h kernel/include/link_state.h \
		kernel/src/network_backend.c kernel/src/network_device.c \
		kernel/src/link_state.c tests/network-backend-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_BACKEND_HOST_TEST \
		-Ikernel/include \
		kernel/src/network_backend.c kernel/src/network_device.c \
		kernel/src/link_state.c \
		tests/network-backend-host.c \
		-o build/test-network-backend-host -no-pie
	build/test-network-backend-host

test-nvme-host: kernel/include/nvme.h kernel/include/pci.h \
		kernel/include/block_device.h kernel/src/nvme.c \
		kernel/src/pci.c kernel/src/block_device.c tests/nvme-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NVME_HOST_TEST \
		-Ikernel/include \
		kernel/src/nvme.c kernel/src/pci.c kernel/src/block_device.c \
		tests/nvme-host.c \
		-o build/test-nvme-host -no-pie
	build/test-nvme-host

test-appliance-gpt-discovery-host: \
		kernel/include/appliance_gpt_discovery.h \
		kernel/include/block_device.h \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		tests/appliance-gpt-discovery-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_GPT_DISCOVERY_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		tests/appliance-gpt-discovery-host.c \
		-o build/test-appliance-gpt-discovery-host -no-pie
	build/test-appliance-gpt-discovery-host

test-appliance-state-layout-host: \
		kernel/include/appliance_state_layout.h \
		kernel/include/appliance_persistent_storage_namespace.h \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		tests/appliance-state-layout-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_STATE_LAYOUT_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		tests/appliance-state-layout-host.c \
		-o build/test-appliance-state-layout-host -no-pie
	build/test-appliance-state-layout-host

test-appliance-state-block-device-host: \
		kernel/include/appliance_state_block_device.h \
		kernel/include/appliance_state_layout.h \
		kernel/include/block_device.h \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/block_device.c \
		tests/appliance-state-block-device-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_STATE_BLOCK_DEVICE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/block_device.c \
		tests/appliance-state-block-device-host.c \
		-o build/test-appliance-state-block-device-host -no-pie
	build/test-appliance-state-block-device-host

test-appliance-state-loader-host: \
		kernel/include/appliance_state_loader.h \
		kernel/include/appliance_state_layout.h \
		kernel/include/appliance_state_block_device.h \
		kernel/include/appliance_language.h \
		kernel/include/appliance_persistent_config.h \
		kernel/include/appliance_persistent_network_config.h \
		kernel/include/appliance_persistent_secret_config.h \
		kernel/include/appliance_admin_credential.h \
		kernel/include/appliance_persistent_service_config.h \
		kernel/include/appliance_gpt_discovery.h \
		kernel/include/block_device.h \
		kernel/include/data_region.h \
		kernel/include/config_journal.h \
		kernel/include/sha256.h \
		kernel/include/appliance_config_validation.h \
		kernel/include/appliance_network_config.h \
		kernel/include/ipv4_config.h \
		kernel/include/service.h \
		kernel/include/service_health.h \
		kernel/include/appliance_service_manager.h \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/appliance-state-loader-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_STATE_LOADER_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/appliance-state-loader-host.c \
		-o build/test-appliance-state-loader-host -no-pie
	build/test-appliance-state-loader-host

# ITEM-598 focused proof: bounded host media records, password-sealed installed
# slots, durable epoch advancement and failure handling on RAM block devices.
.PHONY: test-appliance-host-seed-host
test-appliance-host-seed-host: \
		kernel/include/appliance_host_seed.h \
		kernel/include/appliance_state_crypto.h \
		kernel/include/block_device.h \
		kernel/include/hfoc_state_crypto_config.h \
		kernel/include/sha256.h \
		kernel/src/appliance_host_seed.c \
		kernel/src/appliance_state_crypto.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		$(HFOC_STATE_CRYPTO_HOST_OBJS) \
		tests/appliance-host-seed-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_STATE_ENCRYPTION_HOST_TEST \
		-DHFOC_SHA256_HOST_TEST \
		$(HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS) \
		-Ikernel/include \
		kernel/src/appliance_host_seed.c \
		kernel/src/appliance_state_crypto.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		$(HFOC_STATE_CRYPTO_HOST_OBJS) \
		tests/appliance-host-seed-host.c \
		-o build/test-appliance-host-seed-host -no-pie
	build/test-appliance-host-seed-host

.PHONY: test-host-seed-provision-host
test-host-seed-provision-host: scripts/hfoc-host-seed-provision.py \
		tests/test-host-seed-provision.sh
	bash tests/test-host-seed-provision.sh

# ITEM-373 focused proof: custody, sealed stores, encrypted install and the
# loader's key-required/key-rejected/ready path on RAM block devices.
.PHONY: test-state-encryption-host
test-state-encryption-host: \
		kernel/include/appliance_state_crypto.h \
		kernel/include/appliance_key_custody.h \
		kernel/include/appliance_state_encryption.h \
		kernel/include/hfoc_state_crypto_config.h \
		kernel/include/appliance_state_loader.h \
		kernel/include/appliance_state_layout.h \
		kernel/include/appliance_state_block_device.h \
		kernel/include/appliance_language.h \
		kernel/include/appliance_persistent_config.h \
		kernel/include/appliance_persistent_network_config.h \
		kernel/include/appliance_persistent_secret_config.h \
		kernel/include/appliance_admin_credential.h \
		kernel/include/appliance_persistent_service_config.h \
		kernel/include/appliance_config_validation.h \
		kernel/include/appliance_gpt_discovery.h \
		kernel/include/block_device.h \
		kernel/include/data_region.h \
		kernel/include/config_journal.h \
		kernel/include/sha256.h \
		kernel/include/appliance_network_config.h \
		kernel/include/ipv4_config.h \
		kernel/include/service.h \
		kernel/include/service_health.h \
		kernel/include/appliance_service_manager.h \
		kernel/src/appliance_state_crypto.c \
		kernel/src/appliance_key_custody.c \
		kernel/src/appliance_state_encryption.c \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		$(HFOC_STATE_CRYPTO_HOST_OBJS) \
		tests/state-encryption-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_STATE_ENCRYPTION_HOST_TEST \
		-DHFOC_SHA256_HOST_TEST \
		$(HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS) \
		-Ikernel/include \
		kernel/src/appliance_state_crypto.c \
		kernel/src/appliance_key_custody.c \
		kernel/src/appliance_state_encryption.c \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/state-encryption-host.c \
		$(HFOC_STATE_CRYPTO_HOST_OBJS) \
		-o build/test-state-encryption-host -no-pie
	build/test-state-encryption-host

# ITEM-374 focused proofs: bounded SNTP time sync, the pure network mode
# decision and the fail-closed management ingress binding.
.PHONY: test-appliance-time-sync-host
test-appliance-time-sync-host: kernel/include/appliance_time_sync.h \
		kernel/include/appliance_time_wire.h \
		kernel/src/appliance_time_sync.c \
		kernel/src/appliance_time_wire.c \
		tests/appliance-time-sync-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_TIME_SYNC_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_time_sync.c \
		kernel/src/appliance_time_wire.c \
		tests/appliance-time-sync-host.c \
		-o build/test-appliance-time-sync-host -no-pie
	build/test-appliance-time-sync-host

.PHONY: test-appliance-network-mode-host
test-appliance-network-mode-host: kernel/include/appliance_network_mode.h \
		kernel/include/appliance_management_bind.h \
		kernel/include/appliance_network_config.h \
		kernel/src/appliance_network_mode.c \
		kernel/src/appliance_management_bind.c \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/kstring.c \
		tests/appliance-network-mode-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_MODE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_network_mode.c \
		kernel/src/appliance_management_bind.c \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/kstring.c \
		tests/appliance-network-mode-host.c \
		-o build/test-appliance-network-mode-host -no-pie
	build/test-appliance-network-mode-host

# ITEM-375 focused proof: the management TLS policy, the certificate
# request/import contract, rotation without downtime, the fail-closed
# management ingress gate and the HSTS publication.
.PHONY: test-appliance-management-tls-host
test-appliance-management-tls-host: \
		kernel/include/appliance_management_tls.h \
		kernel/include/appliance_management_bind.h \
		kernel/include/http_response.h \
		kernel/src/appliance_management_tls.c \
		kernel/src/appliance_management_bind.c \
		kernel/src/http_response.c \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/kstring.c \
		tests/appliance-management-tls-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_MANAGEMENT_TLS_HOST_TEST \
		-DHTTP_RESPONSE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_management_tls.c \
		kernel/src/appliance_management_bind.c \
		kernel/src/http_response.c \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/kstring.c \
		tests/appliance-management-tls-host.c \
		-o build/test-appliance-management-tls-host -no-pie
	build/test-appliance-management-tls-host

# ITEM-385 focused proof: the management reachability policy. The local console
# manages the appliance with no network at all, the management plane is bound to
# the declared management interface and unreachable from the outer interface by
# default, remote administration needs an explicit opt-in with a source
# allowlist and, when required, a verified client certificate, and readiness
# stays honest without a management interface.
.PHONY: test-appliance-management-reachability-host
test-appliance-management-reachability-host: \
		kernel/include/appliance_management_reachability.h \
		kernel/src/appliance_management_reachability.c \
		tests/appliance-management-reachability-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_MANAGEMENT_REACHABILITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_management_reachability.c \
		tests/appliance-management-reachability-host.c \
		-o build/test-appliance-management-reachability-host -no-pie
	build/test-appliance-management-reachability-host

# ITEM-376 focused proofs: the durable two-copy update record, the signed
# update bundle and the boot-side update lifecycle.
.PHONY: test-appliance-update-record-host
test-appliance-update-record-host: \
		kernel/include/appliance_update_record.h \
		kernel/include/appliance_update_store.h \
		kernel/include/block_device.h \
		kernel/src/appliance_update_record.c \
		kernel/src/appliance_update_store.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-update-record-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_UPDATE_RECORD_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_update_record.c \
		kernel/src/appliance_update_store.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-update-record-host.c \
		-o build/test-appliance-update-record-host -no-pie
	build/test-appliance-update-record-host

.PHONY: test-appliance-update-lifecycle-host
test-appliance-update-lifecycle-host: \
		kernel/include/appliance_update_lifecycle.h \
		kernel/include/appliance_update_record.h \
		kernel/src/appliance_update_lifecycle.c \
		kernel/src/appliance_update_record.c \
		kernel/src/sha256.c \
		tests/appliance-update-lifecycle-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_UPDATE_LIFECYCLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_update_lifecycle.c \
		kernel/src/appliance_update_record.c \
		kernel/src/sha256.c \
		tests/appliance-update-lifecycle-host.c \
		-o build/test-appliance-update-lifecycle-host -no-pie
	build/test-appliance-update-lifecycle-host

.PHONY: test-appliance-update-bundle-host
test-appliance-update-bundle-host: \
		kernel/include/appliance_update_bundle.h \
		kernel/include/appliance_update_record.h \
		kernel/include/appliance_update_integrity.h \
		kernel/include/appliance_update_manifest.h \
		kernel/include/update_compatibility.h \
		kernel/include/image_signature.h \
		kernel/include/hfoc_verify_config.h \
		kernel/include/hfoc_verify_alloc.h \
		kernel/include/verification_policy.h \
		kernel/include/trust_anchor.h \
		kernel/include/key_use.h \
		kernel/include/sha256.h \
		kernel/include/signed_metadata.h \
		kernel/src/appliance_update_bundle.c \
		kernel/src/appliance_update_record.c \
		kernel/src/appliance_update_integrity.c \
		kernel/src/appliance_update_manifest.c \
		kernel/src/update_compatibility.c \
		kernel/src/image_signature.c \
		kernel/src/image_signature_ecdsa.c \
		kernel/src/key_use.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/sha256.c \
		kernel/src/signed_metadata.c \
		$(HFOC_VERIFY_HOST_OBJS) \
		tests/appliance-update-bundle-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_APPLIANCE_UPDATE_BUNDLE_HOST_TEST \
		$(HFOC_VERIFY_MBEDTLS_CFLAGS) -Ikernel/include -Itests \
		-c kernel/src/image_signature_ecdsa.c \
		-o build/appliance-update-bundle-ecdsa-host.o
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_APPLIANCE_UPDATE_BUNDLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_update_bundle.c \
		kernel/src/appliance_update_record.c \
		kernel/src/appliance_update_integrity.c \
		kernel/src/appliance_update_manifest.c \
		kernel/src/update_compatibility.c \
		kernel/src/image_signature.c \
		kernel/src/key_use.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/sha256.c \
		kernel/src/signed_metadata.c \
		tests/appliance-update-bundle-host.c \
		build/appliance-update-bundle-ecdsa-host.o \
		$(HFOC_VERIFY_HOST_OBJS) \
		-o build/test-appliance-update-bundle-host -no-pie
	build/test-appliance-update-bundle-host

.PHONY: test-appliance-production-boot-host
test-appliance-production-boot-host: \
		kernel/include/appliance_production_boot.h \
		kernel/include/appliance_state_loader.h \
		kernel/include/appliance_state_layout.h \
		kernel/include/appliance_state_block_device.h \
		kernel/include/appliance_gpt_discovery.h \
		kernel/include/block_device.h \
		kernel/src/appliance_production_boot.c \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/appliance-production-boot-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_BOOT_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_boot.c \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/appliance-production-boot-host.c \
		-o build/test-appliance-production-boot-host -no-pie
	build/test-appliance-production-boot-host

.PHONY: test-appliance-production-first-boot-host
test-appliance-production-first-boot-host: \
		kernel/include/appliance_production_first_boot.h \
		kernel/src/appliance_production_first_boot.c \
		tests/appliance-production-first-boot-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_FIRST_BOOT_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_first_boot.c \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/appliance_secret_protection.c \
		kernel/src/installer_first_boot_wizard.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/network_device.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/appliance-production-first-boot-host.c \
		-o build/test-appliance-production-first-boot-host -no-pie
	build/test-appliance-production-first-boot-host

.PHONY: test-efi-boot-environment-host
test-efi-boot-environment-host: kernel/include/efi_boot_environment.h \
		kernel/include/block_device.h \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		tests/efi-boot-environment-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_EFI_BOOT_ENVIRONMENT_HOST_TEST \
		-Ikernel/include \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		tests/efi-boot-environment-host.c \
		-o build/test-efi-boot-environment-host -no-pie
	build/test-efi-boot-environment-host

.PHONY: test-efi-fat32-esp-host
test-efi-fat32-esp-host: kernel/include/efi_fat32_esp.h \
		kernel/include/efi_boot_environment.h \
		kernel/include/block_device.h \
		kernel/include/sha256.h \
		kernel/src/efi_fat32_esp.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/efi-fat32-esp-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_EFI_FAT32_ESP_HOST_TEST \
		-Ikernel/include \
		kernel/src/efi_fat32_esp.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/efi-fat32-esp-host.c \
		-o build/test-efi-fat32-esp-host -no-pie
	build/test-efi-fat32-esp-host

.PHONY: test-efi-fat32-esp-payload-set-host
test-efi-fat32-esp-payload-set-host: kernel/include/efi_fat32_esp.h \
		kernel/include/efi_boot_environment.h \
		kernel/include/block_device.h \
		kernel/include/sha256.h \
		kernel/src/efi_fat32_esp.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/efi-fat32-esp-payload-set-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_EFI_FAT32_ESP_PAYLOAD_SET_HOST_TEST \
		-Ikernel/include \
		kernel/src/efi_fat32_esp.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/efi-fat32-esp-payload-set-host.c \
		-o build/test-efi-fat32-esp-payload-set-host -no-pie
	build/test-efi-fat32-esp-payload-set-host

.PHONY: test-efi-boot-variables-host
test-efi-boot-variables-host: kernel/include/efi_boot_variables.h \
		kernel/include/efi_boot_environment.h \
		kernel/include/block_device.h \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		tests/efi-boot-variables-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_EFI_BOOT_VARIABLES_HOST_TEST \
		-Ikernel/include \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		tests/efi-boot-variables-host.c \
		-o build/test-efi-boot-variables-host -no-pie
	build/test-efi-boot-variables-host

.PHONY: test-hfoc-efi-installer-host
test-hfoc-efi-installer-host: boot/efi/hfoc_efi_installer.h \
		kernel/include/efi_boot_environment.h \
		kernel/include/efi_boot_variables.h \
		kernel/include/efi_fat32_esp.h \
		kernel/include/appliance_gpt_discovery.h \
		kernel/include/installer_hardware_discovery.h \
		kernel/include/block_device.h \
		kernel/include/sha256.h \
		boot/efi/hfoc_efi_installer.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/hfoc-efi-installer-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_EFI_INSTALLER_HOST_TEST \
		-Iboot/efi -Ikernel/include \
		boot/efi/hfoc_efi_installer.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/hfoc-efi-installer-host.c \
		-o build/test-hfoc-efi-installer-host -no-pie
	build/test-hfoc-efi-installer-host

.PHONY: test-efi-ovmf-qemu
test-efi-ovmf-qemu:
	./scripts/test-qemu-efi-ovmf.sh

.PHONY: test-hfoc-efi-installer-qemu
test-hfoc-efi-installer-qemu: $(HFOC_EFI_HELPER_ISO) iso
	@strings $(HFOC_EFI_HELPER) | grep -Fq '[HFOC-EFI] begin' || { \
		echo "FAIL: EFI helper missing begin marker" >&2; exit 1; }
	@strings $(HFOC_EFI_HELPER_ISO) | grep -Fq 'HFOCINST.EFI' || { \
		echo "FAIL: EFI helper ISO missing installer helper" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[HFOC-EFI] begin'; then \
		echo "FAIL: production kernel contains EFI helper marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[HFOC-EFI] begin'; then \
		echo "FAIL: production ISO contains EFI helper marker" >&2; exit 1; fi
	IMAGE='$(HFOC_EFI_HELPER_ISO)' PAYLOAD='$(LIMINE)/BOOTX64.EFI' \
		./scripts/test-qemu-hfoc-efi-installer.sh

test-nvme-state-integration-host: \
		kernel/include/appliance_gpt_discovery.h \
		kernel/include/config_journal.h \
		kernel/include/data_region.h \
		kernel/include/block_device.h \
		kernel/include/sha256.h \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		kernel/src/config_journal.c \
		kernel/src/data_region.c \
		kernel/src/sha256.c \
		tests/nvme-state-integration-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NVME_STATE_INTEGRATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/block_device.c \
		kernel/src/config_journal.c \
		kernel/src/data_region.c \
		kernel/src/sha256.c \
		tests/nvme-state-integration-host.c \
		-o build/test-nvme-state-integration-host -no-pie
	build/test-nvme-state-integration-host

test-virtio-pci-capability-host: kernel/include/virtio_pci_capability.h \
		kernel/include/virtio_pci_identity.h \
		kernel/src/virtio_pci_capability.c \
		kernel/src/virtio_pci_identity.c \
		tests/virtio-pci-capability-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_PCI_CAPABILITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_pci_capability.c \
		kernel/src/virtio_pci_identity.c \
		tests/virtio-pci-capability-host.c \
		-o build/test-virtio-pci-capability-host -no-pie
	build/test-virtio-pci-capability-host

test-virtio-pci-device-status-host: \
		kernel/include/virtio_pci_device_status.h \
		kernel/src/virtio_pci_device_status.c \
		tests/virtio-pci-device-status-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_PCI_DEVICE_STATUS_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_pci_device_status.c \
		tests/virtio-pci-device-status-host.c \
		-o build/test-virtio-pci-device-status-host -no-pie
	build/test-virtio-pci-device-status-host

test-virtio-pci-feature-host: kernel/include/virtio_pci_feature.h \
		kernel/src/virtio_pci_feature.c \
		tests/virtio-pci-feature-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_PCI_FEATURE_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_pci_feature.c \
		tests/virtio-pci-feature-host.c \
		-o build/test-virtio-pci-feature-host -no-pie
	build/test-virtio-pci-feature-host

test-virtio-queue-layout-host: kernel/include/virtio_queue_layout.h \
		kernel/src/virtio_queue_layout.c \
		tests/virtio-queue-layout-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_QUEUE_LAYOUT_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_queue_layout.c \
		tests/virtio-queue-layout-host.c \
		-o build/test-virtio-queue-layout-host -no-pie
	build/test-virtio-queue-layout-host

test-virtio-descriptor-ownership-host: \
		kernel/include/virtio_descriptor_ownership.h \
		kernel/src/virtio_descriptor_ownership.c \
		tests/virtio-descriptor-ownership-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_DESCRIPTOR_OWNERSHIP_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_descriptor_ownership.c \
		tests/virtio-descriptor-ownership-host.c \
		-o build/test-virtio-descriptor-ownership-host -no-pie
	build/test-virtio-descriptor-ownership-host

test-virtio-available-publication-host: \
		kernel/include/virtio_available_publication.h \
		kernel/include/virtio_queue_layout.h \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_queue_layout.c \
		tests/virtio-available-publication-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_AVAILABLE_PUBLICATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_queue_layout.c \
		tests/virtio-available-publication-host.c \
		-o build/test-virtio-available-publication-host -no-pie
	build/test-virtio-available-publication-host

test-virtio-used-consumption-host: \
		kernel/include/virtio_used_consumption.h \
		kernel/include/virtio_queue_layout.h \
		kernel/src/virtio_used_consumption.c \
		kernel/src/virtio_queue_layout.c \
		tests/virtio-used-consumption-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_USED_CONSUMPTION_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_used_consumption.c \
		kernel/src/virtio_queue_layout.c \
		tests/virtio-used-consumption-host.c \
		-o build/test-virtio-used-consumption-host -no-pie
	build/test-virtio-used-consumption-host

test-virtio-dma-region-host: kernel/include/virtio_dma_region.h \
		kernel/src/virtio_dma_region.c \
		tests/virtio-dma-region-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_DMA_REGION_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_dma_region.c \
		tests/virtio-dma-region-host.c \
		-o build/test-virtio-dma-region-host -no-pie
	build/test-virtio-dma-region-host

test-virtio-net-feature-profile-host: \
		kernel/include/virtio_pci_feature.h \
		kernel/include/virtio_pci_identity.h \
		kernel/include/virtio_net_feature_profile.h \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_feature_profile.c \
		tests/virtio-net-feature-profile-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_NET_FEATURE_PROFILE_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_feature_profile.c \
		tests/virtio-net-feature-profile-host.c \
		-o build/test-virtio-net-feature-profile-host -no-pie
	build/test-virtio-net-feature-profile-host

test-virtio-rng-feature-profile-host: \
		kernel/include/virtio_pci_feature.h \
		kernel/include/virtio_pci_identity.h \
		kernel/include/virtio_rng_feature_profile.h \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_rng_feature_profile.c \
		tests/virtio-rng-feature-profile-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_RNG_FEATURE_PROFILE_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_rng_feature_profile.c \
		tests/virtio-rng-feature-profile-host.c \
		-o build/test-virtio-rng-feature-profile-host -no-pie
	build/test-virtio-rng-feature-profile-host

test-virtio-rng-queue-composition-host: \
		kernel/include/virtio_available_publication.h \
		kernel/include/virtio_descriptor_ownership.h \
		kernel/include/virtio_dma_region.h \
		kernel/include/virtio_queue_layout.h \
		kernel/include/virtio_rng_feature_profile.h \
		kernel/include/virtio_rng_queue_composition.h \
		kernel/include/virtio_used_consumption.h \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_rng_feature_profile.c \
		kernel/src/virtio_rng_queue_composition.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-rng-queue-composition-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_RNG_QUEUE_COMPOSITION_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_rng_feature_profile.c \
		kernel/src/virtio_rng_queue_composition.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-rng-queue-composition-host.c \
		-o build/test-virtio-rng-queue-composition-host -no-pie
	build/test-virtio-rng-queue-composition-host

test-virtio-rng-request-host: \
		kernel/include/virtio_rng_request.h \
		kernel/src/virtio_rng_request.c \
		tests/virtio-rng-request-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_RNG_REQUEST_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_rng_request.c \
		tests/virtio-rng-request-host.c \
		-o build/test-virtio-rng-request-host -no-pie
	build/test-virtio-rng-request-host

test-virtio-rng-completion-host: \
		kernel/include/virtio_available_publication.h \
		kernel/include/virtio_descriptor_ownership.h \
		kernel/include/virtio_dma_region.h \
		kernel/include/virtio_queue_layout.h \
		kernel/include/virtio_rng_completion.h \
		kernel/include/virtio_rng_feature_profile.h \
		kernel/include/virtio_rng_queue_composition.h \
		kernel/include/virtio_rng_request.h \
		kernel/include/virtio_used_consumption.h \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_rng_completion.c \
		kernel/src/virtio_rng_feature_profile.c \
		kernel/src/virtio_rng_queue_composition.c \
		kernel/src/virtio_rng_request.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-rng-completion-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_RNG_COMPLETION_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_rng_completion.c \
		kernel/src/virtio_rng_feature_profile.c \
		kernel/src/virtio_rng_queue_composition.c \
		kernel/src/virtio_rng_request.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-rng-completion-host.c \
		-o build/test-virtio-rng-completion-host -no-pie
	build/test-virtio-rng-completion-host

test-entropy-health-host: \
		kernel/include/entropy_health.h \
		kernel/src/entropy_health.c \
		tests/entropy-health-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ENTROPY_HEALTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/entropy_health.c \
		tests/entropy-health-host.c \
		-o build/test-entropy-health-host -no-pie
	build/test-entropy-health-host

test-entropy-accumulator-host: \
		kernel/include/entropy_accumulator.h \
		kernel/src/entropy_accumulator.c \
		tests/entropy-accumulator-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ENTROPY_ACCUMULATOR_HOST_TEST \
		-Ikernel/include \
		kernel/src/entropy_accumulator.c \
		tests/entropy-accumulator-host.c \
		-o build/test-entropy-accumulator-host -no-pie
	build/test-entropy-accumulator-host

test-entropy-seed-fill-host: \
		kernel/include/entropy_accumulator.h \
		kernel/include/entropy_seed_fill.h \
		kernel/src/entropy_accumulator.c \
		kernel/src/entropy_seed_fill.c \
		tests/entropy-seed-fill-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ENTROPY_SEED_FILL_HOST_TEST \
		-Ikernel/include \
		kernel/src/entropy_accumulator.c \
		kernel/src/entropy_seed_fill.c \
		tests/entropy-seed-fill-host.c \
		-o build/test-entropy-seed-fill-host -no-pie
	build/test-entropy-seed-fill-host

test-entropy-diagnostics-host: \
		kernel/include/entropy_diagnostics.h \
		kernel/include/entropy_accumulator.h \
		kernel/include/entropy_health.h \
		kernel/include/entropy_seed_fill.h \
		kernel/src/entropy_accumulator.c \
		kernel/src/entropy_health.c \
		kernel/src/entropy_seed_fill.c \
		kernel/src/entropy_diagnostics.c \
		tests/entropy-diagnostics-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ENTROPY_DIAGNOSTICS_HOST_TEST \
		-Ikernel/include \
		kernel/src/entropy_accumulator.c \
		kernel/src/entropy_health.c \
		kernel/src/entropy_seed_fill.c \
		kernel/src/entropy_diagnostics.c \
		tests/entropy-diagnostics-host.c \
		-o build/test-entropy-diagnostics-host -no-pie
	build/test-entropy-diagnostics-host

test-entropy-restart-host: \
		kernel/include/entropy_restart.h \
		kernel/include/entropy_accumulator.h \
		kernel/include/entropy_health.h \
		kernel/include/entropy_seed_fill.h \
		kernel/src/entropy_accumulator.c \
		kernel/src/entropy_health.c \
		kernel/src/entropy_seed_fill.c \
		kernel/src/entropy_restart.c \
		tests/entropy-restart-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ENTROPY_RESTART_HOST_TEST \
		-Ikernel/include \
		kernel/src/entropy_accumulator.c \
		kernel/src/entropy_health.c \
		kernel/src/entropy_seed_fill.c \
		kernel/src/entropy_restart.c \
		tests/entropy-restart-host.c \
		-o build/test-entropy-restart-host -no-pie
	build/test-entropy-restart-host

test-virtio-net-queue-composition-host: \
		kernel/include/virtio_available_publication.h \
		kernel/include/virtio_descriptor_ownership.h \
		kernel/include/virtio_dma_region.h \
		kernel/include/virtio_net_feature_profile.h \
		kernel/include/virtio_net_queue_composition.h \
		kernel/include/virtio_queue_layout.h \
		kernel/include/virtio_used_consumption.h \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_net_queue_composition.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-net-queue-composition-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_NET_QUEUE_COMPOSITION_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_net_queue_composition.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-net-queue-composition-host.c \
		-o build/test-virtio-net-queue-composition-host -no-pie
	build/test-virtio-net-queue-composition-host

test-virtio-net-rx-host: \
		kernel/include/virtio_available_publication.h \
		kernel/include/virtio_descriptor_ownership.h \
		kernel/include/virtio_dma_region.h \
		kernel/include/virtio_net_feature_profile.h \
		kernel/include/virtio_net_queue_composition.h \
		kernel/include/virtio_net_rx.h \
		kernel/include/virtio_pci_identity.h \
		kernel/include/virtio_queue_layout.h \
		kernel/include/virtio_used_consumption.h \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_net_queue_composition.c \
		kernel/src/virtio_net_rx.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-net-rx-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_NET_RX_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_net_queue_composition.c \
		kernel/src/virtio_net_rx.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-net-rx-host.c \
		-o build/test-virtio-net-rx-host -no-pie
	build/test-virtio-net-rx-host

test-virtio-net-tx-host: \
		kernel/include/virtio_available_publication.h \
		kernel/include/virtio_descriptor_ownership.h \
		kernel/include/virtio_dma_region.h \
		kernel/include/virtio_net_feature_profile.h \
		kernel/include/virtio_net_queue_composition.h \
		kernel/include/virtio_net_tx.h \
		kernel/include/virtio_pci_identity.h \
		kernel/include/virtio_queue_layout.h \
		kernel/include/virtio_used_consumption.h \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_net_queue_composition.c \
		kernel/src/virtio_net_tx.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-net-tx-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_NET_TX_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_net_queue_composition.c \
		kernel/src/virtio_net_tx.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-net-tx-host.c \
		-o build/test-virtio-net-tx-host -no-pie
	build/test-virtio-net-tx-host

test-virtio-net-interrupt-host: \
		kernel/include/virtio_net_interrupt.h \
		kernel/src/virtio_net_interrupt.c \
		tests/virtio-net-interrupt-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_NET_INTERRUPT_HOST_TEST \
		-Ikernel/include \
		kernel/src/virtio_net_interrupt.c \
		tests/virtio-net-interrupt-host.c \
		-o build/test-virtio-net-interrupt-host -no-pie
	build/test-virtio-net-interrupt-host

test-virtio-net-link-state-host: \
		kernel/include/link_state.h \
		kernel/include/network_device.h \
		kernel/include/virtio_net_feature_profile.h \
		kernel/include/virtio_net_link_state.h \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/virtio_net_link_state.c \
		tests/virtio-net-link-state-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_NET_LINK_STATE_HOST_TEST \
		-Ikernel/include \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/virtio_net_link_state.c \
		tests/virtio-net-link-state-host.c \
		-o build/test-virtio-net-link-state-host -no-pie
	build/test-virtio-net-link-state-host

test-virtio-net-lifecycle-host: \
		kernel/include/link_state.h \
		kernel/include/network_device.h \
		kernel/include/virtio_available_publication.h \
		kernel/include/virtio_descriptor_ownership.h \
		kernel/include/virtio_dma_region.h \
		kernel/include/virtio_net_feature_profile.h \
		kernel/include/virtio_net_interrupt.h \
		kernel/include/virtio_net_lifecycle.h \
		kernel/include/virtio_net_link_state.h \
		kernel/include/virtio_net_queue_composition.h \
		kernel/include/virtio_net_rx.h \
		kernel/include/virtio_net_tx.h \
		kernel/include/virtio_pci_device_status.h \
		kernel/include/virtio_pci_identity.h \
		kernel/include/virtio_queue_layout.h \
		kernel/include/virtio_used_consumption.h \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_net_interrupt.c \
		kernel/src/virtio_net_lifecycle.c \
		kernel/src/virtio_net_link_state.c \
		kernel/src/virtio_net_queue_composition.c \
		kernel/src/virtio_net_rx.c \
		kernel/src/virtio_net_tx.c \
		kernel/src/virtio_pci_device_status.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-net-lifecycle-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_NET_LIFECYCLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/virtio_available_publication.c \
		kernel/src/virtio_descriptor_ownership.c \
		kernel/src/virtio_dma_region.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_net_interrupt.c \
		kernel/src/virtio_net_lifecycle.c \
		kernel/src/virtio_net_link_state.c \
		kernel/src/virtio_net_queue_composition.c \
		kernel/src/virtio_net_rx.c \
		kernel/src/virtio_net_tx.c \
		kernel/src/virtio_pci_device_status.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_queue_layout.c \
		kernel/src/virtio_used_consumption.c \
		tests/virtio-net-lifecycle-host.c \
		-o build/test-virtio-net-lifecycle-host -no-pie
	build/test-virtio-net-lifecycle-host

test-ipv4-config-host: kernel/include/ipv4_config.h \
		kernel/src/ipv4_config.c tests/ipv4-config-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_IPV4_CONFIG_HOST_TEST \
		-Ikernel/include \
		kernel/src/ipv4_config.c tests/ipv4-config-host.c \
		-o build/test-ipv4-config-host -no-pie
	build/test-ipv4-config-host

test-network-config-transaction-host: kernel/include/ipv4_config.h \
		kernel/include/network_config_transaction.h \
		kernel/src/ipv4_config.c \
		kernel/src/network_config_transaction.c \
		tests/network-config-transaction-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_CONFIG_TRANSACTION_HOST_TEST \
		-Ikernel/include \
		kernel/src/ipv4_config.c kernel/src/network_config_transaction.c \
		tests/network-config-transaction-host.c \
		-o build/test-network-config-transaction-host -no-pie
	build/test-network-config-transaction-host

test-packet-buffer-host: kernel/include/packet_buffer.h \
		kernel/src/packet_buffer.c tests/packet-buffer-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PACKET_BUFFER_HOST_TEST \
		-Ikernel/include \
		kernel/src/packet_buffer.c tests/packet-buffer-host.c \
		-o build/test-packet-buffer-host -no-pie
	build/test-packet-buffer-host

.PHONY: test-packet-pool-host
test-packet-pool-host: kernel/include/packet_pool.h \
		kernel/src/packet_pool.c tests/packet-pool-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PACKET_POOL_HOST_TEST \
		-Ikernel/include \
		kernel/src/packet_pool.c tests/packet-pool-host.c \
		-o build/test-packet-pool-host -no-pie
	build/test-packet-pool-host

.PHONY: test-packet-lifecycle-host
test-packet-lifecycle-host: kernel/include/packet_pool.h \
		kernel/src/packet_pool.c tests/packet-lifecycle-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PACKET_LIFECYCLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/packet_pool.c tests/packet-lifecycle-host.c \
		-o build/test-packet-lifecycle-host -no-pie
	build/test-packet-lifecycle-host

.PHONY: test-packet-slice-host
test-packet-slice-host: kernel/include/packet_pool.h \
		kernel/include/packet_slice.h \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		tests/packet-slice-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PACKET_SLICE_HOST_TEST \
		-Ikernel/include \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		tests/packet-slice-host.c \
		-o build/test-packet-slice-host -no-pie
	build/test-packet-slice-host

.PHONY: test-ethernet-frame-host
test-ethernet-frame-host: kernel/include/packet_pool.h \
		kernel/include/packet_slice.h \
		kernel/include/ethernet_frame.h \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		kernel/src/ethernet_frame.c tests/ethernet-frame-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ETHERNET_FRAME_HOST_TEST \
		-Ikernel/include \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		kernel/src/ethernet_frame.c tests/ethernet-frame-host.c \
		-o build/test-ethernet-frame-host -no-pie
	build/test-ethernet-frame-host

.PHONY: test-virtio-net-mac-identity-host
test-virtio-net-mac-identity-host: kernel/include/network_device.h \
		kernel/include/virtio_net_feature_profile.h \
		kernel/include/virtio_net_mac_identity.h \
		kernel/src/network_device.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_mac_identity.c \
		tests/virtio-net-mac-identity-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VIRTIO_NET_MAC_IDENTITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/network_device.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_mac_identity.c \
		tests/virtio-net-mac-identity-host.c \
		-o build/test-virtio-net-mac-identity-host -no-pie
	build/test-virtio-net-mac-identity-host

.PHONY: test-network-ipv4-profile-host
test-network-ipv4-profile-host: kernel/include/network_ipv4_profile.h \
		kernel/include/network_config_transaction.h \
		kernel/include/ipv4_config.h \
		kernel/include/virtio_net_mac_identity.h \
		kernel/src/network_ipv4_profile.c \
		kernel/src/network_config_transaction.c \
		kernel/src/ipv4_config.c \
		kernel/src/network_device.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_mac_identity.c \
		tests/network-ipv4-profile-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_IPV4_PROFILE_HOST_TEST \
		-Ikernel/include \
		kernel/src/network_ipv4_profile.c \
		kernel/src/network_config_transaction.c \
		kernel/src/ipv4_config.c \
		kernel/src/network_device.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_mac_identity.c \
		tests/network-ipv4-profile-host.c \
		-o build/test-network-ipv4-profile-host -no-pie
	build/test-network-ipv4-profile-host

.PHONY: test-hfoc-netif-input-host
test-hfoc-netif-input-host: kernel/include/packet_pool.h \
		kernel/include/packet_slice.h \
		kernel/include/ethernet_frame.h \
		kernel/include/hfoc_netif_input.h \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		kernel/src/ethernet_frame.c kernel/src/hfoc_netif_input.c \
		tests/hfoc-netif-input-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETIF_INPUT_HOST_TEST \
		-Ikernel/include \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		kernel/src/ethernet_frame.c kernel/src/hfoc_netif_input.c \
		tests/hfoc-netif-input-host.c \
		-o build/test-hfoc-netif-input-host -no-pie
	build/test-hfoc-netif-input-host

.PHONY: test-hfoc-netif-output-host
test-hfoc-netif-output-host: kernel/include/packet_pool.h \
		kernel/include/packet_slice.h \
		kernel/include/hfoc_netif_output.h \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		kernel/src/hfoc_netif_output.c \
		tests/hfoc-netif-output-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETIF_OUTPUT_HOST_TEST \
		-Ikernel/include \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		kernel/src/hfoc_netif_output.c \
		tests/hfoc-netif-output-host.c \
		-o build/test-hfoc-netif-output-host -no-pie
	build/test-hfoc-netif-output-host

.PHONY: test-packet-pressure-host
test-packet-pressure-host: kernel/include/packet_pool.h \
		kernel/include/packet_pressure.h \
		kernel/src/packet_pool.c kernel/src/packet_pressure.c \
		tests/packet-pressure-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PACKET_PRESSURE_HOST_TEST \
		-Ikernel/include \
		kernel/src/packet_pool.c kernel/src/packet_pressure.c \
		tests/packet-pressure-host.c \
		-o build/test-packet-pressure-host -no-pie
	build/test-packet-pressure-host

test-icmp-reachability-host: kernel/include/icmp_reachability.h \
		kernel/src/icmp_reachability.c tests/icmp-reachability-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ICMP_REACHABILITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/icmp_reachability.c tests/icmp-reachability-host.c \
		-o build/test-icmp-reachability-host -no-pie
	build/test-icmp-reachability-host

test-management-transport-host: kernel/include/management_transport.h \
		kernel/src/management_transport.c \
		tests/management-transport-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_MANAGEMENT_TRANSPORT_HOST_TEST \
		-Ikernel/include \
		kernel/src/management_transport.c \
		tests/management-transport-host.c \
		-o build/test-management-transport-host -no-pie
		build/test-management-transport-host

test-network-admission-host: kernel/include/network_admission.h \
		kernel/src/network_admission.c \
		kernel/src/management_transport.c \
		kernel/src/management_command.c \
		kernel/src/authorization.c \
		tests/network-admission-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_ADMISSION_HOST_TEST \
		-Ikernel/include \
		kernel/src/network_admission.c \
		kernel/src/management_transport.c \
		kernel/src/management_command.c \
		kernel/src/authorization.c \
		tests/network-admission-host.c \
		-o build/test-network-admission-host -no-pie
	build/test-network-admission-host

test-network-event-host: kernel/include/audit.h \
		kernel/include/event_ring.h \
		kernel/include/network_event.h \
		kernel/src/audit.c \
		kernel/src/event_ring.c \
		kernel/src/network_event.c \
		tests/network-event-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_EVENT_HOST_TEST \
		-Ikernel/include \
		kernel/src/audit.c kernel/src/event_ring.c \
		kernel/src/network_event.c tests/network-event-host.c \
		-o build/test-network-event-host -no-pie
	build/test-network-event-host

test-key-use-host: kernel/include/key_use.h \
		kernel/src/key_use.c tests/key-use-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_KEY_USE_HOST_TEST \
		-Ikernel/include \
		kernel/src/key_use.c tests/key-use-host.c \
		-o build/test-key-use-host -no-pie
	build/test-key-use-host

test-trust-anchor-host: kernel/include/trust_anchor.h \
		kernel/include/key_use.h kernel/src/trust_anchor.c \
		kernel/src/key_use.c tests/trust-anchor-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_TRUST_ANCHOR_HOST_TEST \
		-Ikernel/include \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		tests/trust-anchor-host.c \
		-o build/test-trust-anchor-host -no-pie
	build/test-trust-anchor-host

test-verification-policy-host: kernel/include/verification_policy.h \
		kernel/include/trust_anchor.h kernel/include/key_use.h \
		kernel/src/verification_policy.c kernel/src/trust_anchor.c \
		kernel/src/key_use.c tests/verification-policy-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_VERIFICATION_POLICY_HOST_TEST \
		-Ikernel/include \
		kernel/src/verification_policy.c kernel/src/trust_anchor.c \
		kernel/src/key_use.c tests/verification-policy-host.c \
		-o build/test-verification-policy-host -no-pie
	build/test-verification-policy-host

test-signed-metadata-host: kernel/include/signed_metadata.h \
		kernel/include/verification_policy.h kernel/include/trust_anchor.h \
		kernel/include/key_use.h kernel/include/sha256.h \
		kernel/src/signed_metadata.c kernel/src/key_use.c \
		kernel/src/verification_policy.c kernel/src/trust_anchor.c \
		kernel/src/sha256.c tests/signed-metadata-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SIGNED_METADATA_HOST_TEST \
		-Ikernel/include \
		kernel/src/signed_metadata.c kernel/src/key_use.c \
		kernel/src/verification_policy.c kernel/src/trust_anchor.c \
		kernel/src/sha256.c \
		tests/signed-metadata-host.c \
		-o build/test-signed-metadata-host -no-pie
	build/test-signed-metadata-host

test-trust-rotation-host: kernel/include/trust_rotation.h \
		kernel/include/verification_policy.h kernel/include/trust_anchor.h \
		kernel/include/key_use.h kernel/include/signed_metadata.h \
		kernel/include/sha256.h kernel/src/trust_rotation.c \
		kernel/src/verification_policy.c kernel/src/trust_anchor.c \
		kernel/src/key_use.c kernel/src/signed_metadata.c \
		kernel/src/sha256.c tests/trust-rotation-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_TRUST_ROTATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/trust_rotation.c kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		kernel/src/signed_metadata.c kernel/src/sha256.c \
		tests/trust-rotation-host.c \
		-o build/test-trust-rotation-host -no-pie
	build/test-trust-rotation-host

test-trust-revocation-host: kernel/include/trust_revocation.h \
		kernel/include/verification_policy.h kernel/include/trust_anchor.h \
		kernel/include/key_use.h kernel/include/audit.h \
		kernel/include/event_ring.h kernel/include/signed_metadata.h \
		kernel/include/sha256.h kernel/include/trust_rotation.h \
		kernel/src/trust_revocation.c kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		kernel/src/audit.c kernel/src/event_ring.c \
		kernel/src/signed_metadata.c kernel/src/sha256.c \
		kernel/src/trust_rotation.c tests/trust-revocation-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_TRUST_REVOCATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/trust_revocation.c kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		kernel/src/audit.c kernel/src/event_ring.c \
		kernel/src/signed_metadata.c kernel/src/sha256.c \
		kernel/src/trust_rotation.c \
		tests/trust-revocation-host.c \
		-o build/test-trust-revocation-host -no-pie
	build/test-trust-revocation-host

test-image-policy-host: kernel/include/image_policy.h \
		kernel/include/image_integrity.h kernel/include/system_image.h \
		kernel/include/sha256.h kernel/include/verification_policy.h \
		kernel/include/trust_anchor.h kernel/include/key_use.h \
		kernel/src/image_policy.c kernel/src/image_integrity.c \
		kernel/src/sha256.c kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		tests/image-policy-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_IMAGE_POLICY_HOST_TEST \
		-Ikernel/include \
		kernel/src/image_policy.c kernel/src/image_integrity.c \
		kernel/src/sha256.c kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		tests/image-policy-host.c \
		-o build/test-image-policy-host -no-pie
	build/test-image-policy-host

test-boot-evidence-host: kernel/include/boot_evidence.h \
		kernel/include/config_kv.h kernel/include/sha256.h \
		kernel/include/verification_policy.h \
		kernel/include/image_policy.h kernel/include/image_integrity.h \
		kernel/include/system_image.h kernel/include/trust_anchor.h \
		kernel/include/key_use.h \
		kernel/src/boot_evidence.c kernel/src/config_kv.c \
		kernel/src/sha256.c kernel/src/verification_policy.c \
		kernel/src/image_policy.c kernel/src/image_integrity.c \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		tests/boot-evidence-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_BOOT_EVIDENCE_HOST_TEST \
		-Ikernel/include \
		kernel/src/boot_evidence.c kernel/src/config_kv.c \
		kernel/src/sha256.c kernel/src/verification_policy.c \
		kernel/src/image_policy.c kernel/src/image_integrity.c \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		tests/boot-evidence-host.c \
		-o build/test-boot-evidence-host -no-pie
	build/test-boot-evidence-host

test-recovery-trust-host: kernel/include/recovery_trust.h \
		kernel/include/boot_evidence.h kernel/include/config_kv.h \
		kernel/include/sha256.h kernel/include/verification_policy.h \
		kernel/include/image_policy.h kernel/include/image_integrity.h \
		kernel/include/system_image.h kernel/include/signed_metadata.h \
		kernel/include/trust_anchor.h kernel/include/key_use.h \
		kernel/src/recovery_trust.c kernel/src/boot_evidence.c \
		kernel/src/config_kv.c kernel/src/sha256.c \
		kernel/src/verification_policy.c kernel/src/image_policy.c \
		kernel/src/image_integrity.c kernel/src/trust_anchor.c \
		kernel/src/key_use.c kernel/src/signed_metadata.c \
		tests/recovery-trust-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_RECOVERY_TRUST_HOST_TEST \
		-Ikernel/include \
		kernel/src/recovery_trust.c kernel/src/boot_evidence.c \
		kernel/src/config_kv.c kernel/src/sha256.c \
		kernel/src/verification_policy.c kernel/src/image_policy.c \
		kernel/src/image_integrity.c kernel/src/trust_anchor.c \
		kernel/src/key_use.c kernel/src/signed_metadata.c \
	tests/recovery-trust-host.c \
		-o build/test-recovery-trust-host -no-pie
	build/test-recovery-trust-host

.PHONY: test-appliance-update-manifest-host
test-appliance-update-manifest-host: kernel/include/appliance_update_manifest.h \
		kernel/include/update_compatibility.h \
		kernel/src/appliance_update_manifest.c \
		kernel/src/update_compatibility.c \
		tests/appliance-update-manifest-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_UPDATE_MANIFEST_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_update_manifest.c \
		kernel/src/update_compatibility.c \
		tests/appliance-update-manifest-host.c \
		-o build/test-appliance-update-manifest-host -no-pie
	build/test-appliance-update-manifest-host

.PHONY: test-appliance-update-integrity-host
test-appliance-update-integrity-host: kernel/include/appliance_update_integrity.h \
		kernel/include/appliance_update_manifest.h \
		kernel/include/key_use.h \
		kernel/include/sha256.h \
		kernel/include/signed_metadata.h \
		kernel/include/trust_anchor.h \
		kernel/include/update_compatibility.h \
		kernel/include/verification_policy.h \
		kernel/src/appliance_update_integrity.c \
		kernel/src/appliance_update_manifest.c \
		kernel/src/key_use.c \
		kernel/src/sha256.c \
		kernel/src/signed_metadata.c \
		kernel/src/trust_anchor.c \
		kernel/src/update_compatibility.c \
		kernel/src/verification_policy.c \
		tests/appliance-update-integrity-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_UPDATE_INTEGRITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_update_integrity.c \
		kernel/src/appliance_update_manifest.c \
		kernel/src/key_use.c \
		kernel/src/sha256.c \
		kernel/src/signed_metadata.c \
		kernel/src/trust_anchor.c \
		kernel/src/update_compatibility.c \
		kernel/src/verification_policy.c \
		tests/appliance-update-integrity-host.c \
		-o build/test-appliance-update-integrity-host -no-pie
	build/test-appliance-update-integrity-host

.PHONY: test-appliance-recovery-mode-host
test-appliance-recovery-mode-host: kernel/include/appliance_recovery_mode.h \
		kernel/src/appliance_recovery_mode.c \
		tests/appliance-recovery-mode-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_RECOVERY_MODE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_recovery_mode.c \
		tests/appliance-recovery-mode-host.c \
		-o build/test-appliance-recovery-mode-host -no-pie
	build/test-appliance-recovery-mode-host

.PHONY: test-appliance-factory-reset-host
test-appliance-factory-reset-host: kernel/include/appliance_factory_reset.h \
		kernel/src/appliance_factory_reset.c \
		tests/appliance-factory-reset-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_FACTORY_RESET_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_factory_reset.c \
		tests/appliance-factory-reset-host.c \
		-o build/test-appliance-factory-reset-host -no-pie
	build/test-appliance-factory-reset-host

test-update-compatibility-host: kernel/include/update_compatibility.h \
		kernel/src/update_compatibility.c \
		tests/update-compatibility-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_UPDATE_COMPATIBILITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/update_compatibility.c \
		tests/update-compatibility-host.c \
		-o build/test-update-compatibility-host -no-pie
	build/test-update-compatibility-host

test-candidate-slot-inventory-host: kernel/include/candidate_slot_inventory.h \
		kernel/src/candidate_slot_inventory.c \
		tests/candidate-slot-inventory-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CANDIDATE_SLOT_INVENTORY_HOST_TEST \
		-Ikernel/include \
		kernel/src/candidate_slot_inventory.c \
		tests/candidate-slot-inventory-host.c \
		-o build/test-candidate-slot-inventory-host -no-pie
	build/test-candidate-slot-inventory-host

test-candidate-preflight-host: kernel/include/candidate_preflight.h \
		kernel/include/candidate_slot_inventory.h \
		kernel/include/signed_metadata.h \
		kernel/include/update_compatibility.h \
		kernel/src/candidate_preflight.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/update_compatibility.c \
		tests/candidate-preflight-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CANDIDATE_PREFLIGHT_HOST_TEST \
		-Ikernel/include \
		kernel/src/candidate_preflight.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/update_compatibility.c \
		tests/candidate-preflight-host.c \
		-o build/test-candidate-preflight-host -no-pie
	build/test-candidate-preflight-host

test-candidate-staged-write-host: kernel/include/candidate_staged_write.h \
		kernel/include/candidate_preflight.h \
		kernel/include/candidate_slot_inventory.h \
		kernel/include/update_compatibility.h \
		kernel/include/sha256.h \
		kernel/src/candidate_staged_write.c \
		kernel/src/candidate_preflight.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/update_compatibility.c \
		kernel/src/sha256.c \
		tests/candidate-staged-write-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CANDIDATE_STAGED_WRITE_HOST_TEST \
		-Ikernel/include \
		kernel/src/candidate_staged_write.c \
		kernel/src/candidate_preflight.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/update_compatibility.c \
		kernel/src/sha256.c \
		tests/candidate-staged-write-host.c \
		-o build/test-candidate-staged-write-host -no-pie
	build/test-candidate-staged-write-host

test-activation-intent-host: kernel/include/activation_intent.h \
		kernel/include/candidate_staged_write.h \
		kernel/include/candidate_preflight.h \
		kernel/include/candidate_slot_inventory.h \
		kernel/include/sha256.h \
		kernel/src/activation_intent.c \
		tests/activation-intent-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ACTIVATION_INTENT_HOST_TEST \
		-Ikernel/include \
		kernel/src/activation_intent.c \
		tests/activation-intent-host.c \
		-o build/test-activation-intent-host -no-pie
	build/test-activation-intent-host

test-first-boot-health-host: kernel/include/first_boot_health.h \
		kernel/src/first_boot_health.c \
		tests/first-boot-health-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_FIRST_BOOT_HEALTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/first_boot_health.c \
		tests/first-boot-health-host.c \
		-o build/test-first-boot-health-host -no-pie
	build/test-first-boot-health-host

test-health-check-host: kernel/include/health_check.h \
		kernel/src/health_check.c \
		tests/health-check-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_HEALTH_CHECK_HOST_TEST \
		-Ikernel/include \
		kernel/src/health_check.c \
		tests/health-check-host.c \
		-o build/test-health-check-host -no-pie
	build/test-health-check-host

test-storage-health-host: kernel/include/storage_health.h \
		kernel/include/health_check.h \
		kernel/include/journal_compaction.h \
		kernel/include/config_kv.h \
		kernel/include/candidate_slot_inventory.h \
		kernel/include/activation_intent.h \
		kernel/include/candidate_staged_write.h \
		kernel/src/storage_health.c \
		kernel/src/config_kv.c \
		kernel/src/journal_compaction.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/activation_intent.c \
		tests/storage-health-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_STORAGE_HEALTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/storage_health.c \
		kernel/src/config_kv.c \
		kernel/src/journal_compaction.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/activation_intent.c \
		tests/storage-health-host.c \
		-o build/test-storage-health-host -no-pie
	build/test-storage-health-host

test-service-health-host: kernel/include/service_health.h \
		kernel/include/service_supervisor.h \
		kernel/include/service.h \
		kernel/include/health_check.h \
		kernel/src/service_health.c \
		kernel/src/service.c \
		tests/service-health-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SERVICE_HEALTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/service_health.c \
		kernel/src/service.c \
		tests/service-health-host.c \
		-o build/test-service-health-host -no-pie
	build/test-service-health-host

test-appliance-health-host: kernel/include/appliance_health.h \
		kernel/src/appliance_health.c tests/appliance-health-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_HEALTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_health.c \
		tests/appliance-health-host.c \
		-o build/test-appliance-health-host -no-pie
	build/test-appliance-health-host

.PHONY: test-appliance-boot-log-host
test-appliance-boot-log-host: kernel/include/appliance_boot_log.h \
		kernel/src/appliance_boot_log.c \
		tests/appliance-boot-log-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_BOOT_LOG_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_boot_log.c \
		tests/appliance-boot-log-host.c \
		-o build/test-appliance-boot-log-host -no-pie
	build/test-appliance-boot-log-host

.PHONY: test-appliance-boot-log-store-host
test-appliance-boot-log-store-host: kernel/include/appliance_boot_log.h \
		kernel/include/appliance_boot_log_store.h \
		kernel/include/appliance_gpt_discovery.h \
		kernel/include/efi_fat32_esp.h \
		kernel/include/efi_boot_environment.h \
		kernel/include/block_device.h \
		kernel/include/sha256.h \
		kernel/src/appliance_boot_log.c \
		kernel/src/appliance_boot_log_store.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-boot-log-store-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_BOOT_LOG_STORE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_boot_log.c \
		kernel/src/appliance_boot_log_store.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-boot-log-store-host.c \
		-Wl,--wrap=efi_boot_environment_init \
		-o build/test-appliance-boot-log-store-host -no-pie
	build/test-appliance-boot-log-store-host

.PHONY: test-appliance-boot-log-reader-host
test-appliance-boot-log-reader-host: test-appliance-boot-log-store-host \
		scripts/hfoc-esp-read.py \
		tests/appliance-boot-log-reader-host.py
	python3 tests/appliance-boot-log-reader-host.py --require-image

test-appliance-runtime-log-host: kernel/include/appliance_runtime_log.h \
		kernel/src/appliance_runtime_log.c tests/appliance-runtime-log-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_RUNTIME_LOG_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_runtime_log.c \
		tests/appliance-runtime-log-host.c \
		-o build/test-appliance-runtime-log-host -no-pie
	build/test-appliance-runtime-log-host

test-appliance-health-api-host: kernel/include/appliance_health.h \
		kernel/include/appliance_health_api.h kernel/include/http_parser.h \
		kernel/include/http_response.h kernel/src/appliance_health.c \
		kernel/src/appliance_health_api.c kernel/src/http_response.c \
		tests/appliance-health-api-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_HEALTH_API_HOST_TEST \
		-DHTTP_RESPONSE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_health.c \
		kernel/src/appliance_health_api.c \
		kernel/src/http_response.c \
		tests/appliance-health-api-host.c \
		-o build/test-appliance-health-api-host -no-pie
	build/test-appliance-health-api-host

test-network-health-host: kernel/include/network_health.h \
		kernel/include/health_check.h \
		kernel/include/network_device.h \
		kernel/include/link_state.h \
		kernel/include/network_config_transaction.h \
		kernel/include/ipv4_config.h \
		kernel/include/network_admission.h \
		kernel/include/management_transport.h \
		kernel/include/management_command.h \
		kernel/include/authorization.h \
		kernel/src/network_health.c \
		kernel/src/network_device.c \
		kernel/src/link_state.c \
		kernel/src/ipv4_config.c \
		kernel/src/network_config_transaction.c \
		kernel/src/network_admission.c \
		kernel/src/management_transport.c \
		kernel/src/management_command.c \
		kernel/src/authorization.c \
		tests/network-health-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_HEALTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/network_health.c \
		kernel/src/network_device.c \
		kernel/src/link_state.c \
		kernel/src/ipv4_config.c \
		kernel/src/network_config_transaction.c \
		kernel/src/network_admission.c \
		kernel/src/management_transport.c \
		kernel/src/management_command.c \
		kernel/src/authorization.c \
		tests/network-health-host.c \
		-o build/test-network-health-host -no-pie
	build/test-network-health-host

test-audit-ring-health-host: kernel/include/audit_ring_health.h \
		kernel/include/event_ring.h \
		kernel/include/health_check.h \
		kernel/src/audit_ring_health.c \
		kernel/src/event_ring.c \
		tests/audit-ring-health-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_AUDIT_RING_HEALTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/audit_ring_health.c \
		kernel/src/event_ring.c \
		tests/audit-ring-health-host.c \
		-o build/test-audit-ring-health-host -no-pie
	build/test-audit-ring-health-host

# ITEM-377 focused proofs: the hash-chained audit window with its rotation and
# anchor continuity, and the cursor-based external export with its gap and
# acknowledgement rules.
.PHONY: test-appliance-audit-chain-host
test-appliance-audit-chain-host: \
		kernel/include/appliance_audit_chain.h \
		kernel/include/audit.h \
		kernel/include/sha256.h \
		kernel/include/event_ring.h \
		kernel/src/appliance_audit_chain.c \
		kernel/src/audit.c \
		kernel/src/event_ring.c \
		kernel/src/sha256.c \
		tests/appliance-audit-chain-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_AUDIT_CHAIN_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_audit_chain.c \
		kernel/src/audit.c \
		kernel/src/event_ring.c \
		kernel/src/sha256.c \
		tests/appliance-audit-chain-host.c \
		-o build/test-appliance-audit-chain-host -no-pie
	build/test-appliance-audit-chain-host

.PHONY: test-appliance-audit-export-host
test-appliance-audit-export-host: \
		kernel/include/appliance_audit_export.h \
		kernel/include/appliance_audit_chain.h \
		kernel/include/audit.h \
		kernel/include/sha256.h \
		kernel/include/event_ring.h \
		kernel/src/appliance_audit_export.c \
		kernel/src/appliance_audit_chain.c \
		kernel/src/audit.c \
		kernel/src/event_ring.c \
		kernel/src/sha256.c \
		tests/appliance-audit-export-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_AUDIT_EXPORT_HOST_TEST \
		-DHFOC_APPLIANCE_AUDIT_CHAIN_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_audit_export.c \
		kernel/src/appliance_audit_chain.c \
		kernel/src/audit.c \
		kernel/src/event_ring.c \
		kernel/src/sha256.c \
		tests/appliance-audit-export-host.c \
		-o build/test-appliance-audit-export-host -no-pie
	build/test-appliance-audit-export-host

.PHONY: test-appliance-production-audit-host
test-appliance-production-audit-host: \
		kernel/include/appliance_production_audit_boot.h \
		kernel/include/appliance_audit_chain.h \
		kernel/include/appliance_audit_export.h \
		kernel/include/audit.h \
		kernel/include/sha256.h \
		kernel/include/event_ring.h \
		kernel/src/appliance_production_audit_boot.c \
		kernel/src/appliance_audit_chain.c \
		kernel/src/appliance_audit_export.c \
		kernel/src/audit.c \
		kernel/src/event_ring.c \
		kernel/src/sha256.c \
		tests/appliance-production-audit-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PRODUCTION_SERVICES \
		-DHFOC_APPLIANCE_PRODUCTION_AUDIT_HOST_TEST \
		-DHFOC_APPLIANCE_AUDIT_CHAIN_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_audit_boot.c \
		kernel/src/appliance_audit_chain.c \
		kernel/src/appliance_audit_export.c \
		kernel/src/audit.c \
		kernel/src/event_ring.c \
		kernel/src/sha256.c \
		tests/appliance-production-audit-host.c \
		-o build/test-appliance-production-audit-host -no-pie
	build/test-appliance-production-audit-host

test-fault-injection-host: kernel/include/fault_injection.h \
		kernel/src/fault_injection.c tests/fault-injection-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_FAULT_INJECTION_HOST_TEST \
		-Ikernel/include \
	kernel/src/fault_injection.c tests/fault-injection-host.c \
		-o build/test-fault-injection-host -no-pie
	build/test-fault-injection-host

test-storage-interruption-host: kernel/include/config_kv.h \
		kernel/include/journal_compaction.h \
		kernel/include/backup_stream.h kernel/include/backup_producer.h \
		kernel/include/restore_validator.h kernel/include/sha256.h \
		kernel/include/fault_injection.h kernel/src/config_kv.c \
		kernel/src/journal_compaction.c kernel/src/backup_stream.c \
		kernel/src/backup_producer.c kernel/src/restore_validator.c \
		kernel/src/sha256.c kernel/src/fault_injection.c \
		tests/storage-interruption-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_STORAGE_INTERRUPTION_HOST_TEST \
		-Ikernel/include \
		kernel/src/config_kv.c kernel/src/journal_compaction.c \
		kernel/src/backup_stream.c kernel/src/backup_producer.c \
		kernel/src/restore_validator.c kernel/src/sha256.c \
		kernel/src/fault_injection.c tests/storage-interruption-host.c \
		-o build/test-storage-interruption-host -no-pie
	build/test-storage-interruption-host

test-metrics-host: kernel/include/metrics.h \
		kernel/src/metrics.c \
		tests/metrics-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_METRICS_HOST_TEST \
		-Ikernel/include \
		kernel/src/metrics.c \
		tests/metrics-host.c \
		-o build/test-metrics-host -no-pie
	build/test-metrics-host

test-support-bundle-manifest-host: kernel/include/support_bundle_manifest.h \
		kernel/src/support_bundle_manifest.c \
		kernel/src/diagnostic_privacy.c \
		kernel/src/sha256.c \
		kernel/src/config_kv.c \
		tests/support-bundle-manifest-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SUPPORT_BUNDLE_MANIFEST_HOST_TEST \
		-Ikernel/include \
		kernel/src/support_bundle_manifest.c \
		kernel/src/diagnostic_privacy.c \
		kernel/src/sha256.c \
		kernel/src/config_kv.c \
		tests/support-bundle-manifest-host.c \
		-o build/test-support-bundle-manifest-host -no-pie
	build/test-support-bundle-manifest-host

test-support-bundle-exporter-host: kernel/include/support_bundle_exporter.h \
		kernel/src/support_bundle_exporter.c \
		kernel/src/support_bundle_manifest.c \
		kernel/src/diagnostic_privacy.c \
		kernel/src/sha256.c \
		kernel/src/config_kv.c \
		tests/support-bundle-exporter-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SUPPORT_BUNDLE_EXPORTER_HOST_TEST \
		-Ikernel/include \
		kernel/src/support_bundle_exporter.c \
		kernel/src/support_bundle_manifest.c \
		kernel/src/diagnostic_privacy.c \
		kernel/src/sha256.c \
		kernel/src/config_kv.c \
		tests/support-bundle-exporter-host.c \
		-o build/test-support-bundle-exporter-host -no-pie
	build/test-support-bundle-exporter-host

# ITEM-378 focused proof: the signed diagnostic bundle (redaction attestation,
# canonical binding, keyed HMAC-SHA-256 seal and every refusal path).
test-appliance-support-bundle-host: \
		kernel/include/appliance_support_bundle.h \
		kernel/include/appliance_state_crypto.h \
		kernel/include/hfoc_state_crypto_config.h \
		kernel/src/appliance_support_bundle.c \
		kernel/src/support_bundle_exporter.c \
		kernel/src/support_bundle_manifest.c \
		kernel/src/diagnostic_privacy.c \
		kernel/src/sha256.c \
		kernel/src/config_kv.c \
		kernel/src/appliance_state_crypto.c \
		tests/appliance-support-bundle-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_SUPPORT_BUNDLE_HOST_TEST \
		-DHFOC_STATE_ENCRYPTION_HOST_TEST \
		$(HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS) \
		-Ikernel/include \
		kernel/src/appliance_support_bundle.c \
		kernel/src/support_bundle_exporter.c \
		kernel/src/support_bundle_manifest.c \
		kernel/src/diagnostic_privacy.c \
		kernel/src/sha256.c \
		kernel/src/config_kv.c \
		kernel/src/appliance_state_crypto.c \
		tests/appliance-support-bundle-host.c \
		$(HFOC_STATE_CRYPTO_HOST_OBJS) \
		-o build/test-appliance-support-bundle-host -no-pie
	build/test-appliance-support-bundle-host

# ITEM-378 focused proof: the consent-based remote support session.
test-appliance-support-session-host: \
		kernel/include/appliance_support_session.h \
		kernel/src/appliance_support_session.c \
		tests/appliance-support-session-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_SUPPORT_SESSION_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_support_session.c \
		tests/appliance-support-session-host.c \
		-o build/test-appliance-support-session-host -no-pie
	build/test-appliance-support-session-host

# ITEM-378 focused proof: documented break-glass recovery without a shell.
test-appliance-support-break-glass-host: \
		kernel/include/appliance_support_break_glass.h \
		kernel/src/appliance_support_break_glass.c \
		tests/appliance-support-break-glass-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_SUPPORT_BREAK_GLASS_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_support_break_glass.c \
		tests/appliance-support-break-glass-host.c \
		-o build/test-appliance-support-break-glass-host -no-pie
	build/test-appliance-support-break-glass-host

# ITEM-378 focused proof: the production support plane (bundle seal,
# redaction attestation, consent session and break-glass effects).
test-appliance-production-support-host: \
		kernel/include/appliance_production_support.h \
		kernel/include/appliance_support_bundle.h \
		kernel/include/appliance_support_session.h \
		kernel/include/appliance_support_break_glass.h \
		kernel/include/appliance_management_bind.h \
		kernel/include/appliance_state_crypto.h \
		kernel/include/hfoc_state_crypto_config.h \
		kernel/src/appliance_production_support.c \
		kernel/src/appliance_support_bundle.c \
		kernel/src/appliance_support_session.c \
		kernel/src/appliance_support_break_glass.c \
		kernel/src/appliance_management_bind.c \
		kernel/src/support_bundle_exporter.c \
		kernel/src/support_bundle_manifest.c \
		kernel/src/diagnostic_privacy.c \
		kernel/src/sha256.c \
		kernel/src/config_kv.c \
		kernel/src/appliance_state_crypto.c \
		tests/appliance-production-support-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_SUPPORT_HOST_TEST \
		-DHFOC_STATE_ENCRYPTION_HOST_TEST \
		$(HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS) \
		-Ikernel/include \
		kernel/src/appliance_production_support.c \
		kernel/src/appliance_support_bundle.c \
		kernel/src/appliance_support_session.c \
		kernel/src/appliance_support_break_glass.c \
		kernel/src/appliance_management_bind.c \
		kernel/src/support_bundle_exporter.c \
		kernel/src/support_bundle_manifest.c \
		kernel/src/diagnostic_privacy.c \
		kernel/src/sha256.c \
		kernel/src/config_kv.c \
		kernel/src/appliance_state_crypto.c \
		tests/appliance-production-support-host.c \
		$(HFOC_STATE_CRYPTO_HOST_OBJS) \
		-o build/test-appliance-production-support-host -no-pie
	build/test-appliance-production-support-host

test-diagnostic-privacy-host: kernel/include/diagnostic_privacy.h \
		kernel/src/diagnostic_privacy.c \
		tests/diagnostic-privacy-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_DIAGNOSTIC_PRIVACY_HOST_TEST \
		-Ikernel/include \
		kernel/src/diagnostic_privacy.c \
		tests/diagnostic-privacy-host.c \
		-o build/test-diagnostic-privacy-host -no-pie
	build/test-diagnostic-privacy-host

test-integration-boundary-host: kernel/include/integration_boundary.h \
		tests/integration-boundary-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_BOUNDARY_HOST_TEST \
		-Ikernel/include \
		tests/integration-boundary-host.c \
		-o build/test-integration-boundary-host -no-pie
	build/test-integration-boundary-host

test-integration-principal-host: kernel/include/integration_principal.h \
		kernel/include/authorization.h kernel/src/integration_principal.c \
		kernel/src/authorization.c tests/integration-principal-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_PRINCIPAL_HOST_TEST \
		-Ikernel/include \
		kernel/src/integration_principal.c kernel/src/authorization.c \
		tests/integration-principal-host.c \
		-o build/test-integration-principal-host -no-pie
	build/test-integration-principal-host

test-integration-session-host: kernel/include/integration_session.h \
		kernel/include/integration_boundary.h \
		kernel/src/integration_session.c tests/integration-session-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_SESSION_HOST_TEST \
		-Ikernel/include \
		kernel/src/integration_session.c tests/integration-session-host.c \
		-o build/test-integration-session-host -no-pie
	build/test-integration-session-host

test-integration-envelope-host: kernel/include/integration_envelope.h \
		kernel/include/integration_boundary.h kernel/include/sha256.h \
		kernel/src/integration_envelope.c kernel/src/sha256.c \
		tests/integration-envelope-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_ENVELOPE_HOST_TEST \
		-Ikernel/include \
		kernel/src/integration_envelope.c kernel/src/sha256.c \
		tests/integration-envelope-host.c \
		-o build/test-integration-envelope-host -no-pie
	build/test-integration-envelope-host

test-integration-ledger-host: kernel/include/integration_ledger.h \
		kernel/include/integration_envelope.h \
		kernel/include/integration_boundary.h kernel/include/sha256.h \
		kernel/src/integration_ledger.c \
		kernel/src/integration_envelope.c kernel/src/sha256.c \
		tests/integration-ledger-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_LEDGER_HOST_TEST \
		-Ikernel/include \
		kernel/src/integration_ledger.c \
		kernel/src/integration_envelope.c kernel/src/sha256.c \
		tests/integration-ledger-host.c \
		-o build/test-integration-ledger-host -no-pie
	build/test-integration-ledger-host

test-integration-event-cursor-host: kernel/include/integration_event_cursor.h \
		kernel/include/event_ring.h \
		kernel/src/integration_event_cursor.c \
		tests/integration-event-cursor-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_EVENT_CURSOR_HOST_TEST \
		-Ikernel/include \
		kernel/src/integration_event_cursor.c \
		tests/integration-event-cursor-host.c \
		-o build/test-integration-event-cursor-host -no-pie
	build/test-integration-event-cursor-host

test-integration-config-intent-host: \
		kernel/include/integration_config_intent.h \
		kernel/include/audit_correlation.h \
		kernel/include/authorization.h \
		kernel/include/config_schema.h \
		kernel/include/controlled_configuration.h \
		kernel/include/event_ring.h \
		kernel/src/integration_config_intent.c \
		kernel/src/controlled_configuration.c \
		kernel/src/authorization.c kernel/src/audit.c \
		kernel/src/audit_correlation.c kernel/src/config_schema.c \
		kernel/src/event_ring.c \
		tests/integration-config-intent-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_CONFIG_INTENT_HOST_TEST \
		-Ikernel/include \
		kernel/src/integration_config_intent.c \
		kernel/src/controlled_configuration.c \
		kernel/src/authorization.c kernel/src/audit.c \
		kernel/src/audit_correlation.c kernel/src/config_schema.c \
		kernel/src/event_ring.c \
		tests/integration-config-intent-host.c \
		-o build/test-integration-config-intent-host -no-pie
	build/test-integration-config-intent-host

test-integration-failure-isolation-host: \
		kernel/include/integration_failure_isolation.h \
		kernel/src/integration_failure_isolation.c \
		tests/integration-failure-isolation-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_FAILURE_ISOLATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/integration_failure_isolation.c \
		tests/integration-failure-isolation-host.c \
		-o build/test-integration-failure-isolation-host -no-pie
	build/test-integration-failure-isolation-host

test-integration-compatibility-host: \
		kernel/include/integration_compatibility.h \
		kernel/include/integration_boundary.h \
		kernel/include/integration_session.h \
		kernel/src/integration_compatibility.c \
		kernel/src/integration_session.c \
		tests/integration-compatibility-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_COMPATIBILITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/integration_compatibility.c \
		kernel/src/integration_session.c \
		tests/integration-compatibility-host.c \
		-o build/test-integration-compatibility-host -no-pie
	build/test-integration-compatibility-host

test-release-profile-host: \
		kernel/include/release_profile.h \
		kernel/include/config_schema.h \
		kernel/include/integration_boundary.h \
		kernel/include/system_image.h \
		kernel/include/update_compatibility.h \
		kernel/src/release_profile.c \
		tests/release-profile-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_RELEASE_PROFILE_HOST_TEST \
		-Ikernel/include \
		kernel/src/release_profile.c \
		tests/release-profile-host.c \
		-o build/test-release-profile-host -no-pie
	build/test-release-profile-host

test-appliance-identity-host: \
		kernel/include/appliance_identity.h \
		kernel/src/appliance_identity.c \
		tests/appliance-identity-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_IDENTITY_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_identity.c \
		tests/appliance-identity-host.c \
		-o build/test-appliance-identity-host -no-pie
	build/test-appliance-identity-host

test-appliance-persistent-config-host: \
		kernel/include/block_device.h \
		kernel/include/data_region.h \
		kernel/include/config_journal.h \
		kernel/include/sha256.h \
		kernel/include/appliance_language.h \
		kernel/include/appliance_config_validation.h \
		kernel/include/appliance_persistent_config.h \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		tests/appliance-persistent-config-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PERSISTENT_CONFIG_HOST_TEST \
		-Ikernel/include \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		tests/appliance-persistent-config-host.c \
		-o build/test-appliance-persistent-config-host -no-pie
	build/test-appliance-persistent-config-host

test-appliance-config-validation-host: \
		kernel/include/appliance_language.h \
		kernel/include/appliance_config_validation.h \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		tests/appliance-config-validation-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		tests/appliance-config-validation-host.c \
		-o build/test-appliance-config-validation-host -no-pie
	build/test-appliance-config-validation-host

test-appliance-panel-gate-host: \
		kernel/include/appliance_panel_gate.h \
		kernel/src/appliance_panel_gate.c \
		tests/appliance-panel-gate-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_panel_gate.c \
		tests/appliance-panel-gate-host.c \
		-o build/test-appliance-panel-gate-host -no-pie
	build/test-appliance-panel-gate-host

test-appliance-persistent-network-config-host: \
		kernel/include/block_device.h \
		kernel/include/data_region.h \
		kernel/include/config_journal.h \
		kernel/include/sha256.h \
		kernel/include/ipv4_config.h \
		kernel/include/appliance_network_config.h \
		kernel/include/appliance_persistent_network_config.h \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		tests/appliance-persistent-network-config-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PERSISTENT_NETWORK_CONFIG_HOST_TEST \
		-Ikernel/include \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		tests/appliance-persistent-network-config-host.c \
		-o build/test-appliance-persistent-network-config-host -no-pie
	build/test-appliance-persistent-network-config-host

test-appliance-persistent-service-config-host: \
		kernel/include/block_device.h \
		kernel/include/data_region.h \
		kernel/include/config_journal.h \
		kernel/include/sha256.h \
		kernel/include/service.h \
		kernel/include/service_health.h \
		kernel/include/service_supervisor.h \
		kernel/include/health_check.h \
		kernel/include/appliance_service_manager.h \
		kernel/include/appliance_persistent_service_config.h \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/appliance-persistent-service-config-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PERSISTENT_SERVICE_CONFIG_HOST_TEST \
		-Ikernel/include \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/appliance-persistent-service-config-host.c \
		-o build/test-appliance-persistent-service-config-host -no-pie
	build/test-appliance-persistent-service-config-host

test-appliance-network-diagnostics-host: \
		kernel/include/appliance_network_diagnostics.h \
		kernel/include/appliance_network_config.h \
		kernel/include/health_check.h \
		kernel/include/ipv4_config.h \
		kernel/include/link_state.h \
		kernel/include/network_device.h \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/network_device.c \
		kernel/src/link_state.c \
		kernel/src/appliance_network_diagnostics.c \
		tests/appliance-network-diagnostics-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_NETWORK_DIAGNOSTICS_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/network_device.c \
		kernel/src/link_state.c \
		kernel/src/appliance_network_diagnostics.c \
		tests/appliance-network-diagnostics-host.c \
		-o build/test-appliance-network-diagnostics-host -no-pie
	build/test-appliance-network-diagnostics-host

test-appliance-dhcp-client-host: \
		kernel/include/appliance_dhcp_client.h \
		kernel/include/appliance_network_config.h \
		kernel/include/ipv4_config.h \
		kernel/src/appliance_dhcp_client.c \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		tests/appliance-dhcp-client-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_DHCP_CLIENT_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_dhcp_client.c \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		tests/appliance-dhcp-client-host.c \
		-o build/test-appliance-dhcp-client-host -no-pie
	build/test-appliance-dhcp-client-host

test-appliance-dns-resolver-host: \
		kernel/include/appliance_dns_resolver.h \
		kernel/include/ipv4_config.h \
		kernel/src/appliance_dns_resolver.c \
		kernel/src/ipv4_config.c \
		tests/appliance-dns-resolver-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_DNS_RESOLVER_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_dns_resolver.c \
		kernel/src/ipv4_config.c \
		tests/appliance-dns-resolver-host.c \
		-o build/test-appliance-dns-resolver-host -no-pie
	build/test-appliance-dns-resolver-host

.PHONY: test-appliance-network-lifecycle-host
test-appliance-network-lifecycle-host: \
		kernel/include/appliance_network_lifecycle.h \
		kernel/include/appliance_dhcp_client.h \
		kernel/include/appliance_dns_resolver.h \
		kernel/include/appliance_network_config.h \
		kernel/include/appliance_network_diagnostics.h \
		kernel/include/ipv4_config.h \
		kernel/include/link_state.h \
		kernel/include/network_device.h \
		kernel/src/appliance_network_lifecycle.c \
		kernel/src/appliance_dhcp_client.c \
		kernel/src/appliance_dns_resolver.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		tests/appliance-network-lifecycle-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_NETWORK_LIFECYCLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_network_lifecycle.c \
		kernel/src/appliance_dhcp_client.c \
		kernel/src/appliance_dns_resolver.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		tests/appliance-network-lifecycle-host.c \
		-o build/test-appliance-network-lifecycle-host -no-pie
	build/test-appliance-network-lifecycle-host

.PHONY: test-appliance-network-wire-host
test-appliance-network-wire-host: \
		kernel/include/appliance_network_wire.h \
		kernel/include/appliance_network_lifecycle.h \
		kernel/include/appliance_dhcp_client.h \
		kernel/include/appliance_dns_resolver.h \
		kernel/include/appliance_network_config.h \
		kernel/include/appliance_network_diagnostics.h \
		kernel/include/ipv4_config.h \
		kernel/include/link_state.h \
		kernel/include/network_device.h \
		kernel/src/appliance_network_wire.c \
		kernel/src/appliance_network_lifecycle.c \
		kernel/src/appliance_dhcp_client.c \
		kernel/src/appliance_dns_resolver.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		tests/appliance-network-wire-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_NETWORK_WIRE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_network_wire.c \
		kernel/src/appliance_network_lifecycle.c \
		kernel/src/appliance_dhcp_client.c \
		kernel/src/appliance_dns_resolver.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		tests/appliance-network-wire-host.c \
		-o build/test-appliance-network-wire-host -no-pie
	build/test-appliance-network-wire-host

# ITEM-600: the production STATIC boot ordering.  The proof drives the real
# x86 wire translation unit and the real hfoc_lwip_netif.  The first boot link
# is not usable yet (the Realtek bind has staged the netif but the PHY is still
# negotiating), then the composition becomes ready and the saved STATIC
# address must be applied without a reboot.
.PHONY: test-appliance-production-static-boot-host
test-appliance-production-static-boot-host: \
		kernel/include/appliance_network_config.h \
		kernel/include/appliance_network_lifecycle.h \
		kernel/include/appliance_network_wire.h \
		kernel/include/appliance_production_network.h \
		kernel/include/appliance_production_network_wire.h \
		kernel/include/appliance_time_sync.h \
		kernel/include/appliance_time_wire.h \
		kernel/include/hfoc_lwip_netif.h \
		kernel/include/hfoc_lwip_proto.h \
		kernel/include/hfoc_lwip_time.h \
		kernel/include/ipv4_config.h \
		kernel/include/network_device.h \
		kernel/src/appliance_production_network_wire_x86.c \
		kernel/src/appliance_network_wire.c \
		kernel/src/appliance_network_lifecycle.c \
		kernel/src/appliance_dhcp_client.c \
		kernel/src/appliance_dns_resolver.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_mode.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/appliance_time_sync.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/hfoc_lwip_netif.c \
		tests/appliance-production-static-boot-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-ffunction-sections -fdata-sections \
		-DHFOC_PRODUCTION_NETWORK \
		-DHFOC_APPLIANCE_PRODUCTION_NETWORK_WIRE_X86_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/appliance_production_network_wire_x86.c \
		kernel/src/appliance_network_wire.c \
		kernel/src/appliance_network_lifecycle.c \
		kernel/src/appliance_dhcp_client.c \
		kernel/src/appliance_dns_resolver.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_mode.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/appliance_time_sync.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/hfoc_lwip_netif.c \
		tests/appliance-production-static-boot-host.c \
		-Wl,--gc-sections \
		-o build/test-appliance-production-static-boot-host -no-pie
	build/test-appliance-production-static-boot-host

# ITEM-601: the SSHv2 wire/crypto/transport modules against stock OpenSSH.
# The host-only POSIX driver is tests/ssh-interop-server.c; the production
# endpoint is a separate owner.  The host profile enables only the mbedTLS
# primitives the modules use, so this proof links a small, pinned closure.
SSH_HOST_MBEDTLS_SOURCES := \
	third_party/mbedtls/library/aes.c \
	third_party/mbedtls/library/asn1parse.c \
	third_party/mbedtls/library/asn1write.c \
	third_party/mbedtls/library/bignum.c \
	third_party/mbedtls/library/bignum_core.c \
	third_party/mbedtls/library/bignum_mod.c \
	third_party/mbedtls/library/bignum_mod_raw.c \
	third_party/mbedtls/library/block_cipher.c \
	third_party/mbedtls/library/cipher.c \
	third_party/mbedtls/library/cipher_wrap.c \
	third_party/mbedtls/library/constant_time.c \
	third_party/mbedtls/library/ecdh.c \
	third_party/mbedtls/library/ecdsa.c \
	third_party/mbedtls/library/ecp.c \
	third_party/mbedtls/library/ecp_curves.c \
	third_party/mbedtls/library/ecp_curves_new.c \
	third_party/mbedtls/library/gcm.c \
	third_party/mbedtls/library/md.c \
	third_party/mbedtls/library/oid.c \
	third_party/mbedtls/library/platform.c \
	third_party/mbedtls/library/platform_util.c \
	third_party/mbedtls/library/sha256.c
SSH_HOST_MBEDTLS_OBJ_DIR := build/ssh-host-mbedtls
SSH_HOST_MBEDTLS_OBJS := $(patsubst third_party/mbedtls/library/%.c,$(SSH_HOST_MBEDTLS_OBJ_DIR)/%.o,$(SSH_HOST_MBEDTLS_SOURCES))
$(SSH_HOST_MBEDTLS_OBJ_DIR)/%.o: third_party/mbedtls/library/%.c \
		tests/hfoc-ssh-host-config.h
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-Itests -Ithird_party/mbedtls/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc-ssh-host-config.h"' \
		-c $< -o $@

.PHONY: test-ssh-interop-host
test-ssh-interop-host: \
		kernel/include/hfoc_ssh_wire.h \
		kernel/include/hfoc_ssh_crypto.h \
		kernel/include/hfoc_ssh_transport.h \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_transport.c \
		tests/ssh-interop-server.c \
		tests/hfoc-ssh-host-config.h \
		tests/test-ssh-interop-host.sh \
		$(SSH_HOST_MBEDTLS_OBJS)
	@mkdir -p build/ssh-interop
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Itests -Ikernel/include -Ithird_party/mbedtls/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc-ssh-host-config.h"' \
		-DHFOC_PRODUCTION_TLS \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_transport.c \
		tests/ssh-interop-server.c \
		$(SSH_HOST_MBEDTLS_OBJS) \
		-o build/ssh-interop/server -no-pie
	bash tests/test-ssh-interop-host.sh

# ITEM-602: a completed command larger than one channel window/output buffer.
# The transport must defer, not disconnect, and close only after the last byte.
.PHONY: test-ssh-transport-window-host
test-ssh-transport-window-host: \
		kernel/include/hfoc_ssh_wire.h \
		kernel/include/hfoc_ssh_crypto.h \
		kernel/include/hfoc_ssh_transport.h \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_transport.c \
		tests/ssh-transport-window-host.c \
		tests/hfoc-ssh-host-config.h \
		$(SSH_HOST_MBEDTLS_OBJS)
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Itests -Ikernel/include -Ithird_party/mbedtls/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc-ssh-host-config.h"' \
		-DHFOC_PRODUCTION_TLS \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_transport.c \
		tests/ssh-transport-window-host.c \
		$(SSH_HOST_MBEDTLS_OBJS) \
		-o build/test-ssh-transport-window-host -no-pie
	build/test-ssh-transport-window-host

# ITEM-629: bounded terminal queues and the SSH pty/shell request surface.
.PHONY: test-tty-host
test-tty-host: kernel/include/tty.h kernel/src/tty.c tests/tty-host.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include -DHFOC_TTY_HOST_TEST \
		tests/tty-host.c kernel/src/tty.c \
		-o build/test-tty-host -no-pie
	build/test-tty-host

.PHONY: test-ssh-shell-host
test-ssh-shell-host: \
		kernel/include/hfoc_ssh_wire.h \
		kernel/include/hfoc_ssh_crypto.h \
		kernel/include/hfoc_ssh_transport.h \
		kernel/include/tty.h \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_transport.c \
		tests/ssh-shell-host.c \
		tests/hfoc-ssh-host-config.h \
		$(SSH_HOST_MBEDTLS_OBJS)
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Itests -Ikernel/include -Ithird_party/mbedtls/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc-ssh-host-config.h"' \
		-DHFOC_PRODUCTION_TLS \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_transport.c \
		tests/ssh-shell-host.c \
		$(SSH_HOST_MBEDTLS_OBJS) \
		-o build/test-ssh-shell-host -no-pie
	build/test-ssh-shell-host

# ITEM-603: adversarial bytes and deadline transitions on the same transport.
.PHONY: test-ssh-adversarial-host
test-ssh-adversarial-host: \
		kernel/include/hfoc_ssh_wire.h \
		kernel/include/hfoc_ssh_crypto.h \
		kernel/include/hfoc_ssh_transport.h \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_transport.c \
		tests/ssh-adversarial-host.c \
		tests/hfoc-ssh-host-config.h \
		$(SSH_HOST_MBEDTLS_OBJS)
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Itests -Ikernel/include -Ithird_party/mbedtls/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc-ssh-host-config.h"' \
		-DHFOC_PRODUCTION_TLS \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_transport.c \
		tests/ssh-adversarial-host.c \
		$(SSH_HOST_MBEDTLS_OBJS) \
		-o build/test-ssh-adversarial-host -no-pie
	build/test-ssh-adversarial-host

.PHONY: test-appliance-ssh-identity-host
test-appliance-ssh-identity-host: \
		kernel/include/appliance_ssh_identity.h \
		kernel/include/appliance_ssh_authorized_key.h \
		kernel/src/appliance_ssh_identity.c \
		kernel/src/appliance_ssh_authorized_key.c \
		kernel/src/sha256.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/block_device.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		tests/appliance-ssh-identity-host.c \
		tests/fixtures/ssh/qemu-test-key.h \
		tests/hfoc-ssh-host-config.h \
		$(SSH_HOST_MBEDTLS_OBJS)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Itests -Itests/fixtures/ssh -Ikernel/include \
		-Ithird_party/mbedtls/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc-ssh-host-config.h"' \
		-DHFOC_PRODUCTION_TLS \
		-DHFOC_APPLIANCE_SSH_IDENTITY_HOST_TEST \
		kernel/src/appliance_ssh_identity.c \
		kernel/src/appliance_ssh_authorized_key.c \
		kernel/src/sha256.c \
		kernel/src/hfoc_ssh_crypto.c \
		kernel/src/hfoc_ssh_wire.c \
		kernel/src/block_device.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		tests/appliance-ssh-identity-host.c \
		$(SSH_HOST_MBEDTLS_OBJS) \
		-o build/test-appliance-ssh-identity-host -no-pie
	build/test-appliance-ssh-identity-host

# ITEM-602: the command surface and the diag stream framing, driven through the
# same dispatch the production SSH endpoint calls.  The owner functions are
# deterministic stand-ins; the production QEMU proof drives the real owners.
.PHONY: test-appliance-ssh-command-host
test-appliance-ssh-command-host: \
		kernel/include/appliance_ssh_command.h \
		kernel/include/appliance_boot_log_store.h \
		kernel/include/diag_export.h \
		kernel/src/appliance_ssh_command.c \
		kernel/src/diag_session.c \
		tests/appliance-ssh-command-host.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Itests -Ikernel/include \
		-DHFOC_APPLIANCE_SSH_COMMAND_HOST_TEST \
		kernel/src/appliance_ssh_command.c \
		kernel/src/diag_session.c \
		tests/appliance-ssh-command-host.c \
		-o build/test-appliance-ssh-command-host -no-pie
	build/test-appliance-ssh-command-host

test-appliance-production-service-https-host: \
		kernel/include/appliance_production_service_https.h \
		kernel/src/appliance_production_service_https.c \
		tests/appliance-production-service-https-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_SERVICE_HTTPS_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_service_https.c \
		tests/appliance-production-service-https-host.c \
		-o build/test-appliance-production-service-https-host -no-pie
	build/test-appliance-production-service-https-host

test-appliance-production-service-activation-host: \
		kernel/include/appliance_production_service_activation.h \
		kernel/include/appliance_production_service_https.h \
		kernel/src/appliance_production_service_activation.c \
		kernel/src/appliance_production_service_https.c \
		tests/appliance-production-service-activation-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_service_activation.c \
		kernel/src/appliance_production_service_https.c \
		tests/appliance-production-service-activation-host.c \
		-o build/test-appliance-production-service-activation-host -no-pie
	build/test-appliance-production-service-activation-host

test-appliance-production-management-host: \
		kernel/include/appliance_production_management.h \
		kernel/src/appliance_production_management.c \
		tests/appliance-production-management-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_MANAGEMENT_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_management.c \
		tests/appliance-production-management-host.c \
		-o build/test-appliance-production-management-host -no-pie
	build/test-appliance-production-management-host

test-appliance-production-management-api-host: \
		kernel/include/appliance_production_management_api.h \
		kernel/include/appliance_system_api.h \
		kernel/include/appliance_network_api.h \
		kernel/include/appliance_service_api.h \
		kernel/include/appliance_health_api.h \
		kernel/src/appliance_production_management_api.c \
		kernel/src/appliance_system_api.c \
		kernel/src/appliance_network_api.c \
		kernel/src/appliance_service_api.c \
		kernel/src/appliance_health_api.c \
		kernel/src/appliance_health.c \
		kernel/src/appliance_identity.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/http_response.c \
		tests/appliance-production-management-api-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_MANAGEMENT_API_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_management_api.c \
		kernel/src/appliance_system_api.c \
		kernel/src/appliance_network_api.c \
		kernel/src/appliance_service_api.c \
		kernel/src/appliance_health_api.c \
		kernel/src/appliance_health.c \
		kernel/src/appliance_identity.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/http_response.c \
		tests/appliance-production-management-api-host.c \
		-o build/test-appliance-production-management-api-host -no-pie
	build/test-appliance-production-management-api-host

test-appliance-production-management-plane-host: \
		kernel/include/appliance_production_management_plane.h \
		kernel/include/appliance_production_management.h \
		kernel/include/appliance_production_auth.h \
		kernel/include/appliance_admin_auth.h \
		kernel/include/appliance_management_authorization.h \
		kernel/include/authorization.h \
		kernel/include/http_parser.h \
		kernel/src/appliance_production_management_plane.c \
		kernel/src/appliance_production_management.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		tests/appliance-production-management-plane-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_management_plane.c \
		kernel/src/appliance_production_management.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		tests/appliance-production-management-plane-host.c \
		-o build/test-appliance-production-management-plane-host -no-pie
	build/test-appliance-production-management-plane-host

test-appliance-production-mutation-plane-host: \
		kernel/include/appliance_production_mutation_plane.h \
		kernel/include/appliance_production_management_plane.h \
		kernel/include/appliance_production_management.h \
		kernel/include/appliance_production_auth.h \
		kernel/include/appliance_admin_auth.h \
		kernel/include/appliance_management_authorization.h \
		kernel/include/appliance_route_config.h \
		kernel/include/appliance_route_table.h \
		kernel/include/authorization.h \
		kernel/include/http_parser.h \
		kernel/include/network_config_transaction.h \
		kernel/include/ipv4_config.h \
		kernel/src/appliance_production_mutation_plane.c \
		kernel/src/appliance_production_management_plane.c \
		kernel/src/appliance_production_management.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_route_config.c \
		kernel/src/appliance_route_table.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		kernel/src/network_config_transaction.c \
		kernel/src/ipv4_config.c \
		tests/appliance-production-mutation-plane-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_MUTATION_PLANE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_mutation_plane.c \
		kernel/src/appliance_production_management_plane.c \
		kernel/src/appliance_production_management.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_route_config.c \
		kernel/src/appliance_route_table.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		kernel/src/network_config_transaction.c \
		kernel/src/ipv4_config.c \
		tests/appliance-production-mutation-plane-host.c \
		-o build/test-appliance-production-mutation-plane-host -no-pie
	build/test-appliance-production-mutation-plane-host

test-appliance-production-update-plane-host: \
		kernel/include/appliance_production_update_plane.h \
		kernel/include/appliance_production_management_plane.h \
		kernel/include/appliance_production_management.h \
		kernel/include/appliance_production_auth.h \
		kernel/include/appliance_admin_auth.h \
		kernel/include/appliance_management_authorization.h \
		kernel/include/appliance_update_integrity.h \
		kernel/include/appliance_update_manifest.h \
		kernel/include/authorization.h \
		kernel/include/key_use.h \
		kernel/include/sha256.h \
		kernel/include/signed_metadata.h \
		kernel/include/trust_anchor.h \
		kernel/include/update_compatibility.h \
		kernel/include/verification_policy.h \
		kernel/src/appliance_production_update_plane.c \
		kernel/src/appliance_production_management_plane.c \
		kernel/src/appliance_production_management.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		kernel/src/appliance_update_integrity.c \
		kernel/src/appliance_update_manifest.c \
		kernel/src/key_use.c \
		kernel/src/sha256.c \
		kernel/src/signed_metadata.c \
		kernel/src/trust_anchor.c \
		kernel/src/update_compatibility.c \
		kernel/src/verification_policy.c \
		tests/appliance-production-update-plane-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_UPDATE_PLANE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_update_plane.c \
		kernel/src/appliance_production_management_plane.c \
		kernel/src/appliance_production_management.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		kernel/src/appliance_update_integrity.c \
		kernel/src/appliance_update_manifest.c \
		kernel/src/key_use.c \
		kernel/src/sha256.c \
		kernel/src/signed_metadata.c \
		kernel/src/trust_anchor.c \
		kernel/src/update_compatibility.c \
		kernel/src/verification_policy.c \
		tests/appliance-production-update-plane-host.c \
		-o build/test-appliance-production-update-plane-host -no-pie
	build/test-appliance-production-update-plane-host

test-appliance-production-recovery-executor-host: \
		kernel/include/appliance_production_recovery_executor.h \
		kernel/include/appliance_production_management_plane.h \
		kernel/include/appliance_production_management.h \
		kernel/include/appliance_production_auth.h \
		kernel/include/appliance_admin_auth.h \
		kernel/include/appliance_management_authorization.h \
		kernel/include/appliance_factory_reset.h \
		kernel/include/appliance_recovery_mode.h \
		kernel/include/appliance_config_integrity.h \
		kernel/include/appliance_persistent_storage_namespace.h \
		kernel/include/appliance_state_layout.h \
		kernel/include/appliance_state_block_device.h \
		kernel/include/authorization.h \
		kernel/include/block_device.h \
		kernel/include/data_region.h \
		kernel/include/http_parser.h \
		kernel/src/appliance_production_recovery_executor.c \
		kernel/src/appliance_production_management_plane.c \
		kernel/src/appliance_production_management.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/appliance_factory_reset.c \
		kernel/src/appliance_recovery_mode.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/authorization.c \
		kernel/src/block_device.c \
		tests/appliance-production-recovery-executor-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_recovery_executor.c \
		kernel/src/appliance_production_management_plane.c \
		kernel/src/appliance_production_management.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/appliance_factory_reset.c \
		kernel/src/appliance_recovery_mode.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/authorization.c \
		kernel/src/block_device.c \
		tests/appliance-production-recovery-executor-host.c \
		-o build/test-appliance-production-recovery-executor-host -no-pie
	build/test-appliance-production-recovery-executor-host

test-appliance-production-auth-host: \
		kernel/include/appliance_production_auth.h \
		kernel/include/appliance_admin_auth.h \
		kernel/include/appliance_management_authorization.h \
		kernel/include/authorization.h \
		kernel/include/http_parser.h \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		tests/appliance-production-auth-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_AUTH_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		tests/appliance-production-auth-host.c \
		-o build/test-appliance-production-auth-host -no-pie
	build/test-appliance-production-auth-host

test-appliance-production-admin-enrollment-host: \
		kernel/include/appliance_production_admin_enrollment.h \
		kernel/include/appliance_production_auth.h \
		kernel/include/appliance_admin_auth.h \
		kernel/include/appliance_management_authorization.h \
		kernel/include/authorization.h \
		kernel/include/appliance_persistent_secret_config.h \
		kernel/include/appliance_admin_credential.h \
		kernel/include/appliance_secret_persistence.h \
		kernel/include/appliance_secret_protection.h \
		kernel/include/config_journal.h \
		kernel/include/data_region.h \
		kernel/include/block_device.h \
		kernel/include/sha256.h \
		kernel/include/http_parser.h \
		kernel/src/appliance_production_admin_enrollment.c \
		kernel/src/appliance_secret_protection.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		kernel/src/config_journal.c \
		kernel/src/data_region.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-production-admin-enrollment-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_admin_enrollment.c \
		kernel/src/appliance_secret_protection.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		kernel/src/config_journal.c \
		kernel/src/data_region.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-production-admin-enrollment-host.c \
		-o build/test-appliance-production-admin-enrollment-host -no-pie
	build/test-appliance-production-admin-enrollment-host

test-appliance-admin-credential-host: \
		kernel/include/appliance_admin_credential.h \
		kernel/include/appliance_persistent_secret_config.h \
		kernel/include/appliance_secret_persistence.h \
		kernel/include/config_journal.h \
		kernel/include/data_region.h \
		kernel/include/block_device.h \
		kernel/include/sha256.h \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/config_journal.c \
		kernel/src/data_region.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-admin-credential-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_ADMIN_CREDENTIAL_HOST_TEST \
		-DHFOC_SHA256_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/config_journal.c \
		kernel/src/data_region.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-admin-credential-host.c \
		-o build/test-appliance-admin-credential-host -no-pie
	build/test-appliance-admin-credential-host

test-appliance-admin-credentials-prompt-host: \
		kernel/include/appliance_admin_credentials_prompt.h \
		kernel/include/appliance_admin_credential.h \
		kernel/include/appliance_language.h \
		kernel/include/sha256.h \
		kernel/src/appliance_admin_credentials_prompt.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_language.c \
		kernel/src/sha256.c \
		tests/appliance-admin-credentials-prompt-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_ADMIN_CREDENTIALS_PROMPT_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_admin_credentials_prompt.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_language.c \
		kernel/src/sha256.c \
		tests/appliance-admin-credentials-prompt-host.c \
		-o build/test-appliance-admin-credentials-prompt-host -no-pie
	build/test-appliance-admin-credentials-prompt-host

test-installer-credential-screen-host: \
		kernel/include/installer_credential_screen.h \
		kernel/include/appliance_admin_credentials_prompt.h \
		kernel/include/appliance_admin_credential.h \
		kernel/include/appliance_language.h \
		kernel/include/keyboard.h \
		kernel/src/installer_credential_screen.c \
		kernel/src/appliance_admin_credentials_prompt.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_language.c \
		kernel/src/sha256.c \
		tests/installer-credential-screen-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INSTALLER_CREDENTIAL_SCREEN_HOST_TEST \
		-Ikernel/include \
		kernel/src/installer_credential_screen.c \
		kernel/src/appliance_admin_credentials_prompt.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_language.c \
		kernel/src/sha256.c \
		tests/installer-credential-screen-host.c \
		-o build/test-installer-credential-screen-host -no-pie
	build/test-installer-credential-screen-host

test-appliance-first-boot-integration-host: \
		kernel/include/appliance_first_boot_integration.h \
		kernel/src/appliance_first_boot_integration.c \
		tests/appliance-first-boot-integration-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_FIRST_BOOT_INTEGRATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_first_boot_integration.c \
		tests/appliance-first-boot-integration-host.c \
		-o build/test-appliance-first-boot-integration-host -no-pie
	build/test-appliance-first-boot-integration-host

test-appliance-recovery-factory-reset-integration-host: \
		kernel/include/appliance_recovery_factory_reset_integration.h \
		kernel/src/appliance_recovery_factory_reset_integration.c \
		tests/appliance-recovery-factory-reset-integration-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_recovery_factory_reset_integration.c \
		tests/appliance-recovery-factory-reset-integration-host.c \
		-o build/test-appliance-recovery-factory-reset-integration-host -no-pie
	build/test-appliance-recovery-factory-reset-integration-host

test-appliance-service-manager-host: \
		kernel/include/appliance_service_manager.h \
		kernel/include/service.h \
		kernel/include/service_supervisor.h \
		kernel/include/service_health.h \
		kernel/include/health_check.h \
		kernel/src/appliance_service_manager.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		tests/appliance-service-manager-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_SERVICE_MANAGER_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_service_manager.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		tests/appliance-service-manager-host.c \
		-o build/test-appliance-service-manager-host -no-pie
	build/test-appliance-service-manager-host

test-appliance-service-failure-handler-host: \
		kernel/include/appliance_service_failure_handler.h \
		kernel/include/appliance_service_manager.h \
		kernel/include/service.h \
		kernel/include/service_supervisor.h \
		kernel/include/service_health.h \
		kernel/include/health_check.h \
		kernel/src/appliance_service_failure_handler.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		tests/appliance-service-failure-handler-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_SERVICE_FAILURE_HANDLER_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_service_failure_handler.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		tests/appliance-service-failure-handler-host.c \
		-o build/test-appliance-service-failure-handler-host -no-pie
	build/test-appliance-service-failure-handler-host

test-appliance-management-api-host: \
		kernel/include/appliance_management_api.h \
		kernel/include/http_parser.h \
		kernel/include/http_response.h \
		kernel/src/appliance_management_api.c \
		kernel/src/http_response.c \
		tests/appliance-management-api-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHTTP_RESPONSE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_management_api.c \
		kernel/src/http_response.c \
		tests/appliance-management-api-host.c \
		-o build/test-appliance-management-api-host -no-pie
	build/test-appliance-management-api-host

test-appliance-system-api-host: \
		kernel/include/appliance_language.h \
		kernel/include/appliance_config_validation.h \
		kernel/include/appliance_identity.h \
		kernel/include/appliance_system_api.h \
		kernel/include/http_parser.h \
		kernel/include/http_response.h \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_identity.c \
		kernel/src/appliance_system_api.c \
		kernel/src/http_response.c \
		tests/appliance-system-api-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_SYSTEM_API_HOST_TEST \
		-DHTTP_RESPONSE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_identity.c \
		kernel/src/appliance_system_api.c \
		kernel/src/http_response.c \
		tests/appliance-system-api-host.c \
		-o build/test-appliance-system-api-host -no-pie
	build/test-appliance-system-api-host

test-appliance-network-api-host: \
		kernel/include/appliance_network_api.h \
		kernel/include/appliance_network_config.h \
		kernel/include/appliance_network_diagnostics.h \
		kernel/include/http_parser.h \
		kernel/include/http_response.h \
		kernel/include/ipv4_config.h \
		kernel/include/link_state.h \
		kernel/include/network_device.h \
		kernel/src/appliance_network_api.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/http_response.c \
		tests/appliance-network-api-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_NETWORK_API_HOST_TEST \
		-DHTTP_RESPONSE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_network_api.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_network_diagnostics.c \
		kernel/src/ipv4_config.c \
		kernel/src/link_state.c \
		kernel/src/network_device.c \
		kernel/src/http_response.c \
		tests/appliance-network-api-host.c \
		-o build/test-appliance-network-api-host -no-pie
	build/test-appliance-network-api-host

test-appliance-service-api-host: \
		kernel/include/appliance_service_api.h \
		kernel/include/appliance_service_manager.h \
		kernel/include/health_check.h \
		kernel/include/http_parser.h \
		kernel/include/http_response.h \
		kernel/include/service.h \
		kernel/include/service_health.h \
		kernel/include/service_supervisor.h \
		kernel/src/appliance_service_api.c \
		kernel/src/http_response.c \
		tests/appliance-service-api-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_SERVICE_API_HOST_TEST \
		-DHTTP_RESPONSE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_service_api.c \
		kernel/src/http_response.c \
		tests/appliance-service-api-host.c \
		-o build/test-appliance-service-api-host -no-pie
	build/test-appliance-service-api-host

test-appliance-https-service-host: \
		kernel/include/appliance_https_service.h \
		kernel/include/appliance_service_manager.h \
		kernel/include/https_service.h \
		kernel/include/https_service_lifecycle.h \
		kernel/include/service.h \
		kernel/include/service_supervisor.h \
		kernel/src/appliance_https_service.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/https_service.c \
		kernel/src/https_service_lifecycle.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		tests/appliance-https-service-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_HTTPS_SERVICE_HOST_TEST \
		-DHTTPS_SERVICE_LIFECYCLE_HOST_TEST \
	-DAPPLIANCE_MANAGEMENT_PANEL_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_https_service.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/https_service.c \
		kernel/src/https_service_lifecycle.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		tests/appliance-https-service-host.c \
		-o build/test-appliance-https-service-host -no-pie
	build/test-appliance-https-service-host

test-appliance-console-host: \
		kernel/include/appliance_language.h \
		kernel/include/appliance_config_validation.h \
		kernel/include/appliance_console.h \
		kernel/include/appliance_identity.h \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_identity.c \
		kernel/src/appliance_console.c \
		tests/appliance-console-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_CONSOLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_identity.c \
		kernel/src/appliance_console.c \
		tests/appliance-console-host.c \
		-o build/test-appliance-console-host -no-pie
	build/test-appliance-console-host

.PHONY: test-appliance-boot-console-host
test-appliance-boot-console-host: kernel/include/appliance_boot_console.h \
		kernel/src/appliance_boot_console.c \
		kernel/include/appliance_console.h \
		kernel/include/appliance_identity.h \
		kernel/src/appliance_console.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_identity.c \
		tests/appliance-boot-console-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_boot_console.c \
		kernel/src/appliance_console.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_identity.c \
		tests/appliance-boot-console-host.c \
		-o build/test-appliance-boot-console-host -no-pie
	build/test-appliance-boot-console-host

.PHONY: test-appliance-management-web-ui-host
test-appliance-management-web-ui-host: \
		kernel/include/appliance_management_web_ui.h \
		kernel/include/appliance_production_panel_view.h \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_license_inventory.c \
		kernel/src/appliance_production_panel_view.c \
		docs/hfoc-management-web-ui.md \
		tests/appliance-management-web-ui-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_license_inventory.c \
		kernel/src/appliance_production_panel_view.c \
		tests/appliance-management-web-ui-host.c \
		-o build/test-appliance-management-web-ui-host -no-pie
	build/test-appliance-management-web-ui-host docs/hfoc-management-web-ui.md

.PHONY: test-appliance-license-inventory-host
test-appliance-license-inventory-host: \
		kernel/include/appliance_license_inventory.h \
		kernel/include/appliance_management_panel.h \
		kernel/include/appliance_management_web_ui.h \
		kernel/src/appliance_license_inventory.c \
		kernel/src/appliance_management_panel.c \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_panel_login.c \
		release/hfoc-license-inventory.txt \
		tests/appliance-license-inventory-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_LICENSE_INVENTORY_HOST_TEST \
		-DAPPLIANCE_MANAGEMENT_PANEL_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_license_inventory.c \
		kernel/src/appliance_management_panel.c \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_panel_login.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		tests/appliance-license-inventory-host.c \
		-o build/test-appliance-license-inventory-host -no-pie
	build/test-appliance-license-inventory-host \
		release/hfoc-license-inventory.txt

.PHONY: test-appliance-management-panel-host
test-appliance-management-panel-host: \
		kernel/include/appliance_management_panel.h \
		kernel/include/appliance_management_web_ui.h \
		kernel/include/appliance_production_auth.h \
		kernel/include/appliance_admin_auth.h \
		kernel/include/appliance_management_authorization.h \
		kernel/include/authorization.h \
		kernel/include/http_parser.h \
		kernel/src/appliance_management_panel.c \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_license_inventory.c \
		kernel/src/appliance_panel_login.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		tests/appliance-management-panel-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_MANAGEMENT_PANEL_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_management_panel.c \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_license_inventory.c \
		kernel/src/appliance_panel_login.c \
		kernel/src/appliance_production_auth.c \
		kernel/src/appliance_management_authorization.c \
		kernel/src/appliance_admin_auth.c \
		kernel/src/authorization.c \
		tests/appliance-management-panel-host.c \
		-o build/test-appliance-management-panel-host -no-pie
	build/test-appliance-management-panel-host

# ITEM-515: the panel's forms are proven with the bodies the pages draw.  The
# test renders each page, derives every form's body from its controls and
# drives the parser the serving path drives, and it names every network
# refusal the way the HTTP layer quotes it.
.PHONY: test-appliance-panel-forms-host
test-appliance-panel-forms-host: \
		kernel/include/appliance_management_web_ui.h \
		kernel/include/appliance_language.h \
		kernel/include/appliance_panel_gate.h \
		kernel/include/appliance_panel_form.h \
		kernel/include/appliance_panel_login.h \
		kernel/include/appliance_route_config.h \
		kernel/include/appliance_route_table.h \
		kernel/include/appliance_network_config.h \
		kernel/include/ipv4_config.h \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_license_inventory.c \
		kernel/src/appliance_production_panel_view.c \
		kernel/src/appliance_panel_form.c \
		kernel/src/appliance_panel_login.c \
		kernel/src/appliance_route_config.c \
		kernel/src/appliance_route_table.c \
		kernel/src/ipv4_config.c \
		tests/appliance-panel-forms-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_management_web_ui.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_license_inventory.c \
		kernel/src/appliance_production_panel_view.c \
		kernel/src/appliance_panel_form.c \
		kernel/src/appliance_panel_login.c \
		kernel/src/appliance_route_config.c \
		kernel/src/appliance_route_table.c \
		kernel/src/ipv4_config.c \
		tests/appliance-panel-forms-host.c \
		-o build/test-appliance-panel-forms-host -no-pie
	build/test-appliance-panel-forms-host

.PHONY: test-appliance-production-panel-view-host
test-appliance-production-panel-view-host: \
		kernel/include/appliance_production_panel_view.h \
		kernel/include/appliance_management_web_ui.h \
		kernel/src/appliance_production_panel_view.c \
		tests/appliance-production-panel-view-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DAPPLIANCE_PRODUCTION_PANEL_VIEW_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_production_panel_view.c \
		tests/appliance-production-panel-view-host.c \
		-o build/test-appliance-production-panel-view-host -no-pie
	build/test-appliance-production-panel-view-host

test-appliance-network-config-host: \
		kernel/include/appliance_network_config.h \
		kernel/include/ipv4_config.h \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		tests/appliance-network-config-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_APPLIANCE_NETWORK_CONFIG_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		tests/appliance-network-config-host.c \
		-o build/test-appliance-network-config-host -no-pie
	build/test-appliance-network-config-host

test-build-input-manifest-host:
	bash tests/test-build-input-manifest-host.sh

test-reproducible-image-comparison-host:
	bash tests/test-reproducible-image-comparison-host.sh

test-reproducible-image-host:
	@set -e; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	make clean; \
	CCACHE_DISABLE=1 $(MAKE) -j$(HFOC_IMAGE_JOBS) iso; \
	bash scripts/capture-reproducible-image-digests.sh "$$tmp/image-1.digests"; \
	make clean; \
	CCACHE_DISABLE=1 $(MAKE) -j$(HFOC_IMAGE_JOBS) iso; \
	bash scripts/capture-reproducible-image-digests.sh "$$tmp/image-2.digests"; \
	bash scripts/compare-reproducible-image-digests.sh \
		"$$tmp/image-1.digests" "$$tmp/image-2.digests" \
		build/release/reproducibility/comparison-report.txt

# ITEM-380 release gate contracts (host proof) and the release-time gate. The
# host proof drives the SBOM, the CVE runbook and the gate with fixture
# reproduction evidence; the release-time target builds the ISO twice from
# clean trees, records the reproducibility report, generates the SBOM and
# applies the gate. It is not part of make test.  ITEM-642: the target must not
# depend on `iso`: a prerequisite would build the first image incrementally
# before the recipe's first clean and make the comparison meaningless.
test-product-release-gate-host:
	bash tests/test-release-gate-host.sh

test-product-release-gate:
	@set -e; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	make clean; \
	CCACHE_DISABLE=1 $(MAKE) -j$(HFOC_IMAGE_JOBS) iso; \
	bash scripts/capture-reproducible-image-digests.sh "$$tmp/image-1.digests"; \
	make clean; \
	CCACHE_DISABLE=1 $(MAKE) -j$(HFOC_IMAGE_JOBS) iso; \
	bash scripts/capture-reproducible-image-digests.sh "$$tmp/image-2.digests"; \
	bash scripts/compare-reproducible-image-digests.sh \
		"$$tmp/image-1.digests" "$$tmp/image-2.digests" \
		build/release/reproducibility/comparison-report.txt; \
	bash scripts/generate-release-sbom.sh \
		build/release/sbom/HFOC-APPLIANCE-V1.spdx; \
	bash scripts/verify-product-release-gate.sh build/release

test-release-artifact-manifest-host:
	bash tests/test-release-artifact-manifest-host.sh

test-release-artifact-host: test-license-payload-iso-host release-license-payload
	@set -e; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	bash scripts/make-release-update-bundle.sh "$$tmp/hfoc-update.bundle"; \
	HFOC_ARTIFACT_IMAGE='$(IMAGE)' \
	HFOC_ARTIFACT_RECOVERY_IMAGE='$(FALLBACK_ISO)' \
	HFOC_ARTIFACT_UPDATE_BUNDLE="$$tmp/hfoc-update.bundle" \
	HFOC_ARTIFACT_SUPPORT_SCHEMA='kernel/include/support_bundle_manifest.h' \
	HFOC_ARTIFACT_LICENSE_MANIFEST='build/release/licenses/LICENSES.sha256' \
		bash scripts/generate-release-artifact-manifest.sh \
		"$$tmp/release-artifacts.txt"; \
	HFOC_ARTIFACT_IMAGE='$(IMAGE)' \
	HFOC_ARTIFACT_RECOVERY_IMAGE='$(FALLBACK_ISO)' \
	HFOC_ARTIFACT_UPDATE_BUNDLE="$$tmp/hfoc-update.bundle" \
	HFOC_ARTIFACT_SUPPORT_SCHEMA='kernel/include/support_bundle_manifest.h' \
	HFOC_ARTIFACT_LICENSE_MANIFEST='build/release/licenses/LICENSES.sha256' \
		bash scripts/verify-release-artifact-manifest.sh \
		"$$tmp/release-artifacts.txt"

test-release-upgrade-matrix-host:
	bash tests/test-release-upgrade-matrix-host.sh

test-release-upgrade-matrix:
	bash scripts/verify-release-upgrade-matrix.sh \
		release/release-upgrade-matrix.txt

test-release-backup-restore-matrix-host: kernel/include/config_kv.h \
		kernel/include/backup_stream.h kernel/include/restore_validator.h \
		kernel/include/sha256.h kernel/src/config_kv.c \
		kernel/src/backup_stream.c kernel/src/restore_validator.c \
		kernel/src/sha256.c tests/release-backup-restore-matrix-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/backup_stream.c kernel/src/config_kv.c \
		kernel/src/restore_validator.c kernel/src/sha256.c \
		tests/release-backup-restore-matrix-host.c \
		-o build/test-release-backup-restore-matrix-host -no-pie
	build/test-release-backup-restore-matrix-host
	bash tests/test-release-backup-restore-matrix-host.sh

test-release-backup-restore-matrix:
	bash scripts/verify-release-backup-restore-matrix.sh \
		release/release-backup-restore-matrix.txt

test-release-security-baseline-host:
	bash tests/test-release-security-baseline-host.sh

test-release-security-baseline:
	bash scripts/test-release-security-baseline.sh

test-release-operational-baseline-host:
	bash tests/test-release-operational-baseline-host.sh

test-release-operational-qemu-liveness:
	bash scripts/test-release-operational-qemu-liveness.sh

test-release-operational-baseline:
	bash scripts/test-release-operational-baseline.sh

test-release-disaster-recovery-rehearsal-host:
	bash tests/test-release-disaster-recovery-rehearsal-host.sh

test-release-disaster-recovery-rehearsal:
	bash scripts/test-release-disaster-recovery-rehearsal.sh

test-release-appliance-v1-acceptance-host:
	bash tests/test-release-appliance-v1-acceptance-host.sh

test-release-appliance-v1-acceptance:
	bash scripts/test-release-appliance-v1-acceptance.sh

test-hfoc-appliance-platform-release-acceptance-host:
	bash tests/test-hfoc-appliance-platform-release-acceptance-host.sh

test-hfoc-appliance-platform-release-acceptance:
	bash scripts/test-hfoc-appliance-platform-release-acceptance.sh

test-hfoc-integrated-installer-dual-boot-management-acceptance-host:
	bash tests/test-hfoc-integrated-installer-dual-boot-management-acceptance-host.sh

test-hfoc-integrated-installer-dual-boot-management-acceptance:
	bash scripts/test-hfoc-integrated-installer-dual-boot-management-acceptance.sh

.PHONY: test-production-end-to-end-acceptance-host test-production-end-to-end-acceptance
test-production-end-to-end-acceptance-host:
	bash tests/test-production-end-to-end-acceptance-host.sh

# ITEM-381 hardware acceptance harness, matrix and report. The host proof never
# touches /dev/sda; the physical run itself stays human-triggered and is not
# part of make test, the QEMU suite or CI.
test-hardware-acceptance-host:
	bash tests/test-hardware-acceptance-host.sh

test-production-end-to-end-acceptance:
	bash scripts/test-production-end-to-end-acceptance.sh

.PHONY: test-nvme-production-acceptance-host test-nvme-production-acceptance
test-nvme-production-acceptance-host:
	bash tests/test-hfoc-nvme-production-acceptance-host.sh

test-nvme-production-acceptance:
	bash scripts/test-hfoc-nvme-production-acceptance.sh

test-hfoc-release-documentation-host:
	bash tests/test-hfoc-release-documentation-host.sh

test-hfoc-release-documentation:
	bash scripts/test-hfoc-release-documentation.sh

test-https-vertical-slice-acceptance-host:
	bash tests/test-https-vertical-slice-acceptance-host.sh

test-https-vertical-slice-acceptance:
	bash scripts/test-https-vertical-slice-acceptance.sh

test-integration-replay-outage-drill-host: \
		kernel/include/fault_injection.h \
		kernel/include/integration_boundary.h \
		kernel/include/integration_compatibility.h \
		kernel/include/integration_envelope.h \
		kernel/include/integration_failure_isolation.h \
		kernel/include/integration_ledger.h \
		kernel/include/integration_session.h \
		kernel/include/sha256.h \
		kernel/src/fault_injection.c \
		kernel/src/integration_compatibility.c \
		kernel/src/integration_envelope.c \
		kernel/src/integration_failure_isolation.c \
		kernel/src/integration_ledger.c \
		kernel/src/integration_session.c \
		kernel/src/sha256.c \
		tests/integration-replay-outage-drill-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_HOST_TEST \
		-Ikernel/include \
		kernel/src/fault_injection.c \
		kernel/src/integration_compatibility.c \
		kernel/src/integration_envelope.c \
		kernel/src/integration_failure_isolation.c \
		kernel/src/integration_ledger.c \
		kernel/src/integration_session.c \
		kernel/src/sha256.c \
		tests/integration-replay-outage-drill-host.c \
		-o build/test-integration-replay-outage-drill-host -no-pie
	build/test-integration-replay-outage-drill-host

test-power-cycle-continuity-drill-host: \
		kernel/include/activation_intent.h \
		kernel/include/audit.h \
		kernel/include/automatic_rollback.h \
		kernel/include/candidate_slot_inventory.h \
		kernel/include/candidate_staged_write.h \
		kernel/include/event_ring.h \
		kernel/include/first_boot_health.h \
		kernel/include/health_check.h \
		kernel/include/service.h \
		kernel/include/service_health.h \
		kernel/include/service_supervisor.h \
		kernel/src/activation_intent.c \
		kernel/src/audit.c \
		kernel/src/automatic_rollback.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/event_ring.c \
		kernel/src/first_boot_health.c \
		kernel/src/health_check.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		tests/power-cycle-continuity-drill-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_POWER_CYCLE_CONTINUITY_DRILL_HOST_TEST \
		-Ikernel/include \
		kernel/src/activation_intent.c \
		kernel/src/audit.c \
		kernel/src/automatic_rollback.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/event_ring.c \
		kernel/src/first_boot_health.c \
		kernel/src/health_check.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		tests/power-cycle-continuity-drill-host.c \
		-o build/test-power-cycle-continuity-drill-host -no-pie
	build/test-power-cycle-continuity-drill-host

test-automatic-rollback-host: kernel/include/automatic_rollback.h \
		kernel/include/activation_intent.h \
		kernel/include/first_boot_health.h \
		kernel/src/automatic_rollback.c \
		kernel/src/activation_intent.c \
		kernel/src/first_boot_health.c \
		tests/automatic-rollback-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_AUTOMATIC_ROLLBACK_HOST_TEST \
		-Ikernel/include \
		kernel/src/automatic_rollback.c \
		kernel/src/activation_intent.c \
		kernel/src/first_boot_health.c \
		tests/automatic-rollback-host.c \
		-o build/test-automatic-rollback-host -no-pie
	build/test-automatic-rollback-host

test-update-acceptance-host: kernel/include/update_acceptance.h \
		kernel/include/activation_intent.h \
		kernel/include/first_boot_health.h \
		kernel/src/update_acceptance.c \
		kernel/src/activation_intent.c \
		kernel/src/first_boot_health.c \
		tests/update-acceptance-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_UPDATE_ACCEPTANCE_HOST_TEST \
		-Ikernel/include \
		kernel/src/update_acceptance.c \
		kernel/src/activation_intent.c \
		kernel/src/first_boot_health.c \
		tests/update-acceptance-host.c \
		-o build/test-update-acceptance-host -no-pie
	build/test-update-acceptance-host

test-update-retention-host: kernel/include/update_retention.h \
		kernel/include/activation_intent.h \
		kernel/include/candidate_slot_inventory.h \
		kernel/src/update_retention.c \
		kernel/src/activation_intent.c \
		kernel/src/candidate_slot_inventory.c \
		tests/update-retention-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_UPDATE_RETENTION_HOST_TEST \
		-Ikernel/include \
		kernel/src/update_retention.c \
		kernel/src/activation_intent.c \
		kernel/src/candidate_slot_inventory.c \
		tests/update-retention-host.c \
		-o build/test-update-retention-host -no-pie
	build/test-update-retention-host

test-service-dependency-host: kernel/include/service.h \
		kernel/src/service.c tests/service-dependency-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SERVICE_DEPENDENCY_HOST_TEST \
		-Ikernel/include \
		kernel/src/service.c tests/service-dependency-host.c \
		-o build/test-service-dependency-host -no-pie
	build/test-service-dependency-host

test-service-graph-host: kernel/include/service.h \
		kernel/src/service.c tests/service-graph-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SERVICE_GRAPH_HOST_TEST \
		-Ikernel/include \
		kernel/src/service.c tests/service-graph-host.c \
		-o build/test-service-graph-host -no-pie
	build/test-service-graph-host

test-service-restart-policy-host: kernel/include/service_restart_policy.h \
		kernel/src/service_restart_policy.c \
		tests/service-restart-policy-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SERVICE_RESTART_POLICY_HOST_TEST \
		-Ikernel/include \
		kernel/src/service_restart_policy.c \
		tests/service-restart-policy-host.c \
		-o build/test-service-restart-policy-host -no-pie
	build/test-service-restart-policy-host

test-service-supervisor-host: kernel/include/service.h \
		kernel/include/service_supervisor.h kernel/include/process.h \
		kernel/include/service_restart_policy.h \
		kernel/include/descriptor.h kernel/include/capability.h \
		kernel/include/context.h kernel/include/cpu.h \
		kernel/include/fault_injection.h \
		kernel/src/service.c kernel/src/service_supervisor.c \
		kernel/src/service_restart_policy.c \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c kernel/src/fault_injection.c \
		tests/service-supervisor-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SERVICE_SUPERVISOR_HOST_TEST -DHFOC_PROCESS_HOST_TEST \
		-Ikernel/include \
		kernel/src/service.c kernel/src/service_supervisor.c \
		kernel/src/service_restart_policy.c \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c kernel/src/fault_injection.c \
		tests/service-supervisor-host.c \
		-o build/test-service-supervisor-host -no-pie
	build/test-service-supervisor-host

test-service-cascade-drill-host: kernel/include/service.h \
		kernel/include/service_supervisor.h kernel/include/process.h \
		kernel/include/service_restart_policy.h \
		kernel/include/descriptor.h kernel/include/capability.h \
		kernel/include/context.h kernel/include/cpu.h \
		kernel/include/fault_injection.h \
		kernel/src/service.c kernel/src/service_supervisor.c \
		kernel/src/service_restart_policy.c \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c kernel/src/fault_injection.c \
		tests/service-supervisor-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SERVICE_SUPERVISOR_HOST_TEST -DHFOC_PROCESS_HOST_TEST \
		-DHFOC_SERVICE_CASCADE_DRILL_HOST_TEST \
		-Ikernel/include \
		kernel/src/service.c kernel/src/service_supervisor.c \
		kernel/src/service_restart_policy.c \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c kernel/src/fault_injection.c \
		tests/service-supervisor-host.c \
		-o build/test-service-cascade-drill-host -no-pie
	build/test-service-cascade-drill-host

test-local-control-outage-drill-host: kernel/include/auth_throttle.h \
		kernel/include/fault_injection.h kernel/include/local_session.h \
		kernel/include/management_command.h \
		kernel/src/auth_throttle.c kernel/src/fault_injection.c \
		kernel/src/local_session.c kernel/src/management_command.c \
		tests/local-control-outage-drill-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LOCAL_CONTROL_OUTAGE_DRILL_HOST_TEST \
		-Ikernel/include \
		kernel/src/auth_throttle.c kernel/src/fault_injection.c \
		kernel/src/local_session.c kernel/src/management_command.c \
		tests/local-control-outage-drill-host.c \
		-o build/test-local-control-outage-drill-host -no-pie
	build/test-local-control-outage-drill-host

test-network-partition-drill-host: kernel/include/audit.h \
		kernel/include/event_ring.h \
		kernel/include/fault_injection.h \
		kernel/include/ipv4_config.h \
		kernel/include/link_state.h \
		kernel/include/management_command.h \
		kernel/include/network_admission.h \
		kernel/include/network_config_transaction.h \
		kernel/include/network_device.h \
		kernel/include/network_event.h \
		kernel/include/network_health.h \
		kernel/src/audit.c kernel/src/event_ring.c \
		kernel/src/fault_injection.c kernel/src/ipv4_config.c \
		kernel/src/link_state.c kernel/src/management_command.c \
		kernel/src/management_transport.c kernel/src/network_admission.c \
		kernel/src/network_config_transaction.c \
		kernel/src/network_device.c kernel/src/network_event.c \
		kernel/src/network_health.c kernel/src/authorization.c \
		tests/network-partition-drill-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NETWORK_PARTITION_DRILL_HOST_TEST \
		-Ikernel/include \
		kernel/src/audit.c kernel/src/event_ring.c \
		kernel/src/fault_injection.c kernel/src/ipv4_config.c \
		kernel/src/link_state.c kernel/src/management_command.c \
		kernel/src/management_transport.c kernel/src/network_admission.c \
		kernel/src/network_config_transaction.c \
		kernel/src/network_device.c kernel/src/network_event.c \
		kernel/src/network_health.c kernel/src/authorization.c \
		tests/network-partition-drill-host.c \
		-o build/test-network-partition-drill-host -no-pie
	build/test-network-partition-drill-host

test-trust-failure-drill-host: kernel/include/audit.h \
		kernel/include/boot_evidence.h \
		kernel/include/config_kv.h \
		kernel/include/event_ring.h \
		kernel/include/fault_injection.h \
		kernel/include/image_integrity.h \
		kernel/include/image_policy.h \
		kernel/include/key_use.h \
		kernel/include/recovery_trust.h \
		kernel/include/sha256.h \
		kernel/include/signed_metadata.h \
		kernel/include/system_image.h \
		kernel/include/trust_anchor.h \
		kernel/include/trust_revocation.h \
		kernel/include/verification_policy.h \
		kernel/src/audit.c kernel/src/boot_evidence.c \
		kernel/src/config_kv.c kernel/src/event_ring.c \
		kernel/src/fault_injection.c kernel/src/image_integrity.c \
		kernel/src/image_policy.c kernel/src/key_use.c \
		kernel/src/recovery_trust.c kernel/src/sha256.c \
		kernel/src/signed_metadata.c kernel/src/system_image.c \
		kernel/src/trust_anchor.c kernel/src/trust_revocation.c \
		kernel/src/verification_policy.c \
		tests/trust-failure-drill-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_TRUST_FAILURE_DRILL_HOST_TEST \
		-Ikernel/include \
		kernel/src/audit.c kernel/src/boot_evidence.c \
		kernel/src/config_kv.c kernel/src/event_ring.c \
		kernel/src/fault_injection.c kernel/src/image_integrity.c \
		kernel/src/image_policy.c kernel/src/key_use.c \
		kernel/src/recovery_trust.c kernel/src/sha256.c \
		kernel/src/signed_metadata.c kernel/src/system_image.c \
		kernel/src/trust_anchor.c kernel/src/trust_revocation.c \
		kernel/src/verification_policy.c \
		tests/trust-failure-drill-host.c \
		-o build/test-trust-failure-drill-host -no-pie
	build/test-trust-failure-drill-host

test-update-failure-drill-host: kernel/include/activation_intent.h \
		kernel/include/automatic_rollback.h \
		kernel/include/candidate_preflight.h \
		kernel/include/candidate_slot_inventory.h \
		kernel/include/candidate_staged_write.h \
		kernel/include/fault_injection.h \
		kernel/include/first_boot_health.h \
		kernel/include/sha256.h \
		kernel/include/signed_metadata.h \
		kernel/include/update_compatibility.h \
		kernel/src/activation_intent.c \
		kernel/src/automatic_rollback.c \
		kernel/src/candidate_preflight.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/candidate_staged_write.c \
		kernel/src/fault_injection.c \
		kernel/src/first_boot_health.c \
		kernel/src/sha256.c \
		kernel/src/update_compatibility.c \
		tests/update-failure-drill-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_UPDATE_FAILURE_DRILL_HOST_TEST \
		-Ikernel/include \
		kernel/src/activation_intent.c \
		kernel/src/automatic_rollback.c \
		kernel/src/candidate_preflight.c \
		kernel/src/candidate_slot_inventory.c \
		kernel/src/candidate_staged_write.c \
		kernel/src/fault_injection.c \
		kernel/src/first_boot_health.c \
		kernel/src/sha256.c \
		kernel/src/update_compatibility.c \
		tests/update-failure-drill-host.c \
		-o build/test-update-failure-drill-host -no-pie
	build/test-update-failure-drill-host

test-process-scheduler-host: kernel/include/process.h \
		kernel/include/scheduler.h kernel/include/descriptor.h \
		kernel/include/capability.h kernel/include/context.h \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c kernel/src/scheduler.c \
		kernel/src/arch/x86_64/sched_trampoline.S \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/process-scheduler-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PROCESS_HOST_TEST -DHFOC_PROCESS_SCHEDULER_HOST_TEST \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c \
		kernel/src/scheduler.c \
		kernel/src/arch/x86_64/sched_trampoline.S \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/process-scheduler-host.c \
		-o build/test-process-scheduler-host -no-pie
	build/test-process-scheduler-host

test-process-limit-host: kernel/include/process.h \
		kernel/include/scheduler.h kernel/include/descriptor.h \
		kernel/include/capability.h kernel/include/context.h \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c kernel/src/scheduler.c \
		kernel/src/arch/x86_64/sched_trampoline.S \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/process-limit-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PROCESS_HOST_TEST -DHFOC_PROCESS_SCHEDULER_HOST_TEST \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Ikernel/include \
		kernel/src/process.c kernel/src/capability.c \
		kernel/src/descriptor.c \
		kernel/src/scheduler.c \
		kernel/src/arch/x86_64/sched_trampoline.S \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/process-limit-host.c \
		-o build/test-process-limit-host -no-pie
	build/test-process-limit-host

test-scheduler-yield: $(SCHED_YIELD_ISO) iso
	@echo "Scheduler yield marker checks: before QEMU proof"
	@strings $(SCHED_YIELD_KERNEL) | grep -Fq '[SCHED] thread-A step=' || { echo "FAIL: scheduler yield kernel missing thread-A marker" >&2; exit 1; }
	@strings $(SCHED_YIELD_ISO) | grep -Fq '[SCHED] thread-A step=' || { echo "FAIL: scheduler yield ISO missing thread-A marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SCHED] thread-A step='; then echo "FAIL: production kernel contains scheduler yield marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SCHED] thread-A step='; then echo "FAIL: production ISO contains scheduler yield marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SCHED_YIELD_ISO)' \
		./scripts/test-qemu-scheduler-yield.sh
	@echo "Scheduler yield marker checks: after QEMU proof"
	@strings $(SCHED_YIELD_KERNEL) | grep -Fq '[SCHED] thread-A step=' || { echo "FAIL: scheduler yield kernel missing thread-A marker after QEMU proof" >&2; exit 1; }
	@strings $(SCHED_YIELD_ISO) | grep -Fq '[SCHED] thread-A step=' || { echo "FAIL: scheduler yield ISO missing thread-A marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SCHED] thread-A step='; then echo "FAIL: production kernel contains scheduler yield marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SCHED] thread-A step='; then echo "FAIL: production ISO contains scheduler yield marker after QEMU proof" >&2; exit 1; fi

test-scheduler-tick: $(SCHED_TICK_ISO) iso
	@echo "Scheduler tick marker checks: before QEMU proof"
	@strings $(SCHED_TICK_KERNEL) | grep -Fq '[SCHED-TICK] thread-A step=' || { echo "FAIL: scheduler tick kernel missing thread-A marker" >&2; exit 1; }
	@strings $(SCHED_TICK_ISO) | grep -Fq '[SCHED-TICK] thread-A step=' || { echo "FAIL: scheduler tick ISO missing thread-A marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SCHED-TICK] thread-A step='; then echo "FAIL: production kernel contains scheduler tick marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SCHED-TICK] thread-A step='; then echo "FAIL: production ISO contains scheduler tick marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SCHED_TICK_ISO)' \
		./scripts/test-qemu-scheduler-tick.sh
	@echo "Scheduler tick marker checks: after QEMU proof"
	@strings $(SCHED_TICK_KERNEL) | grep -Fq '[SCHED-TICK] thread-A step=' || { echo "FAIL: scheduler tick kernel missing thread-A marker after QEMU proof" >&2; exit 1; }
	@strings $(SCHED_TICK_ISO) | grep -Fq '[SCHED-TICK] thread-A step=' || { echo "FAIL: scheduler tick ISO missing thread-A marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[SCHED-TICK] thread-A step='; then echo "FAIL: production kernel contains scheduler tick marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[SCHED-TICK] thread-A step='; then echo "FAIL: production ISO contains scheduler tick marker after QEMU proof" >&2; exit 1; fi

test-scheduler-wait: $(SCHED_WAIT_ISO) iso
	@echo "Scheduler wait marker checks: before QEMU proof"
	@strings $(SCHED_WAIT_KERNEL) | grep -Fq '[WAIT] sleep start' || { echo "FAIL: scheduler wait kernel missing sleep start marker" >&2; exit 1; }
	@strings $(SCHED_WAIT_ISO) | grep -Fq '[WAIT] sleep start' || { echo "FAIL: scheduler wait ISO missing sleep start marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[WAIT] sleep start'; then echo "FAIL: production kernel contains scheduler wait marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[WAIT] sleep start'; then echo "FAIL: production ISO contains scheduler wait marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SCHED_WAIT_ISO)' \
		./scripts/test-qemu-scheduler-wait.sh
	@echo "Scheduler wait marker checks: after QEMU proof"
	@strings $(SCHED_WAIT_KERNEL) | grep -Fq '[WAIT] sleep done' || { echo "FAIL: scheduler wait kernel missing sleep done marker after QEMU proof" >&2; exit 1; }
	@strings $(SCHED_WAIT_ISO) | grep -Fq '[WAIT] sleep done' || { echo "FAIL: scheduler wait ISO missing sleep done marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[WAIT] sleep start'; then echo "FAIL: production kernel contains scheduler wait marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[WAIT] sleep start'; then echo "FAIL: production ISO contains scheduler wait marker after QEMU proof" >&2; exit 1; fi

test-scheduler-exit: $(SCHED_EXIT_ISO) iso
	@echo "Scheduler exit marker checks: before QEMU proof"
	@strings $(SCHED_EXIT_KERNEL) | grep -Fq '[EXIT] thread-A before exit' || { echo "FAIL: scheduler exit kernel missing before-exit marker" >&2; exit 1; }
	@strings $(SCHED_EXIT_ISO) | grep -Fq '[EXIT] thread-A before exit' || { echo "FAIL: scheduler exit ISO missing before-exit marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[EXIT] thread-A before exit'; then echo "FAIL: production kernel contains scheduler exit marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[EXIT] thread-A before exit'; then echo "FAIL: production ISO contains scheduler exit marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SCHED_EXIT_ISO)' \
		./scripts/test-qemu-scheduler-exit.sh
	@echo "Scheduler exit marker checks: after QEMU proof"
	@strings $(SCHED_EXIT_KERNEL) | grep -Fq '[EXIT] thread-A before exit' || { echo "FAIL: scheduler exit kernel missing before-exit marker after QEMU proof" >&2; exit 1; }
	@strings $(SCHED_EXIT_ISO) | grep -Fq '[EXIT] thread-A before exit' || { echo "FAIL: scheduler exit ISO missing before-exit marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[EXIT] thread-A before exit'; then echo "FAIL: production kernel contains scheduler exit marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[EXIT] thread-A before exit'; then echo "FAIL: production ISO contains scheduler exit marker after QEMU proof" >&2; exit 1; fi

test-scheduler-stress: $(SCHED_STRESS_ISO) iso
	@echo "Scheduler stress marker checks: before QEMU proof"
	@strings $(SCHED_STRESS_KERNEL) | grep -Fq '[STRESS] driver start' || { echo "FAIL: scheduler stress kernel missing [STRESS] driver start marker" >&2; exit 1; }
	@strings $(SCHED_STRESS_ISO) | grep -Fq '[STRESS] driver start' || { echo "FAIL: scheduler stress ISO missing [STRESS] driver start marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[STRESS] driver start'; then echo "FAIL: production kernel contains scheduler stress marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[STRESS] driver start'; then echo "FAIL: production ISO contains scheduler stress marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(SCHED_STRESS_ISO)' \
		./scripts/test-qemu-scheduler-stress.sh
	@echo "Scheduler stress marker checks: after QEMU proof"
	@strings $(SCHED_STRESS_KERNEL) | grep -Fq '[STRESS] driver start' || { echo "FAIL: scheduler stress kernel missing [STRESS] driver start marker after QEMU proof" >&2; exit 1; }
	@strings $(SCHED_STRESS_ISO) | grep -Fq '[STRESS] driver start' || { echo "FAIL: scheduler stress ISO missing [STRESS] driver start marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[STRESS] driver start'; then echo "FAIL: production kernel contains scheduler stress marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[STRESS] driver start'; then echo "FAIL: production ISO contains scheduler stress marker after QEMU proof" >&2; exit 1; fi

test-process-scheduler: $(PROCESS_SCHED_ISO) iso
	@echo "Process scheduler marker checks: before QEMU proof"
	@strings $(PROCESS_SCHED_KERNEL) | grep -Fq '[PROC] parent reaped all' || { echo "FAIL: process scheduler kernel missing parent reaped marker" >&2; exit 1; }
	@strings $(PROCESS_SCHED_ISO) | grep -Fq '[PROC] parent reaped all' || { echo "FAIL: process scheduler ISO missing parent reaped marker" >&2; exit 1; }
	@strings $(PROCESS_SCHED_KERNEL) | grep -Fq '[PROC] limited child start' || { echo "FAIL: process scheduler kernel missing limited child marker" >&2; exit 1; }
	@strings $(PROCESS_SCHED_ISO) | grep -Fq '[PROC] limited child start' || { echo "FAIL: process scheduler ISO missing limited child marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PROC] parent reaped all'; then echo "FAIL: production kernel contains process scheduler marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PROC] parent reaped all'; then echo "FAIL: production ISO contains process scheduler marker" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[PROC] limited child start'; then echo "FAIL: production kernel contains process limit marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PROC] limited child start'; then echo "FAIL: production ISO contains process limit marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(PROCESS_SCHED_ISO)' \
		./scripts/test-qemu-process-scheduler.sh
	@echo "Process scheduler marker checks: after QEMU proof"
	@strings $(PROCESS_SCHED_KERNEL) | grep -Fq '[PROC] parent reaped all' || { echo "FAIL: process scheduler kernel missing parent reaped marker after QEMU proof" >&2; exit 1; }
	@strings $(PROCESS_SCHED_ISO) | grep -Fq '[PROC] parent reaped all' || { echo "FAIL: process scheduler ISO missing parent reaped marker after QEMU proof" >&2; exit 1; }
	@strings $(PROCESS_SCHED_KERNEL) | grep -Fq '[PROC] limited child start' || { echo "FAIL: process scheduler kernel missing limited child marker after QEMU proof" >&2; exit 1; }
	@strings $(PROCESS_SCHED_ISO) | grep -Fq '[PROC] limited child start' || { echo "FAIL: process scheduler ISO missing limited child marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[PROC] parent reaped all'; then echo "FAIL: production kernel contains process scheduler marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PROC] parent reaped all'; then echo "FAIL: production ISO contains process scheduler marker after QEMU proof" >&2; exit 1; fi
	@if strings $(KERNEL) | grep -Fq '[PROC] limited child start'; then echo "FAIL: production kernel contains process limit marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[PROC] limited child start'; then echo "FAIL: production ISO contains process limit marker after QEMU proof" >&2; exit 1; fi

test-smp-host: kernel/include/smp.h kernel/src/smp.c \
		kernel/include/cpu.h kernel/src/cpu.c tests/smp-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_SMP_HOST_TEST -DHFOC_CPU_HOST_TEST \
		-Ikernel/include \
		kernel/src/cpu.c kernel/src/smp.c tests/smp-host.c \
		-o build/test-smp-host -no-pie
	build/test-smp-host

test-smp: $(LAPIC_RUNTIME_ISO) iso
	@strings $(LAPIC_RUNTIME_KERNEL) | grep -Fq '[LAPIC-TIMER] tick policy: lapic-transition' || { echo "FAIL: LAPIC runtime kernel missing transition policy marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[LAPIC-TIMER] tick policy: lapic-transition'; then echo "FAIL: production kernel contains the LAPIC transition policy marker" >&2; exit 1; fi
	@strings $(KERNEL) | grep -Fq '[LAPIC-TIMER] tick policy: pit' || { echo "FAIL: production kernel missing PIT tick policy marker" >&2; exit 1; }
	QEMU_MACHINE='q35' QEMU_SMP='2' QEMU_CPU='max' IMAGE='$(LAPIC_RUNTIME_ISO)' \
		./scripts/test-qemu-smp.sh

test-lapic-host: kernel/include/cpu.h kernel/src/cpu.c \
		kernel/include/lapic.h kernel/src/lapic.c tests/lapic-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LAPIC_HOST_TEST -DHFOC_CPU_HOST_TEST \
		-Ikernel/include \
		-Itests \
		kernel/src/cpu.c kernel/src/lapic.c tests/lapic-host.c \
		-o build/test-lapic-host -no-pie
	build/test-lapic-host

test-lapic-timer-host: kernel/include/lapic_timer.h kernel/src/lapic_timer.c tests/lapic-timer-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LAPIC_TIMER_HOST_TEST \
		-Ikernel/include \
		kernel/src/lapic_timer.c tests/lapic-timer-host.c \
		-o build/test-lapic-timer-host -no-pie
	build/test-lapic-timer-host

test-lapic-timer-runtime-host: kernel/include/arch/x86_64/interrupts.h \
		kernel/include/cpu.h kernel/src/cpu.c \
		kernel/src/arch/x86_64/interrupts.c \
		kernel/include/lapic_timer.h kernel/include/ioapic.h \
		kernel/include/arch/x86_64/pic.h \
		kernel/include/scheduler.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/lapic-timer-runtime-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LAPIC_TIMER_RUNTIME_HOST_TEST -DHFOC_CPU_HOST_TEST \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Itests/include -Ikernel/include \
		kernel/src/cpu.c kernel/src/arch/x86_64/interrupts.c \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/lapic-timer-runtime-host.c \
		-Wl,--wrap=sched_on_tick \
		-o build/test-lapic-timer-runtime-host -no-pie
	build/test-lapic-timer-runtime-host

test-lapic-timer-oneshot-runtime-host: kernel/include/arch/x86_64/interrupts.h \
		kernel/include/cpu.h kernel/src/cpu.c \
		kernel/src/arch/x86_64/interrupts.c \
		kernel/include/lapic_timer.h kernel/include/ioapic.h \
		kernel/include/arch/x86_64/pic.h \
		kernel/include/scheduler.h kernel/src/scheduler.c \
		kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/lapic-timer-runtime-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LAPIC_TIMER_RUNTIME_HOST_TEST \
		-DHFOC_LAPIC_TIMER_ONESHOT_TEST -DHFOC_CPU_HOST_TEST \
		-DHFOC_SCHEDULER_HOST_TEST \
		-Itests/include -Ikernel/include \
		kernel/src/cpu.c kernel/src/arch/x86_64/interrupts.c \
		kernel/src/scheduler.c kernel/src/arch/x86_64/context.c \
		kernel/src/arch/x86_64/context_switch.S \
		tests/lapic-timer-runtime-host.c \
		-Wl,--wrap=sched_on_tick \
		-o build/test-lapic-timer-oneshot-runtime-host -no-pie
	build/test-lapic-timer-oneshot-runtime-host

test-lapic: $(LAPIC_RUNTIME_ISO) iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(LAPIC_RUNTIME_ISO)' \
		./scripts/test-qemu-lapic.sh

test-ioapic: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-ioapic-probe.sh

test-ioapic-host: kernel/include/ioapic.h kernel/include/lapic.h kernel/src/ioapic.c tests/ioapic-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_IOAPIC_HOST_TEST \
		-Ikernel/include \
		-Itests \
		kernel/src/ioapic.c tests/ioapic-host.c \
		-o build/test-ioapic-host -no-pie
	build/test-ioapic-host

test-isa-irq3-7-host: kernel/include/isa_irq3_7.h kernel/include/ioapic.h \
		kernel/include/arch/x86_64/pic.h kernel/src/isa_irq3_7.c \
		tests/isa-irq3-7-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ISA_IRQ3_7_HOST_TEST \
		-Ikernel/include \
		kernel/src/isa_irq3_7.c tests/isa-irq3-7-host.c \
		-o build/test-isa-irq3-7-host -no-pie
	build/test-isa-irq3-7-host

test-isa-irq8-15-host: kernel/include/isa_irq8_15.h kernel/include/ioapic.h \
		kernel/include/arch/x86_64/pic.h kernel/src/isa_irq8_15.c \
		tests/isa-irq8-15-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_ISA_IRQ8_15_HOST_TEST \
		-Ikernel/include \
		kernel/src/isa_irq8_15.c tests/isa-irq8-15-host.c \
		-o build/test-isa-irq8-15-host -no-pie
	build/test-isa-irq8-15-host

test-keyboard-host: kernel/include/keyboard.h kernel/src/keyboard.c tests/keyboard-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_KEYBOARD_HOST_TEST \
		-Itests/include -Ikernel/include \
		kernel/src/keyboard.c tests/keyboard-host.c \
		-o build/test-keyboard-host -no-pie
	build/test-keyboard-host

# ITEM-392: a machine with no i8042 must boot and publish why local keyboard
# input is unavailable, so the initialisation failures are classified and each
# one is proven survivable here, next to the QEMU proof that boots the
# production ISO with the controller absent.
test-keyboard-init-host: kernel/include/keyboard.h kernel/src/keyboard.c \
		tests/keyboard-init-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_KEYBOARD_INIT_HOST_TEST \
		-Itests/include -Ikernel/include \
		kernel/src/keyboard.c tests/keyboard-init-host.c \
		-o build/test-keyboard-init-host -no-pie
	build/test-keyboard-init-host

# ITEM-395: a machine with no PS/2 controller gets its local console input from
# a USB HID boot-protocol keyboard behind an xHCI controller. The QEMU proof
# boots the production ISO with the controller removed and the keyboard on an
# emulated xHCI bus and types a command through it; these proofs drive the
# transport-free halves on the host, where a wrong bit position or a confused
# context index would otherwise only ever show up as a controller error:
#
#   test-usb-hid-host      the configuration descriptor walk (which interface
#                          and endpoint a boot keyboard is) and the 8-byte boot
#                          report decode, including the console's whole command
#                          alphabet, Shift, Caps Lock and Ctrl, key repeat and
#                          the bounded event buffer, and - ITEM-406 - the
#                          acceptance host's own K120 descriptors (two
#                          interfaces, low speed, 8-byte endpoint) with the
#                          negative shapes: a receiver with no boot keyboard
#                          interface and a mouse-only device;
#   test-xhci-host         the TRB, ring, event and device context encoding the
#                          controller accepts, with the two halves of the
#                          context addressing rule pinned separately, and -
#                          ITEM-406 - the low-speed endpoint context (the
#                          descriptor's 8-byte packet, the interval the speed
#                          implies), the writable subset of PORTSC (PED is never
#                          written back, which is what disabled the port after a
#                          reset on real hardware) and the controller/port scan
#                          policy, driven over the acceptance host's machine
#                          shape: a controller whose only device is not a boot
#                          keyboard must not end the scan, and the keyboard on a
#                          later controller must be reached;
#   test-local-input-host  the rule that keeps the two keyboards from fighting:
#                          PS/2 first, a USB event deferred by one pass and
#                          never dropped, and the published source.
.PHONY: test-usb-hid-host
test-usb-hid-host: kernel/include/usb_hid.h kernel/include/keyboard.h \
		kernel/src/usb_hid.c tests/usb-hid-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_USB_HID_HOST_TEST \
		-Ikernel/include \
		kernel/src/usb_hid.c tests/usb-hid-host.c \
		-o build/test-usb-hid-host -no-pie
	build/test-usb-hid-host

# ITEM-406: the scan policy is declared in xhci.h and implemented in
# xhci_trb.c, and its host proof drives the descriptor walk the driver feeds it,
# so this proof links the HID decoder as well.
.PHONY: test-diag-host
test-diag-host: kernel/include/diag_export.h \
		kernel/include/diag_failure.h kernel/src/diag_session.c \
		kernel/src/diag_failure.c tests/diag-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/diag_session.c kernel/src/diag_failure.c \
		tests/diag-host.c \
		-o build/test-diag-host -no-pie
	build/test-diag-host

# ITEM-590: `diag save` on a journal that is FULL.
#
# The installed Toshiba's diagnostic stick bound its reserved FAT12 file, the
# telemetry filled all 512 slots it declares, and the export refused the medium
# with NO_MEDIUM before it reached the rewind that exists for exactly this case.
# This target links the real seam, the real journal core, the real preallocated
# FAT12 writer and the real `diag_export_save()`, fills the journal with the
# loop's per-second records, saves twice, refuses a failed write and a failed
# flush by name, and hands the finished image to the operator's own Debian
# extractor.  The build identity is compiled in so the extracted BEGIN names
# this proof and not a stale object.
.PHONY: test-diag-export-full-host
test-diag-export-full-host: kernel/include/diag_export.h \
		kernel/include/diag_failure.h \
		kernel/include/kernel_login_trace.h \
		kernel/include/appliance_panel_auth_trace.h \
		kernel/include/install_journal_platform.h \
		kernel/include/install_journal_core.h \
		kernel/include/install_fat12_journal.h \
		kernel/include/block_device.h \
		kernel/src/diag_export.c kernel/src/diag_session.c \
		kernel/src/diag_failure.c kernel/src/kernel_login_trace.c \
		kernel/src/appliance_panel_auth_trace.c \
		kernel/src/install_journal_platform.c \
		kernel/src/install_journal_core.c \
		kernel/src/install_fat12_journal.c \
		kernel/src/install_medium.c kernel/src/block_device.c \
		kernel/src/hfoc_build.c tests/diag-export-full-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_BUILD_ID='"host-590"' \
		-DHFOC_VERSION_STRING='"1.0.0"' \
		-Ikernel/include \
		kernel/src/diag_export.c kernel/src/diag_session.c \
		kernel/src/diag_failure.c kernel/src/kernel_login_trace.c \
		kernel/src/appliance_panel_auth_trace.c \
		kernel/src/install_journal_platform.c \
		kernel/src/install_journal_core.c \
		kernel/src/install_fat12_journal.c \
		kernel/src/install_medium.c kernel/src/block_device.c \
		kernel/src/hfoc_build.c tests/diag-export-full-host.c \
		-o build/test-diag-export-full-host -no-pie
	build/test-diag-export-full-host

.PHONY: test-ssh-hostkey-export-host
test-ssh-hostkey-export-host: scripts/test-ssh-hostkey-export-host.sh \
		scripts/hfoc-ssh-hostkey.py scripts/hfoc-diag-read.py
	bash scripts/test-ssh-hostkey-export-host.sh

.PHONY: test-ehci-walk-host
test-ehci-walk-host: kernel/include/ehci.h kernel/include/ehci_walk.h \
		kernel/src/ehci_walk.c kernel/src/ehci_queue.c \
		tests/ehci-walk-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_walk.c kernel/src/ehci_queue.c \
		tests/ehci-walk-host.c \
		-o build/test-ehci-walk-host -no-pie
	build/test-ehci-walk-host

.PHONY: test-ehci-regs-host
test-ehci-regs-host: kernel/include/ehci.h kernel/include/ehci_regs.h \
		kernel/include/ehci_engine.h kernel/src/ehci_regs.c \
		kernel/src/ehci_engine.c kernel/src/ehci_queue.c \
		tests/ehci-regs-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_regs.c kernel/src/ehci_engine.c \
		kernel/src/ehci_queue.c \
		tests/ehci-regs-host.c \
		-o build/test-ehci-regs-host -no-pie
	build/test-ehci-regs-host

.PHONY: test-ehci-port-host
test-ehci-port-host: kernel/include/ehci.h kernel/include/ehci_port.h \
		kernel/include/ehci_walk.h kernel/src/ehci_port.c \
		kernel/src/ehci_walk.c kernel/src/ehci_queue.c \
		tests/ehci-port-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_port.c kernel/src/ehci_walk.c \
		kernel/src/ehci_queue.c \
		tests/ehci-port-host.c \
		-o build/test-ehci-port-host -no-pie
	build/test-ehci-port-host

# ITEM-567: the root port reset sequence itself.  The proof drives the
# production module through the production MMIO accessor over a fake BAR with a
# non-zero CAPLENGTH, with a controller whose PR never self-clears, whose
# completion (PE) appears after the clear and optionally several polls later,
# and which may latch PR through the clear.  It asserts the words that crossed
# the wire, the 50 ms hold, the explicit clear and what was awaited, so the
# hardware run's `detail=PR` failure is reproduced and fixed as behaviour.
# ITEM-568: the BIOS -> OS handoff, in PCI configuration space.  The proof
# drives the production sequence over a fake configuration space with a decoy
# capability planted in the MMIO window at EECP*4, so a walk that reads the
# wrong address space (or a dword-scaled pointer) fails.
# ITEM-568: every register mask against EHCI 1.0, including the decode of the
# words the Toshiba printed (0x00080010 and 0x00007010).
# ITEM-569: the 64-bit DMA format.  The proof drives the production builders
# into a pool image and decodes the bytes by EHCI 1.0 Appendix B's offsets, then
# proves the pool's regions disjoint and the frame list out of a data write's
# reach.
.PHONY: test-ehci-dma64-host
test-ehci-dma64-host: kernel/include/ehci.h kernel/src/ehci_queue.c \
		tests/ehci-dma64-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_queue.c tests/ehci-dma64-host.c \
		-o build/test-ehci-dma64-host -no-pie
	build/test-ehci-dma64-host

.PHONY: test-ehci-masks-host
test-ehci-masks-host: kernel/include/ehci.h kernel/include/ehci_legacy.h \
		tests/ehci-masks-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		tests/ehci-masks-host.c \
		-o build/test-ehci-masks-host -no-pie
	build/test-ehci-masks-host

.PHONY: test-ehci-legacy-host
test-ehci-legacy-host: kernel/include/ehci_legacy.h \
		kernel/src/ehci_legacy.c tests/ehci-legacy-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_legacy.c tests/ehci-legacy-host.c \
		-o build/test-ehci-legacy-host -no-pie
	build/test-ehci-legacy-host

.PHONY: test-ehci-port-reset-host
test-ehci-port-reset-host: kernel/include/ehci.h kernel/include/ehci_port.h \
		kernel/include/ehci_regs.h kernel/src/ehci_port.c \
		kernel/src/ehci_regs.c kernel/src/ehci_queue.c \
		tests/ehci-port-reset-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_port.c kernel/src/ehci_regs.c \
		kernel/src/ehci_queue.c \
		tests/ehci-port-reset-host.c \
		-o build/test-ehci-port-reset-host -no-pie
	build/test-ehci-port-reset-host

.PHONY: test-ehci-verdict-host
test-ehci-verdict-host: kernel/include/ehci.h kernel/include/ehci_walk.h \
		kernel/include/ehci_verdict.h kernel/src/ehci_verdict.c \
		kernel/src/ehci_walk.c kernel/src/ehci_queue.c \
		tests/ehci-verdict-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_verdict.c kernel/src/ehci_walk.c \
		kernel/src/ehci_queue.c \
		tests/ehci-verdict-host.c \
		-o build/test-ehci-verdict-host -no-pie
	build/test-ehci-verdict-host

.PHONY: test-ehci-init-host
.PHONY: test-ehci-ring-host
.PHONY: test-ehci-engine-host
.PHONY: test-ehci-host
test-ehci-init-host: kernel/include/ehci.h kernel/include/ehci_init.h \
		kernel/src/ehci_queue.c kernel/src/ehci_init.c \
		tests/ehci-init-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_queue.c kernel/src/ehci_init.c \
		tests/ehci-init-host.c \
		-o build/test-ehci-init-host -no-pie
	build/test-ehci-init-host

test-ehci-ring-host: kernel/include/ehci.h kernel/src/ehci_queue.c \
		tests/ehci-ring-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_queue.c tests/ehci-ring-host.c \
		-o build/test-ehci-ring-host -no-pie
	build/test-ehci-ring-host

test-ehci-engine-host: kernel/include/ehci.h \
		kernel/include/ehci_engine.h kernel/src/ehci_queue.c \
		kernel/src/ehci_engine.c tests/ehci-engine-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_queue.c kernel/src/ehci_engine.c \
		tests/ehci-engine-host.c \
		-o build/test-ehci-engine-host -no-pie
	build/test-ehci-engine-host

test-ehci-host: kernel/include/ehci.h kernel/src/ehci_queue.c \
		tests/ehci-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/ehci_queue.c tests/ehci-host.c \
		-o build/test-ehci-host -no-pie
	build/test-ehci-host

# ITEM-573: the durable per-controller xHCI evidence.  The proof keeps a
# controller, destroys the source the way xhci_session_release() does, and
# renders the records the exported report carries - ports, protocols, roles
# and the pointer verdict - for one controller, a re-visit, two controllers and
# every pointer outcome.
.PHONY: test-xhci-evidence-host
test-xhci-evidence-host: kernel/include/xhci_evidence.h \
		kernel/src/xhci_evidence.c tests/xhci-evidence-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/xhci_evidence.c tests/xhci-evidence-host.c \
		-o build/test-xhci-evidence-host -no-pie
	build/test-xhci-evidence-host

.PHONY: test-xhci-host
test-xhci-host: kernel/include/xhci_trb.h kernel/include/xhci.h \
		kernel/include/usb_hid.h kernel/include/keyboard.h \
		kernel/include/xhci_evidence.h kernel/include/xhci_route.h \
		kernel/src/xhci_trb.c kernel/src/usb_hid.c tests/xhci-host.c \
		kernel/src/xhci.c kernel/src/xhci_evidence.c \
		kernel/src/xhci_route.c tests/xhci-mmio-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_XHCI_HOST_TEST \
		-Ikernel/include \
		kernel/src/xhci_trb.c kernel/src/usb_hid.c tests/xhci-host.c \
		-o build/test-xhci-host -no-pie
	build/test-xhci-host
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-ffunction-sections -fdata-sections -Ikernel/include \
		tests/xhci-mmio-host.c kernel/src/xhci_trb.c kernel/src/usb_hid.c \
		kernel/src/xhci_evidence.c kernel/src/xhci_route.c \
		-Wl,--gc-sections -no-pie -o build/test-xhci-mmio-host
	build/test-xhci-mmio-host

# ITEM-574: the Intel xHCI USB port-routing decision.  The proof substitutes the
# four configuration-space registers and pins the cases that must never write
# hardware blindly: another vendor, another Intel chipset, no EHCI companion,
# two companions, an unreadable register, an impossible mask, the apply order,
# preserved foreign and EHCI bits, a refused write and a lying read-back.
.PHONY: test-xhci-route-host
test-xhci-route-host: kernel/include/xhci_route.h \
		kernel/src/xhci_route.c tests/xhci-route-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/xhci_route.c tests/xhci-route-host.c \
		-o build/test-xhci-route-host -no-pie
	build/test-xhci-route-host

# ITEM-577: the DMA geometry.  The proof drives the production start and
# address-device paths against a register model and a fake physical pool, then
# reads the pool's bytes at the controller's own addresses: scratchpads per
# HCSPARAMS2, 32-record slot areas for 32- and 64-byte contexts, slot 1's EP0
# surviving slot 2, the pool bound, and HSE stopping commands while the first
# fatal snapshot survives later captures.
.PHONY: test-xhci-dma-host
test-xhci-dma-host: kernel/include/xhci_route.h \
		kernel/include/xhci_evidence.h kernel/include/xhci_trb.h \
		kernel/include/xhci.h kernel/include/usb_hid.h \
		kernel/src/xhci.c kernel/src/xhci_trb.c kernel/src/usb_hid.c \
		kernel/src/xhci_evidence.c kernel/src/xhci_route.c \
		tests/xhci-dma-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie -ffunction-sections -fdata-sections \
		-DHFOC_XHCI_MMIO_HOST_TEST -Ikernel/include \
		tests/xhci-dma-host.c kernel/src/xhci_trb.c kernel/src/usb_hid.c \
		kernel/src/xhci_evidence.c kernel/src/xhci_route.c \
		-Wl,--gc-sections -o build/test-xhci-dma-host -no-pie
	build/test-xhci-dma-host

# ITEM-578: the report-descriptor handover and the second role's address.  The
# proof drives the production parser, attach and full port-walk paths against a
# controller and device model: the descriptor is written where the Data Stage
# TRB points, a failed read is not a classification verdict, the walk resets
# the port before the second role addresses the device, and the SET_PROTOCOL
# fallback parses the descriptor as the role it is binding.
.PHONY: test-xhci-handover-host
test-xhci-handover-host: kernel/include/xhci_route.h \
		kernel/include/xhci_evidence.h kernel/include/xhci_trb.h \
		kernel/include/xhci.h kernel/include/usb_hid.h \
		kernel/src/xhci.c kernel/src/xhci_trb.c kernel/src/usb_hid.c \
		kernel/src/xhci_evidence.c kernel/src/xhci_route.c \
		tests/xhci-handover-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie -ffunction-sections -fdata-sections \
		-DHFOC_XHCI_MMIO_HOST_TEST -Ikernel/include \
		tests/xhci-handover-host.c kernel/src/xhci_trb.c kernel/src/usb_hid.c \
		kernel/src/xhci_evidence.c kernel/src/xhci_route.c \
		-Wl,--gc-sections -o build/test-xhci-handover-host -no-pie
	build/test-xhci-handover-host

# ITEM-579: the composite device and the retry that keeps the pointer alive.
# The proof drives the whole production initialization path - the port walk,
# the silent keyboard candidate, the pointer's binding and the final
# silent-candidate retry - plus the service pass that decodes reports, and
# pins the shared path, the endpoint-repair commands and the wheel.
.PHONY: test-xhci-composite-host
test-xhci-composite-host: kernel/include/xhci_route.h \
		kernel/include/xhci_evidence.h kernel/include/xhci_trb.h \
		kernel/include/xhci.h kernel/include/usb_hid.h \
		kernel/src/xhci.c kernel/src/xhci_trb.c kernel/src/usb_hid.c \
		kernel/src/xhci_evidence.c kernel/src/xhci_route.c \
		tests/xhci-composite-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic -Werror \
		-ffreestanding -fno-pic -fno-pie -ffunction-sections -fdata-sections \
		-DHFOC_XHCI_MMIO_HOST_TEST -Ikernel/include \
		tests/xhci-composite-host.c kernel/src/xhci_trb.c kernel/src/usb_hid.c \
		kernel/src/xhci_evidence.c kernel/src/xhci_route.c \
		-Wl,--gc-sections -o build/test-xhci-composite-host -no-pie
	build/test-xhci-composite-host

# ITEM-519 increment 1: the BOT/SCSI core of USB mass storage.  The proof drives
# the CBW/CSW sequence, the SCSI command set, every wire failure with its reset
# recovery, and the block_device adapter over a scripted device, so no
# controller and no QEMU machine is needed for this half.
.PHONY: test-usb-storage-host
test-usb-storage-host: kernel/include/usb_storage.h \
		kernel/include/block_device.h \
		kernel/src/usb_storage.c kernel/src/block_device.c \
		tests/usb-storage-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_USB_STORAGE_HOST_TEST \
		-Ikernel/include \
		kernel/src/usb_storage.c kernel/src/block_device.c \
		tests/usb-storage-host.c \
		-o build/test-usb-storage-host -no-pie
	build/test-usb-storage-host

# ITEM-519 increment 5: the boot-medium guard's pure rule.  The kernel reads
# one sector and asks whether it is the medium the loader booted from; every
# accept and refuse shape is driven here on synthetic sectors.
.PHONY: test-install-medium-host
test-install-medium-host: kernel/include/install_medium.h \
		kernel/src/install_medium.c tests/install-medium-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INSTALL_MEDIUM_HOST_TEST \
		-Ikernel/include \
		kernel/src/install_medium.c tests/install-medium-host.c \
		-o build/test-install-medium-host -no-pie
	build/test-install-medium-host

# ITEM-521 increment 1: the placement-independent core of the install journal.
# The owner's medium must carry the install run and every refusal; this proof
# drives the pre-bind staging ring, the named losses and the two flush paths
# over a RAM backend, so no placement (ITEM-520) has to be chosen for this half.
.PHONY: test-install-journal-core-host
test-install-journal-core-host: kernel/include/install_journal_core.h \
		kernel/src/install_journal_core.c \
		tests/install-journal-core-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/install_journal_core.c \
		tests/install-journal-core-host.c \
		-o build/test-install-journal-core-host -no-pie
	build/test-install-journal-core-host

# ITEM-521 increment 8, completed by increment 12: the platform seam and the
# naming of a proven medium that got no placement.  The real platform module is
# linked beside the real core and the placement the owner chose - the FAT12
# file - with the two kernel services it reports through (serial, the durable
# boot-log store) stubbed, so requirement 3's "the write about it goes into the
# ordinary records" is checked as text and as a route, the RAM medium proves
# the bind writes only inside its own FAT12 partition, and a refused volume
# proves the named loss costs no write.
.PHONY: test-install-journal-platform-host
test-install-journal-platform-host: kernel/include/install_journal_platform.h \
		kernel/include/install_journal_core.h \
		kernel/include/install_fat12_journal.h \
		kernel/include/block_device.h \
		kernel/src/install_journal_platform.c \
		kernel/src/install_journal_core.c \
		kernel/src/install_fat12_journal.c \
		kernel/src/install_medium.c \
		kernel/src/block_device.c \
		tests/install-journal-platform-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/install_journal_platform.c \
		kernel/src/install_journal_core.c \
		kernel/src/install_fat12_journal.c \
		kernel/src/install_medium.c \
		kernel/src/block_device.c \
		tests/install-journal-platform-host.c \
		-o build/test-install-journal-platform-host -no-pie
	build/test-install-journal-platform-host

# ITEM-521 increment 12, the owner's answer (c): the reserved FAT12 file the
# build writes and the kernel only reads.  This target runs the witness that
# pins the no-allocation bind, the in-reservation commit point and the named
# refusals (231 checks) inside the tree.
.PHONY: test-install-fat12-journal-host
test-install-fat12-journal-host: kernel/include/install_fat12_journal.h \
		kernel/include/install_journal_core.h \
		kernel/include/block_device.h \
		kernel/src/install_fat12_journal.c \
		tests/install-fat12-journal-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/install_fat12_journal.c \
		kernel/src/install_journal_core.c \
		kernel/src/block_device.c \
		tests/install-fat12-journal-host.c \
		-o build/test-install-fat12-journal-host -no-pie
	build/test-install-fat12-journal-host

# ITEM-521 increment 12, the owner's answer (c): the seam body that binds the
# file the medium's build reserved, linked with the real core and the real
# module; only serial and the durable store are stubbed.
.PHONY: test-install-journal-platform-fat12-host
test-install-journal-platform-fat12-host: kernel/include/install_journal_platform.h \
		kernel/include/install_journal_core.h \
		kernel/include/install_fat12_journal.h \
		kernel/src/install_journal_platform.c \
		kernel/src/install_journal_core.c \
		kernel/src/install_fat12_journal.c \
		kernel/src/install_medium.c \
		tests/install-journal-platform-fat12-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/install_journal_platform.c \
		kernel/src/install_journal_core.c \
		kernel/src/install_fat12_journal.c \
		kernel/src/install_medium.c \
		kernel/src/block_device.c \
		tests/install-journal-platform-fat12-host.c \
		-o build/test-install-journal-platform-fat12-host -no-pie
	build/test-install-journal-platform-fat12-host

# ITEM-521 requirement 4: the journal the medium carries is read back with one
# command.  The reader autodetects the placement (FAT12 file, preallocated file
# or raw extent), so it does not choose the still-open ITEM-520 fork; the host
# proof builds every form from the documented contract and drives the reader as
# an operator would.
.PHONY: test-install-log-read-host
test-install-log-read-host: scripts/hfoc-install-log-read.py \
		tests/install-log-reader-host.py
	python3 tests/install-log-reader-host.py

# ITEM-521 increment 12, the owner's answer (c) to the ITEM-520 fork: the
# installer medium's build reserves \HFOC\INSTALL.LOG in the FAT12 ESP so the
# kernel only reads the chain the build wrote.  The witness drives the
# preallocator as a subprocess over synthetic FAT12 media and checks the
# chain, both FAT copies, the slot-0 header, idempotence and every refusal.
.PHONY: test-install-media-prealloc-host
test-install-media-prealloc-host: scripts/hfoc-install-media-prealloc.py \
		tests/install-media-prealloc-host.py
	python3 tests/install-media-prealloc-host.py

# ITEM-521 increment 10: the machine facts the install journal must carry.
# Requirement 1 names the machine - its cores and its memory - next to the
# medium and every refusal; the queue-side dry run of the prepared placement
# read the kernel's journal off the emulated medium and found that half absent.
# The renderer is pure, so the exact record, both counter boundaries and the
# too-small-buffer rule are host facts before any medium is touched.
.PHONY: test-machine-facts-host
test-machine-facts-host: kernel/include/machine_facts.h \
		kernel/src/machine_facts.c \
		tests/machine-facts-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/machine_facts.c \
		tests/machine-facts-host.c \
		-o build/test-machine-facts-host -no-pie
	build/test-machine-facts-host

# ITEM-521 increment 11: the panic names itself in the durable journal.
# Requirement 1 names the panic among the records the medium's journal must
# carry; the path flushed what was staged before the machine stopped but never
# staged the panic itself.  The two renderers are pure, so the exact records,
# the clamp that marks an over-long message, the too-small-buffer rule and the
# fit inside the shared record bound are host facts before any medium is
# touched.
.PHONY: test-panic-record-host
test-panic-record-host: kernel/include/panic_record.h \
		kernel/src/panic_record.c \
		tests/panic-record-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/panic_record.c \
		tests/panic-record-host.c \
		-o build/test-panic-record-host -no-pie
	build/test-panic-record-host

# CI-SPEEDUP-3: the expected-refusal console call answers the console once and
# ends on the refusal line it is looking for.  The measurement in PR #761 put
# 121 s of the main HTTPS proof into that call's wait for a session a retired
# password can never be issued.  The witness extracts the shipped helper from
# the proof - it cannot drift from the code under test - and drives the shipped
# `refuse` mode against a fake QEMU monitor: a fresh refusal passes, and a
# refusal that predates the attempt, an issued session, a refusal for another
# reason, a silent console and a hung-up monitor all fail.
.PHONY: test-https-refusal-answer-host
test-https-refusal-answer-host: scripts/test-qemu-production-service-https.sh \
		tests/https-refusal-answer-host.py
	python3 tests/https-refusal-answer-host.py

# ITEM-525: the credential-durability boot's gate assertions read the console
# stream the proof's embedded python already normalises.  Branch run 38264136090
# lost that proof to an interrupt-context diagnostic printed inside the
# `[PRODUCTION-SECURITY] gate=ENABLED` record: python stripped the diagnostic and
# saw the record, the shell greps read the raw file and reported it missing.
# The witness extracts the shipped normaliser, the shipped helper and the
# shipped block and runs them against the recorded interleaving, with a
# raw-file negative control that must still fail.
.PHONY: test-https-gate-serial-view-host
test-https-gate-serial-view-host: scripts/test-qemu-production-service-https.sh \
		tests/https-gate-serial-view-host.py
	python3 tests/https-gate-serial-view-host.py

# ITEM-526: the same class ITEM-525 fixed in one boot - a shell assertion
# grepping the raw serial file while an interrupt-context diagnostic can land
# inside a record the console prints one character at a time - stayed in the
# https proof's other nine guest boots, in its own first boot (whose serial is
# `-serial "file:$SERIAL_LOG"`) and in the five proofs that had no normaliser.
# The witness pins the shared renderer, each proof's serial_view helper, the
# absence of raw serial assertions (only >&2 diagnostics may print raw) and the
# behavioural recovery of the record from the bytes run 38264136090 recorded.
.PHONY: test-serial-view-normalised-host
test-serial-view-normalised-host: scripts/hfoc-serial-view.py \
		scripts/test-qemu-production-service-https.sh \
		scripts/test-qemu-production-sni-selection.sh \
		scripts/test-qemu-acme-order.sh \
		scripts/test-qemu-acme-boot-order.sh \
		scripts/test-qemu-acme-http01.sh \
		scripts/test-qemu-acme-directory.sh \
		scripts/test-qemu-platform-power.sh \
		scripts/test-qemu-irqs.sh \
		scripts/test-qemu-production-first-boot.sh \
		scripts/test-qemu-production-forward.sh \
		scripts/test-qemu-production-recovery-reset.sh \
		scripts/test-qemu-production-storage-boot.sh \
		scripts/test-qemu-usb-mouse-menu.sh \
		scripts/test-qemu-usb-two-controllers.sh \
		scripts/test-qemu-installed-boot.sh \
		tests/serial-view-normalised-host.py
	python3 tests/serial-view-normalised-host.py

.PHONY: test-local-input-host
test-local-input-host: kernel/include/local_input.h kernel/include/xhci.h \
		kernel/include/xhci_trb.h kernel/include/usb_hid.h \
		kernel/include/keyboard.h kernel/src/local_input.c \
		tests/local-input-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LOCAL_INPUT_HOST_TEST \
		-Ikernel/include \
		kernel/src/local_input.c tests/local-input-host.c \
		-o build/test-local-input-host -no-pie
	build/test-local-input-host

# ITEM-448: where the pointer is, what a movement does to it, and which menu
# rectangle a click lands in - the arithmetic between the driver's decoded
# report and the console's painted menu.
.PHONY: test-pointer-host
test-pointer-host: kernel/include/pointer.h kernel/src/pointer.c \
		tests/pointer-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_POINTER_HOST_TEST \
		-Ikernel/include \
		kernel/src/pointer.c tests/pointer-host.c \
		-o build/test-pointer-host -no-pie
	build/test-pointer-host

# ITEM-447: the network the machine's own menu sets.
.PHONY: test-local-network-setup-host
test-local-network-setup-host: kernel/include/local_network_setup.h \
		kernel/include/ipv4_config.h \
		kernel/src/local_network_setup.c \
		kernel/src/ipv4_config.c \
		tests/local-network-setup-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LOCAL_NETWORK_SETUP_HOST_TEST \
		-Ikernel/include \
		kernel/src/local_network_setup.c \
		kernel/src/ipv4_config.c \
		tests/local-network-setup-host.c \
		-o build/test-local-network-setup-host -no-pie
	build/test-local-network-setup-host

test-local-menu-host: kernel/include/local_menu.h \
		kernel/include/appliance_language.h \
		kernel/include/keyboard.h kernel/src/local_menu.c \
		kernel/src/appliance_language.c \
		tests/local-menu-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LOCAL_MENU_HOST_TEST \
		-Ikernel/include \
		kernel/src/local_menu.c kernel/src/appliance_language.c \
		tests/local-menu-host.c \
		-o build/test-local-menu-host -no-pie
	build/test-local-menu-host

# ITEM-428: the main loop's heartbeat cadence, proven without the machine.
.PHONY: test-loop-heartbeat-host
test-loop-heartbeat-host: kernel/include/appliance_loop_heartbeat.h \
		kernel/src/appliance_loop_heartbeat.c \
		tests/loop-heartbeat-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_loop_heartbeat.c \
		tests/loop-heartbeat-host.c \
		-o build/test-loop-heartbeat-host -no-pie
	build/test-loop-heartbeat-host

# ITEM-479: the loop turn's phase attribution, proven without the machine.
.PHONY: test-loop-budget-host
test-loop-budget-host: kernel/include/loop_budget.h \
		kernel/src/loop_budget.c tests/loop-budget-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/loop_budget.c tests/loop-budget-host.c \
		-o build/test-loop-budget-host -no-pie
	build/test-loop-budget-host

# ITEM-486 (round 24): the mouse event path's named outcome.  input_path.c is
# linked into the drill, so the classifier the kernel publishes is the one the
# host cases exercise, and every outcome the contract names is pinned.
# ITEM-488: the display abstraction and the installer UI that draws through it.
# Both are pure modules, so the proof covers the cases the hardware cannot be
# asked for on demand: no framebuffer, a malformed one, an unsupported pixel
# format, and the text path that has to carry the same flow.
.PHONY: test-display-host
test-display-host: kernel/include/display.h \
		kernel/src/display.c tests/display-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/display.c tests/display-host.c \
		-o build/test-display-host -no-pie
	build/test-display-host

# ITEM-489: the storage screen's model.  Pure code, so the rules that keep an
# operator in charge - nothing chosen automatically, an unreadable disk never
# selectable, no continuation without a choice - are proven on the host.
.PHONY: test-installer-storage-host
test-installer-storage-host: kernel/include/installer_hardware_discovery.h \
		kernel/include/installer_scheme.h \
		kernel/include/installer_storage.h \
		kernel/include/installer_storage_summary.h \
		kernel/include/installer_layout_contract.h \
		kernel/include/installer_ui.h \
		kernel/include/appliance_language.h \
		kernel/include/appliance_storage_report.h \
		kernel/src/installer_storage.c \
		kernel/src/installer_storage_summary.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_storage_report.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-storage-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/installer_storage.c \
		kernel/src/installer_storage_summary.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_storage_report.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-storage-host.c \
		-o build/test-installer-storage-host -no-pie
	build/test-installer-storage-host

# ITEM-509 increment 19: the install banner's two sentences read the one
# catalog, so the English text the QEMU-proof banner prints is pinned byte for
# byte, the Russian column is proven and an unknown language is the named
# refusal.  The banner itself is not driven here: it counts real seconds down
# through the reset delay, which is the QEMU proof's job.
.PHONY: test-installer-notice-host
test-installer-notice-host: kernel/include/installer_notice.h \
		kernel/include/appliance_language.h \
		kernel/src/installer_notice.c \
		kernel/src/appliance_language.c \
		tests/installer-notice-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/installer_notice.c \
		kernel/src/appliance_language.c \
		tests/installer-notice-host.c \
		-o build/test-installer-notice-host -no-pie
	build/test-installer-notice-host

.PHONY: test-installer-ui-host
test-installer-ui-host: kernel/include/display.h \
		kernel/include/installer_ui.h \
		kernel/include/appliance_language.h \
		kernel/src/display.c \
		kernel/src/appliance_language.c \
		kernel/src/installer_ui.c tests/installer-ui-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/display.c kernel/src/appliance_language.c \
		kernel/src/installer_ui.c \
		tests/installer-ui-host.c \
		-o build/test-installer-ui-host -no-pie
	build/test-installer-ui-host

.PHONY: test-input-path-host
test-input-path-host: kernel/include/input_path.h \
		kernel/src/input_path.c tests/input-path-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/input_path.c tests/input-path-host.c \
		-o build/test-input-path-host -no-pie
	build/test-input-path-host

# ITEM-444: the screen draws lowercase letters as lowercase.  framebuffer.c is
# linked into the drill and pointed at a host surface, so the cell read back is
# what framebuffer_char() painted, not a copy of the table.
.PHONY: test-framebuffer-glyph-host
test-framebuffer-glyph-host: kernel/include/framebuffer.h \
		kernel/include/pointer.h \
		kernel/include/limine_min.h \
		kernel/include/appliance_language.h \
		kernel/src/framebuffer.c \
		kernel/src/appliance_language.c \
		kernel/src/pointer.c \
		tests/framebuffer-glyph-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/framebuffer.c \
		kernel/src/appliance_language.c \
		kernel/src/pointer.c \
		tests/framebuffer-glyph-host.c \
		-o build/test-framebuffer-glyph-host -no-pie
	build/test-framebuffer-glyph-host

# ITEM-457: the console's scroll moves whole rows.  framebuffer.c is linked
# into the drill and pointed at a host surface, so the pixels read back after a
# scroll are the ones the real move left there: the surviving rows carry the
# row below them, the rows that came in carry the fill colour, row padding is
# untouched, and both row alignments a 32-bpp surface can have are covered.
.PHONY: test-framebuffer-scroll-host
test-framebuffer-scroll-host: kernel/include/framebuffer.h \
		kernel/include/pointer.h \
		kernel/include/limine_min.h \
		kernel/include/appliance_language.h \
		kernel/src/framebuffer.c \
		kernel/src/appliance_language.c \
		kernel/src/pointer.c \
		tests/framebuffer-scroll-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/framebuffer.c \
		kernel/src/appliance_language.c \
		kernel/src/pointer.c \
		tests/framebuffer-scroll-host.c \
		-o build/test-framebuffer-scroll-host -no-pie
	build/test-framebuffer-scroll-host

# ITEM-445: the browser sign-in form and the one session cookie.  The kernel
# module is linked into the drill, so the bytes parsed and produced here are the
# bytes the panel parses and produces on the machine.
.PHONY: test-panel-login-host
test-panel-login-host: kernel/include/appliance_panel_login.h \
		kernel/include/appliance_admin_auth.h \
		kernel/include/appliance_language.h \
		kernel/src/appliance_panel_login.c \
		kernel/src/appliance_language.c \
		tests/panel-login-form-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_panel_login.c \
		kernel/src/appliance_language.c \
		tests/panel-login-form-host.c \
		-o build/test-panel-login-host -no-pie
	build/test-panel-login-host

# ITEM-449: the forward declaration and its access rule, proven without the
# machine.  The declaration is the whole access rule - a malformed one opens
# nothing, and a source that is not a declared client is denied by name.
.PHONY: test-forward-config-host
test-forward-config-host: kernel/include/forward_config.h \
		kernel/src/forward_config.c \
		tests/forward-config-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/forward_config.c \
		tests/forward-config-host.c \
		-o build/test-forward-config-host -no-pie
	build/test-forward-config-host

# ITEM-504: the facade route table as a pure value.  The name grammar, the
# backend rule, the duplicate rule, the route limit and the fail-closed lookup
# are decided in one place, so the declaration, the panel and the serving path
# cannot disagree about what a route means.
.PHONY: test-appliance-route-table-host
test-appliance-route-table-host: kernel/include/appliance_route_table.h \
		kernel/src/appliance_route_table.c \
		tests/appliance-route-table-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_route_table.c \
		tests/appliance-route-table-host.c \
		-o build/test-appliance-route-table-host -no-pie
	build/test-appliance-route-table-host

# ITEM-504 part B: the candidate pair behind the ROUTING_CONFIG mutation.  A
# stage applies one validated add or remove to a copy of the applied table;
# the commit replaces the applied table whole, and a refusal, cancel or expiry
# leaves it exactly as it was.
.PHONY: test-appliance-route-config-host
test-appliance-route-config-host: kernel/include/appliance_route_config.h \
		kernel/include/appliance_route_table.h \
		kernel/src/appliance_route_config.c \
		kernel/src/appliance_route_table.c \
		tests/appliance-route-config-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_route_config.c \
		kernel/src/appliance_route_table.c \
		tests/appliance-route-config-host.c \
		-o build/test-appliance-route-config-host -no-pie
	build/test-appliance-route-config-host

# ITEM-504 increment 6: the route table as durable state.  A saved table loads
# back whole, a blank record is an empty table, a corrupt record of any kind is
# refused as the empty table rather than partially applied, a write the device
# does not keep fails the save, and a stored table enters the composition only
# through the model's own validate().
.PHONY: test-appliance-route-store-host
test-appliance-route-store-host: kernel/include/appliance_route_store.h \
		kernel/include/appliance_route_config.h \
		kernel/include/appliance_route_table.h \
		kernel/include/appliance_state_block_device.h \
		kernel/include/appliance_state_layout.h \
		kernel/src/appliance_route_store.c \
		kernel/src/appliance_route_config.c \
		kernel/src/appliance_route_table.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/sha256.c \
		tests/appliance-route-store-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_route_store.c \
		kernel/src/appliance_route_config.c \
		kernel/src/appliance_route_table.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/block_device.c \
		kernel/src/data_region.c \
		kernel/src/sha256.c \
		tests/appliance-route-store-host.c \
		-o build/test-appliance-route-store-host -no-pie
	build/test-appliance-route-store-host

.PHONY: test-appliance-certificate-name-store-host
test-appliance-certificate-name-store-host: \
		kernel/include/appliance_certificate_name_store.h \
		kernel/include/appliance_route_table.h \
		kernel/include/appliance_state_block_device.h \
		kernel/include/appliance_state_layout.h \
		kernel/src/appliance_certificate_name_store.c \
		kernel/src/appliance_route_table.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-certificate-name-store-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_certificate_name_store.c \
		kernel/src/appliance_route_table.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-certificate-name-store-host.c \
		-o build/test-appliance-certificate-name-store-host -no-pie
	build/test-appliance-certificate-name-store-host

# ITEM-508 increment 34: the durable ACME control record next to the ITEM-503
# certificate record and the ITEM-507 name table in the CERTIFICATE store.
.PHONY: test-appliance-acme-control-host
test-appliance-acme-control-host: \
		kernel/include/appliance_acme_control.h \
		kernel/include/appliance_certificate_name_store.h \
		kernel/include/appliance_production_certificate.h \
		kernel/include/appliance_state_block_device.h \
		kernel/include/appliance_state_layout.h \
		kernel/src/appliance_acme_control.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-acme-control-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/appliance_acme_control.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		tests/appliance-acme-control-host.c \
		-o build/test-appliance-acme-control-host -no-pie
	build/test-appliance-acme-control-host

test-keyboard-line-editing: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-keyboard-line-editing.sh

# ITEM-466 (round 5): the console chooses no mode of its own.  The same image
# is proven at three EDID preferences - 1024x768, 1920x1080 and the owner's
# 3840x2160 - and each run must boot the firmware's own preferred mode with the
# automatic font scale the grid minimum gives (2, 3 and 3), the cell and the
# grid following that scale, and the load phase bounding the paint and loop
# costs in microseconds either way.
test-console-history: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		HFOC_CONSOLE_HISTORY_DISPLAY='VGA,edid=on,xres=1024,yres=768' \
		HFOC_CONSOLE_HISTORY_EXPECT_DISPLAY='1024x768' \
		HFOC_CONSOLE_HISTORY_EXPECT_FALLBACK='0' \
		HFOC_CONSOLE_HISTORY_EXPECT_SCALE='1' \
		./scripts/test-qemu-console-history.sh
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		HFOC_CONSOLE_HISTORY_DISPLAY='VGA,edid=on,xres=1920,yres=1080' \
		HFOC_CONSOLE_HISTORY_EXPECT_DISPLAY='1920x1080' \
		HFOC_CONSOLE_HISTORY_EXPECT_FALLBACK='0' \
		HFOC_CONSOLE_HISTORY_EXPECT_SCALE='2' \
		./scripts/test-qemu-console-history.sh
	# The 4K EDID is the owner's hardware preference.  QEMU's VGA device has
	# too little video memory for a 33 MB 3840x2160 surface (ITEM-463's own
	# self-review recorded that), so this run boots with the 4K EDID but lets
	# the actual mode be whatever the firmware could provide; the generic
	# checks still require the fallback flag and the automatic scale to follow
	# that actual mode, and the pure 3840x2160 -> 3 rule is pinned by the host
	# proof (tests/framebuffer-scroll-host.c).
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		HFOC_CONSOLE_HISTORY_DISPLAY='VGA,edid=on,xres=3840,yres=2160' \
		./scripts/test-qemu-console-history.sh

.PHONY: test-qemu-console-scrollback
test-qemu-console-scrollback: iso
	IMAGE='$(IMAGE)' ./scripts/test-qemu-console-scrollback.sh

.PHONY: test-appliance-console-qemu
test-appliance-console-qemu: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-appliance-console.sh


# ITEM-417: the installer tells the operator, on the screen, that it finished
# and is about to reboot.  The flow runs before the console exists, so the proof
# reads the guest's own display through the monitor instead of the serial
# stream.
.PHONY: test-install-notice-qemu
test-install-notice-qemu: iso production-installer-whole-disk-media
	QEMU_MACHINE='q35' QEMU_SMP='1' \
		IMAGE='build/production-installer/hfoc-os-$(VERSION)-production-installer-whole-disk.iso' \
		./scripts/test-qemu-install-notice.sh

.PHONY: test-appliance-boot-console-qemu
test-appliance-boot-console-qemu: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-appliance-boot-console.sh

.PHONY: test-local-menu-qemu
test-local-menu-qemu: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-local-menu.sh

# ITEM-448: the mouse next to the keyboard on one xHCI controller, and a click
# that runs the menu entry under the pointer through the keyboard's own path.
.PHONY: test-usb-mouse-menu-qemu
test-usb-mouse-menu-qemu: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-usb-mouse-menu.sh

# ITEM-452: two xHCI controllers at fixed PCI addresses, the keyboard on one
# and the pointer on the other - the owner's machine shape - plus the negative
# where the second controller carries a device that is not HID at all and must
# still publish its own vid/pid and reason.
.PHONY: test-usb-two-controllers-qemu
test-usb-two-controllers-qemu: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-usb-two-controllers.sh

# ITEM-551: the boot medium on the machine's EHCI function (the black USB 2
# port on the owner's laptop), bound through the USB 2.0 mass-storage path and
# proven by reading the journal the boot itself wrote back out of the image.
.PHONY: test-diag-failure-qemu
# ITEM-557: the refusal line the operator reads, on the shipped console medium
# booted as a disc (no journal medium, exactly the Toshiba's state).
test-diag-failure-qemu: production-installer-console-media
	IMAGE='$(PRODUCTION_INSTALLER_CONSOLE_ISO)' \
	EXPECTED_BUILD_ID='$(BUILD_ID)' \
	EXPECTED_KERNEL='$(PRODUCTION_INSTALLER_KERNEL)' \
	./scripts/test-qemu-diag-failure.sh

.PHONY: test-diag-export-qemu
test-diag-export-qemu: diag-export-qemu-media
	IMAGE='$(DIAG_EXPORT_ISO)' \
	EXPECTED_BUILD_ID='$(BUILD_ID)' \
	EXPECTED_VERSION='$(VERSION)' \
	EXPECTED_KERNEL='$(DIAG_EXPORT_KERNEL)' \
	./scripts/test-qemu-diag-export.sh

.PHONY: test-ehci-storage-qemu
test-ehci-storage-qemu: production-installer-console-media
	./scripts/test-qemu-ehci-storage.sh

# ITEM-418: the console commands that reboot and power off the machine.  The
# proof types them into the boot prompt through the keyboard and requires the
# module's records *and* QEMU leaving on its own, which is what a platform that
# honours the request does.
.PHONY: test-platform-power-qemu
test-platform-power-qemu: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-platform-power.sh

.PHONY: test-integrated-appliance-boot-acceptance-qemu
test-integrated-appliance-boot-acceptance-qemu: iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IMAGE)' \
		./scripts/test-qemu-integrated-appliance-boot-acceptance.sh

test-console-host: kernel/include/console.h kernel/include/keyboard.h \
		kernel/include/console_credentials_command.h \
		kernel/include/console_gate_command.h \
		kernel/include/console_language_command.h \
		kernel/include/appliance_admin_credentials_prompt.h \
		kernel/include/appliance_admin_credential.h \
		kernel/include/sha256.h \
		kernel/src/appliance_admin_credentials_prompt.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/sha256.c \
		kernel/include/framebuffer.h \
		kernel/include/appliance_console.h \
		kernel/include/appliance_boot_console.h \
		kernel/include/local_network_setup.h \
		kernel/src/local_network_setup.c \
		kernel/src/ipv4_config.c \
		kernel/include/appliance_language.h \
		kernel/include/appliance_config_validation.h \
		kernel/include/appliance_panel_gate.h \
		kernel/include/appliance_identity.h \
		kernel/include/appliance_storage_report.h \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_panel_gate.c \
		kernel/src/appliance_identity.c \
		kernel/src/appliance_console.c \
		kernel/src/appliance_boot_console.c \
		kernel/src/appliance_storage_report.c \
		kernel/src/hfoc_build.c \
		kernel/src/console.c tests/console-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_CONSOLE_HOST_TEST \
		-Ikernel/include \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_panel_gate.c \
		kernel/src/appliance_identity.c \
		kernel/src/appliance_console.c \
		kernel/src/appliance_boot_console.c \
		kernel/src/appliance_admin_credentials_prompt.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/sha256.c \
		kernel/src/local_menu.c \
		kernel/src/local_network_setup.c \
		kernel/src/ipv4_config.c \
		kernel/src/pointer.c \
		kernel/src/appliance_storage_report.c \
		kernel/src/hfoc_build.c \
		kernel/src/console.c tests/console-host.c \
		-o build/test-console-host -no-pie
	build/test-console-host

test-pic-host: kernel/include/arch/x86_64/pic.h kernel/src/arch/x86_64/pic.c tests/pic-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PIC_HOST_TEST \
		-Ikernel/include \
		kernel/src/arch/x86_64/pic.c tests/pic-host.c \
		-o build/test-pic-host -no-pie
	build/test-pic-host

test-ioapic-irq1: $(LAPIC_RUNTIME_ISO) iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(LAPIC_RUNTIME_ISO)' \
		./scripts/test-qemu-ioapic-irq1.sh

test-ioapic-irq0: $(LAPIC_RUNTIME_ISO) iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(LAPIC_RUNTIME_ISO)' \
		./scripts/test-qemu-ioapic-irq0.sh

test-ioapic-irq3-7: $(IRQ3_7_ISO) iso
	@echo "IRQ3-7 marker checks: before QEMU proof"
	@strings $(IRQ3_7_KERNEL) | grep -Fq '[UART] irq=3 vector=0x44' || { echo "FAIL: IRQ3-7 kernel missing UART hook marker" >&2; exit 1; }
	@strings $(IRQ3_7_ISO) | grep -Fq '[UART] irq=3 vector=0x44' || { echo "FAIL: IRQ3-7 ISO missing UART hook marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[UART] irq=3 vector=0x44'; then echo "FAIL: production kernel contains UART hook marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[UART] irq=3 vector=0x44'; then echo "FAIL: production ISO contains UART hook marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IRQ3_7_ISO)' \
		./scripts/test-qemu-ioapic-irq3-7.sh
	@echo "IRQ3-7 marker checks: after QEMU proof"
	@strings $(IRQ3_7_KERNEL) | grep -Fq '[UART] irq=3 vector=0x44' || { echo "FAIL: IRQ3-7 kernel missing UART hook marker after QEMU proof" >&2; exit 1; }
	@strings $(IRQ3_7_ISO) | grep -Fq '[UART] irq=3 vector=0x44' || { echo "FAIL: IRQ3-7 ISO missing UART hook marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[UART] irq=3 vector=0x44'; then echo "FAIL: production kernel contains UART hook marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[UART] irq=3 vector=0x44'; then echo "FAIL: production ISO contains UART hook marker after QEMU proof" >&2; exit 1; fi

test-ioapic-irq8-15: $(IRQ8_15_ISO) iso
	@echo "IRQ8-15 marker checks: before QEMU proof"
	@strings $(IRQ8_15_KERNEL) | grep -Fq '[UART] irq=8 vector=0x49' || { echo "FAIL: IRQ8-15 kernel missing UART hook marker" >&2; exit 1; }
	@strings $(IRQ8_15_ISO) | grep -Fq '[UART] irq=8 vector=0x49' || { echo "FAIL: IRQ8-15 ISO missing UART hook marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[UART] irq=8 vector=0x49'; then echo "FAIL: production kernel contains UART hook marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[UART] irq=8 vector=0x49'; then echo "FAIL: production ISO contains UART hook marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(IRQ8_15_ISO)' \
		./scripts/test-qemu-ioapic-irq8-15.sh
	@echo "IRQ8-15 marker checks: after QEMU proof"
	@strings $(IRQ8_15_KERNEL) | grep -Fq '[UART] irq=8 vector=0x49' || { echo "FAIL: IRQ8-15 kernel missing UART hook marker after QEMU proof" >&2; exit 1; }
	@strings $(IRQ8_15_ISO) | grep -Fq '[UART] irq=8 vector=0x49' || { echo "FAIL: IRQ8-15 ISO missing UART hook marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[UART] irq=8 vector=0x49'; then echo "FAIL: production kernel contains UART hook marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[UART] irq=8 vector=0x49'; then echo "FAIL: production ISO contains UART hook marker after QEMU proof" >&2; exit 1; fi

test-pic-irq0-masking: $(LAPIC_RUNTIME_ISO) iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(LAPIC_RUNTIME_ISO)' \
		./scripts/test-qemu-pic-irq0-masking.sh

test-lapic-timer-calibration: $(LAPIC_RUNTIME_ISO) iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(LAPIC_RUNTIME_ISO)' \
		./scripts/test-qemu-lapic-timer-calibration.sh

test-lapic-timer-runtime: $(LAPIC_RUNTIME_ISO) iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(LAPIC_RUNTIME_ISO)' \
		./scripts/test-qemu-lapic-timer-runtime.sh

test-pic-disable: $(LAPIC_RUNTIME_ISO) iso
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(LAPIC_RUNTIME_ISO)' \
		./scripts/test-qemu-pic-disable.sh

test-x2apic: $(X2APIC_ISO) iso
	@echo "X2APIC marker checks: before QEMU proof"
	@strings $(X2APIC_KERNEL) | grep -Fq '[X2APIC] transition failed:' || { echo "FAIL: x2APIC kernel missing test hook marker" >&2; exit 1; }
	@strings $(X2APIC_ISO) | grep -Fq '[X2APIC] transition failed:' || { echo "FAIL: x2APIC ISO missing test hook marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[X2APIC] transition failed:'; then echo "FAIL: production kernel contains x2APIC test hook marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[X2APIC] transition failed:'; then echo "FAIL: production ISO contains x2APIC test hook marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' QEMU_CPU='max' IMAGE='$(X2APIC_ISO)' \
		./scripts/test-qemu-x2apic.sh
	@echo "X2APIC marker checks: after QEMU proof"
	@strings $(X2APIC_KERNEL) | grep -Fq '[X2APIC] transition failed:' || { echo "FAIL: x2APIC kernel missing test hook marker after QEMU proof" >&2; exit 1; }
	@strings $(X2APIC_ISO) | grep -Fq '[X2APIC] transition failed:' || { echo "FAIL: x2APIC ISO missing test hook marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[X2APIC] transition failed:'; then echo "FAIL: production kernel contains x2APIC test hook marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[X2APIC] transition failed:'; then echo "FAIL: production ISO contains x2APIC test hook marker after QEMU proof" >&2; exit 1; fi

test-lapic-timer-oneshot: $(ONESHOT_ISO) iso
	@echo "One-shot marker checks: before QEMU proof"
	@strings $(ONESHOT_KERNEL) | grep -Fq '[LAPIC-TIMER] oneshot armed: vector=0x43' || { echo "FAIL: one-shot kernel missing hook marker" >&2; exit 1; }
	@strings $(ONESHOT_ISO) | grep -Fq '[LAPIC-TIMER] oneshot armed: vector=0x43' || { echo "FAIL: one-shot ISO missing hook marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[LAPIC-TIMER] oneshot armed: vector=0x43'; then echo "FAIL: production kernel contains hook marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[LAPIC-TIMER] oneshot armed: vector=0x43'; then echo "FAIL: production ISO contains hook marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(ONESHOT_ISO)' \
		./scripts/test-qemu-lapic-timer-oneshot.sh
	@echo "One-shot marker checks: after QEMU proof"
	@strings $(ONESHOT_KERNEL) | grep -Fq '[LAPIC-TIMER] oneshot armed: vector=0x43' || { echo "FAIL: one-shot kernel missing hook marker after QEMU proof" >&2; exit 1; }
	@strings $(ONESHOT_ISO) | grep -Fq '[LAPIC-TIMER] oneshot armed: vector=0x43' || { echo "FAIL: one-shot ISO missing hook marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[LAPIC-TIMER] oneshot armed: vector=0x43'; then echo "FAIL: production kernel contains hook marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[LAPIC-TIMER] oneshot armed: vector=0x43'; then echo "FAIL: production ISO contains hook marker after QEMU proof" >&2; exit 1; fi

test-pic-irq0-masking-fallback: $(FALLBACK_ISO) iso
	@echo "Fallback marker checks: before QEMU proof"
	@strings $(FALLBACK_KERNEL) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure' || { echo "FAIL: fallback kernel missing route failure marker" >&2; exit 1; }
	@strings $(FALLBACK_ISO) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure' || { echo "FAIL: fallback ISO missing route failure marker" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure'; then echo "FAIL: production kernel contains route failure marker" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure'; then echo "FAIL: production ISO contains route failure marker" >&2; exit 1; fi
	QEMU_MACHINE='q35' QEMU_SMP='1' IMAGE='$(FALLBACK_ISO)' \
		./scripts/test-qemu-pic-irq0-masking-fallback.sh
	@echo "Fallback marker checks: after QEMU proof"
	@strings $(FALLBACK_KERNEL) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure' || { echo "FAIL: fallback kernel missing route failure marker after QEMU proof" >&2; exit 1; }
	@strings $(FALLBACK_ISO) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure' || { echo "FAIL: fallback ISO missing route failure marker after QEMU proof" >&2; exit 1; }
	@if strings $(KERNEL) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure'; then echo "FAIL: production kernel contains route failure marker after QEMU proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure'; then echo "FAIL: production ISO contains route failure marker after QEMU proof" >&2; exit 1; fi
	$(MAKE) test-pic-irq0-masking
	@echo "Production contamination recheck: marker absent"
	@if strings $(KERNEL) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure'; then echo "FAIL: production kernel contains route failure marker after success proof" >&2; exit 1; fi
	@if strings $(IMAGE) | grep -Fq '[IOAPIC] test: forcing IRQ0 route failure'; then echo "FAIL: production ISO contains route failure marker after success proof" >&2; exit 1; fi

QEMU_TEST_IMAGES := \
	$(FALLBACK_ISO) \
	$(ONESHOT_ISO) \
	$(LAPIC_RUNTIME_ISO) \
	$(IRQ3_7_ISO) \
	$(IRQ8_15_ISO) \
	$(X2APIC_ISO) \
	$(VMM_AS_ISO) \
	$(SCHED_YIELD_ISO) \
	$(SCHED_TICK_ISO) \
	$(SCHED_WAIT_ISO) \
	$(SCHED_EXIT_ISO) \
	$(SCHED_STRESS_ISO) \
	$(PROCESS_SCHED_ISO) \
	$(USER_IRETQ_ISO) \
	$(SYSCALL_ISO) \
	$(USER_INIT_ISO) \
	$(BOOT_POLICY_ISO) \
	$(BLOCK_DEVICE_ISO) \
	$(DATA_REGION_ISO) \
	$(BLOCK_CACHE_ISO) \
	$(CONFIG_JOURNAL_ISO) \
	$(PHYSICAL_STORAGE_ISO) \
	$(PHYSICAL_SATA_ACCEPTANCE_ISO) \
	$(PRODUCTION_INSTALLER_ISO) \
	$(BOOT_CHAIN_ISO) \
	$(PERSISTENT_CONFIG_ISO) \
	$(NETWORK_CONFIG_ISO) \
	$(PERSISTENT_NETWORK_CONFIG_ISO) \
	$(BACKUP_RESTORE_ISO) \
	$(SUPERVISOR_COMPOSITION_ISO) \
	$(LOCAL_CONTROL_ISO) \
	$(NETWORK_FOUNDATION_ISO) \
	$(VIRTIO_FOUNDATION_ISO) \
	$(VIRTIO_NET_FRAME_ISO) \
	$(PACKET_BOUNDARY_ISO) \
	$(ARP_ICMP_ISO) \
	$(TCP_LISTENER_ISO) \
	$(NETWORK_RESOURCE_ISO) \
	$(TRUST_POLICY_ISO) \
	$(UPDATE_LIFECYCLE_ISO) \
	$(SUPPORTABILITY_ISO) \
	$(ACCOUNTING_BOUNDARY_ISO) \
	$(STORAGE_INTERRUPTION_ISO) \
	$(SERVICE_CASCADE_DRILL_ISO) \
	$(LOCAL_CONTROL_OUTAGE_DRILL_ISO) \
	$(NETWORK_PARTITION_DRILL_ISO) \
	$(TRUST_FAILURE_DRILL_ISO) \
	$(UPDATE_FAILURE_DRILL_ISO) \
	$(INTEGRATION_REPLAY_OUTAGE_DRILL_ISO) \
	$(POWER_CYCLE_CONTINUITY_DRILL_ISO) \
	$(ENTROPY_ISO) \
	$(MBEDTLS_TEST_SECRET_ISO) \
	$(MBEDTLS_TLS_QEMU_ISO) \
	$(HTTPS_SERVICE_QEMU_ISO) \
	$(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO) \
	$(HTTPS_RESTART_SOAK_QEMU_ISO) \
	$(SNI_SELECTION_ISO)

QEMU_SERIAL_TARGETS := \
	test-host-provisioned-entropy-qemu \
	test-pic-irq0-masking-fallback

# CI-SPEEDUP-5: the installer proof starts first because it is the tail.
#
# The acceptance journal of run 41802298685 named
# `test-production-installer-qemu` as the last target to finish: it started at
# +667 s of the suite's window and ran 595 s, while the rest of the queue had
# finished by +1044 s.  A controlled comparison on this host (one runner, the
# same prebuilt images, four slots) moved it from +590 s to +47 s in the queue
# and shortened the parallel part from 1145 s to 1022 s.  Every other entry
# keeps the order it had.
QEMU_PARALLEL_PHASE_1 := \
	test-production-installer-qemu \
	test-production-installer-update-qemu \
	test-installed-persistence-qemu \
	test-pic-irq0-masking \
	test-lapic-timer-calibration \
	test-lapic-timer-runtime \
	test-lapic-timer-oneshot \
	test-pic-disable \
	test-irqs \
	test-panic \
	test-pagefault \
	test-doublefault \
	test-nmi \
	test-acpi \
	test-pmm \
	test-heap \
	test-vmm \
	test-vmm-as \
	test-local-control-qemu \
	test-network-foundation-qemu \
	test-production-network-qemu \
	test-appliance-ssh-qemu \
	test-production-dhcp-dns-qemu \
	test-production-dhcp-dns-e1000-qemu \
	test-production-first-boot-qemu \
	test-production-service-https-main-qemu \
	test-production-service-https-noentropy-qemu \
	test-production-sni-selection-qemu \
	test-acme-directory-qemu \
	test-acme-order-qemu \
	test-acme-boot-order-qemu \
	test-acme-http01-qemu \
	test-virtio-foundation-qemu \
	test-virtio-net-frame-qemu \
	test-packet-boundary-qemu \
	test-arp-icmp-qemu \
	test-tcp-listener-qemu \
	test-network-resource-qemu \
	test-tls-qemu \
	test-https-service-qemu \
	test-management-plane-qemu \
	test-https-client-qemu \
	test-https-response-matrix-qemu \
	test-https-malformed-request-matrix-qemu \
	test-https-size-boundary-matrix-qemu \
	test-https-exhaustion-matrix-qemu \
	test-entropy-fail-closed-matrix-qemu \
	test-https-restart-soak-qemu \
	test-trust-policy-qemu \
	test-update-lifecycle-qemu \
	test-supportability-qemu \
	test-accounting-boundary-qemu \
	test-resilience-matrix-qemu \
	test-scheduler-yield \
	test-scheduler-tick \
	test-scheduler-wait \
	test-scheduler-exit \
	test-process-scheduler \
	test-user-iretq \
	test-syscall \
	test-user-init \
	test-boot-policy-regression \
	test-data-region \
	test-block-cache \
	test-hfocfs-qemu \
	test-hfocfs-workspace-qemu \
	test-config-journal \
	test-physical-storage-qemu \
	test-physical-sata-acceptance-qemu \
	test-production-storage-boot-qemu \
	test-ehci-storage-qemu \
	test-diag-export-qemu \
	test-diag-failure-qemu \
	test-production-recovery-reset-qemu \
	test-production-forward-qemu \
	test-production-boot-chain-qemu \
	test-efi-ovmf-qemu \
	test-hfoc-efi-installer-qemu \
	test-appliance-persistent-config-qemu \
	test-persistent-config-reboot-acceptance-qemu \
	test-appliance-network-config-qemu \
	test-appliance-persistent-network-config-qemu \
	test-backup-restore-qemu \
	test-service-supervisor-qemu \
	test-smp \
	test-lapic \
	test-ioapic \
	test-ioapic-irq1 \
	test-ioapic-irq0 \
	test-ioapic-irq3-7 \
	test-ioapic-irq8-15 \
	test-keyboard-line-editing \
	test-console-history \
	test-appliance-console-qemu \
	test-appliance-boot-console-qemu \
	test-platform-power-qemu \
	test-local-menu-qemu \
	test-usb-mouse-menu-qemu \
	test-usb-two-controllers-qemu \
	test-integrated-appliance-boot-acceptance-qemu \
	test-entropy-qemu

QEMU_ACCEPTANCE_PROVEN_TARGETS := \
	test-accounting-boundary-qemu \
	test-backup-restore-qemu \
	test-boot-policy-regression \
	test-entropy-fail-closed-matrix-qemu \
	test-https-client-qemu \
	test-https-exhaustion-matrix-qemu \
	test-https-malformed-request-matrix-qemu \
	test-https-response-matrix-qemu \
	test-https-restart-soak-qemu \
	test-https-size-boundary-matrix-qemu \
	test-supportability-qemu

QEMU_PARALLEL_RERUN_TARGETS := \
	$(filter-out $(QEMU_ACCEPTANCE_PROVEN_TARGETS),$(QEMU_PARALLEL_PHASE_1))


# Canonical diagnostic order; keep this sequence unchanged.
test-serial:
	$(MAKE) check
	$(MAKE) test-pic-irq0-masking-fallback
	$(MAKE) test-pic-irq0-masking
	$(MAKE) test-lapic-timer-host
	$(MAKE) test-lapic-timer-calibration
	$(MAKE) test-lapic-timer-runtime-host
	$(MAKE) test-lapic-timer-oneshot-runtime-host
	$(MAKE) test-lapic-timer-runtime
	$(MAKE) test-lapic-timer-oneshot
	$(MAKE) test-pic-disable
	$(MAKE) test-irqs
	$(MAKE) test-panic
	$(MAKE) test-pagefault
	$(MAKE) test-doublefault
	$(MAKE) test-nmi
	$(MAKE) test-acpi
	$(MAKE) test-acpi-host
	$(MAKE) test-platform-reset-host
	$(MAKE) test-pmm-host
	$(MAKE) test-pmm
	$(MAKE) test-heap-host
	$(MAKE) test-heap
	$(MAKE) test-vmm-host
	$(MAKE) test-vmm-user-host
	$(MAKE) test-vmm
	$(MAKE) test-vmm-as
	$(MAKE) test-cpu-host
	$(MAKE) test-context-host
	$(MAKE) test-user-iretq-host
	$(MAKE) test-syscall-host
	$(MAKE) test-user-init-process-host
	$(MAKE) test-scheduler-host
	$(MAKE) test-scheduler-rsp0-host
	$(MAKE) test-scheduler-thread-host
	$(MAKE) test-scheduler-yield-host
	$(MAKE) test-scheduler-tick-host
	$(MAKE) test-scheduler-wait-host
	$(MAKE) test-scheduler-accounting-host
	$(MAKE) test-scheduler-discipline-host
	$(MAKE) test-scheduler-exit-host
	$(MAKE) test-scheduler-stress-host
	$(MAKE) test-process-host
	$(MAKE) test-syscall-origin-host
	$(MAKE) test-syscall-gate-host
	$(MAKE) test-process-create-host
	$(MAKE) test-process-limit-host
	$(MAKE) test-descriptor-host
	$(MAKE) test-vfs-host
	$(MAKE) test-hfocfs-host
	$(MAKE) test-hfocfs-workspace-host
	$(MAKE) test-initramfs-host
	$(MAKE) test-vfs-fd-host
	$(MAKE) test-user-copy-host
	$(MAKE) test-elf-user-host
	$(MAKE) test-user-runtime-host
	$(MAKE) test-pipe-host
	$(MAKE) test-tty-host
	$(MAKE) test-syscall-file-host
	$(MAKE) test-capability-host
	$(MAKE) test-event-ring-host
	$(MAKE) test-audit-host
	$(MAKE) test-audit-snapshot-host
	$(MAKE) test-audit-observability-host
	$(MAKE) test-config-schema-host
	$(MAKE) test-config-snapshot-host
	$(MAKE) test-boot-policy-host
	$(MAKE) test-system-image-host
	$(MAKE) test-sha256-host
	$(MAKE) test-image-integrity-host
	$(MAKE) test-recovery-image-host
	$(MAKE) test-block-device-host
	$(MAKE) test-data-region-host
	$(MAKE) test-block-cache-host
	$(MAKE) test-config-journal-host
	$(MAKE) test-pem-decode-host
	$(MAKE) test-appliance-certificate-import-host
	$(MAKE) test-appliance-certificate-upload-host
	$(MAKE) test-appliance-acme-trust-host
	$(MAKE) test-appliance-production-acme-order-host
	$(MAKE) test-appliance-acme-message-host
	$(MAKE) test-appliance-acme-jws-host
	$(MAKE) test-appliance-acme-flow-host
	$(MAKE) test-appliance-acme-challenge-host
	$(MAKE) test-appliance-acme-http01-host
	$(MAKE) test-appliance-acme-renewal-host
	$(MAKE) test-appliance-acme-http-host
	$(MAKE) test-appliance-acme-transport-host
	$(MAKE) test-appliance-acme-directory-fixture-host
	$(MAKE) test-appliance-acme-directory-stub-host
	$(MAKE) test-appliance-language-host
	$(MAKE) test-config-kv-host
	$(MAKE) test-state-schema-host
	$(MAKE) test-state-migration-host
	$(MAKE) test-journal-compaction-host
	$(MAKE) test-backup-stream-host
	$(MAKE) test-backup-manifest-host
	$(MAKE) test-backup-producer-host
	$(MAKE) test-restore-validator-host
	$(MAKE) test-restore-planner-host
	$(MAKE) test-restore-activation-host
	$(MAKE) test-service-descriptor-host
	$(MAKE) test-https-service-descriptor-host
	$(MAKE) test-https-admission-host
	$(MAKE) test-https-connection-host
	$(MAKE) test-https-data-flow-host
	$(MAKE) test-https-handler-host
	$(MAKE) test-https-health-host
	$(MAKE) test-local-principal-host
	$(MAKE) test-credential-verifier-host
	$(MAKE) test-auth-throttle-host
	$(MAKE) test-local-session-host
	$(MAKE) test-management-command-host
	$(MAKE) test-authorization-host
	$(MAKE) test-appliance-status-host
	$(MAKE) test-controlled-configuration-host
	$(MAKE) test-audit-correlation-host
	$(MAKE) test-local-control-qemu
	$(MAKE) test-network-foundation-qemu
	$(MAKE) test-production-network-qemu
	$(MAKE) test-appliance-ssh-qemu
	$(MAKE) test-production-dhcp-dns-qemu
	$(MAKE) test-production-dhcp-dns-e1000-qemu
	$(MAKE) test-production-first-boot-qemu
	$(MAKE) test-virtio-foundation-qemu
	$(MAKE) test-virtio-net-frame-qemu
	$(MAKE) test-packet-boundary-qemu
	$(MAKE) test-arp-icmp-qemu
	$(MAKE) test-tcp-listener-qemu
	$(MAKE) test-network-resource-qemu
	$(MAKE) test-tls-qemu
	$(MAKE) test-https-service-qemu
	$(MAKE) test-https-client-qemu
	$(MAKE) test-https-response-matrix-qemu
	$(MAKE) test-https-malformed-request-matrix-qemu
	$(MAKE) test-https-size-boundary-matrix-qemu
	$(MAKE) test-https-exhaustion-matrix-qemu
	$(MAKE) test-entropy-fail-closed-matrix-qemu
	$(MAKE) test-https-restart-soak-qemu
	$(MAKE) test-trust-policy-qemu
	$(MAKE) test-update-lifecycle-qemu
	$(MAKE) test-supportability-qemu
	$(MAKE) test-accounting-boundary-qemu
	$(MAKE) test-storage-interruption-qemu
	$(MAKE) test-service-cascade-drill-qemu
	$(MAKE) test-local-control-outage-drill-qemu
	$(MAKE) test-network-partition-drill-qemu
	$(MAKE) test-trust-failure-drill-qemu
	$(MAKE) test-update-failure-drill-qemu
	$(MAKE) test-network-device-host
	$(MAKE) test-link-state-host
	$(MAKE) test-virtio-pci-identity-host
	$(MAKE) test-virtio-pci-capability-host
	$(MAKE) test-virtio-pci-device-status-host
	$(MAKE) test-virtio-pci-feature-host
	$(MAKE) test-virtio-queue-layout-host
	$(MAKE) test-virtio-descriptor-ownership-host
	$(MAKE) test-virtio-available-publication-host
	$(MAKE) test-virtio-used-consumption-host
	$(MAKE) test-virtio-dma-region-host
	$(MAKE) test-virtio-net-feature-profile-host
	$(MAKE) test-virtio-rng-feature-profile-host
	$(MAKE) test-virtio-rng-queue-composition-host
	$(MAKE) test-virtio-rng-request-host
	$(MAKE) test-virtio-rng-completion-host
	$(MAKE) test-entropy-health-host
	$(MAKE) test-entropy-accumulator-host
	$(MAKE) test-entropy-seed-fill-host
	$(MAKE) test-entropy-restart-host
	$(MAKE) test-virtio-net-queue-composition-host
	$(MAKE) test-virtio-net-rx-host
	$(MAKE) test-virtio-net-tx-host
	$(MAKE) test-virtio-net-interrupt-host
	$(MAKE) test-virtio-net-link-state-host
	$(MAKE) test-virtio-net-lifecycle-host
	$(MAKE) test-ipv4-config-host
	$(MAKE) test-network-config-transaction-host
	$(MAKE) test-packet-buffer-host
	$(MAKE) test-packet-pool-host
	$(MAKE) test-packet-lifecycle-host
	$(MAKE) test-packet-slice-host
	$(MAKE) test-ethernet-frame-host
	$(MAKE) test-virtio-net-mac-identity-host
	$(MAKE) test-network-ipv4-profile-host
	$(MAKE) test-hfoc-netif-input-host
	$(MAKE) test-hfoc-netif-output-host
	$(MAKE) test-icmp-reachability-host
	$(MAKE) test-management-transport-host
	$(MAKE) test-network-admission-host
	$(MAKE) test-network-event-host
	$(MAKE) test-key-use-host
	$(MAKE) test-trust-anchor-host
	$(MAKE) test-verification-policy-host
	$(MAKE) test-signed-metadata-host
	$(MAKE) test-trust-rotation-host
	$(MAKE) test-trust-revocation-host
	$(MAKE) test-image-policy-host
	$(MAKE) test-boot-evidence-host
	$(MAKE) test-recovery-trust-host
	$(MAKE) test-appliance-update-manifest-host
	$(MAKE) test-appliance-update-integrity-host
	$(MAKE) test-appliance-recovery-mode-host
	$(MAKE) test-appliance-factory-reset-host
	$(MAKE) test-update-compatibility-host
	$(MAKE) test-candidate-slot-inventory-host
	$(MAKE) test-candidate-preflight-host
	$(MAKE) test-candidate-staged-write-host
	$(MAKE) test-activation-intent-host
	$(MAKE) test-first-boot-health-host
	$(MAKE) test-health-check-host
	$(MAKE) test-storage-health-host
	$(MAKE) test-service-health-host
	$(MAKE) test-network-health-host
	$(MAKE) test-audit-ring-health-host
	$(MAKE) test-metrics-host
	$(MAKE) test-fault-injection-host
	$(MAKE) test-storage-interruption-host
	$(MAKE) test-support-bundle-manifest-host
	$(MAKE) test-support-bundle-exporter-host
	$(MAKE) test-appliance-support-bundle-host
	$(MAKE) test-appliance-support-session-host
	$(MAKE) test-appliance-support-break-glass-host
	$(MAKE) test-appliance-production-support-host
	$(MAKE) test-hardware-acceptance-host
	$(MAKE) test-product-release-gate-host
	$(MAKE) test-boot-chain-determinism-host
	$(MAKE) test-diagnostic-privacy-host
	$(MAKE) test-integration-boundary-host
	$(MAKE) test-integration-principal-host
	$(MAKE) test-integration-session-host
	$(MAKE) test-integration-envelope-host
	$(MAKE) test-integration-ledger-host
	$(MAKE) test-integration-event-cursor-host
	$(MAKE) test-integration-config-intent-host
	$(MAKE) test-integration-failure-isolation-host
	$(MAKE) test-integration-compatibility-host
	$(MAKE) test-release-profile-host
	$(MAKE) test-build-input-manifest-host
	$(MAKE) test-automatic-rollback-host
	$(MAKE) test-update-acceptance-host
	$(MAKE) test-update-retention-host
	$(MAKE) test-service-dependency-host
	$(MAKE) test-service-graph-host
	$(MAKE) test-service-restart-policy-host
	$(MAKE) test-service-supervisor-host
	$(MAKE) test-service-cascade-drill-host
	$(MAKE) test-local-control-outage-drill-host
	$(MAKE) test-network-partition-drill-host
	$(MAKE) test-trust-failure-drill-host
	$(MAKE) test-update-failure-drill-host
	$(MAKE) test-integration-replay-outage-drill-host
	$(MAKE) test-power-cycle-continuity-drill-host
	$(MAKE) test-resilience-matrix-host
	$(MAKE) test-process-scheduler-host
	$(MAKE) test-scheduler-yield
	$(MAKE) test-scheduler-tick
	$(MAKE) test-scheduler-wait
	$(MAKE) test-scheduler-exit
	$(MAKE) test-scheduler-stress
	$(MAKE) test-process-scheduler
	$(MAKE) test-user-iretq
	$(MAKE) test-syscall
	$(MAKE) test-user-init
	$(MAKE) test-boot-policy-regression
	$(MAKE) test-block-device
	$(MAKE) test-data-region
	$(MAKE) test-block-cache
	$(MAKE) test-hfocfs-qemu
	$(MAKE) test-hfocfs-workspace-qemu
	$(MAKE) test-config-journal
	$(MAKE) test-physical-storage-qemu
	$(MAKE) test-appliance-persistent-config-qemu
	$(MAKE) test-persistent-config-reboot-acceptance-qemu
	$(MAKE) test-appliance-network-config-qemu
	$(MAKE) test-appliance-persistent-network-config-qemu
	$(MAKE) test-backup-restore-qemu
	$(MAKE) test-service-supervisor-qemu
	$(MAKE) test-smp-host
	$(MAKE) test-smp
	$(MAKE) test-lapic-host
	$(MAKE) test-lapic
	$(MAKE) test-ioapic-host
	$(MAKE) test-isa-irq3-7-host
	$(MAKE) test-isa-irq8-15-host
	$(MAKE) test-ioapic
	$(MAKE) test-ioapic-irq1
	$(MAKE) test-ioapic-irq0
	$(MAKE) test-ioapic-irq3-7
	$(MAKE) test-ioapic-irq8-15
	$(MAKE) test-keyboard-host
	$(MAKE) test-keyboard-line-editing
	$(MAKE) test-console-history
	$(MAKE) test-appliance-console-qemu
	$(MAKE) test-appliance-boot-console-qemu
	$(MAKE) test-integrated-appliance-boot-acceptance-qemu
	$(MAKE) test-management-plane-qemu
	$(MAKE) test-console-host
	$(MAKE) test-pic-host
	$(MAKE) test-build-input-manifest-host
	$(MAKE) test-reproducible-image-comparison-host
	$(MAKE) test-release-artifact-manifest-host
	$(MAKE) test-release-upgrade-matrix-host
	$(MAKE) test-release-backup-restore-matrix-host
	$(MAKE) test-release-security-baseline-host
	$(MAKE) test-release-operational-baseline-host
	$(MAKE) test-release-disaster-recovery-rehearsal-host
	$(MAKE) test-release-appliance-v1-acceptance-host
	$(MAKE) test-https-vertical-slice-acceptance-host
	$(MAKE) test-qemu-suite-host

.PHONY: test-fetch-limine-host
test-fetch-limine-host:
	bash tests/test-fetch-limine-host.sh

.PHONY: test-licensing-policy-host
test-licensing-policy-host:
	bash tests/test-licensing-policy-host.sh

.PHONY: test-third-party-license-docs-host
test-third-party-license-docs-host:
	bash tests/test-third-party-license-docs-host.sh

.PHONY: test-license-payload-host test-license-payload-iso-host
test-license-payload-host:
	bash tests/test-license-payload-host.sh

test-license-payload-iso-host: iso $(FALLBACK_ISO)
	bash tests/test-license-payload-iso-host.sh

.PHONY: test-trademark-policy-host
test-trademark-policy-host:
	bash tests/test-trademark-policy-host.sh

.PHONY: test-lwip-source-lock-host
test-lwip-source-lock-host:
	bash tests/test-lwip-source-lock-host.sh

.PHONY: test-mbedtls-source-lock-host
test-mbedtls-source-lock-host:
	bash tests/test-mbedtls-source-lock-host.sh

.PHONY: test-mbedtls-config-host
test-mbedtls-config-host:
	bash tests/test-mbedtls-config-host.sh

# ITEM-386: the two mbedTLS profiles that share the production image must stay
# in disjoint object partitions.  The verifier reads the built objects and the
# linked artifact, so it depends on the kernel being up to date.
.PHONY: verify-mbedtls-object-partition
verify-mbedtls-object-partition: $(KERNEL)
	bash scripts/verify-mbedtls-object-partition.sh --production $(KERNEL)

.PHONY: test-mbedtls-alloc-host
test-mbedtls-alloc-host: kernel/include/hfoc_mbedtls_alloc.h \
		kernel/src/hfoc_mbedtls_alloc.c tests/mbedtls-alloc-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-Ikernel/include \
		kernel/src/hfoc_mbedtls_alloc.c tests/mbedtls-alloc-host.c \
		-o build/test-mbedtls-alloc-host -no-pie
	build/test-mbedtls-alloc-host

MBEDTLS_DRBG_SOURCES := \
	third_party/mbedtls/library/aes.c \
	third_party/mbedtls/library/ctr_drbg.c \
	third_party/mbedtls/library/platform.c \
	third_party/mbedtls/library/platform_util.c

.PHONY: test-mbedtls-drbg-host
test-mbedtls-drbg-host: kernel/include/hfoc_mbedtls_drbg.h \
		kernel/src/hfoc_mbedtls_drbg.c \
		kernel/src/entropy_accumulator.c \
		kernel/src/entropy_seed_fill.c \
		kernel/src/hfoc_mbedtls_alloc.c tests/mbedtls-drbg-host.c
	rm -rf build/mbedtls-drbg-host
	mkdir -p build/mbedtls-drbg-host
	for src in $(MBEDTLS_DRBG_SOURCES); do \
		obj=build/mbedtls-drbg-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-Ithird_party/mbedtls/include -Ikernel/include \
			-DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_config.h"' \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_MBEDTLS_DRBG_HOST_TEST \
		-Ithird_party/mbedtls/include -Ikernel/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_config.h"' \
		kernel/src/hfoc_mbedtls_drbg.c \
		kernel/src/entropy_accumulator.c \
		kernel/src/entropy_seed_fill.c \
		kernel/src/hfoc_mbedtls_alloc.c tests/mbedtls-drbg-host.c \
		build/mbedtls-drbg-host/*.o \
		-o build/test-mbedtls-drbg-host -no-pie
	build/test-mbedtls-drbg-host

MBEDTLS_MANIFEST_SOURCES := \
	third_party/mbedtls/library/asn1parse.c \
	third_party/mbedtls/library/asn1write.c \
	third_party/mbedtls/library/bignum.c \
	third_party/mbedtls/library/bignum_core.c \
	third_party/mbedtls/library/bignum_mod.c \
	third_party/mbedtls/library/bignum_mod_raw.c \
	third_party/mbedtls/library/constant_time.c \
	third_party/mbedtls/library/ecdsa.c \
	third_party/mbedtls/library/ecp.c \
	third_party/mbedtls/library/ecp_curves.c \
	third_party/mbedtls/library/ecp_curves_new.c \
	third_party/mbedtls/library/md.c \
	third_party/mbedtls/library/oid.c \
	third_party/mbedtls/library/pk.c \
	third_party/mbedtls/library/pk_ecc.c \
	third_party/mbedtls/library/pk_wrap.c \
	third_party/mbedtls/library/pkparse.c \
	third_party/mbedtls/library/pkwrite.c \
	third_party/mbedtls/library/platform.c \
	third_party/mbedtls/library/platform_util.c \
	third_party/mbedtls/library/rsa.c \
	third_party/mbedtls/library/rsa_alt_helpers.c \
	third_party/mbedtls/library/sha256.c \
	third_party/mbedtls/library/x509.c \
	third_party/mbedtls/library/x509_create.c \
	third_party/mbedtls/library/x509_crt.c \
	third_party/mbedtls/library/x509write.c \
	third_party/mbedtls/library/x509write_csr.c

.PHONY: test-mbedtls-manifest-host
test-mbedtls-manifest-host: kernel/include/hfoc_mbedtls_manifest.h \
		kernel/src/hfoc_mbedtls_manifest.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		tests/hfoc-mbedtls-manifest-host-config.h \
		tests/mbedtls-manifest-host.c
	rm -rf build/mbedtls-manifest-host
	mkdir -p build/mbedtls-manifest-host
	for src in $(MBEDTLS_MANIFEST_SOURCES); do \
		obj=build/mbedtls-manifest-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-Ithird_party/mbedtls/include -Ikernel/include -Itests \
			-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_MBEDTLS_MANIFEST_HOST_TEST \
		-Ithird_party/mbedtls/include -Ikernel/include -Itests \
		-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
		kernel/src/hfoc_mbedtls_manifest.c \
		kernel/src/hfoc_mbedtls_alloc.c tests/mbedtls-manifest-host.c \
		build/mbedtls-manifest-host/*.o \
		-o build/test-mbedtls-manifest-host -no-pie
	build/test-mbedtls-manifest-host

# ITEM-503: the BYO validation gate over the real mbedTLS parser.  The host arm
# reuses the manifest profile's object shape, so the certificate, key, SAN and
# chain checks are proven against the same library the kernel links.
#
# ITEM-503 follow-up: the manifest host profile's default arena is the
# verification-only one (32 tiny slots), which is too small for a certificate
# chain verification - mbedTLS fails the walk with an allocation error and the
# chain is then refused for the wrong reason.  The production kernel compiles
# the same allocator and objects with HFOC_PRODUCTION_TLS, which selects the
# connection-sized arena; the proof compiles with those bounds so the tested
# capacity is the capacity the appliance runs with.
HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS := \
	-DHFOC_MBEDTLS_CONNECTION_MAX_TINY=128 \
	-DHFOC_MBEDTLS_CONNECTION_MAX_SMALL=64 \
	-DHFOC_MBEDTLS_CONNECTION_MAX_MEDIUM=32 \
	-DHFOC_MBEDTLS_CONNECTION_MAX_LARGE=16 \
	-DHFOC_MBEDTLS_POOL_SLOT_COUNT_TINY=256 \
	-DHFOC_MBEDTLS_POOL_SLOT_COUNT_SMALL=128 \
	-DHFOC_MBEDTLS_POOL_SLOT_COUNT_MEDIUM=64 \
	-DHFOC_MBEDTLS_POOL_SLOT_COUNT_LARGE=16 \
	-DHFOC_MBEDTLS_POOL_SLOT_COUNT_MAX=256

.PHONY: test-appliance-certificate-import-host
test-appliance-certificate-import-host: \
		kernel/include/appliance_certificate_import.h \
		kernel/src/appliance_certificate_import.c \
		tests/appliance-certificate-import-host.c
	rm -rf build/mbedtls-cert-import-host
	mkdir -p build/mbedtls-cert-import-host
	for src in $(MBEDTLS_MANIFEST_SOURCES); do \
		obj=build/mbedtls-cert-import-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
			-Ithird_party/mbedtls/include -Ikernel/include -Itests \
			-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_APPLIANCE_CERTIFICATE_IMPORT_HOST_TEST \
		$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
		-Ithird_party/mbedtls/include -Ikernel/include -Itests \
		-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
		kernel/src/appliance_certificate_import.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		tests/appliance-certificate-import-host.c \
		build/mbedtls-cert-import-host/*.o \
		-o build/test-appliance-certificate-import-host -no-pie
	build/test-appliance-certificate-import-host

# ITEM-508 increment 2: the ACME client role's trust decision, driven on the
# host.  The proof binds a fixture CA, presents chained, unrelated, expired,
# not-yet-valid and malformed chains, and pins the image's own anchor by
# digest; it needs no network and no ACME server.
.PHONY: test-appliance-acme-trust-host
test-appliance-acme-trust-host: \
		kernel/include/appliance_acme_trust.h \
		kernel/src/appliance_acme_trust.c \
		kernel/src/appliance_acme_trust_anchor.c \
		kernel/src/sha256.c \
		tests/appliance-acme-trust-host.c \
		tests/appliance-certificate-fixtures.h
	rm -rf build/mbedtls-acme-trust-host
	mkdir -p build/mbedtls-acme-trust-host
	for src in $(MBEDTLS_MANIFEST_SOURCES); do \
		obj=build/mbedtls-acme-trust-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
			-Ithird_party/mbedtls/include -Ikernel/include -Itests \
			-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_APPLIANCE_ACME_TRUST_HOST_TEST \
		$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
		-Ithird_party/mbedtls/include -Ikernel/include -Itests \
		-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
		kernel/src/appliance_acme_trust.c \
		kernel/src/appliance_acme_trust_anchor.c \
		kernel/src/sha256.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		tests/appliance-acme-trust-host.c \
		build/mbedtls-acme-trust-host/*.o \
		-o build/test-appliance-acme-trust-host -no-pie
	build/test-appliance-acme-trust-host

# ITEM-508 increment 14: the offline ACME stub's identity.  The QEMU directory
# proof dials the stub as the literal `10.0.2.2` and the frozen TLS client
# verifies the peer's name; the shared fixtures carry only the DNS SAN
# `hfoc-appliance`, so this proof pins a chain whose leaf also carries the
# literal, the exact verification the binding performs, and the key the stub
# serves it with.
.PHONY: test-appliance-acme-directory-fixture-host
test-appliance-acme-directory-fixture-host: \
		tests/appliance-acme-directory-fixtures.h \
		tests/appliance-certificate-fixtures.h \
		tests/appliance-acme-directory-fixture-host.c
	rm -rf build/mbedtls-acme-directory-fixture-host
	mkdir -p build/mbedtls-acme-directory-fixture-host
	for src in $(MBEDTLS_MANIFEST_SOURCES); do \
		obj=build/mbedtls-acme-directory-fixture-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
			-Ithird_party/mbedtls/include -Ikernel/include -Itests \
			-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
		-Ithird_party/mbedtls/include -Ikernel/include -Itests \
		-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
		kernel/src/sha256.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		tests/appliance-acme-directory-fixture-host.c \
		build/mbedtls-acme-directory-fixture-host/*.o \
		-o build/test-appliance-acme-directory-fixture-host -no-pie
	build/test-appliance-acme-directory-fixture-host

# ITEM-508 increment 15: the offline stub itself is proven before any guest
# boots.  An independent client (openssl, sharing no code with the stub) must
# accept the served chain for the literal `10.0.2.2` and receive exactly the
# directory document the guest driver will demand; the proof also fails if any
# log carries private key material.
.PHONY: test-appliance-acme-directory-stub-host
test-appliance-acme-directory-stub-host: \
		scripts/hfoc-acme-directory-stub.py \
		scripts/test-acme-directory-stub-host.sh \
		tests/appliance-acme-directory-fixtures.h
	bash scripts/test-acme-directory-stub-host.sh

# ITEM-509 increment 1: the one string catalog of the appliance interface and
# the code-point inventory the console glyph proof consumes later.  The proof
# is pure host code - no kernel, no QEMU, no storage - and it refuses an empty
# translation, a duplicate key, a fallback to English and a non-UTF-8 value.
.PHONY: test-appliance-language-host
test-appliance-language-host: \
		kernel/include/appliance_language.h \
		kernel/src/appliance_language.c \
		tests/appliance-language-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-Ikernel/include \
		kernel/src/appliance_language.c \
		tests/appliance-language-host.c \
		-o build/test-appliance-language-host -no-pie
	build/test-appliance-language-host

# ITEM-508 increment 3: the bounded JSON reader and the ACME reply readers,
# driven on the host on fixed data; no mbedTLS, no network, no server.
.PHONY: test-appliance-acme-message-host
test-appliance-acme-message-host: \
		kernel/include/appliance_acme_json.h \
		kernel/src/appliance_acme_json.c \
		kernel/include/appliance_acme_message.h \
		kernel/src/appliance_acme_message.c \
		tests/appliance-acme-message-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-Ikernel/include \
		kernel/src/appliance_acme_json.c \
		kernel/src/appliance_acme_message.c \
		tests/appliance-acme-message-host.c \
		-o build/test-appliance-acme-message-host -no-pie
	build/test-appliance-acme-message-host

# ITEM-508 increment 4: the account key, base64url and the JWS envelope,
# driven on the host; no network and no ACME server.
.PHONY: test-appliance-acme-jws-host
test-appliance-acme-jws-host: \
		kernel/include/appliance_acme_jws.h \
		kernel/src/appliance_acme_jws.c \
		tests/appliance-acme-jws-host.c \
		tests/appliance-certificate-fixtures.h
	rm -rf build/mbedtls-acme-jws-host
	mkdir -p build/mbedtls-acme-jws-host
	for src in $(MBEDTLS_MANIFEST_SOURCES); do \
		obj=build/mbedtls-acme-jws-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
			-Ithird_party/mbedtls/include -Ikernel/include -Itests \
			-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_APPLIANCE_ACME_JWS_HOST_TEST \
		$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
		-Ithird_party/mbedtls/include -Ikernel/include -Itests \
		-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
		kernel/src/appliance_acme_jws.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		tests/appliance-acme-jws-host.c \
		build/mbedtls-acme-jws-host/*.o \
		-o build/test-appliance-acme-jws-host -no-pie
	build/test-appliance-acme-jws-host
	build/test-appliance-acme-jws-host --write-csr build/appliance-acme-csr.der
	openssl req -inform DER -in build/appliance-acme-csr.der -noout -verify \
		>build/appliance-acme-csr-verify.txt 2>&1
	grep -Fq 'verify OK' build/appliance-acme-csr-verify.txt
	openssl req -inform DER -in build/appliance-acme-csr.der -noout -subject \
		>build/appliance-acme-csr-subject.txt 2>&1
	grep -Fq 'hfoc-appliance' build/appliance-acme-csr-subject.txt

# ITEM-508 increment 5: the ACME order's steps, records and clock refusal,
# driven on the host; no network and no ACME server.
.PHONY: test-appliance-acme-flow-host
test-appliance-acme-flow-host: \
		kernel/include/appliance_acme_flow.h \
		kernel/include/appliance_acme_message.h \
		kernel/include/appliance_acme_jws.h \
		kernel/include/appliance_acme_trust.h \
		kernel/include/appliance_acme_challenge.h \
		kernel/include/appliance_production_acme_http01.h \
		kernel/src/appliance_acme_flow.c \
		kernel/src/appliance_acme_json.c \
		kernel/src/appliance_acme_message.c \
		kernel/src/appliance_acme_jws.c \
		kernel/src/appliance_acme_trust.c \
		kernel/src/appliance_acme_trust_anchor.c \
		kernel/src/appliance_acme_challenge.c \
		kernel/src/appliance_production_acme_http01.c \
		kernel/src/appliance_production_acme_http01_flow.c \
		kernel/src/pem_decode.c \
		kernel/src/sha256.c \
		$(LWIP_TCP_SOURCES) \
		tests/appliance-acme-flow-host.c \
		tests/appliance-certificate-fixtures.h
	rm -rf build/mbedtls-acme-flow-host
	mkdir -p build/mbedtls-acme-flow-host
	for src in $(MBEDTLS_MANIFEST_SOURCES); do \
		obj=build/mbedtls-acme-flow-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
			-Ithird_party/mbedtls/include -Ikernel/include -Itests \
			-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	rm -rf build/acme-flow-host
	mkdir -p build/acme-flow-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/acme-flow-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_APPLIANCE_ACME_FLOW_HOST_TEST \
		-DHFOC_APPLIANCE_ACME_JWS_HOST_TEST \
		-DHFOC_APPLIANCE_ACME_TRUST_HOST_TEST \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_APPLIANCE_ACME_HTTP01_HOST_TEST \
		$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
		-Ithird_party/mbedtls/include -Ikernel/include -Itests \
		-Ithird_party/lwip/src/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-manifest-host-config.h"' \
		kernel/src/appliance_acme_flow.c \
		kernel/src/appliance_acme_json.c \
		kernel/src/appliance_acme_message.c \
		kernel/src/appliance_acme_jws.c \
		kernel/src/appliance_acme_trust.c \
		kernel/src/appliance_acme_trust_anchor.c \
		kernel/src/appliance_acme_challenge.c \
		kernel/src/appliance_production_acme_http01.c \
		kernel/src/appliance_production_acme_http01_flow.c \
		kernel/src/hfoc_lwip.c \
		kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/pem_decode.c \
		kernel/src/sha256.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		tests/appliance-acme-flow-host.c \
		build/mbedtls-acme-flow-host/*.o \
		build/acme-flow-host/*.o \
		-o build/test-appliance-acme-flow-host -no-pie
	build/test-appliance-acme-flow-host

# ITEM-508 increment 7: the HTTP-01 answer decision, the request line it reads
# and the lifetime of the armed token, driven on the host; no network, no
# socket and no ACME server.
.PHONY: test-appliance-acme-challenge-host
test-appliance-acme-challenge-host: \
		kernel/include/appliance_acme_challenge.h \
		kernel/include/appliance_acme_message.h \
		kernel/src/appliance_acme_challenge.c \
		tests/appliance-acme-challenge-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-Ikernel/include \
		kernel/src/appliance_acme_challenge.c \
		tests/appliance-acme-challenge-host.c \
		-o build/test-appliance-acme-challenge-host -no-pie
	build/test-appliance-acme-challenge-host

# ITEM-508 increment 28: the HTTP-01 listener - the exact response bytes, the
# unarmed production boot state and the arm/disarm lifecycle - driven on the
# host over the same lwIP objects the wrapper proof uses.  No TCP exchange is
# faked here; the wire itself is the QEMU proof.
.PHONY: test-appliance-acme-http01-host
test-appliance-acme-http01-host: \
		kernel/include/appliance_acme_challenge.h \
		kernel/include/appliance_acme_message.h \
		kernel/include/appliance_production_acme_http01.h \
		kernel/src/appliance_acme_challenge.c \
		kernel/src/appliance_production_acme_http01.c \
		tests/appliance-acme-http01-host.c
	rm -rf build/acme-http01-host
	mkdir -p build/acme-http01-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/acme-http01-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_APPLIANCE_ACME_HTTP01_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/appliance_acme_challenge.c \
		kernel/src/appliance_production_acme_http01.c \
		tests/appliance-acme-http01-host.c \
		build/acme-http01-host/*.o \
		-o build/test-appliance-acme-http01-host -no-pie
	build/test-appliance-acme-http01-host

# ITEM-508 increment 9: the renewal decision - the window, the retry, the
# bounded attempt budget and the record - driven on the host; no network, no
# CA and no clock of its own.
.PHONY: test-appliance-acme-renewal-host
test-appliance-acme-renewal-host: \
		kernel/include/appliance_acme_renewal.h \
		kernel/src/appliance_acme_renewal.c \
		tests/appliance-acme-renewal-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-Ikernel/include \
		kernel/src/appliance_acme_renewal.c \
		tests/appliance-acme-renewal-host.c \
		-o build/test-appliance-acme-renewal-host -no-pie
	build/test-appliance-acme-renewal-host

# ITEM-508 increment 10: the ACME client's HTTP exchange - the request it
# builds and the reply it reads - driven on the host with fixed bytes; no
# socket, no TLS and no ACME server.
.PHONY: test-appliance-acme-http-host
test-appliance-acme-http-host: \
		kernel/include/appliance_acme_http.h \
		kernel/src/appliance_acme_http.c \
		tests/appliance-acme-http-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-Ikernel/include \
		kernel/src/appliance_acme_http.c \
		tests/appliance-acme-http-host.c \
		-o build/test-appliance-acme-http-host -no-pie
	build/test-appliance-acme-http-host

# ITEM-508 increment 11: the ACME exchange driver - the request that leaves
# whole and the reply that is complete only when its framing says so - driven
# on the host over a scripted link; no socket, no TLS and no ACME server.
.PHONY: test-appliance-acme-transport-host
test-appliance-acme-transport-host: \
		kernel/include/appliance_acme_transport.h \
		kernel/src/appliance_acme_transport.c \
		kernel/include/appliance_acme_http.h \
		kernel/src/appliance_acme_http.c \
		tests/appliance-acme-transport-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-Ikernel/include \
		kernel/src/appliance_acme_transport.c \
		kernel/src/appliance_acme_http.c \
		tests/appliance-acme-transport-host.c \
		-o build/test-appliance-acme-transport-host -no-pie
	build/test-appliance-acme-transport-host

.PHONY: test-appliance-certificate-upload-host
test-appliance-certificate-upload-host: \
		kernel/include/appliance_acme_account.h \
		kernel/include/appliance_acme_jws.h \
		kernel/include/appliance_acme_renewal.h \
		kernel/include/appliance_certificate_upload.h \
		kernel/include/appliance_certificate_import.h \
		kernel/include/appliance_production_acme_service.h \
		kernel/include/appliance_production_certificate.h \
		kernel/include/appliance_state_layout.h \
		kernel/src/appliance_acme_account.c \
		kernel/src/appliance_acme_jws.c \
		kernel/src/appliance_acme_renewal.c \
		kernel/src/appliance_certificate_upload.c \
		kernel/src/appliance_certificate_import.c \
		kernel/src/appliance_production_acme_service.c \
		kernel/src/appliance_production_certificate.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/pem_decode.c \
		kernel/src/sha256.c \
		tests/hfoc-mbedtls-upload-host-config.h \
		tests/appliance-certificate-fixtures.h \
		tests/appliance-certificate-upload-host.c
	rm -rf build/mbedtls-cert-upload-host
	mkdir -p build/mbedtls-cert-upload-host
	for src in $(MBEDTLS_MANIFEST_SOURCES) \
		third_party/mbedtls/library/x509_create.c \
		third_party/mbedtls/library/x509write.c \
		third_party/mbedtls/library/x509write_crt.c \
		third_party/mbedtls/library/x509write_csr.c; do \
		obj=build/mbedtls-cert-upload-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
			-Ithird_party/mbedtls/include -Ikernel/include -Itests \
			-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-upload-host-config.h"' \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_APPLIANCE_CERTIFICATE_UPLOAD_HOST_TEST \
		-DHFOC_APPLIANCE_CERTIFICATE_IMPORT_HOST_TEST \
		-DHFOC_APPLIANCE_ACME_SERVICE_HOST_TEST \
		-DHFOC_PRODUCTION_TLS \
		$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
		-Ithird_party/mbedtls/include -Ikernel/include -Itests \
		-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-upload-host-config.h"' \
		kernel/src/appliance_acme_account.c \
		kernel/src/appliance_acme_jws.c \
		kernel/src/appliance_acme_renewal.c \
		kernel/src/appliance_certificate_upload.c \
		kernel/src/appliance_certificate_import.c \
		kernel/src/appliance_production_acme_service.c \
		kernel/src/appliance_production_certificate.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/block_device.c \
		kernel/src/pem_decode.c \
		kernel/src/sha256.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		tests/appliance-certificate-upload-host.c \
		build/mbedtls-cert-upload-host/*.o \
		-o build/test-appliance-certificate-upload-host -no-pie
	build/test-appliance-certificate-upload-host

# ITEM-508 increment 35: the order path ordinary boot runs, proven on the host
# against a scripted transport link.  The mbedTLS arm is the same shape the
# upload proof uses (the frozen profile's sources plus the X.509 write half the
# generated identity needs), so the driver is exercised against the library the
# kernel links.
.PHONY: test-appliance-production-acme-order-host
test-appliance-production-acme-order-host: \
		kernel/include/appliance_production_acme_order.h \
		kernel/include/appliance_acme_flow.h \
		kernel/include/appliance_acme_jws.h \
		kernel/include/appliance_acme_trust.h \
		kernel/include/appliance_acme_transport.h \
		kernel/include/appliance_acme_renewal.h \
		kernel/include/appliance_acme_challenge.h \
		kernel/include/appliance_production_certificate.h \
		kernel/include/appliance_state_layout.h \
		kernel/src/appliance_production_acme_order.c \
		kernel/src/appliance_acme_flow.c \
		kernel/src/appliance_acme_json.c \
		kernel/src/appliance_acme_message.c \
		kernel/src/appliance_acme_jws.c \
		kernel/src/appliance_acme_account.c \
		kernel/src/appliance_acme_trust.c \
		kernel/src/appliance_acme_transport.c \
		kernel/src/appliance_acme_http.c \
		kernel/src/appliance_acme_challenge.c \
		kernel/src/appliance_acme_renewal.c \
		kernel/src/appliance_production_certificate.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/block_device.c \
		kernel/src/pem_decode.c \
		kernel/src/sha256.c \
		tests/hfoc-mbedtls-upload-host-config.h \
		tests/appliance-acme-directory-fixtures.h \
		tests/appliance-production-acme-order-host.c
	rm -rf build/mbedtls-acme-order-host
	mkdir -p build/mbedtls-acme-order-host
	for src in $(MBEDTLS_MANIFEST_SOURCES) \
		third_party/mbedtls/library/x509_create.c \
		third_party/mbedtls/library/x509write.c \
		third_party/mbedtls/library/x509write_crt.c \
		third_party/mbedtls/library/x509write_csr.c; do \
		obj=build/mbedtls-acme-order-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
			-Ithird_party/mbedtls/include -Ikernel/include -Itests \
			-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-upload-host-config.h"' \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_APPLIANCE_ACME_ORDER_HOST_TEST \
		-DHFOC_PRODUCTION_TLS \
		$(HFOC_CERTIFICATE_IMPORT_HOST_POOL_FLAGS) \
		-Ithird_party/mbedtls/include -Ikernel/include -Itests \
		-DMBEDTLS_CONFIG_FILE='"hfoc-mbedtls-upload-host-config.h"' \
		kernel/src/appliance_production_acme_order.c \
		kernel/src/appliance_acme_flow.c \
		kernel/src/appliance_acme_json.c \
		kernel/src/appliance_acme_message.c \
		kernel/src/appliance_acme_jws.c \
		kernel/src/appliance_acme_account.c \
		kernel/src/appliance_acme_trust.c \
		kernel/src/appliance_acme_transport.c \
		kernel/src/appliance_acme_http.c \
		kernel/src/appliance_acme_challenge.c \
		kernel/src/appliance_acme_renewal.c \
		kernel/src/appliance_production_certificate.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/block_device.c \
		kernel/src/pem_decode.c \
		kernel/src/sha256.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		tests/appliance-production-acme-order-host.c \
		build/mbedtls-acme-order-host/*.o \
		-o build/test-appliance-production-acme-order-host -no-pie
	build/test-appliance-production-acme-order-host

.PHONY: test-image-signature-host
test-image-signature-host: kernel/include/image_signature.h \
		kernel/include/hfoc_verify_config.h \
		kernel/include/hfoc_verify_alloc.h \
		kernel/include/verification_policy.h kernel/include/trust_anchor.h \
		kernel/include/key_use.h kernel/include/sha256.h \
		kernel/include/signed_metadata.h \
		kernel/src/image_signature.c kernel/src/image_signature_ecdsa.c \
		kernel/src/key_use.c \
		kernel/src/verification_policy.c kernel/src/trust_anchor.c \
		kernel/src/sha256.c kernel/src/signed_metadata.c \
		$(HFOC_VERIFY_HOST_OBJS) \
		tests/image-signature-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_IMAGE_SIGNATURE_HOST_TEST \
		$(HFOC_VERIFY_MBEDTLS_CFLAGS) -Ikernel/include -Itests \
		-c kernel/src/image_signature_ecdsa.c \
		-o build/image-signature-ecdsa-host.o
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_IMAGE_SIGNATURE_HOST_TEST \
		-Ikernel/include \
		kernel/src/image_signature.c kernel/src/key_use.c \
		kernel/src/verification_policy.c kernel/src/trust_anchor.c \
		kernel/src/sha256.c kernel/src/signed_metadata.c \
		tests/image-signature-host.c \
		build/image-signature-ecdsa-host.o \
		$(HFOC_VERIFY_HOST_OBJS) \
		-o build/test-image-signature-host -no-pie
	build/test-image-signature-host

.PHONY: test-boot-measurements-host
test-boot-measurements-host: kernel/include/boot_measurements.h \
		kernel/include/sha256.h \
		kernel/src/boot_measurements.c kernel/src/sha256.c \
		tests/boot-measurements-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_BOOT_MEASUREMENTS_HOST_TEST \
		-Ikernel/include \
		kernel/src/boot_measurements.c kernel/src/sha256.c \
		tests/boot-measurements-host.c \
		-o build/test-boot-measurements-host -no-pie
	build/test-boot-measurements-host

.PHONY: test-boot-chain-host
test-boot-chain-host: kernel/include/boot_chain.h \
		kernel/include/boot_measurements.h \
		kernel/include/image_signature.h \
		kernel/include/hfoc_verify_config.h \
		kernel/include/hfoc_verify_alloc.h \
		kernel/include/signed_metadata.h \
		kernel/include/verification_policy.h \
		kernel/include/trust_anchor.h kernel/include/key_use.h \
		kernel/include/sha256.h \
		kernel/src/boot_chain.c kernel/src/boot_measurements.c \
		kernel/src/image_signature.c kernel/src/image_signature_ecdsa.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c kernel/src/trust_anchor.c \
		kernel/src/key_use.c kernel/src/sha256.c \
		$(HFOC_VERIFY_HOST_OBJS) \
		tests/boot-chain-vector.c tests/boot-chain-vector.h \
		tests/boot-chain-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_IMAGE_SIGNATURE_HOST_TEST \
		$(HFOC_VERIFY_MBEDTLS_CFLAGS) -Ikernel/include -Itests \
		-c kernel/src/image_signature_ecdsa.c \
		-o build/boot-chain-ecdsa-host.o
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_BOOT_CHAIN_HOST_TEST \
		-Ikernel/include -Itests \
		kernel/src/boot_chain.c kernel/src/boot_measurements.c \
		kernel/src/image_signature.c kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c kernel/src/trust_anchor.c \
		kernel/src/key_use.c kernel/src/sha256.c \
		tests/boot-chain-host.c tests/boot-chain-vector.c \
		build/boot-chain-ecdsa-host.o \
		$(HFOC_VERIFY_HOST_OBJS) \
		-o build/test-boot-chain-host -no-pie
	build/test-boot-chain-host

test-boot-chain-determinism-host:
	bash tests/test-boot-chain-determinism-host.sh

.PHONY: test-boot-attestation-host
test-boot-attestation-host: test-boot-chain-host \
		kernel/include/boot_attestation.h \
		kernel/include/boot_chain.h \
		kernel/include/boot_measurements.h \
		kernel/include/image_signature.h \
		kernel/include/hfoc_verify_config.h \
		kernel/include/hfoc_verify_alloc.h \
		kernel/include/signed_metadata.h \
		kernel/include/verification_policy.h \
		kernel/include/trust_anchor.h kernel/include/key_use.h \
		kernel/include/sha256.h \
		kernel/src/boot_attestation.c kernel/src/boot_chain.c \
		kernel/src/boot_measurements.c kernel/src/image_signature.c \
		kernel/src/image_signature_ecdsa.c \
		kernel/src/signed_metadata.c kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		kernel/src/sha256.c \
		$(HFOC_VERIFY_HOST_OBJS) \
		tests/boot-chain-vector.c tests/boot-chain-vector.h \
		tests/boot-attestation-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_IMAGE_SIGNATURE_HOST_TEST \
		$(HFOC_VERIFY_MBEDTLS_CFLAGS) -Ikernel/include -Itests \
		-c kernel/src/image_signature_ecdsa.c \
		-o build/boot-attestation-ecdsa-host.o
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_BOOT_ATTESTATION_HOST_TEST \
		-Ikernel/include -Itests \
		kernel/src/boot_attestation.c kernel/src/boot_chain.c \
		kernel/src/boot_measurements.c kernel/src/image_signature.c \
		kernel/src/signed_metadata.c kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c kernel/src/key_use.c \
		kernel/src/sha256.c \
		tests/boot-attestation-host.c tests/boot-chain-vector.c \
		build/boot-attestation-ecdsa-host.o \
		$(HFOC_VERIFY_HOST_OBJS) \
		-o build/test-boot-attestation-host -no-pie
	build/test-boot-attestation-host

.PHONY: test-certificate-fail-closed-matrix-host
test-certificate-fail-closed-matrix-host: test-mbedtls-manifest-host \
		test-https-service-descriptor-host
	bash scripts/test-certificate-fail-closed-matrix.sh

.PHONY: test-production-artifact-secret-gate-host
test-production-artifact-secret-gate-host: iso
	bash scripts/test-production-artifact-secret-gate.sh

# ITEM-493: the compiled-in test certificate, checked before it rots.  Every
# HTTPS proof curls the appliance with these bytes; the certificate that expired
# on 2026-09-28 turned every PR red hours after the fact, with the cause only in
# a guest log.  This check requires the embedded certificate to stay valid for a
# year and to match the digests the header declares.
.PHONY: test-mbedtls-secret-validity-host
test-mbedtls-secret-validity-host: kernel/src/hfoc_mbedtls_test_secret.c \
		kernel/include/hfoc_mbedtls_test_secret.h \
		tests/test-mbedtls-secret-validity.sh
	bash tests/test-mbedtls-secret-validity.sh

.PHONY: test-mbedtls-secret-host test-mbedtls-secret-iso
test-mbedtls-secret-iso: $(MBEDTLS_TEST_SECRET_ISO)

test-mbedtls-secret-host: $(MBEDTLS_TEST_SECRET_ISO) iso
	bash tests/test-mbedtls-secret-host.sh

.PHONY: test-mbedtls-bio-host
test-mbedtls-bio-host: kernel/include/hfoc_mbedtls_bio.h \
		kernel/src/hfoc_mbedtls_bio.c tests/mbedtls-bio-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_MBEDTLS_BIO_HOST_TEST \
		-Ithird_party/mbedtls/include -Ikernel/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_config.h"' \
		kernel/src/hfoc_mbedtls_bio.c tests/mbedtls-bio-host.c \
		-o build/test-mbedtls-bio-host -no-pie
	build/test-mbedtls-bio-host

.PHONY: test-tls-handshake-host
test-tls-handshake-host: kernel/include/hfoc_tls_handshake.h \
		kernel/src/hfoc_tls_handshake.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		kernel/src/hfoc_mbedtls_bio.c tests/tls-handshake-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_TLS_HANDSHAKE_HOST_TEST \
		-DHFOC_MBEDTLS_BIO_HOST_TEST \
		-Ithird_party/mbedtls/include -Ikernel/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_config.h"' \
		kernel/src/hfoc_tls_handshake.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		kernel/src/hfoc_mbedtls_bio.c tests/tls-handshake-host.c \
		-o build/test-tls-handshake-host -no-pie
	build/test-tls-handshake-host

.PHONY: test-tls-close-host
test-tls-close-host: kernel/include/hfoc_tls_close.h \
		kernel/src/hfoc_tls_close.c \
		kernel/include/hfoc_tls_handshake.h \
		kernel/src/hfoc_tls_handshake.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		kernel/src/hfoc_mbedtls_bio.c tests/tls-close-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_TLS_CLOSE_HOST_TEST \
		-DHFOC_TLS_HANDSHAKE_HOST_TEST \
		-DHFOC_MBEDTLS_BIO_HOST_TEST \
		-Ithird_party/mbedtls/include -Ikernel/include \
		-DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_config.h"' \
		kernel/src/hfoc_tls_close.c \
		kernel/src/hfoc_tls_handshake.c \
		kernel/src/hfoc_mbedtls_alloc.c \
		kernel/src/hfoc_mbedtls_bio.c tests/tls-close-host.c \
		-o build/test-tls-close-host -no-pie
	build/test-tls-close-host

.PHONY: test-http-parser-state-host
test-http-parser-state-host: kernel/include/http_parser.h \
		kernel/src/http_parser.c tests/http-parser-state-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHTTP_PARSER_HOST_TEST -Ikernel/include \
		kernel/src/http_parser.c tests/http-parser-state-host.c \
		-o build/test-http-parser-state-host -no-pie
	build/test-http-parser-state-host

.PHONY: test-http-request-line-host
test-http-request-line-host: kernel/include/http_parser.h \
		kernel/src/http_parser.c tests/http-request-line-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHTTP_PARSER_HOST_TEST -DHTTP_REQUEST_LINE_HOST_TEST \
		-Ikernel/include \
		kernel/src/http_parser.c tests/http-request-line-host.c \
		-o build/test-http-request-line-host -no-pie
	build/test-http-request-line-host

.PHONY: test-http-header-host
test-http-header-host: kernel/include/http_parser.h \
		kernel/src/http_parser.c tests/http-header-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHTTP_PARSER_HOST_TEST -DHTTP_REQUEST_LINE_HOST_TEST \
		-DHTTP_HEADER_HOST_TEST -Ikernel/include \
		kernel/src/http_parser.c tests/http-header-host.c \
		-o build/test-http-header-host -no-pie
	build/test-http-header-host

.PHONY: test-http-framing-host
test-http-framing-host: kernel/include/http_parser.h \
		kernel/src/http_parser.c tests/http-framing-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHTTP_PARSER_HOST_TEST -DHTTP_REQUEST_LINE_HOST_TEST \
		-DHTTP_HEADER_HOST_TEST -DHTTP_FRAMING_HOST_TEST \
		-Ikernel/include \
		kernel/src/http_parser.c tests/http-framing-host.c \
		-o build/test-http-framing-host -no-pie
	build/test-http-framing-host

.PHONY: test-http-policy-host
test-http-policy-host: kernel/include/http_parser.h \
		kernel/src/http_parser.c tests/http-policy-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHTTP_PARSER_HOST_TEST -DHTTP_REQUEST_LINE_HOST_TEST \
		-DHTTP_HEADER_HOST_TEST -DHTTP_POLICY_HOST_TEST \
		-Ikernel/include \
		kernel/src/http_parser.c tests/http-policy-host.c \
		-o build/test-http-policy-host -no-pie
	build/test-http-policy-host

.PHONY: test-http-route-host
test-http-route-host: kernel/include/http_parser.h \
		kernel/src/http_parser.c tests/http-route-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHTTP_PARSER_HOST_TEST -DHTTP_REQUEST_LINE_HOST_TEST \
		-DHTTP_ROUTES_HOST_TEST -Ikernel/include \
		kernel/src/http_parser.c tests/http-route-host.c \
		-o build/test-http-route-host -no-pie
	build/test-http-route-host

.PHONY: test-http-response-host
test-http-response-host: kernel/include/http_response.h \
		kernel/src/http_response.c tests/http-response-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHTTP_RESPONSE_HOST_TEST -Ikernel/include \
		kernel/src/http_response.c tests/http-response-host.c \
		-o build/test-http-response-host -no-pie
	build/test-http-response-host

.PHONY: test-http-error-matrix-host
test-http-error-matrix-host: kernel/include/http_error_matrix.h \
		kernel/src/http_error_matrix.c tests/http-error-matrix-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHTTP_ERROR_MATRIX_HOST_TEST -Ikernel/include \
		kernel/src/http_error_matrix.c tests/http-error-matrix-host.c \
		-o build/test-http-error-matrix-host -no-pie
	build/test-http-error-matrix-host

.PHONY: test-http-acceptance-host
test-http-acceptance-host: kernel/include/http_parser.h \
		kernel/include/http_response.h kernel/include/http_error_matrix.h \
		kernel/src/http_parser.c kernel/src/http_response.c \
		kernel/src/http_error_matrix.c tests/http-acceptance-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHTTP_PARSER_HOST_TEST \
		-DHTTP_REQUEST_LINE_HOST_TEST \
		-DHTTP_HEADER_HOST_TEST \
		-DHTTP_FRAMING_HOST_TEST \
		-DHTTP_POLICY_HOST_TEST \
		-DHTTP_ROUTES_HOST_TEST \
		-DHTTP_RESPONSE_HOST_TEST \
		-DHTTP_ERROR_MATRIX_HOST_TEST \
		-Ikernel/include \
		kernel/src/http_parser.c kernel/src/http_response.c \
		kernel/src/http_error_matrix.c tests/http-acceptance-host.c \
		-o build/test-http-acceptance-host -no-pie
	build/test-http-acceptance-host

.PHONY: test-http-acceptance-sanitize-host
test-http-acceptance-sanitize-host: kernel/include/http_parser.h \
		kernel/include/http_response.h kernel/include/http_error_matrix.h \
		kernel/src/http_parser.c kernel/src/http_response.c \
		kernel/src/http_error_matrix.c tests/http-acceptance-host.c
	@if printf 'int main(void){return 0;}\n' | \
			$(CC) -x c - -fsanitize=address,undefined \
			-o /tmp/hfoc-http-acceptance-sanitize-probe \
			2>/dev/null; then \
		echo "HTTP acceptance sanitizer probe: OK"; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-fsanitize=address,undefined \
			-DHTTP_PARSER_HOST_TEST \
			-DHTTP_REQUEST_LINE_HOST_TEST \
			-DHTTP_HEADER_HOST_TEST \
			-DHTTP_FRAMING_HOST_TEST \
			-DHTTP_POLICY_HOST_TEST \
			-DHTTP_ROUTES_HOST_TEST \
			-DHTTP_RESPONSE_HOST_TEST \
			-DHTTP_ERROR_MATRIX_HOST_TEST \
			-Ikernel/include \
			kernel/src/http_parser.c kernel/src/http_response.c \
			kernel/src/http_error_matrix.c tests/http-acceptance-host.c \
			-o build/test-http-acceptance-sanitize-host -no-pie; \
		build/test-http-acceptance-sanitize-host; \
	else \
		echo "HTTP acceptance sanitizers: SKIP (compiler does not support address/undefined sanitizers)"; \
	fi

.PHONY: test-lwip-config-host
test-lwip-config-host:
	bash tests/test-lwip-config-host.sh

.PHONY: test-lwip-alloc-host
test-lwip-alloc-host: kernel/include/hfoc_lwip.h kernel/src/hfoc_lwip.c \
		tests/lwip-alloc-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-Ikernel/include \
		kernel/src/hfoc_lwip.c tests/lwip-alloc-host.c \
		-o build/test-lwip-alloc-host -no-pie
	build/test-lwip-alloc-host

.PHONY: test-lwip-time-host
test-lwip-time-host: kernel/include/hfoc_lwip_time.h \
		kernel/src/hfoc_lwip_time.c tests/lwip-time-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_LWIP_TIME_HOST_TEST -Ikernel/include \
		kernel/src/hfoc_lwip_time.c tests/lwip-time-host.c \
		-o build/test-lwip-time-host -no-pie
	build/test-lwip-time-host

# ITEM-605: the production lwIP timeout list.  The test links the real lwIP
# timeout and TCP timer sources and advances them only through the production
# entry point; the old code (no sys_check_timeouts() call) fails it.
.PHONY: test-lwip-timeouts-service-host
test-lwip-timeouts-service-host: kernel/include/hfoc_lwip_timeouts.h \
		kernel/src/hfoc_lwip_timeouts.c \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		tests/lwip-timeouts-service-host.c
	rm -rf build/lwip-timeouts-service-host
	mkdir -p build/lwip-timeouts-service-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-timeouts-service-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TIMEOUTS_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_timeouts.c \
		tests/lwip-timeouts-service-host.c \
		build/lwip-timeouts-service-host/*.o \
		-o build/test-lwip-timeouts-service-host -no-pie
	build/test-lwip-timeouts-service-host

# ITEM-644: the relay's failed-backend error path.  The real relay and the
# real lwIP retry timer are exercised; the target SYN is blackholed and the
# stack-originated ERR_ABRT must fail the accepted client, publish the verdict
# and free the one slot for the next client.
.PHONY: test-lwip-forward-host
test-lwip-forward-host: kernel/include/hfoc_lwip_forward.h \
		kernel/include/hfoc_lwip_timeouts.h \
		kernel/src/hfoc_lwip_forward.c \
		kernel/src/hfoc_lwip_timeouts.c \
		kernel/src/hfoc_lwip.c \
		kernel/src/ipv4_config.c \
		kernel/src/forward_config.c \
		tests/lwip-forward-host.c
	rm -rf build/lwip-forward-host
	mkdir -p build/lwip-forward-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-forward-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_FORWARD_HOST_TEST -DHFOC_LWIP_TIMEOUTS_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip_forward.c \
		kernel/src/forward_config.c \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_timeouts.c \
		tests/lwip-forward-host.c \
		build/lwip-forward-host/*.o \
		-o build/test-lwip-forward-host -no-pie
	build/test-lwip-forward-host

.PHONY: test-lwip-pbuf-host
test-lwip-pbuf-host: kernel/include/hfoc_lwip_pbuf.h \
		kernel/include/packet_pool.h kernel/include/packet_slice.h \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		kernel/src/hfoc_lwip_pbuf.c tests/lwip-pbuf-host.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-DHFOC_LWIP_PBUF_HOST_TEST -Ikernel/include \
		-Ithird_party/lwip/src/include \
		kernel/src/packet_pool.c kernel/src/packet_slice.c \
		kernel/src/hfoc_lwip_pbuf.c tests/lwip-pbuf-host.c \
		-o build/test-lwip-pbuf-host -no-pie
	build/test-lwip-pbuf-host

.PHONY: test-lwip-netif-host
test-lwip-netif-host: kernel/include/hfoc_lwip_netif.h \
		kernel/include/network_backend.h \
		kernel/include/network_ipv4_profile.h \
		kernel/include/virtio_net_link_state.h \
		kernel/src/hfoc_lwip_netif.c \
		kernel/src/network_backend.c \
		kernel/src/network_ipv4_profile.c \
		kernel/src/network_config_transaction.c \
		kernel/src/ipv4_config.c \
		kernel/src/network_device.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_mac_identity.c \
		kernel/src/link_state.c \
		kernel/src/virtio_net_link_state.c \
		tests/lwip-netif-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_NETIF_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip_netif.c \
		kernel/src/network_backend.c \
		kernel/src/network_ipv4_profile.c \
		kernel/src/network_config_transaction.c \
		kernel/src/ipv4_config.c \
		kernel/src/network_device.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_mac_identity.c \
		kernel/src/link_state.c \
		kernel/src/virtio_net_link_state.c \
		tests/lwip-netif-host.c \
		-o build/test-lwip-netif-host -no-pie
	build/test-lwip-netif-host

.PHONY: test-lwip-arp-icmp-host
test-lwip-arp-icmp-host: kernel/include/hfoc_lwip_proto.h \
		kernel/src/hfoc_lwip_proto.c tests/lwip-arp-icmp-host.c
	rm -rf build/lwip-arp-icmp-host
	mkdir -p build/lwip-arp-icmp-host
	for src in $(LWIP_PROTO_SOURCES); do \
		obj=build/lwip-arp-icmp-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_PROTO_HOST_TEST -DHFOC_LWIP_TIME_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/kstring.c kernel/src/hfoc_lwip.c \
		kernel/src/hfoc_lwip_time.c kernel/src/hfoc_lwip_proto.c \
		kernel/src/icmp_reachability.c \
		tests/lwip-arp-icmp-host.c build/lwip-arp-icmp-host/*.o \
		-o build/test-lwip-arp-icmp-host -no-pie
	build/test-lwip-arp-icmp-host

.PHONY: test-appliance-production-network-host
test-appliance-production-network-host: \
		kernel/include/appliance_production_network.h \
		kernel/include/network_backend.h \
		kernel/src/appliance_production_network.c \
		kernel/src/network_backend.c \
		tests/appliance-production-network-host.c
	rm -rf build/appliance-production-network-host
	mkdir -p build/appliance-production-network-host
	for src in $(LWIP_PROTO_SOURCES); do \
		obj=build/appliance-production-network-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PRODUCTION_NETWORK_HOST_TEST \
		-DHFOC_LWIP_PROTO_HOST_TEST -DHFOC_LWIP_TIME_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/kstring.c kernel/src/hfoc_lwip.c \
		kernel/src/hfoc_lwip_time.c kernel/src/hfoc_lwip_netif.c \
		kernel/src/hfoc_lwip_proto.c kernel/src/icmp_reachability.c \
		kernel/src/appliance_production_network.c \
		kernel/src/network_device.c kernel/src/link_state.c \
		kernel/src/network_backend.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_net_link_state.c \
		kernel/src/virtio_net_mac_identity.c \
		kernel/src/network_ipv4_profile.c \
		kernel/src/network_config_transaction.c \
		kernel/src/ipv4_config.c \
		tests/appliance-production-network-host.c \
		build/appliance-production-network-host/*.o \
		-o build/test-appliance-production-network-host -no-pie
	build/test-appliance-production-network-host

.PHONY: test-lwip-tcp-host
test-lwip-tcp-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/src/hfoc_lwip_tcp.c tests/lwip-tcp-host.c
	rm -rf build/lwip-tcp-host
	mkdir -p build/lwip-tcp-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c tests/lwip-tcp-host.c \
		build/lwip-tcp-host/*.o \
		-o build/test-lwip-tcp-host -no-pie
	build/test-lwip-tcp-host

.PHONY: test-lwip-tcp-conn-host
test-lwip-tcp-conn-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		tests/lwip-tcp-conn-host.c
	rm -rf build/lwip-tcp-conn-host
	mkdir -p build/lwip-tcp-conn-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-conn-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST -DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c tests/lwip-tcp-conn-host.c \
		build/lwip-tcp-conn-host/*.o \
		-o build/test-lwip-tcp-conn-host -no-pie
	build/test-lwip-tcp-conn-host

.PHONY: test-lwip-tcp-pool-host
test-lwip-tcp-pool-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		tests/lwip-tcp-pool-host.c
	rm -rf build/lwip-tcp-pool-host
	mkdir -p build/lwip-tcp-pool-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-pool-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST -DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c tests/lwip-tcp-pool-host.c \
		build/lwip-tcp-pool-host/*.o \
		-o build/test-lwip-tcp-pool-host -no-pie
	build/test-lwip-tcp-pool-host

.PHONY: test-lwip-tcp-deadline-host
test-lwip-tcp-deadline-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/include/hfoc_lwip_tcp_deadline.h \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_deadline.c \
		tests/lwip-tcp-deadline-host.c
	rm -rf build/lwip-tcp-deadline-host
	mkdir -p build/lwip-tcp-deadline-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-deadline-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST -DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-DHFOC_LWIP_TCP_DEADLINE_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_deadline.c \
		tests/lwip-tcp-deadline-host.c \
		build/lwip-tcp-deadline-host/*.o \
		-o build/test-lwip-tcp-deadline-host -no-pie
	build/test-lwip-tcp-deadline-host

.PHONY: test-lwip-tcp-backlog-host
test-lwip-tcp-backlog-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_backlog.h \
		kernel/src/hfoc_lwip_tcp_backlog.c \
		tests/lwip-tcp-backlog-host.c
	rm -rf build/lwip-tcp-backlog-host
	mkdir -p build/lwip-tcp-backlog-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-backlog-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_BACKLOG_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c \
		kernel/src/hfoc_lwip_tcp_backlog.c \
		tests/lwip-tcp-backlog-host.c \
		build/lwip-tcp-backlog-host/*.o \
		-o build/test-lwip-tcp-backlog-host -no-pie
	build/test-lwip-tcp-backlog-host

.PHONY: test-lwip-tcp-rx-backpressure-host
test-lwip-tcp-rx-backpressure-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/include/hfoc_lwip_tcp_rx_backpressure.h \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_rx_backpressure.c \
		tests/lwip-tcp-rx-backpressure-host.c
	rm -rf build/lwip-tcp-rx-backpressure-host
	mkdir -p build/lwip-tcp-rx-backpressure-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-rx-backpressure-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_RX_BACKPRESSURE_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_rx_backpressure.c \
		tests/lwip-tcp-rx-backpressure-host.c \
		build/lwip-tcp-rx-backpressure-host/*.o \
		-o build/test-lwip-tcp-rx-backpressure-host -no-pie
	build/test-lwip-tcp-rx-backpressure-host

.PHONY: test-lwip-tcp-tx-backpressure-host
test-lwip-tcp-tx-backpressure-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/include/hfoc_lwip_tcp_tx_backpressure.h \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_tx_backpressure.c \
		tests/lwip-tcp-tx-backpressure-host.c
	rm -rf build/lwip-tcp-tx-backpressure-host
	mkdir -p build/lwip-tcp-tx-backpressure-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-tx-backpressure-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_TX_BACKPRESSURE_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_tx_backpressure.c \
		tests/lwip-tcp-tx-backpressure-host.c \
		build/lwip-tcp-tx-backpressure-host/*.o \
		-o build/test-lwip-tcp-tx-backpressure-host -no-pie
	build/test-lwip-tcp-tx-backpressure-host

.PHONY: test-lwip-tcp-quarantine-host
test-lwip-tcp-quarantine-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/include/hfoc_lwip_tcp_quarantine.h \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_quarantine.c \
		tests/lwip-tcp-quarantine-host.c
	rm -rf build/lwip-tcp-quarantine-host
	mkdir -p build/lwip-tcp-quarantine-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-quarantine-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-DHFOC_LWIP_TCP_QUARANTINE_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_quarantine.c \
		tests/lwip-tcp-quarantine-host.c \
		build/lwip-tcp-quarantine-host/*.o \
		-o build/test-lwip-tcp-quarantine-host -no-pie
	build/test-lwip-tcp-quarantine-host

.PHONY: test-lwip-tcp-isolation-host
test-lwip-tcp-isolation-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/include/hfoc_lwip_tcp_quarantine.h \
		kernel/include/hfoc_lwip_tcp_isolation.h \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_quarantine.c \
		kernel/src/hfoc_lwip_tcp_isolation.c \
		tests/lwip-tcp-isolation-host.c
	rm -rf build/lwip-tcp-isolation-host
	mkdir -p build/lwip-tcp-isolation-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-isolation-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-DHFOC_LWIP_TCP_QUARANTINE_HOST_TEST \
		-DHFOC_LWIP_TCP_ISOLATION_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_quarantine.c \
		kernel/src/hfoc_lwip_tcp_isolation.c \
		tests/lwip-tcp-isolation-host.c \
		build/lwip-tcp-isolation-host/*.o \
		-o build/test-lwip-tcp-isolation-host -no-pie
	build/test-lwip-tcp-isolation-host

.PHONY: test-lwip-tcp-close-policy-host
test-lwip-tcp-close-policy-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/include/hfoc_lwip_tcp_quarantine.h \
		kernel/include/hfoc_lwip_tcp_close_policy.h \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_quarantine.c \
		kernel/src/hfoc_lwip_tcp_close_policy.c \
		tests/lwip-tcp-close-policy-host.c
	rm -rf build/lwip-tcp-close-policy-host
	mkdir -p build/lwip-tcp-close-policy-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-close-policy-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-DHFOC_LWIP_TCP_QUARANTINE_HOST_TEST \
		-DHFOC_LWIP_TCP_CLOSE_POLICY_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_quarantine.c \
		kernel/src/hfoc_lwip_tcp_close_policy.c \
		tests/lwip-tcp-close-policy-host.c \
		build/lwip-tcp-close-policy-host/*.o \
		-o build/test-lwip-tcp-close-policy-host -no-pie
	build/test-lwip-tcp-close-policy-host

.PHONY: test-lwip-tcp-drain-restart-host
test-lwip-tcp-drain-restart-host: kernel/include/hfoc_lwip_tcp.h \
		kernel/include/hfoc_lwip_tcp_conn.h \
		kernel/include/hfoc_lwip_tcp_pool.h \
		kernel/include/hfoc_lwip_tcp_deadline.h \
		kernel/include/hfoc_lwip_tcp_backlog.h \
		kernel/include/hfoc_lwip_tcp_quarantine.h \
		kernel/include/hfoc_lwip_tcp_close_policy.h \
		kernel/include/hfoc_lwip_tcp_drain_restart.h \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_deadline.c \
		kernel/src/hfoc_lwip_tcp_backlog.c \
		kernel/src/hfoc_lwip_tcp_quarantine.c \
		kernel/src/hfoc_lwip_tcp_close_policy.c \
		kernel/src/hfoc_lwip_tcp_drain_restart.c \
		kernel/src/hfoc_lwip_netif.c \
		kernel/src/network_ipv4_profile.c \
		kernel/src/network_config_transaction.c \
		kernel/src/ipv4_config.c \
		kernel/src/network_device.c \
		kernel/src/network_backend.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_mac_identity.c \
		kernel/src/link_state.c \
		kernel/src/virtio_net_link_state.c \
		tests/lwip-tcp-drain-restart-host.c
	rm -rf build/lwip-tcp-drain-restart-host
	mkdir -p build/lwip-tcp-drain-restart-host
	for src in $(LWIP_TCP_SOURCES); do \
		obj=build/lwip-tcp-drain-restart-host/$$(basename "$${src%.*}").o; \
		$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
			-ffreestanding -fno-pic -fno-pie \
			-Ikernel/include -Ithird_party/lwip/src/include \
			-c "$$src" -o "$$obj" || exit 1; \
	done
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_LWIP_TCP_HOST_TEST \
		-DHFOC_LWIP_TCP_CONN_HOST_TEST \
		-DHFOC_LWIP_TCP_POOL_HOST_TEST \
		-DHFOC_LWIP_TCP_DEADLINE_HOST_TEST \
		-DHFOC_LWIP_TCP_BACKLOG_HOST_TEST \
		-DHFOC_LWIP_TCP_QUARANTINE_HOST_TEST \
		-DHFOC_LWIP_TCP_CLOSE_POLICY_HOST_TEST \
		-DHFOC_LWIP_TCP_DRAIN_RESTART_HOST_TEST \
		-Ikernel/include -Ithird_party/lwip/src/include \
		kernel/src/hfoc_lwip.c kernel/src/ipv4_config.c \
		kernel/src/hfoc_lwip_tcp.c \
		kernel/src/hfoc_lwip_tcp_conn.c \
		kernel/src/hfoc_lwip_tcp_pool.c \
		kernel/src/hfoc_lwip_tcp_deadline.c \
		kernel/src/hfoc_lwip_tcp_backlog.c \
		kernel/src/hfoc_lwip_tcp_quarantine.c \
		kernel/src/hfoc_lwip_tcp_close_policy.c \
		kernel/src/hfoc_lwip_tcp_drain_restart.c \
		kernel/src/hfoc_lwip_netif.c \
		kernel/src/network_ipv4_profile.c \
		kernel/src/network_config_transaction.c \
		kernel/src/network_device.c \
		kernel/src/network_backend.c \
		kernel/src/virtio_net_feature_profile.c \
		kernel/src/virtio_pci_identity.c \
		kernel/src/virtio_net_mac_identity.c \
		kernel/src/link_state.c \
		kernel/src/virtio_net_link_state.c \
		tests/lwip-tcp-drain-restart-host.c \
		build/lwip-tcp-drain-restart-host/*.o \
		-o build/test-lwip-tcp-drain-restart-host -no-pie
	build/test-lwip-tcp-drain-restart-host

# Bounded host suite manifest. The serial recipe below is the canonical 418-target
# source; phase variables classify only scheduling, never the target set.
HOST_TEST_TARGETS := $(shell sed -n '/^test-host-suite-serial:/,/^[^[:space:]]/p' Makefile | sed -n 's/^[[:space:]]*$$(MAKE) //p')
# ITEM-582: `make check` is the suite's first serial target, so a standalone
# `make test-host-suite` runs the static check before the tests.  The CI
# static-host phase already ran `make check` immediately before the suite and
# passed `HOST_SUITE_CHECK=0`, which drops exactly that duplicate; the default
# keeps the standalone behavior unchanged.
HOST_SERIAL_TARGETS := check
HOST_SUITE_CHECK ?= 1
# ITEM-485: `test-appliance-boot-log-reader-host` runs the store proof as its
# prerequisite and reads the image it leaves behind, so it is scheduled with
# the serial gate phase, after the parallel phase.  In the parallel phase its
# own store run and the standalone `test-appliance-boot-log-store-host` target
# wrote `build/test-appliance-boot-log-host.img` at the same time, which
# corrupted the image the store's own assertions read (run 36230645539).
HOST_GATE_TARGETS := \
	test-certificate-fail-closed-matrix-host \
	test-production-artifact-secret-gate-host \
	test-mbedtls-secret-host \
	test-appliance-boot-log-reader-host
HOST_PARALLEL_PHASE_1 := $(filter-out $(HOST_SERIAL_TARGETS) $(HOST_GATE_TARGETS), $(HOST_TEST_TARGETS))

.PHONY: test-host-suite test-host-suite-serial test-host-suite-runner test-host-suite-manifest test-item-301-audit-host test-item-319-authentication-architecture-host test-item-329-update-package-architecture-host test-item-338-installer-architecture-audit-host test-appliance-identity-host test-appliance-persistent-config-host test-appliance-config-validation-host test-appliance-panel-gate-host test-appliance-console-host test-appliance-network-config-host test-appliance-network-diagnostics-host test-appliance-dhcp-client-host test-appliance-dns-resolver-host test-appliance-service-manager-host test-appliance-https-service-host test-appliance-production-service-https-host test-appliance-production-service-activation-host test-appliance-production-management-host test-appliance-production-management-plane-host test-appliance-production-mutation-plane-host test-appliance-production-update-plane-host test-appliance-production-recovery-executor-host test-appliance-production-auth-host test-appliance-first-boot-integration-host test-appliance-recovery-factory-reset-integration-host test-appliance-runtime-log-host test-appliance-health-api-host host-suite-phase-list
.PHONY: test-appliance-management-api-host
.PHONY: test-appliance-system-api-host
.PHONY: test-appliance-network-api-host
.PHONY: test-appliance-service-api-host
.PHONY: test-appliance-persistent-network-config-host
.PHONY: test-appliance-persistent-service-config-host
.PHONY: test-appliance-state-layout-host
.PHONY: test-appliance-state-block-device-host
.PHONY: test-appliance-state-loader-host
.PHONY: test-appliance-service-failure-handler-host
.PHONY: test-acceptance-suite-runner test-acceptance-suite-manifest
test-item-301-audit-host:
	bash tests/test-item-301-audit.sh
test-item-319-authentication-architecture-host:
	bash tests/test-item-319-authentication-architecture.sh
test-item-329-update-package-architecture-host:
	bash tests/test-item-329-update-package-architecture.sh
test-item-338-installer-architecture-audit-host:
	bash tests/test-item-338-installer-architecture-audit.sh

test-host-suite:
	bash scripts/run-host-suite.sh \
		$(if $(filter 1,$(HOST_SUITE_CHECK)),--serial $(HOST_SERIAL_TARGETS)) \
		--parallel $(HOST_PARALLEL_PHASE_1) \
		--serial test-certificate-fail-closed-matrix-host \
		--serial test-production-artifact-secret-gate-host \
		--serial test-mbedtls-secret-host \
		--serial test-appliance-boot-log-reader-host

.PHONY: test-installer-hardware-discovery-host
test-installer-hardware-discovery-host: kernel/include/installer_hardware_discovery.h \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-hardware-discovery-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-hardware-discovery-host.c \
		-o build/test-installer-hardware-discovery-host -no-pie
	build/test-installer-hardware-discovery-host

.PHONY: test-installer-layout-host
test-installer-layout-host: kernel/include/installer_layout.h \
		kernel/src/installer_layout.c \
		kernel/include/installer_hardware_discovery.h \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-layout-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/installer_layout.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-layout-host.c \
		-o build/test-installer-layout-host -no-pie
	build/test-installer-layout-host

.PHONY: test-installer-gpt-preserve-host
test-installer-gpt-preserve-host: \
		kernel/include/installer_gpt_preserve.h \
		kernel/src/installer_gpt_preserve.c \
		kernel/include/installer_layout.h \
		kernel/src/installer_layout.c \
		kernel/src/block_device.c \
		tests/installer-gpt-preserve-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INSTALLER_GPT_PRESERVE_HOST_TEST \
		-Ikernel/include \
		kernel/src/installer_gpt_preserve.c \
		kernel/src/installer_layout.c \
		kernel/src/block_device.c \
		tests/installer-gpt-preserve-host.c \
		-o build/test-installer-gpt-preserve-host -no-pie
	build/test-installer-gpt-preserve-host

.PHONY: test-installer-layout-contract-host
test-installer-layout-contract-host: \
		kernel/include/installer_layout_contract.h \
		kernel/src/installer_layout_contract.c \
		kernel/include/installer_layout.h \
		kernel/src/installer_layout.c \
		kernel/include/installer_dual_boot.h \
		kernel/src/installer_dual_boot.c \
		kernel/include/installer_hardware_discovery.h \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-layout-contract-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INSTALLER_LAYOUT_CONTRACT_HOST_TEST \
		-Ikernel/include \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-layout-contract-host.c \
		-o build/test-installer-layout-contract-host -no-pie
	build/test-installer-layout-contract-host

.PHONY: test-installer-dual-boot-host
test-installer-dual-boot-host: kernel/include/installer_dual_boot.h \
		kernel/src/installer_dual_boot.c \
		kernel/include/installer_hardware_discovery.h \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-dual-boot-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		tests/installer-dual-boot-host.c \
		-o build/test-installer-dual-boot-host -no-pie
	build/test-installer-dual-boot-host

.PHONY: test-installer-boot-recovery-host
test-installer-boot-recovery-host: kernel/include/installer_boot_recovery.h \
		kernel/src/installer_boot_recovery.c \
		kernel/include/installer_hardware_discovery.h \
		tests/installer-boot-recovery-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/installer_boot_recovery.c \
		tests/installer-boot-recovery-host.c \
		-o build/test-installer-boot-recovery-host -no-pie
	build/test-installer-boot-recovery-host

# ITEM-517: the generated menu on a legacy-BIOS machine.  The scanner and the
# builder run against a synthetic FAT32 ESP, so the UEFI configuration and
# record are pinned byte for byte next to the BIOS vector that records every
# found system and disk NO_ESP instead of offering an EFI chainload entry.
.PHONY: test-installer-boot-menu-host
test-installer-boot-menu-host: kernel/include/installer_boot_menu.h \
		kernel/include/installer_gpt_preserve.h \
		kernel/include/installer_layout.h \
		kernel/include/efi_fat32_esp.h \
		kernel/include/efi_boot_environment.h \
		kernel/include/installer_hardware_discovery.h \
		kernel/src/installer_boot_menu.c \
		kernel/src/installer_gpt_preserve.c \
		kernel/src/installer_layout.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		kernel/src/installer_hardware_discovery.c \
		tests/installer-boot-menu-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INSTALLER_BOOT_MENU_HOST_TEST \
		-Ikernel/include \
		kernel/src/installer_boot_menu.c \
		kernel/src/installer_gpt_preserve.c \
		kernel/src/installer_layout.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/block_device.c \
		kernel/src/sha256.c \
		kernel/src/installer_hardware_discovery.c \
		tests/installer-boot-menu-host.c \
		-o build/test-installer-boot-menu-host -no-pie
	build/test-installer-boot-menu-host

.PHONY: test-installer-first-boot-wizard-host
test-installer-first-boot-wizard-host: kernel/include/installer_first_boot_wizard.h \
		kernel/src/installer_first_boot_wizard.c \
		kernel/include/installer_hardware_discovery.h \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		kernel/include/appliance_language.h \
		kernel/include/appliance_config_validation.h \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/include/appliance_network_config.h \
		kernel/include/ipv4_config.h \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		kernel/include/appliance_secret_protection.h \
		tests/installer-first-boot-wizard-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/installer_first_boot_wizard.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/block_device.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_network_config.c \
		kernel/src/ipv4_config.c \
		tests/installer-first-boot-wizard-host.c \
		-o build/test-installer-first-boot-wizard-host -no-pie
	build/test-installer-first-boot-wizard-host

.PHONY: test-production-install-media-host
# ITEM-492: the discovery half.  The PCI walk is driven over a fake
# configuration space, so "finds the class, not the vendor", the unsupported
# device and the multi-device machine are proven without a machine.
.PHONY: test-nic-discovery-host
test-nic-discovery-host: kernel/include/nic_discovery.h \
		kernel/include/nic_registry.h \
		kernel/include/pci.h \
		kernel/src/nic_discovery.c \
		kernel/src/nic_registry.c \
		kernel/src/pci.c \
		tests/nic-discovery-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_NIC_DISCOVERY_HOST_TEST \
		-Ikernel/include \
		kernel/src/nic_discovery.c kernel/src/nic_registry.c \
		kernel/src/pci.c tests/nic-discovery-host.c \
		-o build/test-nic-discovery-host -no-pie
	build/test-nic-discovery-host

# ITEM-492: the NIC registry and the unified device description.  Pure values, so
# matching, the states, the records and the bounded refusals are proven on the
# host, case by case, including the cases a machine hides (no driver, a driver
# that refused, no device at all).
.PHONY: test-nic-registry-host
test-nic-registry-host: kernel/include/nic_registry.h \
		kernel/src/nic_registry.c \
		tests/nic-registry-host.c
	@mkdir -p build
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-Ikernel/include \
		kernel/src/nic_registry.c tests/nic-registry-host.c \
		-o build/test-nic-registry-host -no-pie
	build/test-nic-registry-host

test-production-install-media-host: \
		kernel/include/production_install_media.h \
		kernel/include/appliance_language.h \
		kernel/src/appliance_language.c \
		kernel/src/production_install_media.c \
		kernel/include/installer_hardware_discovery.h \
		kernel/include/ipv4_config.h \
		kernel/src/ipv4_config.c \
		kernel/include/forward_config.h \
		kernel/src/forward_config.c \
		tests/production-install-media-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PRODUCTION_INSTALL_MEDIA_HOST_TEST \
		-Ikernel/include \
		kernel/src/production_install_media.c \
		kernel/src/appliance_language.c \
		kernel/src/ipv4_config.c \
		kernel/src/forward_config.c \
		tests/production-install-media-host.c \
		-o build/test-production-install-media-host -no-pie
	build/test-production-install-media-host

.PHONY: test-production-installer-host
test-production-installer-host: kernel/include/production_installer.h \
		kernel/include/installer_layout.h \
		kernel/include/installer_hardware_discovery.h \
		kernel/include/installer_media_verification.h \
		kernel/include/installer_supported_hardware.h \
		kernel/include/signed_metadata.h \
		kernel/include/verification_policy.h \
		kernel/include/trust_anchor.h \
		kernel/include/key_use.h \
		kernel/include/sha256.h \
		kernel/src/production_installer.c \
		kernel/src/installer_layout.c \
		kernel/include/installer_layout_contract.h \
		kernel/src/installer_layout_contract.c \
		kernel/include/installer_dual_boot.h \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/sha256.c \
		kernel/src/block_device.c \
		tests/production-installer-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PRODUCTION_INSTALLER_HOST_TEST \
		-Ikernel/include \
		kernel/src/production_installer.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/sha256.c \
		kernel/src/block_device.c \
		tests/production-installer-host.c \
		-o build/test-production-installer-host -no-pie
	build/test-production-installer-host

.PHONY: test-installer-hardening-host
test-installer-hardening-host: \
		kernel/include/installer_media_verification.h \
		kernel/include/installer_supported_hardware.h \
		kernel/include/installer_uninstall.h \
		kernel/include/production_installer.h \
		kernel/include/installer_layout.h \
		kernel/include/installer_hardware_discovery.h \
		kernel/include/signed_metadata.h \
		kernel/include/verification_policy.h \
		kernel/include/trust_anchor.h \
		kernel/include/key_use.h \
		kernel/include/sha256.h \
		kernel/include/block_device.h \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/installer_uninstall.c \
		kernel/src/production_installer.c \
		kernel/src/installer_layout.c \
		kernel/include/installer_layout_contract.h \
		kernel/src/installer_layout_contract.c \
		kernel/include/installer_dual_boot.h \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/sha256.c \
		kernel/src/block_device.c \
		tests/installer-hardening-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INSTALLER_HARDENING_HOST_TEST \
		-Ikernel/include \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/installer_uninstall.c \
		kernel/src/production_installer.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/sha256.c \
		kernel/src/block_device.c \
		tests/installer-hardening-host.c \
		-o build/test-installer-hardening-host -no-pie
	build/test-installer-hardening-host

.PHONY: test-installed-boot-disk-host
test-installed-boot-disk-host: \
		kernel/include/production_installer.h \
		kernel/include/production_installer_boot_stage.h \
		kernel/include/production_installer_esp.h \
		kernel/include/production_installer_state.h \
		kernel/include/efi_fat32_esp.h \
		kernel/include/installer_layout_contract.h \
		kernel/include/appliance_state_layout.h \
		kernel/src/production_installer_boot_stage.c \
		kernel/src/production_installer_esp.c \
		kernel/src/production_installer.c \
		kernel/src/production_installer_efi.c \
		kernel/src/production_installer_state.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/block_device.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/sha256.c \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/installed-boot-disk-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_INSTALLED_BOOT_DISK_HOST_TEST \
		-Ikernel/include \
		kernel/src/production_installer_boot_stage.c \
		kernel/src/production_installer_esp.c \
		kernel/src/production_installer.c \
		kernel/src/production_installer_efi.c \
		kernel/src/production_installer_state.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/block_device.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/sha256.c \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/installed-boot-disk-host.c \
		-o build/test-installed-boot-disk-host -no-pie
	build/test-installed-boot-disk-host

.PHONY: test-production-installer-boot-stage-host
test-production-installer-boot-stage-host: \
		kernel/include/production_installer_boot_stage.h \
		kernel/include/production_installer.h \
		kernel/include/production_installer_esp.h \
		kernel/include/efi_fat32_esp.h \
		kernel/include/installer_layout_contract.h \
		kernel/src/production_installer_boot_stage.c \
		kernel/src/production_installer_esp.c \
		kernel/src/production_installer.c \
		kernel/src/production_installer_efi.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/block_device.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/sha256.c \
		tests/production-installer-boot-stage-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PRODUCTION_INSTALLER_BOOT_STAGE_HOST_TEST \
		-Ikernel/include \
		kernel/src/production_installer_boot_stage.c \
		kernel/src/production_installer_esp.c \
		kernel/src/production_installer.c \
		kernel/src/production_installer_efi.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/block_device.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/sha256.c \
		tests/production-installer-boot-stage-host.c \
		-o build/test-production-installer-boot-stage-host -no-pie
	build/test-production-installer-boot-stage-host

.PHONY: test-production-installer-bios-boot-host
test-production-installer-bios-boot-host: \
		kernel/include/production_installer_bios_boot.h \
		kernel/include/production_installer_boot_stage.h \
		kernel/include/production_installer.h \
		kernel/include/production_installer_esp.h \
		kernel/include/efi_fat32_esp.h \
		kernel/include/installer_layout_contract.h \
		kernel/include/installer_gpt_preserve.h \
		kernel/src/production_installer_bios_boot.c \
		kernel/src/production_installer_bios_blob.c \
		kernel/src/production_installer_boot_stage.c \
		kernel/src/production_installer_esp.c \
		kernel/src/production_installer.c \
		kernel/src/production_installer_efi.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/installer_gpt_preserve.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/block_device.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/sha256.c \
		$(PRODUCTION_INSTALLER_LIMINE_BIOS_SYS_OBJ) \
		deps/limine/limine-bios-hdd.h \
		tests/production-installer-bios-boot-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PRODUCTION_INSTALLER_BIOS_BOOT_HOST_TEST \
		-DHFOC_PRODUCTION_INSTALL_MEDIA \
		-Ikernel/include -Ideps/limine \
		kernel/src/production_installer_bios_boot.c \
		kernel/src/production_installer_bios_blob.c \
		kernel/src/production_installer_boot_stage.c \
		kernel/src/production_installer_esp.c \
		kernel/src/production_installer.c \
		kernel/src/production_installer_efi.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/installer_gpt_preserve.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/block_device.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/sha256.c \
		$(PRODUCTION_INSTALLER_LIMINE_BIOS_SYS_OBJ) \
		tests/production-installer-bios-boot-host.c \
		-o build/test-production-installer-bios-boot-host -no-pie
	build/test-production-installer-bios-boot-host

.PHONY: test-production-installer-orchestration-host
test-production-installer-orchestration-host: \
		kernel/include/production_installer.h \
		kernel/include/production_installer_esp.h \
		kernel/include/production_installer_state.h \
		kernel/include/production_installer_efi.h \
		kernel/include/installer_layout.h \
		kernel/include/installer_hardware_discovery.h \
		kernel/include/installer_media_verification.h \
		kernel/include/installer_supported_hardware.h \
		kernel/include/signed_metadata.h \
		kernel/include/verification_policy.h \
		kernel/include/trust_anchor.h \
		kernel/include/key_use.h \
		kernel/include/efi_boot_environment.h \
		kernel/include/efi_boot_variables.h \
		kernel/include/efi_fat32_esp.h \
		kernel/include/appliance_state_layout.h \
		kernel/include/appliance_state_loader.h \
		kernel/src/production_installer.c \
		kernel/src/production_installer_esp.c \
		kernel/src/production_installer_state.c \
		kernel/src/production_installer_efi.c \
		kernel/src/installer_layout.c \
		kernel/include/installer_layout_contract.h \
		kernel/src/installer_layout_contract.c \
		kernel/include/installer_dual_boot.h \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/block_device.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/production-installer-orchestration-host.c
	$(CC) -std=gnu11 -O0 -g -Wall -Wextra -Wpedantic \
		-ffreestanding -fno-pic -fno-pie \
		-DHFOC_PRODUCTION_INSTALLER_ORCHESTRATION_HOST_TEST \
		-Ikernel/include \
		kernel/src/production_installer.c \
		kernel/src/production_installer_esp.c \
		kernel/src/production_installer_state.c \
		kernel/src/production_installer_efi.c \
		kernel/src/installer_layout.c \
		kernel/src/installer_layout_contract.c \
		kernel/src/installer_dual_boot.c \
		kernel/src/installer_hardware_discovery.c \
		kernel/src/installer_media_verification.c \
		kernel/src/installer_supported_hardware.c \
		kernel/src/signed_metadata.c \
		kernel/src/verification_policy.c \
		kernel/src/trust_anchor.c \
		kernel/src/key_use.c \
		kernel/src/block_device.c \
		kernel/src/efi_boot_environment.c \
		kernel/src/efi_boot_variables.c \
		kernel/src/efi_fat32_esp.c \
		kernel/src/appliance_state_loader.c \
		kernel/src/appliance_state_layout.c \
		kernel/src/appliance_state_block_device.c \
		kernel/src/appliance_gpt_discovery.c \
		kernel/src/data_region.c \
		kernel/src/config_journal.c \
		kernel/src/sha256.c \
		kernel/src/appliance_language.c \
		kernel/src/appliance_config_validation.c \
		kernel/src/appliance_persistent_config.c \
		kernel/src/ipv4_config.c \
		kernel/src/appliance_network_config.c \
		kernel/src/appliance_persistent_network_config.c \
		kernel/src/appliance_persistent_secret_config.c \
		kernel/src/appliance_admin_credential.c \
		kernel/src/appliance_secret_persistence.c \
		kernel/src/appliance_persistent_storage_namespace.c \
		kernel/src/service.c \
		kernel/src/service_health.c \
		kernel/src/appliance_service_manager.c \
		kernel/src/appliance_persistent_service_config.c \
		tests/production-installer-orchestration-host.c \
		-o build/test-production-installer-orchestration-host -no-pie
	build/test-production-installer-orchestration-host

test-host-suite-serial:
	$(MAKE) test-fetch-limine-host
	$(MAKE) test-licensing-policy-host
	$(MAKE) test-third-party-license-docs-host
	$(MAKE) test-license-payload-host
	$(MAKE) test-trademark-policy-host
	$(MAKE) test-lwip-source-lock-host
	$(MAKE) test-mbedtls-source-lock-host
	$(MAKE) test-mbedtls-config-host
	$(MAKE) test-mbedtls-alloc-host
	$(MAKE) test-mbedtls-drbg-host
	$(MAKE) test-mbedtls-manifest-host
	$(MAKE) test-certificate-fail-closed-matrix-host
	$(MAKE) test-production-artifact-secret-gate-host
	$(MAKE) test-mbedtls-bio-host
	$(MAKE) test-tls-handshake-host
	$(MAKE) test-tls-close-host
	$(MAKE) test-http-parser-state-host
	$(MAKE) test-http-request-line-host
	$(MAKE) test-http-header-host
	$(MAKE) test-http-framing-host
	$(MAKE) test-http-policy-host
	$(MAKE) test-http-route-host
	$(MAKE) test-http-response-host
	$(MAKE) test-http-error-matrix-host
	$(MAKE) test-http-acceptance-host
	$(MAKE) test-http-acceptance-sanitize-host
	$(MAKE) test-https-service-descriptor-host
	$(MAKE) test-https-admission-host
	$(MAKE) test-https-connection-host
	$(MAKE) test-https-data-flow-host
	$(MAKE) test-https-handler-host
	$(MAKE) test-https-health-host
	$(MAKE) test-https-peer-throttle-host
	$(MAKE) test-https-service-lifecycle-host
	$(MAKE) test-lwip-config-host
	$(MAKE) test-lwip-alloc-host
	$(MAKE) test-lwip-time-host
	$(MAKE) test-lwip-timeouts-service-host
	$(MAKE) test-lwip-forward-host
	$(MAKE) test-lwip-pbuf-host
	$(MAKE) test-lwip-netif-host
	$(MAKE) test-lwip-arp-icmp-host
	$(MAKE) test-appliance-production-network-host
	$(MAKE) test-lwip-tcp-host
	$(MAKE) test-lwip-tcp-conn-host
	$(MAKE) test-lwip-tcp-pool-host
	$(MAKE) test-lwip-tcp-deadline-host
	$(MAKE) test-lwip-tcp-backlog-host
	$(MAKE) test-lwip-tcp-rx-backpressure-host
	$(MAKE) test-lwip-tcp-tx-backpressure-host
	$(MAKE) test-lwip-tcp-quarantine-host
	$(MAKE) test-lwip-tcp-isolation-host
	$(MAKE) test-lwip-tcp-close-policy-host
	$(MAKE) test-lwip-tcp-drain-restart-host
	$(MAKE) check
	$(MAKE) test-heavy-lock-host
	$(MAKE) test-item-301-audit-host
	$(MAKE) test-item-319-authentication-architecture-host
	$(MAKE) test-item-329-update-package-architecture-host
	$(MAKE) test-item-338-installer-architecture-audit-host
	$(MAKE) test-appliance-admin-auth-host
	$(MAKE) test-appliance-management-authorization-host
	$(MAKE) test-appliance-secret-protection-host
	$(MAKE) test-appliance-persistent-storage-namespace-host
	$(MAKE) test-appliance-config-integrity-host
	$(MAKE) test-appliance-secret-persistence-host
	$(MAKE) test-appliance-identity-host
	$(MAKE) test-appliance-persistent-config-host
	$(MAKE) test-appliance-config-validation-host
	$(MAKE) test-appliance-panel-gate-host
	$(MAKE) test-appliance-console-host
	$(MAKE) test-appliance-boot-console-host
	$(MAKE) test-appliance-management-web-ui-host
	$(MAKE) test-appliance-management-panel-host
	$(MAKE) test-appliance-panel-forms-host
	$(MAKE) test-appliance-production-panel-view-host
	$(MAKE) test-appliance-license-inventory-host
	$(MAKE) test-appliance-network-config-host
	$(MAKE) test-appliance-network-mode-host
	$(MAKE) test-appliance-management-tls-host
	$(MAKE) test-appliance-management-reachability-host
	$(MAKE) test-appliance-update-record-host
	$(MAKE) test-appliance-update-bundle-host
	$(MAKE) test-appliance-update-lifecycle-host
	$(MAKE) test-appliance-time-sync-host
	$(MAKE) test-appliance-persistent-network-config-host
	$(MAKE) test-appliance-persistent-service-config-host
	$(MAKE) test-appliance-state-layout-host
	$(MAKE) test-appliance-state-block-device-host
	$(MAKE) test-appliance-state-loader-host
	$(MAKE) test-appliance-host-seed-host
	$(MAKE) test-host-seed-provision-host
	$(MAKE) test-state-encryption-host
	$(MAKE) test-appliance-production-boot-host
	$(MAKE) test-efi-boot-environment-host
	$(MAKE) test-efi-fat32-esp-host
	$(MAKE) test-appliance-boot-log-host
	$(MAKE) test-appliance-boot-log-store-host
	$(MAKE) test-appliance-boot-log-reader-host
	$(MAKE) test-efi-fat32-esp-payload-set-host
	$(MAKE) test-efi-boot-variables-host
	$(MAKE) test-hfoc-efi-installer-host
	$(MAKE) test-appliance-network-diagnostics-host
	$(MAKE) test-appliance-dhcp-client-host
	$(MAKE) test-appliance-dns-resolver-host
	$(MAKE) test-appliance-network-lifecycle-host
	$(MAKE) test-appliance-network-wire-host
	$(MAKE) test-appliance-production-static-boot-host
	$(MAKE) test-ssh-interop-host
	$(MAKE) test-ssh-transport-window-host
	$(MAKE) test-ssh-shell-host
	$(MAKE) test-ssh-adversarial-host
	$(MAKE) test-appliance-ssh-identity-host
	$(MAKE) test-appliance-ssh-command-host
	$(MAKE) test-appliance-production-service-https-host
	$(MAKE) test-appliance-production-service-activation-host
	$(MAKE) test-appliance-production-management-host
	$(MAKE) test-appliance-production-management-api-host
	$(MAKE) test-appliance-production-management-plane-host
	$(MAKE) test-appliance-production-mutation-plane-host
	$(MAKE) test-appliance-production-update-plane-host
	$(MAKE) test-appliance-production-recovery-executor-host
	$(MAKE) test-appliance-production-auth-host
	$(MAKE) test-appliance-production-admin-enrollment-host
	$(MAKE) test-appliance-admin-credential-host
	$(MAKE) test-appliance-admin-credentials-prompt-host
	$(MAKE) test-installer-credential-screen-host
	$(MAKE) test-appliance-first-boot-integration-host
	$(MAKE) test-appliance-production-first-boot-host
	$(MAKE) test-appliance-recovery-factory-reset-integration-host
	$(MAKE) test-appliance-service-manager-host
	$(MAKE) test-appliance-service-failure-handler-host
	$(MAKE) test-appliance-management-api-host
	$(MAKE) test-appliance-system-api-host
	$(MAKE) test-appliance-network-api-host
	$(MAKE) test-appliance-service-api-host
	$(MAKE) test-appliance-https-service-host
	$(MAKE) test-mbedtls-secret-host
	$(MAKE) test-packet-pool-host
	$(MAKE) test-packet-lifecycle-host
	$(MAKE) test-packet-slice-host
	$(MAKE) test-ethernet-frame-host
	$(MAKE) test-virtio-net-mac-identity-host
	$(MAKE) test-network-ipv4-profile-host
	$(MAKE) test-packet-pressure-host
	$(MAKE) test-lapic-timer-host
	$(MAKE) test-lapic-timer-runtime-host
	$(MAKE) test-lapic-timer-oneshot-runtime-host
	$(MAKE) test-acpi-host
	$(MAKE) test-platform-reset-host
	$(MAKE) test-pmm-host
	$(MAKE) test-pmm-limited-host
	$(MAKE) test-pmm-highmem-host
	$(MAKE) test-pmm-pci-hole-host
	$(MAKE) test-drill-stack-budget-host
	$(MAKE) test-drill-stack-budget-race-host
	$(MAKE) test-heap-host
	$(MAKE) test-vmm-host
	$(MAKE) test-vmm-user-host
	$(MAKE) test-cpu-host
	$(MAKE) test-context-host
	$(MAKE) test-user-iretq-host
	$(MAKE) test-syscall-host
	$(MAKE) test-user-init-process-host
	$(MAKE) test-scheduler-host
	$(MAKE) test-scheduler-rsp0-host
	$(MAKE) test-scheduler-thread-host
	$(MAKE) test-scheduler-yield-host
	$(MAKE) test-scheduler-tick-host
	$(MAKE) test-scheduler-wait-host
	$(MAKE) test-scheduler-accounting-host
	$(MAKE) test-scheduler-discipline-host
	$(MAKE) test-scheduler-exit-host
	$(MAKE) test-scheduler-stress-host
	$(MAKE) test-process-host
	$(MAKE) test-syscall-origin-host
	$(MAKE) test-syscall-gate-host
	$(MAKE) test-process-create-host
	$(MAKE) test-process-limit-host
	$(MAKE) test-descriptor-host
	$(MAKE) test-vfs-host
	$(MAKE) test-hfocfs-host
	$(MAKE) test-initramfs-host
	$(MAKE) test-vfs-fd-host
	$(MAKE) test-user-copy-host
	$(MAKE) test-elf-user-host
	$(MAKE) test-user-runtime-host
	$(MAKE) test-pipe-host
	$(MAKE) test-tty-host
	$(MAKE) test-syscall-file-host
	$(MAKE) test-capability-host
	$(MAKE) test-event-ring-host
	$(MAKE) test-audit-host
	$(MAKE) test-audit-snapshot-host
	$(MAKE) test-audit-observability-host
	$(MAKE) test-config-schema-host
	$(MAKE) test-config-snapshot-host
	$(MAKE) test-boot-policy-host
	$(MAKE) test-system-image-host
	$(MAKE) test-sha256-host
	$(MAKE) test-image-integrity-host
	$(MAKE) test-recovery-image-host
	$(MAKE) test-block-device-host
	$(MAKE) test-data-region-host
	$(MAKE) test-block-cache-host
	$(MAKE) test-config-journal-host
	$(MAKE) test-pem-decode-host
	$(MAKE) test-appliance-certificate-import-host
	$(MAKE) test-appliance-certificate-upload-host
	$(MAKE) test-appliance-acme-trust-host
	$(MAKE) test-appliance-production-acme-order-host
	$(MAKE) test-appliance-acme-message-host
	$(MAKE) test-appliance-acme-jws-host
	$(MAKE) test-appliance-acme-flow-host
	$(MAKE) test-appliance-acme-challenge-host
	$(MAKE) test-appliance-acme-http01-host
	$(MAKE) test-appliance-acme-renewal-host
	$(MAKE) test-appliance-acme-http-host
	$(MAKE) test-appliance-acme-transport-host
	$(MAKE) test-appliance-acme-directory-fixture-host
	$(MAKE) test-appliance-acme-directory-stub-host
	$(MAKE) test-appliance-language-host
	$(MAKE) test-config-kv-host
	$(MAKE) test-state-schema-host
	$(MAKE) test-state-migration-host
	$(MAKE) test-journal-compaction-host
	$(MAKE) test-backup-stream-host
	$(MAKE) test-backup-manifest-host
	$(MAKE) test-backup-producer-host
	$(MAKE) test-restore-validator-host
	$(MAKE) test-restore-planner-host
	$(MAKE) test-restore-activation-host
	$(MAKE) test-service-descriptor-host
	$(MAKE) test-local-principal-host
	$(MAKE) test-credential-verifier-host
	$(MAKE) test-auth-throttle-host
	$(MAKE) test-local-session-host
	$(MAKE) test-management-command-host
	$(MAKE) test-authorization-host
	$(MAKE) test-appliance-status-host
	$(MAKE) test-controlled-configuration-host
	$(MAKE) test-audit-correlation-host
	$(MAKE) test-virtio-pci-identity-host
	$(MAKE) test-virtio-pci-capability-host
	$(MAKE) test-virtio-pci-device-status-host
	$(MAKE) test-virtio-pci-feature-host
	$(MAKE) test-virtio-queue-layout-host
	$(MAKE) test-virtio-descriptor-ownership-host
	$(MAKE) test-virtio-available-publication-host
	$(MAKE) test-virtio-used-consumption-host
	$(MAKE) test-virtio-dma-region-host
	$(MAKE) test-virtio-net-feature-profile-host
	$(MAKE) test-virtio-rng-feature-profile-host
	$(MAKE) test-virtio-rng-queue-composition-host
	$(MAKE) test-virtio-rng-request-host
	$(MAKE) test-virtio-rng-completion-host
	$(MAKE) test-entropy-health-host
	$(MAKE) test-entropy-accumulator-host
	$(MAKE) test-entropy-seed-fill-host
	$(MAKE) test-entropy-diagnostics-host
	$(MAKE) test-entropy-restart-host
	$(MAKE) test-virtio-net-queue-composition-host
	$(MAKE) test-virtio-net-rx-host
	$(MAKE) test-virtio-net-tx-host
	$(MAKE) test-kernel-status-format-host
	$(MAKE) test-rtl8168-host
	$(MAKE) test-e1000-host
	$(MAKE) test-network-backend-host
	$(MAKE) test-virtio-net-interrupt-host
	$(MAKE) test-virtio-net-link-state-host
	$(MAKE) test-virtio-net-lifecycle-host
	$(MAKE) test-network-device-host
	$(MAKE) test-link-state-host
	$(MAKE) test-ipv4-config-host
	$(MAKE) test-network-config-transaction-host
	$(MAKE) test-packet-buffer-host
	$(MAKE) test-icmp-reachability-host
	$(MAKE) test-management-transport-host
	$(MAKE) test-network-admission-host
	$(MAKE) test-network-event-host
	$(MAKE) test-key-use-host
	$(MAKE) test-trust-anchor-host
	$(MAKE) test-verification-policy-host
	$(MAKE) test-signed-metadata-host
	$(MAKE) test-trust-rotation-host
	$(MAKE) test-trust-revocation-host
	$(MAKE) test-image-policy-host
	$(MAKE) test-boot-evidence-host
	$(MAKE) test-recovery-trust-host
	$(MAKE) test-appliance-update-manifest-host
	$(MAKE) test-appliance-update-integrity-host
	$(MAKE) test-appliance-recovery-mode-host
	$(MAKE) test-appliance-factory-reset-host
	$(MAKE) test-update-compatibility-host
	$(MAKE) test-candidate-slot-inventory-host
	$(MAKE) test-candidate-preflight-host
	$(MAKE) test-candidate-staged-write-host
	$(MAKE) test-activation-intent-host
	$(MAKE) test-first-boot-health-host
	$(MAKE) test-health-check-host
	$(MAKE) test-storage-health-host
	$(MAKE) test-service-health-host
	$(MAKE) test-appliance-health-host
	$(MAKE) test-appliance-runtime-log-host
	$(MAKE) test-appliance-health-api-host
	$(MAKE) test-installer-hardware-discovery-host
	$(MAKE) test-installer-layout-host
	$(MAKE) test-installer-layout-contract-host
	$(MAKE) test-installer-gpt-preserve-host
	$(MAKE) test-installer-dual-boot-host
	$(MAKE) test-installer-boot-recovery-host
	$(MAKE) test-installer-boot-menu-host
	$(MAKE) test-installer-first-boot-wizard-host
	$(MAKE) test-installer-hardening-host
	$(MAKE) test-boot-measurements-host
	$(MAKE) test-image-signature-host
	$(MAKE) test-boot-chain-host
	$(MAKE) test-boot-attestation-host
	$(MAKE) test-production-installer-host
	$(MAKE) test-production-install-media-host
	$(MAKE) test-production-installer-orchestration-host
	$(MAKE) test-production-installer-boot-stage-host
	$(MAKE) test-production-installer-bios-boot-host
	$(MAKE) test-installed-boot-disk-host
	$(MAKE) test-network-health-host
	$(MAKE) test-audit-ring-health-host
	$(MAKE) test-appliance-audit-chain-host
	$(MAKE) test-appliance-audit-export-host
	$(MAKE) test-appliance-production-audit-host
	$(MAKE) test-metrics-host
	$(MAKE) test-fault-injection-host
	$(MAKE) test-storage-interruption-host
	$(MAKE) test-support-bundle-manifest-host
	$(MAKE) test-support-bundle-exporter-host
	$(MAKE) test-appliance-support-bundle-host
	$(MAKE) test-appliance-support-session-host
	$(MAKE) test-appliance-support-break-glass-host
	$(MAKE) test-appliance-production-support-host
	$(MAKE) test-hardware-acceptance-host
	$(MAKE) test-product-release-gate-host
	$(MAKE) test-boot-chain-determinism-host
	$(MAKE) test-diagnostic-privacy-host
	$(MAKE) test-integration-boundary-host
	$(MAKE) test-integration-principal-host
	$(MAKE) test-integration-session-host
	$(MAKE) test-integration-envelope-host
	$(MAKE) test-integration-ledger-host
	$(MAKE) test-integration-event-cursor-host
	$(MAKE) test-integration-config-intent-host
	$(MAKE) test-integration-failure-isolation-host
	$(MAKE) test-integration-compatibility-host
	$(MAKE) test-automatic-rollback-host
	$(MAKE) test-update-acceptance-host
	$(MAKE) test-update-retention-host
	$(MAKE) test-service-dependency-host
	$(MAKE) test-service-graph-host
	$(MAKE) test-service-restart-policy-host
	$(MAKE) test-service-supervisor-host
	$(MAKE) test-service-cascade-drill-host
	$(MAKE) test-local-control-outage-drill-host
	$(MAKE) test-network-partition-drill-host
	$(MAKE) test-trust-failure-drill-host
	$(MAKE) test-update-failure-drill-host
	$(MAKE) test-integration-replay-outage-drill-host
	$(MAKE) test-power-cycle-continuity-drill-host
	$(MAKE) test-resilience-matrix-host
	$(MAKE) test-process-scheduler-host
	$(MAKE) test-smp-host
	$(MAKE) test-lapic-host
	$(MAKE) test-ioapic-host
	$(MAKE) test-isa-irq3-7-host
	$(MAKE) test-isa-irq8-15-host
	$(MAKE) test-keyboard-host
	$(MAKE) test-keyboard-init-host
	$(MAKE) test-console-host
	$(MAKE) test-pic-host
	$(MAKE) test-pci-host
	$(MAKE) test-ahci-host
	$(MAKE) test-nvme-host
	$(MAKE) test-appliance-gpt-discovery-host
	$(MAKE) test-nvme-state-integration-host
	$(MAKE) test-nvme-production-acceptance-host
	$(MAKE) test-physical-sata-continue-host
	$(MAKE) test-usb-hid-host
	$(MAKE) test-xhci-host
	$(MAKE) test-ehci-host
	$(MAKE) test-xhci-evidence-host
	$(MAKE) test-xhci-route-host
	$(MAKE) test-xhci-dma-host
	$(MAKE) test-xhci-handover-host
	$(MAKE) test-xhci-composite-host
	$(MAKE) test-ehci-ring-host
	$(MAKE) test-ehci-init-host
	$(MAKE) test-ehci-verdict-host
	$(MAKE) test-ehci-port-host
	$(MAKE) test-ehci-regs-host
	$(MAKE) test-ehci-engine-host
	$(MAKE) test-ehci-walk-host
	$(MAKE) test-diag-host
	$(MAKE) test-diag-export-full-host
	$(MAKE) test-ssh-hostkey-export-host
	$(MAKE) test-net-phy-journal-host
	$(MAKE) test-network-io-record-host
	$(MAKE) test-panel-auth-trace-host
	$(MAKE) test-network-rx-accounting-host
	$(MAKE) test-usb-storage-host
	$(MAKE) test-install-medium-host
	$(MAKE) test-pointer-host
	$(MAKE) test-local-input-host
	$(MAKE) test-local-network-setup-host
	$(MAKE) test-local-menu-host
	$(MAKE) test-loop-heartbeat-host
	$(MAKE) test-loop-budget-host
	$(MAKE) test-display-host
	$(MAKE) test-installer-ui-host
	$(MAKE) test-installer-storage-host
	$(MAKE) test-installer-notice-host
	$(MAKE) test-nic-registry-host
	$(MAKE) test-nic-discovery-host
	$(MAKE) test-mbedtls-secret-validity-host
	$(MAKE) test-input-path-host
	$(MAKE) test-qemu-suite-host
	$(MAKE) test-qemu-marker-wait-host
	$(MAKE) test-console-retype-host
	$(MAKE) test-merge-guard-host
	$(MAKE) test-build-warning-gate-host
	$(MAKE) test-qemu-accel-host
	$(MAKE) test-framebuffer-glyph-host
	$(MAKE) test-framebuffer-scroll-host
	$(MAKE) test-panel-login-host
	$(MAKE) test-forward-config-host
	$(MAKE) test-appliance-route-table-host
	$(MAKE) test-appliance-route-config-host
	$(MAKE) test-appliance-route-store-host
	$(MAKE) test-appliance-certificate-name-store-host
	$(MAKE) test-appliance-acme-control-host
	$(MAKE) test-ci-artifacts-host
	$(MAKE) test-install-journal-core-host
	$(MAKE) test-install-journal-platform-host
	$(MAKE) test-install-fat12-journal-host
	$(MAKE) test-install-journal-platform-fat12-host
	$(MAKE) test-install-log-read-host
	$(MAKE) test-install-media-prealloc-host
	$(MAKE) test-machine-facts-host
	$(MAKE) test-panic-record-host
	$(MAKE) test-https-gate-serial-view-host
	$(MAKE) test-https-refusal-answer-host
	$(MAKE) test-serial-view-normalised-host

.PHONY: test-physical-sata-continue-host
test-physical-sata-continue-host:
	bash tests/test-hfoc-physical-sata-continue-host.sh

.PHONY: qemu-test-images
HFOC_IMAGE_JOBS ?= 1
qemu-test-images:
	$(MAKE) -j$(HFOC_IMAGE_JOBS) iso $(QEMU_TEST_IMAGES) \
		$(PRODUCTION_INSTALLER_QEMU_IMAGES)
test-qemu-parallel: qemu-test-images
	bash scripts/run-qemu-suite.sh --serial $(QEMU_SERIAL_TARGETS) --parallel $(QEMU_PARALLEL_PHASE_1)
.PHONY: test-qemu-parallel-after-acceptance
test-qemu-parallel-after-acceptance: qemu-test-images
	bash scripts/run-qemu-suite.sh --serial $(QEMU_SERIAL_TARGETS) --parallel $(QEMU_PARALLEL_RERUN_TARGETS)
test-qemu-suite-host:
	bash tests/test-qemu-suite-host.sh
# ITEM-609: the update proof's marker waiter, driven on fixture serial logs.
# The waiter is scripts/hfoc-qemu-marker-wait.py; the QEMU proof and this host
# regression run the same file.
.PHONY: test-qemu-marker-wait-host
test-qemu-marker-wait-host: scripts/hfoc-qemu-marker-wait.py \
		tests/qemu-marker-wait-host.sh
	bash tests/qemu-marker-wait-host.sh
# ITEM-431: the merge guard's refusals, driven against a fake gh.
.PHONY: test-merge-guard-host
test-merge-guard-host:
	bash tests/test-merge-guard-host.sh

# ITEM-432: the build-warning gate's reading, driven on captured build logs.
.PHONY: test-build-warning-gate-host
test-build-warning-gate-host:
	bash tests/test-build-warning-gate-host.sh

# ITEM-462: the CI artifact budget's three rules, driven on real directories
# and a mutated workflow.  The tool under test is scripts/hfoc-ci-artifacts.sh;
# the static slice that pins the workflow wiring is
# scripts/check.d/69-ci-artifacts.sh.
.PHONY: test-ci-artifacts-host
test-ci-artifacts-host: scripts/hfoc-ci-artifacts.sh \
		tests/test-ci-artifacts-host.sh
	bash tests/test-ci-artifacts-host.sh

# ITEM-432: recompile the production kernel for the warning gate.
#
# The acceptance step builds the production kernel with its output redirected
# into its own artifact logs, so by the time the gate's step runs, the objects
# are current and the compiler is silent.  This target makes the compiler run
# again for the one goal the item is about: `-B` recompiles the kernel and the
# libraries it links, and nothing else.
.PHONY: kernel-rebuild
kernel-rebuild:
	$(MAKE) -B $(KERNEL)

# ITEM-442: the QEMU accelerator plumbing, driven on the host.  The decision is
# one shell file and the shim is another, so both are exercised without starting
# a virtual machine: auto with and without the device, a demanded kvm that
# cannot be used (a loud refusal, never a silent fallback), tcg, an unknown
# value, and the shim's pass-through.
.PHONY: test-qemu-accel-host
test-qemu-accel-host: scripts/hfoc-qemu-accel.sh \
		scripts/qemu-accel/qemu-system-x86_64 \
		tests/test-qemu-accel-host.sh
	bash tests/test-qemu-accel-host.sh

test-console-retype-host:
	bash tests/test-console-retype-host.sh
test-resilience-matrix-qemu:
	bash scripts/test-resilience-matrix.sh
test-resilience-matrix-host:
	bash tests/test-resilience-matrix-host.sh
test-host-suite-runner:
	bash tests/test-host-suite-runner.sh
.PHONY: test-heavy-lock-host
test-heavy-lock-host:
	bash tests/test-heavy-lock-host.sh
test-host-suite-manifest:
	bash tests/test-host-suite-manifest.sh
test-acceptance-suite-runner:
	bash tests/test-acceptance-suite-runner.sh
test-acceptance-suite-manifest:
	bash tests/test-acceptance-suite-manifest.sh
host-suite-phase-list:
	@printf '%s\n' $(HOST_SERIAL_TARGETS)
	@printf '%s\n' $(HOST_PARALLEL_PHASE_1)
	@printf '%s\n' $(HOST_GATE_TARGETS)
test:
	$(MAKE) test-host-suite
	$(MAKE) test-release-appliance-v1-acceptance
	$(MAKE) test-https-vertical-slice-acceptance
	$(MAKE) test-qemu-parallel-after-acceptance
	$(MAKE) test-hfoc-appliance-platform-release-acceptance
	$(MAKE) test-hfoc-integrated-installer-dual-boot-management-acceptance
	$(MAKE) test-nvme-production-acceptance
	$(MAKE) test-hfoc-release-documentation
	$(MAKE) test-production-end-to-end-acceptance

clean:
	rm -rf build/obj build/iso_root build/fallback build/release build/oneshot build/lapic-runtime build/irq3-7 build/irq8-15 build/x2apic build/vmm-as build/sched-yield build/sched-tick build/sched-wait build/sched-exit build/sched-stress build/process-sched build/user-iretq build/syscall build/user-init build/boot-policy build/block-device build/data-region build/block-cache build/hfocfs-qemu build/hfocfs-workspace-qemu build/config-journal build/persistent-config build/network-config build/persistent-network-config build/backup-restore build/supervisor-composition build/local-control build/network-foundation build/virtio-foundation build/virtio-net-frame build/packet-boundary build/arp-icmp build/tcp-listener build/network-resource build/prodnet-lwip build/trust-policy build/update-lifecycle build/supportability build/accounting-boundary build/storage-interruption build/service-cascade-drill build/local-control-outage-drill build/network-partition-drill build/trust-failure-drill build/update-failure-drill build/integration-replay-outage-drill build/power-cycle-continuity-drill build/entropy $(KERNEL) \
	$(PRODUCTION_INSTALLER_HOST_SEEDED_ISO_ROOT) \
	$(PRODUCTION_INSTALLER_HOST_SEEDED_ISO) \
	$(PRODUCTION_INSTALLER_HOST_SEEDED_ISO).sha256 \
	build/physical-storage \
build/physical-sata-acceptance \
	build/efi \
	build/efi-helper \
	build/mbedtls-test-secret \
	build/mbedtls-tls-qemu \
	build/https-service-qemu \
	build/https-entropy-fail-closed-qemu \
	build/https-restart-soak-qemu \
	build/sni-selection \
	build/acme-directory \
	build/acme-order \
		build/hfoc-os-*.iso build/hfoc-os-*.iso.sha256 \
			build/hfoc-build-input.txt

distclean: clean
	rm -rf deps

-include $(OBJECTS:.o=.d)
-include $(PRODUCTION_LWIP_DEPS)
-include $(FALLBACK_DEPS)
-include $(ONESHOT_DEPS)
-include $(LAPIC_RUNTIME_DEPS)
-include $(IRQ3_7_DEPS)
-include $(IRQ8_15_DEPS)
-include $(X2APIC_DEPS)
-include $(VMM_AS_DEPS)
-include $(SCHED_YIELD_DEPS)
-include $(SCHED_TICK_DEPS)
-include $(SCHED_WAIT_DEPS)
-include $(SCHED_EXIT_DEPS)
-include $(SCHED_STRESS_DEPS)
-include $(PROCESS_SCHED_DEPS)
-include $(USER_IRETQ_DEPS)
-include $(SYSCALL_DEPS)
-include $(USER_INIT_DEPS)
-include $(BOOT_POLICY_DEPS)
-include $(BLOCK_DEVICE_DEPS)
-include $(DATA_REGION_DEPS)
-include $(BLOCK_CACHE_DEPS)
-include $(HFOCFS_QEMU_DEPS)
-include $(HFOCFS_WORKSPACE_QEMU_DEPS)
-include $(CONFIG_JOURNAL_DEPS)
-include $(PERSISTENT_CONFIG_DEPS)
# ITEM-394: the installer profile's header dependencies were not tracked, so a
# header change could leave the installer image stale while the appliance
# image rebuilt.  The flow now depends on the installer image being current.
-include $(PRODUCTION_INSTALLER_DEPS)
-include $(DIAG_EXPORT_DEPS)
-include $(NETWORK_CONFIG_DEPS)
-include $(SNI_SELECTION_DEPS)
-include $(ACME_DIRECTORY_DEPS)
-include $(ACME_ORDER_DEPS)
-include $(PERSISTENT_NETWORK_CONFIG_DEPS)
-include $(BACKUP_RESTORE_DEPS)
-include $(SUPERVISOR_COMPOSITION_DEPS)
-include $(LOCAL_CONTROL_DEPS)
-include $(VIRTIO_FOUNDATION_DEPS)
-include $(VIRTIO_NET_FRAME_DEPS)
-include $(PACKET_BOUNDARY_DEPS)
-include $(ARP_ICMP_DEPS)
-include $(TCP_LISTENER_DEPS)
-include $(NETWORK_RESOURCE_DEPS)
-include $(TRUST_POLICY_DEPS)
-include $(UPDATE_LIFECYCLE_DEPS)
-include $(SUPPORTABILITY_DEPS)
-include $(ACCOUNTING_BOUNDARY_DEPS)
-include $(STORAGE_INTERRUPTION_DEPS)
-include $(SERVICE_CASCADE_DRILL_DEPS)
-include $(LOCAL_CONTROL_OUTAGE_DRILL_DEPS)
-include $(NETWORK_PARTITION_DRILL_DEPS)
-include $(TRUST_FAILURE_DRILL_DEPS)
-include $(UPDATE_FAILURE_DRILL_DEPS)
-include $(INTEGRATION_REPLAY_OUTAGE_DRILL_DEPS)
-include $(POWER_CYCLE_CONTINUITY_DRILL_DEPS)
-include $(ENTROPY_DEPS)
-include $(MBEDTLS_TEST_SECRET_DEPS)
-include $(MBEDTLS_TLS_QEMU_DEPS)
-include $(MBEDTLS_TLS_QEMU_LWIP_DEPS)
-include $(MBEDTLS_TLS_QEMU_MBEDTLS_DEPS)
-include $(HTTPS_SERVICE_QEMU_DEPS)
-include $(HTTPS_SERVICE_QEMU_LWIP_DEPS)
-include $(HTTPS_SERVICE_QEMU_MBEDTLS_DEPS)
-include $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_DEPS)
-include $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_LWIP_DEPS)
-include $(HTTPS_ENTROPY_FAIL_CLOSED_QEMU_MBEDTLS_DEPS)
-include $(HTTPS_RESTART_SOAK_QEMU_DEPS)
-include $(HTTPS_RESTART_SOAK_QEMU_LWIP_DEPS)
-include $(HTTPS_RESTART_SOAK_QEMU_MBEDTLS_DEPS)
-include $(HFOC_EFI_HELPER_DEPS)
