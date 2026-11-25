#!/usr/bin/env bash
# ITEM-394: stage the production installer medium's ISO root.
# ITEM-399: the medium carries two kernels, and only one of them is installed.
#
# Three callers use this: the shipped installer ISO (which carries a medium
# without an install declaration, so an unattended boot refuses) and the
# operator-authorised media the hardware acceptance run writes to a stick, in
# both the preserving and the whole-disk declaration.
#
# Everything that decides whether a disk may be written lives in these files:
#
#   boot/hfoc-installer-kernel.elf
#                             the *installer* kernel the medium's own Limine
#                             entries boot (`boot/limine-installer.conf`): it
#                             carries the install flow, the QEMU fixture and
#                             the installer build's macro.  It is never staged
#                             anywhere.
#   boot/hfoc-kernel.elf      the *installed* kernel: the product appliance
#                             build ($(KERNEL)), and the exact bytes the boot
#                             stage stages into the installed ESP as
#                             \boot\hfoc-kernel.elf.  ITEM-371's sealed media
#                             manifest binds it as its kernel component and
#                             ITEM-372's sealed boot chain seals it, so the
#                             installed disk runs the product kernel and not
#                             the installer (ITEM-399).
#   boot/hfoc-install.token   the declaration: confirmation token, target
#                             disk serial, the management scope the installed
#                             appliance will publish and the interface language
#                             the medium declares (ITEM-509 509.7c-2);
#   boot/hfoc-install.man     the sealed manifest over the installer payloads;
#   boot/hfoc-install.payload the signed payload blob the kernel hashes;
#   boot/hfoc-installed.conf  the installed system's own boot configuration,
#                             staged verbatim into the installed ESP as
#                             \boot\limine\limine.conf (ITEM-397).  ITEM-399
#                             generates it from the sealed template
#                             `boot/hfoc-installed.conf` with one `cmdline:`
#                             line naming the declared management scope, so the
#                             installed appliance states its scope instead of
#                             falling back to the kernel's default;
#   boot/hfoc-installed.chain the sealed boot chain of the installed system
#                             (that kernel + that configuration), staged into
#                             the ESP as \boot\hfoc-boot.man so the installed
#                             disk attests what it booted (ITEM-397).
#
# The ITEM-371 sealed media manifest keeps its declared component order - boot
# payload, loader configuration, kernel - with the *installed* configuration as
# its loader configuration and the *installed* kernel as its kernel, so the
# bytes the installer stages are the bytes the medium sealed.  The installer
# kernel is not a component of that manifest: nothing about it reaches a disk.
#
# Usage:
#   hfoc-install-media-stage.sh ISO_ROOT INSTALLER_KERNEL_ELF INSTALLED_KERNEL_ELF \
#       CONFIRM TARGET MANAGEMENT [LAYOUT [NETWORK [ADDRESS [GATEWAY [DNS]]]]]
#
# ITEM-446: NETWORK is `dhcp` or `static`; a static declaration also names the
# address (a.b.c.d/prefix), the gateway and the DNS.  The declaration travels
# into the installed boot entry, so the installed appliance comes up on the
# address the operator chose.
#
# ITEM-449: the forward is declared through the environment -
# HFOC_INSTALL_MEDIA_FORWARD=on|off with HFOC_INSTALL_MEDIA_FORWARD_TARGET,
# _TARGET_PORT, _LISTEN_PORT and _CLIENTS when it is on.  It travels into the
# installed boot entry the same way, so the installed appliance states the
# forwarding rule it serves - and an absent declaration forwards nothing.
#
# ITEM-490: HFOC_INSTALL_MEDIA_ENTRY=console makes the *operator* entry the
# first (default) entry of the medium's Limine menu, so a machine booted from
# this medium lands on the storage screen without anybody navigating a menu.
# `unattended` (the default) keeps the unattended entry first, which is what the
# automated media and every CI proof need.  The entries are the same file either
# way - only their order changes - and the reorder refuses to guess: it requires
# exactly one entry with the requested install mode.
#
# An empty CONFIRM or TARGET stages a medium that carries no declaration, so
# the install entry refuses (TOKEN_MISSING) instead of installing anything.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

if [[ $# -lt 6 || $# -gt 11 ]]; then
    echo "usage: $0 ISO_ROOT INSTALLER_KERNEL_ELF INSTALLED_KERNEL_ELF CONFIRM TARGET MANAGEMENT [LAYOUT [NETWORK [ADDRESS [GATEWAY [DNS]]]]]" >&2
    exit 2
fi

ISO_ROOT=$1
# ITEM-638: the kernel parser's finite declaration bound is the format bound.
# Read it from the one owner instead of copying 1024 into this script; a token
# the kernel would refuse must never be sealed onto a medium.
DECLARATION_LIMIT=$(sed -n \
    's/^#define PRODUCTION_INSTALL_MEDIA_DECLARATION_MAX \([0-9][0-9]*\)U$/\1/p' \
    kernel/include/production_install_media.h)
if [[ ! $DECLARATION_LIMIT =~ ^[0-9]+$ ]] || (( DECLARATION_LIMIT < 512 )); then
    echo "cannot read the install declaration limit from the kernel header" >&2
    exit 2
fi

INSTALLER_KERNEL_ELF=$2
INSTALLED_KERNEL_ELF=$3
CONFIRM=$4
TARGET=$5
MANAGEMENT=$6
# ITEM-398/ITEM-586: the operator's layout declaration.  Empty means "no
# declaration": the run then preserves a target disk that carries an existing
# system and takes the whole-disk path only when there is nothing to keep.
# `update` is the explicit state-preserving reinstall of an appliance that is
# already installed; it is never inferred from a disk.
LAYOUT=${7:-}
NETWORK=${8:-}
ADDRESS=${9:-}
GATEWAY=${10:-}
DNS=${11:-}
# ITEM-654: the optional explicit SNTP server, read from the environment for
# the same reason the forward directive is: media that declare no time server
# keep their exact command lines, and the kernel validates the literal IPv4.
SNTP=${HFOC_INSTALL_MEDIA_SNTP:-}
# ITEM-449: the forward declaration.  It is read from the environment rather
# than from the positional list so the media that declare no forward - the
# shipped medium, the authorised medium and the network medium - keep their
# exact command lines: no forward means no forward declaration, fail-closed.
FORWARD=${HFOC_INSTALL_MEDIA_FORWARD:-}
FORWARD_TARGET=${HFOC_INSTALL_MEDIA_FORWARD_TARGET:-}
FORWARD_TARGET_PORT=${HFOC_INSTALL_MEDIA_FORWARD_TARGET_PORT:-}
FORWARD_LISTEN_PORT=${HFOC_INSTALL_MEDIA_FORWARD_LISTEN_PORT:-}
FORWARD_CLIENTS=${HFOC_INSTALL_MEDIA_FORWARD_CLIENTS:-}
HOST_SEED=${HFOC_INSTALL_MEDIA_HOST_SEED:-}
# ITEM-606: the owner's SSH client public key and the fingerprint confirmed on
# the trusted Debian machine.  Both or neither: a key module without the
# confirmed fingerprint is refused, and so is a fingerprint without the key.
SSH_KEY=${HFOC_INSTALL_MEDIA_SSH_KEY:-}
SSH_KEY_CONFIRM=${HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM:-}
# ITEM-636: the root initramfs source.  `make iso` stages rootfs/ together with
# the native userland built from user/ (the /bin programs and /bin/sh); the
# production media name that staged tree through the environment so the
# installed system carries the same root.  A direct caller that passes no
# declaration keeps the bare `rootfs` source, which is what the small staging
# fixtures use; a declared production rootfs must carry /bin/sh, and the
# missing shell refuses the medium instead of sealing an unusable image.
ROOTFS_SOURCE=${HFOC_INSTALL_MEDIA_ROOTFS:-rootfs}
# ITEM-509 increment 27 (509.7c-2): the language the medium declares.  Like the
# forward declaration it comes from the environment rather than the positional
# list, so every medium that declares no language keeps its exact token bytes:
# no variable means no directive, and the kernel reads an absent directive as
# the default `en`.  The value is not parsed here - `appliance_language_parse()`
# is the one owner of the vocabulary, and a misspelled value must refuse the
# medium at boot (BAD_TEXT, zero writes) instead of being guessed at build time.
LANGUAGE=${HFOC_INSTALL_MEDIA_LANGUAGE:-}
# ITEM-490: which entry the medium's Limine menu offers first.  `unattended` is
# the default and keeps every existing build byte-identical; `console` puts the
# operator entry (the storage screen) first for a medium a person boots.
ENTRY=${HFOC_INSTALL_MEDIA_ENTRY:-unattended}
case "$ENTRY" in
unattended | console) ;;
*)
    echo "Invalid HFOC_INSTALL_MEDIA_ENTRY: $ENTRY (unattended or console)" >&2
    exit 2
    ;;
esac
if [[ -n "$HOST_SEED" ]]; then
    if [[ "$ENTRY" != console ]]; then
        echo "A host-seeded medium must use HFOC_INSTALL_MEDIA_ENTRY=console" >&2
        exit 2
    fi
    if [[ -z "$CONFIRM" || -z "$TARGET" ]]; then
        echo "A host-seeded medium must carry its confirmation and target serial" >&2
        exit 2
    fi
    case "$LAYOUT" in
    whole-disk | update) ;;
    *)
        echo "A host-seeded medium must declare whole-disk or update layout" >&2
        exit 2
        ;;
    esac
fi
if [[ -n "$SSH_KEY" || -n "$SSH_KEY_CONFIRM" ]]; then
    if [[ -z "$SSH_KEY" || -z "$SSH_KEY_CONFIRM" ]]; then
        echo "An SSH key needs both HFOC_INSTALL_MEDIA_SSH_KEY and HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM" >&2
        exit 2
    fi
    if [[ -z "$CONFIRM" || -z "$TARGET" ]]; then
        echo "An SSH key medium must carry its install confirmation and target serial" >&2
        exit 2
    fi
    command -v ssh-keygen >/dev/null || {
        echo "Missing command: ssh-keygen" >&2
        exit 1
    }
    if [[ ! -f "$SSH_KEY" ]]; then
        echo "Missing SSH public key file: $SSH_KEY" >&2
        exit 2
    fi
    mapfile -t ssh_key_lines <"$SSH_KEY"
    if [[ ${#ssh_key_lines[@]} -ne 1 ]]; then
        echo "The SSH public key file must carry exactly one line" >&2
        exit 2
    fi
    read -r ssh_key_type ssh_key_blob _ <<<"${ssh_key_lines[0]}"
    if [[ "$ssh_key_type" != "ecdsa-sha2-nistp256" ]]; then
        echo "The SSH public key must be an ECDSA P-256 key" >&2
        exit 2
    fi
    ssh_key_fingerprint=$(ssh-keygen -lf "$SSH_KEY" | awk '{print $2}')
    if [[ "$ssh_key_fingerprint" != "$SSH_KEY_CONFIRM" ]]; then
        echo "The SSH public key fingerprint does not match HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM" >&2
        exit 2
    fi
fi

LIMINE=${LIMINE:-deps/limine}
SIGNING_KEY=${HFOC_INSTALL_MEDIA_SIGNING_KEY:-boot/keys/hfoc-development-signing-key.pem}
RELEASE_ID=${HFOC_INSTALL_MEDIA_RELEASE_ID:-1}
SIGNER_ID=${HFOC_INSTALL_MEDIA_SIGNER_ID:-1}

command -v python3 >/dev/null || {
    echo "Missing command: python3" >&2
    exit 1
}

for path in \
    "$INSTALLER_KERNEL_ELF" \
    "$INSTALLED_KERNEL_ELF" \
    boot/limine-installer.conf \
    boot/hfoc-installed.conf \
    "$LIMINE/BOOTX64.EFI" \
    "$LIMINE/limine-bios.sys" \
    "$LIMINE/limine-bios-cd.bin" \
    "$LIMINE/limine-uefi-cd.bin" \
    "$SIGNING_KEY" \
    scripts/hfoc-rootfs-pack.py \
    scripts/hfoc-install-media-manifest.py \
    scripts/hfoc-installed-config.py \
    scripts/hfoc-boot-chain-manifest.py; do
    [[ -f "$path" ]] || {
        echo "Missing required file: $path" >&2
        exit 1
    }
done

case "$MANAGEMENT" in
    local-only | management-interface) ;;
    *)
        echo "Invalid management scope: $MANAGEMENT" >&2
        exit 2
        ;;
esac

# ITEM-446: the declared network is validated here as well as in the kernel, so
# a medium that would be refused before its first write is refused at build
# time instead of being sealed and shipped.
case "$NETWORK" in
    "")
        if [[ -n "$ADDRESS" || -n "$GATEWAY" || -n "$DNS" ]]; then
            echo "An address, gateway or DNS needs a network mode" >&2
            exit 2
        fi
        ;;
    dhcp)
        if [[ -n "$ADDRESS" || -n "$GATEWAY" || -n "$DNS" ]]; then
            echo "A static value cannot accompany a DHCP declaration" >&2
            exit 2
        fi
        ;;
    static)
        if [[ -z "$ADDRESS" || -z "$GATEWAY" || -z "$DNS" ]]; then
            echo "A static declaration needs an address, a gateway and a DNS" >&2
            exit 2
        fi
        ;;
    *)
        echo "Invalid network mode: $NETWORK" >&2
        exit 2
        ;;
esac

# ITEM-449: the declared forward is validated here as well as in the kernel, so
# a medium that would be refused before its first write is refused at build
# time instead of being sealed and shipped.  The generator below repeats the
# whole rule (literal IPv4 target, bounded ports, exact clients); these checks
# are the ones that do not need an address parser.
case "$FORWARD" in
    "")
        if [[ -n "$FORWARD_TARGET" || -n "$FORWARD_TARGET_PORT" ||
              -n "$FORWARD_LISTEN_PORT" || -n "$FORWARD_CLIENTS" ]]; then
            echo "A forward value needs HFOC_INSTALL_MEDIA_FORWARD=on|off" >&2
            exit 2
        fi
        ;;
    off)
        if [[ -n "$FORWARD_TARGET" || -n "$FORWARD_TARGET_PORT" ||
              -n "$FORWARD_LISTEN_PORT" || -n "$FORWARD_CLIENTS" ]]; then
            echo "An off forward cannot carry a target, ports or clients" >&2
            exit 2
        fi
        ;;
    on)
        if [[ -z "$FORWARD_TARGET" || -z "$FORWARD_TARGET_PORT" ||
              -z "$FORWARD_LISTEN_PORT" || -z "$FORWARD_CLIENTS" ]]; then
            echo "An enabled forward needs a target, both ports and clients" >&2
            exit 2
        fi
        if [[ "$FORWARD_LISTEN_PORT" == 443 ]]; then
            echo "A forward cannot claim the management serving port 443" >&2
            exit 2
        fi
        ;;
    *)
        echo "Invalid forward mode: $FORWARD" >&2
        exit 2
        ;;
esac

# ITEM-399: the two kernels must be different files.  An installer kernel that
# is also the installed kernel is exactly the defect this item closes - an
# installed appliance would run the installer build - so the stage refuses it
# instead of sealing the same bytes under both names.
installer_kernel_sha=$(sha256sum "$INSTALLER_KERNEL_ELF" | awk '{print $1}')
installed_kernel_sha=$(sha256sum "$INSTALLED_KERNEL_ELF" | awk '{print $1}')
if [[ "$installer_kernel_sha" == "$installed_kernel_sha" ]]; then
    echo "The installer kernel and the installed kernel are the same file: $INSTALLED_KERNEL_ELF" >&2
    exit 2
fi
# ITEM-636: refuse before the first staged byte when the declared root has no
# shell.  The production Makefile always declares the staged userland; a direct
# fixture caller without the declaration keeps the bare tree above.
if [[ ! -d "$ROOTFS_SOURCE" ]]; then
    echo "Missing root initramfs source: $ROOTFS_SOURCE" >&2
    exit 2
fi
if [[ -n "${HFOC_INSTALL_MEDIA_ROOTFS:-}" && ! -f "$ROOTFS_SOURCE/bin/sh" ]]; then
    echo "The declared root initramfs has no /bin/sh: $ROOTFS_SOURCE" >&2
    exit 2
fi

rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"
cp "$INSTALLER_KERNEL_ELF" "$ISO_ROOT/boot/hfoc-installer-kernel.elf"
cp "$INSTALLED_KERNEL_ELF" "$ISO_ROOT/boot/hfoc-kernel.elf"
# ITEM-623: the installed system's root initramfs.  The installer kernel mounts
# the same module as its own `/`; the install flow stages these bytes beside the
# kernel so the installed kernel has a root instead of the empty RAM fallback.
# ITEM-636: the source is the declared production rootfs (native /bin and sh).
python3 scripts/hfoc-rootfs-pack.py "$ROOTFS_SOURCE" \
    "$ISO_ROOT/boot/hfoc-rootfs.arc"
cp boot/limine-installer.conf "$ISO_ROOT/boot/limine/limine.conf"
if [[ -n "$HOST_SEED" ]]; then
    python3 scripts/hfoc-host-seed-provision.py \
        --verify "$HOST_SEED" --target "$TARGET" --build-id "$RELEASE_ID"
    cp "$HOST_SEED" "$ISO_ROOT/boot/hfoc-seed.bin"
    awk '
        /^[[:space:]]*# HFOC_HOST_SEED_MODULE[[:space:]]*$/ {
            print "    module_path: boot():/boot/hfoc-seed.bin"
            next
        }
        { print }
    ' "$ISO_ROOT/boot/limine/limine.conf" \
        >"$ISO_ROOT/boot/limine/limine.conf.personalised"
    mv "$ISO_ROOT/boot/limine/limine.conf.personalised" \
        "$ISO_ROOT/boot/limine/limine.conf"
else
    awk '!/^[[:space:]]*# HFOC_HOST_SEED_MODULE[[:space:]]*$/' \
        "$ISO_ROOT/boot/limine/limine.conf" \
        >"$ISO_ROOT/boot/limine/limine.conf.public"
    mv "$ISO_ROOT/boot/limine/limine.conf.public" \
        "$ISO_ROOT/boot/limine/limine.conf"
fi
if [[ -n "$SSH_KEY" ]]; then
    ssh_key_point=$(python3 - "$ssh_key_blob" <<'PY'
import base64
import struct
import sys

try:
    blob = base64.b64decode(sys.argv[1], validate=True)
except Exception:
    raise SystemExit(1)
offset = 0


def field():
    global offset
    if offset + 4 > len(blob):
        raise SystemExit(1)
    length = struct.unpack(">I", blob[offset:offset + 4])[0]
    offset += 4
    if offset + length > len(blob):
        raise SystemExit(1)
    value = blob[offset:offset + length]
    offset += length
    return value


key_type = field()
curve = field()
point = field()
if (key_type != b"ecdsa-sha2-nistp256" or curve != b"nistp256" or
        offset != len(blob) or len(point) != 65 or point[0] != 0x04):
    raise SystemExit(1)
print(base64.b64encode(point).decode("ascii"))
PY
) || {
        echo "The SSH public key is not a valid ECDSA P-256 OpenSSH key" >&2
        exit 2
    }
    if [[ ${#ssh_key_point} -ne 88 ]]; then
        echo "The SSH public key point is not the 88-character console form" >&2
        exit 2
    fi
    printf '%s' "$ssh_key_point" >"$ISO_ROOT/boot/hfoc-ssh-key.point"
    awk '
        /^[[:space:]]*# HFOC_SSH_KEY_MODULE[[:space:]]*$/ {
            print "    module_path: boot():/boot/hfoc-ssh-key.point"
            next
        }
        { print }
    ' "$ISO_ROOT/boot/limine/limine.conf" \
        >"$ISO_ROOT/boot/limine/limine.conf.ssh-key"
    mv "$ISO_ROOT/boot/limine/limine.conf.ssh-key" \
        "$ISO_ROOT/boot/limine/limine.conf"
else
    awk '!/^[[:space:]]*# HFOC_SSH_KEY_MODULE[[:space:]]*$/' \
        "$ISO_ROOT/boot/limine/limine.conf" \
        >"$ISO_ROOT/boot/limine/limine.conf.no-ssh-key"
    mv "$ISO_ROOT/boot/limine/limine.conf.no-ssh-key" \
        "$ISO_ROOT/boot/limine/limine.conf"
fi
# ITEM-490: the operator medium offers the storage screen first.  The reorder is
# a permutation of the same entries, and it refuses a config in which the
# requested install mode is not declared exactly once rather than guessing.
if [[ "$ENTRY" == console ]]; then
    awk -v want='hfoc.install=console' '
        /^\// {
            ++blocks
            title[blocks] = $0
            body[blocks] = ""
            next
        }
        {
            if (blocks == 0) {
                header = header $0 "\n"
            } else {
                body[blocks] = body[blocks] $0 "\n"
            }
        }
        END {
            chosen = 0
            for (slot = 1; slot <= blocks; ++slot) {
                if (index(body[slot], want) > 0) {
                    ++chosen
                    first = slot
                }
            }
            if (blocks == 0 || chosen != 1) {
                exit 3
            }
            printf "%s", header
            printf "%s\n%s", title[first], body[first]
            for (slot = 1; slot <= blocks; ++slot) {
                if (slot != first) {
                    printf "%s\n%s", title[slot], body[slot]
                }
            }
        }
    ' "$ISO_ROOT/boot/limine/limine.conf" >"$ISO_ROOT/boot/limine/limine.conf.entry" || {
        echo "The medium config does not declare $ENTRY exactly once" >&2
        exit 2
    }
    mv "$ISO_ROOT/boot/limine/limine.conf.entry" \
        "$ISO_ROOT/boot/limine/limine.conf"
fi
network_arguments=()
if [[ -n "$NETWORK" ]]; then
    network_arguments=(--network "$NETWORK")
    if [[ "$NETWORK" == static ]]; then
        network_arguments+=(--address "$ADDRESS" --gateway "$GATEWAY" --dns "$DNS")
    fi
fi
forward_arguments=()
if [[ -n "$FORWARD" ]]; then
    forward_arguments=(--forward "$FORWARD")
    if [[ "$FORWARD" == on ]]; then
        forward_arguments+=(--forward-target "$FORWARD_TARGET"
            --forward-target-port "$FORWARD_TARGET_PORT"
            --forward-listen-port "$FORWARD_LISTEN_PORT"
            --forward-clients "$FORWARD_CLIENTS")
    fi
fi
python3 scripts/hfoc-installed-config.py \
    --template boot/hfoc-installed.conf \
    --management "$MANAGEMENT" \
    "${network_arguments[@]}" \
    "${forward_arguments[@]}" \
    --output "$ISO_ROOT/boot/hfoc-installed.conf"
cp "$LIMINE/BOOTX64.EFI" "$ISO_ROOT/EFI/BOOT/"
cp "$LIMINE/limine-bios.sys" "$LIMINE/limine-bios-cd.bin" \
    "$LIMINE/limine-uefi-cd.bin" "$ISO_ROOT/boot/limine/"

if [[ -n "$CONFIRM" && -n "$TARGET" ]]; then
    case "$LAYOUT" in
    ""|whole-disk|preserve|update) ;;
    *)
        echo "Invalid layout declaration: $LAYOUT" >&2
        exit 2
        ;;
    esac
    {
        printf 'HFOC-INSTALL-CONFIRM=%s\n' "$CONFIRM"
        printf 'HFOC-INSTALL-TARGET=%s\n' "$TARGET"
        printf 'HFOC-INSTALL-MANAGEMENT=%s\n' "$MANAGEMENT"
        if [[ -n "$LAYOUT" ]]; then
            printf 'HFOC-INSTALL-LAYOUT=%s\n' "$LAYOUT"
        fi
        if [[ -n "$HOST_SEED" ]]; then
            printf 'HFOC-INSTALL-HOST-SEED=required\n'
        fi
        if [[ -n "$SSH_KEY" ]]; then
            printf 'HFOC-INSTALL-SSH-KEY=%s\n' "$SSH_KEY_CONFIRM"
        fi
        if [[ -n "$LANGUAGE" ]]; then
            printf 'HFOC-INSTALL-LANGUAGE=%s\n' "$LANGUAGE"
        fi
        if [[ -n "$NETWORK" ]]; then
            printf 'HFOC-INSTALL-NETWORK=%s\n' "$NETWORK"
            if [[ "$NETWORK" == static ]]; then
                printf 'HFOC-INSTALL-ADDRESS=%s\n' "$ADDRESS"
                printf 'HFOC-INSTALL-GATEWAY=%s\n' "$GATEWAY"
                printf 'HFOC-INSTALL-DNS=%s\n' "$DNS"
            fi
        fi
        if [[ -n "$SNTP" ]]; then
            printf 'HFOC-INSTALL-SNTP=%s\n' "$SNTP"
        fi
        if [[ -n "$FORWARD" ]]; then
            printf 'HFOC-INSTALL-FORWARD=%s\n' "$FORWARD"
            if [[ "$FORWARD" == on ]]; then
                printf 'HFOC-INSTALL-FORWARD-TARGET=%s\n' "$FORWARD_TARGET"
                printf 'HFOC-INSTALL-FORWARD-TARGET-PORT=%s\n' \
                    "$FORWARD_TARGET_PORT"
                printf 'HFOC-INSTALL-FORWARD-LISTEN-PORT=%s\n' \
                    "$FORWARD_LISTEN_PORT"
                printf 'HFOC-INSTALL-FORWARD-CLIENTS=%s\n' "$FORWARD_CLIENTS"
            fi
        fi
    } >"$ISO_ROOT/boot/hfoc-install.token"
else
    {
        printf '# ITEM-394: this medium carries no install declaration.\n'
        printf '# The unattended entry refuses (TOKEN_MISSING, zero writes);\n'
        printf '# the appliance entry boots the installer console normally.\n'
        printf '# Build an authorised medium with HFOC_INSTALL_MEDIA_CONFIRM and\n'
        printf '# HFOC_INSTALL_MEDIA_TARGET set (docs/hfoc-production-install-flow.md).\n'
    } >"$ISO_ROOT/boot/hfoc-install.token"
fi

DECLARATION_BYTES=$(wc -c <"$ISO_ROOT/boot/hfoc-install.token")
if (( DECLARATION_BYTES > DECLARATION_LIMIT )); then
    echo "install declaration is $DECLARATION_BYTES bytes; the kernel accepts at most $DECLARATION_LIMIT" >&2
    exit 2
fi

python3 scripts/hfoc-install-media-manifest.py \
    --boot-payload "$ISO_ROOT/EFI/BOOT/BOOTX64.EFI" \
    --limine-config "$ISO_ROOT/boot/hfoc-installed.conf" \
    --kernel "$ISO_ROOT/boot/hfoc-kernel.elf" \
    --key "$SIGNING_KEY" \
    --release-id "$RELEASE_ID" --signer-id "$SIGNER_ID" \
    --payload-output "$ISO_ROOT/boot/hfoc-install.payload" \
    --envelope-output "$ISO_ROOT/boot/hfoc-install.man"

# The installed system's own sealed boot chain: the kernel the installer
# stages and the configuration that names it.  It is written as
# \boot\hfoc-boot.man into the ESP, which is the path the installed
# configuration declares, so the installed disk verifies the same chain the
# medium sealed (ITEM-372 discipline).
python3 scripts/hfoc-boot-chain-manifest.py \
    --kernel "$ISO_ROOT/boot/hfoc-kernel.elf" \
    --config "$ISO_ROOT/boot/hfoc-installed.conf" \
    --key "$SIGNING_KEY" \
    --release-id "$RELEASE_ID" --signer-id "$SIGNER_ID" \
    --output "$ISO_ROOT/boot/hfoc-installed.chain"

echo "Installer medium staged: $ISO_ROOT"
echo "  installer kernel: $INSTALLER_KERNEL_ELF ($installer_kernel_sha)"
echo "  installed kernel: $INSTALLED_KERNEL_ELF ($installed_kernel_sha)"
