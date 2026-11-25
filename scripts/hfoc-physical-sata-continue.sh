#!/usr/bin/env bash
# Host continuation for the ITEM-357 physical SATA acceptance boot.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

HFOC_PHYSICAL_SATA_DISK=/dev/sda
HFOC_PHYSICAL_SATA_EXPECTED_MODEL='HFOC Example SATA*'
HFOC_PHYSICAL_SATA_EXPECTED_SERIAL='DEADBEEF00000001'
HFOC_PHYSICAL_SATA_EXPECTED_ROOT='/dev/nvme0n1p2'
HFOC_PHYSICAL_SATA_PHASE1_MARKER='HFOCPHYS1P1MARK'
HFOC_PHYSICAL_SATA_PHASE2_MARKER='HFOCPHYS2P2MARK'
HFOC_PHYSICAL_SATA_EVIDENCE_LBA=1024

STATE_DIR=/srv/hfoc-os/build
# ITEM-410: the repository the host-side boot-log reader is run from, so the
# appliance's own boot log is captured as evidence with the rest of the run.
HFOC_PHYSICAL_SATA_REPO=${HFOC_PHYSICAL_SATA_REPO:-/srv/hfoc-os}
HFOC_PHYSICAL_SATA_BOOT_LOG_FILE=$STATE_DIR/hfoc-item410-boot-log.txt
LOG=$STATE_DIR/hfoc-physical-boot.log
STATE_FILE=$STATE_DIR/hfoc-physical-boot.state
EVIDENCE_FILE=$STATE_DIR/hfoc-item357-physical-boot-evidence.txt
BOOT_NUM_FILE=$STATE_DIR/hfoc-physical-bootnum
ORDER_FILE=$STATE_DIR/hfoc-physical-bootorder
COMMIT_FILE=$STATE_DIR/hfoc-physical-bootcommit
SERVICE=hfoc-item357-physical-sata-acceptance.service

hfoc_physical_sata_normalize_marker() {
    local marker=${1:-}

    # Raw evidence may end in LF. Command substitution also strips it, so
    # compare the 15-byte marker text rather than a brittle newline form.
    marker=${marker%$'\n'}
    marker=${marker%$'\r'}
    printf '%s' "$marker"
}

# ITEM-410: capture the appliance's own boot log from the acceptance disk.
#
# The appliance writes its records into its own ESP as it runs, so the run that
# just rebooted the machine can be read back instead of photographed.  This runs
# after the identity guard has passed and after the machine came back from the
# acceptance disk; the disk is only read here.
hfoc_physical_sata_capture_boot_log() {
    local reader=$HFOC_PHYSICAL_SATA_REPO/scripts/hfoc-esp-read.py

    if [[ ! -f "$reader" ]]; then
        log "appliance boot log reader missing: $reader"
        return 0
    fi
    if python3 "$reader" "$HFOC_PHYSICAL_SATA_DISK" --bootlog \
        >"$HFOC_PHYSICAL_SATA_BOOT_LOG_FILE" 2>&1; then
        log "appliance boot log captured: $(head -n 1 "$HFOC_PHYSICAL_SATA_BOOT_LOG_FILE")"
    else
        log "appliance boot log UNAVAILABLE (see $HFOC_PHYSICAL_SATA_BOOT_LOG_FILE)"
    fi
    return 0
}

log() {
    printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >>"$LOG"
}

hfoc_physical_sata_continue() {
    local model
    local serial
    local root
    local raw_marker
    local marker
    local state
    local boot_num
    local saved_order
    local commit
    local disk_name=${HFOC_PHYSICAL_SATA_DISK##*/}

    if [[ "$HFOC_PHYSICAL_SATA_EXPECTED_SERIAL" == DEADBEEF00000001 ]]; then
        printf '%s\n' 'identity guard: release fixture is not a physical declaration' >&2
        exit 90
    fi

    mkdir -p "$STATE_DIR"

    model=$(cat "/sys/block/$disk_name/device/model")
    serial=$(lsblk -dn -o SERIAL "$HFOC_PHYSICAL_SATA_DISK")
    root=$(findmnt -no SOURCE -T /)

    if [[ "$serial" != "$HFOC_PHYSICAL_SATA_EXPECTED_SERIAL" ||
          "$root" != "$HFOC_PHYSICAL_SATA_EXPECTED_ROOT" ]]; then
        log "identity guard failed: model=$model serial=$serial root=$root"
        exit 90
    fi
    # shellcheck disable=SC2254
    case "$model" in
        $HFOC_PHYSICAL_SATA_EXPECTED_MODEL) ;;
        *)
            log "identity guard failed: model=$model serial=$serial root=$root"
            exit 90
            ;;
    esac

    log "start model=$model serial=$serial root=$root"

    raw_marker=$(
        dd if="$HFOC_PHYSICAL_SATA_DISK" bs=512 count=1 \
            skip="$HFOC_PHYSICAL_SATA_EVIDENCE_LBA" status=none 2>/dev/null |
            head -c 16 || true
    )
    marker=$(hfoc_physical_sata_normalize_marker "$raw_marker")

    if [[ "$marker" == "$HFOC_PHYSICAL_SATA_PHASE1_MARKER" ]]; then
        log "phase1 marker observed"
        printf 'PHASE1\n' >"$STATE_FILE"
        boot_num=$(cat "$BOOT_NUM_FILE" 2>/dev/null || true)
        if [[ -z "$boot_num" ]]; then
            log "phase1 boot number missing"
            exit 91
        fi
        efibootmgr -n "$boot_num"
        log "phase1 BootNext set to $boot_num; rebooting"
        systemctl reboot
        exit 0
    fi

    if [[ "$marker" == "$HFOC_PHYSICAL_SATA_PHASE2_MARKER" ]]; then
        log "phase2 marker observed"
        printf 'PASS\n' >"$STATE_FILE"

        boot_num=$(cat "$BOOT_NUM_FILE" 2>/dev/null || true)
        if [[ -n "$boot_num" ]]; then
            efibootmgr -b "$boot_num" -B >>"$LOG" 2>&1 || true
            log "removed temporary boot entry $boot_num"
        fi

        if [[ -f "$ORDER_FILE" ]]; then
            saved_order=$(cat "$ORDER_FILE")
            if [[ -n "$saved_order" ]]; then
                efibootmgr -o "$saved_order" >>"$LOG" 2>&1 || true
                log "restored BootOrder $saved_order"
            fi
        fi

        commit=$(cat "$COMMIT_FILE" 2>/dev/null || true)
        {
            printf 'HFOC ITEM-357 PHYSICAL SATA ACCEPTANCE\n'
            printf 'RESULT=PASS\n'
            printf 'DEVICE=%s\n' "$HFOC_PHYSICAL_SATA_DISK"
            printf 'MODEL=%s\n' "$model"
            printf 'SERIAL=%s\n' "$serial"
            printf 'ROOT=%s\n' "$root"
            printf 'MARKER=%s\n' "$HFOC_PHYSICAL_SATA_PHASE2_MARKER"
            printf 'LBA=%s\n' "$HFOC_PHYSICAL_SATA_EVIDENCE_LBA"
            printf 'COMMIT=%s\n' "$commit"
            printf 'DATE_UTC=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        } >"$EVIDENCE_FILE"
        # ITEM-410: the appliance's own record of the run it just performed.
        hfoc_physical_sata_capture_boot_log
        if [[ -s "$HFOC_PHYSICAL_SATA_BOOT_LOG_FILE" ]]; then
            {
                printf 'APPLIANCE_BOOT_LOG=%s\n' "$HFOC_PHYSICAL_SATA_BOOT_LOG_FILE"
                printf 'APPLIANCE_BOOT_LOG_RECORDS=%s\n' \
                    "$(grep -c '^#' "$HFOC_PHYSICAL_SATA_BOOT_LOG_FILE")"
            } >>"$EVIDENCE_FILE"
        fi
        log "physical evidence PASS recorded"

        systemctl disable "$SERVICE" >>"$LOG" 2>&1 || true
        rm -f "/etc/systemd/system/$SERVICE"
        rm -f "/opt/hfoc-physical-sata-acceptance-continue.sh"
        rm -f "$STATE_DIR/hfoc-physical-sata-continue.sh"
        systemctl daemon-reload

        systemctl start hfoc-item357-physical-resume.service
        rm -f /etc/systemd/system/hfoc-item357-physical-resume.service
        rm -f /opt/hfoc-item357-physical-resume.sh
        systemctl daemon-reload >/dev/null 2>&1 || true
        exit 0
    fi

    state=$(cat "$STATE_FILE" 2>/dev/null || true)
    if [[ "$state" == "PENDING" ]]; then
        log "marker not observed after intended BootNext: $marker"
        printf 'FAIL\n' >"$STATE_FILE"
        {
            printf 'HFOC ITEM-357 PHYSICAL SATA ACCEPTANCE\n'
            printf 'RESULT=FAIL\n'
            printf 'DEVICE=%s\n' "$HFOC_PHYSICAL_SATA_DISK"
            printf 'MODEL=%s\n' "$model"
            printf 'SERIAL=%s\n' "$serial"
            printf 'ROOT=%s\n' "$root"
            printf 'OBSERVED_MARKER=%s\n' "$marker"
            printf 'DATE_UTC=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        } >"$EVIDENCE_FILE"

        boot_num=$(cat "$BOOT_NUM_FILE" 2>/dev/null || true)
        if [[ -n "$boot_num" ]]; then
            efibootmgr -b "$boot_num" -B >>"$LOG" 2>&1 || true
        fi
        if [[ -f "$ORDER_FILE" ]]; then
            saved_order=$(cat "$ORDER_FILE")
            if [[ -n "$saved_order" ]]; then
                efibootmgr -o "$saved_order" >>"$LOG" 2>&1 || true
            fi
        fi
        systemctl disable "$SERVICE" >>"$LOG" 2>&1 || true
        rm -f "/etc/systemd/system/$SERVICE"
        rm -f "/opt/hfoc-physical-sata-acceptance-continue.sh"
        rm -f "$STATE_DIR/hfoc-physical-sata-continue.sh"
        systemctl daemon-reload
        systemctl start hfoc-item357-physical-resume.service
        rm -f /etc/systemd/system/hfoc-item357-physical-resume.service
        rm -f /opt/hfoc-item357-physical-resume.sh
        systemctl daemon-reload >/dev/null 2>&1 || true
        exit 0
    fi

    log "marker not observed (state=$state): $marker"
    exit 0
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    hfoc_physical_sata_continue "$@"
fi
