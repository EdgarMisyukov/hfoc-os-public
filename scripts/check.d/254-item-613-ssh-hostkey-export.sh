#!/usr/bin/env bash
# ITEM-613: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.
#
# The owner asked for the SSH host public key on the diagnostic medium instead
# of another photograph.  The claims pinned here:
#
#   * the production status report carries one [PRODUCTION-SSH-HOSTKEY] record
#     with the already-loaded public blob, its no-padding SHA256 fingerprint,
#     the actual address/port and the build, never the private scalar;
#   * the Debian reader validates the OpenSSH blob/algorithm/curve/point and
#     recomputes the fingerprint, writes .pub/.fingerprint/known_hosts
#     candidate, and refuses a corrupt/truncated/contradicting record;
#   * old sessions without the record stay valid and name the absent key;
#   * the real journal proof and the production SSH QEMU proof carry the same
#     record through the shared snapshot composer.

for item613_file in \
    kernel/src/appliance_production_ssh.c \
    kernel/include/appliance_production_ssh.h \
    kernel/src/kernel.c \
    scripts/hfoc-diag-read.py \
    scripts/hfoc-ssh-hostkey.py \
    scripts/test-ssh-hostkey-export-host.sh \
    scripts/test-qemu-ssh.sh \
    tests/diag-export-full-host.c \
    docs/hfoc-ssh-host-key-export.md; do
    [[ -f $item613_file ]] || {
        echo "ITEM-613: отсутствует $item613_file" >&2
        exit 1
    }
done

for item613_render in \
    'appliance_production_ssh_render_hostkey(' \
    'state=READY algorithm=' \
    'ecdsa-sha2-nistp256 blob=' \
    'fingerprint=' \
    'address=' \
    ' port=' \
    'build=' \
    'appliance_ssh_identity_fingerprint_text('; do
    grep -Fq -- "$item613_render" kernel/src/appliance_production_ssh.c || {
        echo "ITEM-613: рендер ключа не содержит:" "$item613_render" >&2
        exit 1
    }
done
grep -Fq '[PRODUCTION-SSH-HOSTKEY] ' kernel/src/kernel.c || {
    echo "ITEM-613: snapshot не публикует host key" >&2
    exit 1
}
grep -Fq 'appliance_production_ssh_render_hostkey(' kernel/src/kernel.c || {
    echo "ITEM-613: snapshot не вызывает рендер ключа" >&2
    exit 1
}

# The reader validates and writes the public files.
for item613_reader in \
    'def validate_blob(' \
    'def host_pattern(' \
    'def write_candidate(' \
    'contradicting host-key records' \
    'the public blob is not strict base64' \
    'the declared fingerprint' \
    'known_hosts.candidate' \
    'HFOC-SSH-HOST-' \
    'chmod(0o600)'; do
    grep -Fq -- "$item613_reader" scripts/hfoc-ssh-hostkey.py || {
        echo "ITEM-613: reader не содержит:" "$item613_reader" >&2
        exit 1
    }
done
grep -Fq 'load_hostkey(HOSTKEY)' scripts/hfoc-diag-read.py || {
    echo "ITEM-613: diag reader не вызывает key reader" >&2
    exit 1
}
grep -Fq 'hostkey=ABSENT reason=NO_HOSTKEY_RECORD' scripts/hfoc-diag-read.py || {
    echo "ITEM-613: старый экспорт без ключа не назван" >&2
    exit 1
}
grep -Fq 'hostkey=REFUSED' scripts/hfoc-diag-read.py || {
    echo "ITEM-613: испорченный ключ не отвергается" >&2
    exit 1
}

# The real journal proof includes the record and the files.
grep -Fq '[PRODUCTION-SSH-HOSTKEY]' tests/diag-export-full-host.c || {
    echo "ITEM-613: journal proof не несёт host key" >&2
    exit 1
}
for item613_proof in \
    'the extractor writes the OpenSSH public key' \
    'the extractor writes the independently checked fingerprint' \
    'known_hosts candidate for the record' \
    'hostkey=SHA256:'; do
    grep -Fq -- "$item613_proof" tests/diag-export-full-host.c || {
        echo "ITEM-613: journal proof не утверждает:" "$item613_proof" >&2
        exit 1
    }
done
for item613_qemu in \
    '[PRODUCTION-SSH-HOSTKEY]' \
    'the exported blob is not the handshake host key' \
    'the declared fingerprint' \
    'SSH QEMU host-key export'; do
    grep -Fq -- "$item613_qemu" scripts/test-qemu-ssh.sh || {
        echo "ITEM-613: SSH QEMU proof не утверждает:" "$item613_qemu" >&2
        exit 1
    }
done
grep -Fq 'test-ssh-hostkey-export-host' Makefile || {
    echo "ITEM-613: host proof не в наборе" >&2
    exit 1
}
