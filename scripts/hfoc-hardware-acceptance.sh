#!/usr/bin/env bash
# ITEM-381 hardware acceptance harness.
#
# The hardware acceptance run happens on the acceptance host against the
# dedicated acceptance disk behind the identity guard of AGENTS.md 3a. This
# harness owns the parts that must be identical for every run and that must
# never be improvised:
#
#   - the declared identity and the guard: the acceptance disk, its model, its
#     serial and the running root device must match the declared constants, and
#     a mismatch aborts with exit 90 before anything else happens. The
#     constants come from scripts/hfoc-physical-sata-continue.sh, the reference
#     identity guard, so a matrix or an operator cannot widen them;
#   - the pre-write audit line: before the human physical procedure starts, the
#     run appends who or what started it, when, the disk model and serial, the
#     declared constants and the image digest to the acceptance log, and quotes
#     the same line in the machine-readable report;
#   - the run manifest and the report: every stage of the hardware acceptance
#     matrix is recorded by a human observation, and --report publishes a
#     machine-readable report where every stage is PASS, DEGRADED or
#     NOT_TESTED (or FAIL/UNOBSERVED when the gate rejects).
#
# The harness never writes to the acceptance disk. It has no code path that
# opens the device for writing: the physical procedure (install, uninstall,
# power cycle, factory reset) is performed by a human on the acceptance host,
# and the unattended worker never starts it. In CI the harness refuses to arm.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

# The declared acceptance identity and its guard constants live in one place.
# shellcheck disable=SC1091
source "$ROOT/scripts/hfoc-physical-sata-continue.sh"

MATRIX=${HFOC_HARDWARE_ACCEPTANCE_MATRIX:-$ROOT/release/hfoc-hardware-acceptance-matrix.txt}
DEVICE=${HFOC_HARDWARE_ACCEPTANCE_DEVICE:-$HFOC_PHYSICAL_SATA_DISK}
SYSFS_ROOT=${HFOC_HARDWARE_ACCEPTANCE_SYSFS_ROOT:-/sys}
IMAGE=${HFOC_HARDWARE_ACCEPTANCE_IMAGE:-}
INITIATOR=${HFOC_HARDWARE_ACCEPTANCE_INITIATOR:-${USER:-uid-$(id -u)}}
RUN_DIR=${HFOC_HARDWARE_ACCEPTANCE_RUN_DIR:-}
MANIFEST=${HFOC_HARDWARE_ACCEPTANCE_MANIFEST:-}
CONFIRM=${HFOC_HARDWARE_ACCEPTANCE_CONFIRM:-}
MODE=--plan

usage() {
    cat >&2 <<'EOF'
usage: hfoc-hardware-acceptance.sh [--plan|--guard|--arm|--record|--report]

  --plan                     print the acceptance plan and the observed identity (read-only)
  --guard                    run the identity guard only; exit 90 on a mismatch
  --arm                      human-triggered start: guard, pre-write audit line and run
                             manifest; requires HFOC_HARDWARE_ACCEPTANCE_CONFIRM=ITEM-381
                             and refuses inside CI; exits 92 awaiting the human physical run
  --record STAGE STATUS EVIDENCE
                             record one human hardware observation into the armed manifest
  --report                   render and verify the machine-readable report

environment:
  HFOC_HARDWARE_ACCEPTANCE_IMAGE      image whose SHA-256 the audit line quotes (required by --arm)
  HFOC_HARDWARE_ACCEPTANCE_RUN_DIR    run artifact directory
  HFOC_HARDWARE_ACCEPTANCE_MANIFEST   run manifest path (default <run dir>/manifest.txt)
  HFOC_HARDWARE_ACCEPTANCE_DEVICE     acceptance device (default /dev/sda)
EOF
    exit 2
}

RECORD_STAGE=
RECORD_STATUS=
RECORD_EVIDENCE=
while (( $# > 0 )); do
    case "$1" in
        --plan|--guard|--arm|--report)
            MODE=$1
            shift
            ;;
        --record)
            (( $# == 4 )) || usage
            MODE=--record
            RECORD_STAGE=$2
            RECORD_STATUS=$3
            RECORD_EVIDENCE=$4
            shift 4
            ;;
        --help|-h)
            usage
            ;;
        *)
            usage
            ;;
    esac
done

read_observed_model() {
    cat "$SYSFS_ROOT/block/${DEVICE##*/}/device/model" 2>/dev/null || true
}

read_observed_serial() {
    lsblk -dn -o SERIAL "$DEVICE" 2>/dev/null || true
}

read_observed_root() {
    findmnt -no SOURCE -T / 2>/dev/null || true
}

observed_model=$(read_observed_model)
observed_serial=$(read_observed_serial)
observed_root=$(read_observed_root)
observed_model=${observed_model%$'\n'}
observed_serial=${observed_serial%$'\n'}
observed_root=${observed_root%$'\n'}

identity_matches() {
    [[ "$observed_serial" == "$HFOC_PHYSICAL_SATA_EXPECTED_SERIAL" &&
       "$observed_root" == "$HFOC_PHYSICAL_SATA_EXPECTED_ROOT" ]] || return 1
    # shellcheck disable=SC2254
    case "$observed_model" in
        $HFOC_PHYSICAL_SATA_EXPECTED_MODEL) return 0 ;;
        *) return 1 ;;
    esac
}

guard_or_exit() {
    if ! identity_matches; then
        echo "identity guard failed: device=${DEVICE} model=${observed_model} serial=${observed_serial} root=${observed_root}" >&2
        exit 90
    fi
}

require_matrix() {
    bash "$ROOT/scripts/verify-hardware-acceptance-matrix.sh" "$MATRIX" >/dev/null || {
        echo "hardware acceptance matrix is invalid: $MATRIX" >&2
        exit 91
    }
}

resolve_run_dir() {
    if [[ -z "$RUN_DIR" ]]; then
        RUN_DIR="$ROOT/build/test-artifacts/hardware-acceptance-$(date -u +%Y%m%dT%H%M%SZ)-$$"
    fi
    if [[ -z "$MANIFEST" ]]; then
        MANIFEST=$RUN_DIR/manifest.txt
    fi
}

matrix_stage_count() {
    sed -n 's/^stage_count=//p' "$MATRIX"
}

stage_id_at() {
    sed -n "s/^stage_$1_id=//p" "$MATRIX"
}

stage_field_at() {
    sed -n "s/^stage_$1_$2=//p" "$MATRIX"
}

write_manifest() {
    local stage_count=$1
    local index
    local now=$2

    mkdir -p "$RUN_DIR"
    {
        printf 'run_id=%s\n' "${RUN_DIR##*/}"
        printf 'source_revision=%s\n' "$(git rev-parse HEAD 2>/dev/null || echo unknown)"
        printf 'acceptance_device=%s\n' "$DEVICE"
        printf 'declared_model=%s\n' "$HFOC_PHYSICAL_SATA_EXPECTED_MODEL"
        printf 'declared_serial=%s\n' "$HFOC_PHYSICAL_SATA_EXPECTED_SERIAL"
        printf 'declared_root=%s\n' "$HFOC_PHYSICAL_SATA_EXPECTED_ROOT"
        printf 'observed_model=%s\n' "$observed_model"
        printf 'observed_serial=%s\n' "$observed_serial"
        printf 'observed_root=%s\n' "$observed_root"
        printf 'image_sha256=%s\n' "$image_sha256"
        printf 'audit_line=%s\n' "$audit_line"
        printf 'run_started_utc=%s\n' "$now"
        printf 'stage_count=%s\n' "$stage_count"
        for ((index = 1; index <= stage_count; ++index)); do
            printf 'stage_%s_id=%s\n' "$index" "$(stage_id_at "$index")"
            printf 'stage_%s_name=%s\n' "$index" "$(stage_field_at "$index" name)"
            printf 'stage_%s_owner=%s\n' "$index" "$(stage_field_at "$index" owner)"
            printf 'stage_%s_kind=%s\n' "$index" "$(stage_field_at "$index" kind)"
            printf 'stage_%s_status=NOT_TESTED\n' "$index"
            printf 'stage_%s_evidence=none\n' "$index"
            printf 'stage_%s_reason=the physical observation has not been recorded\n' "$index"
            printf 'stage_%s_hardware_evidence=\n' "$index"
        done
    } >"$MANIFEST"
}

print_plan() {
    local stage_count=$1
    local index

    printf 'hardware_acceptance_plan=1\n'
    printf 'acceptance_device=%s\n' "$DEVICE"
    printf 'declared_model=%s\n' "$HFOC_PHYSICAL_SATA_EXPECTED_MODEL"
    printf 'declared_serial=%s\n' "$HFOC_PHYSICAL_SATA_EXPECTED_SERIAL"
    printf 'declared_root=%s\n' "$HFOC_PHYSICAL_SATA_EXPECTED_ROOT"
    printf 'observed_model=%s\n' "${observed_model:-UNAVAILABLE}"
    printf 'observed_serial=%s\n' "${observed_serial:-UNAVAILABLE}"
    printf 'observed_root=%s\n' "${observed_root:-UNAVAILABLE}"
    printf 'stage_count=%s\n' "$stage_count"
    for ((index = 1; index <= stage_count; ++index)); do
        printf 'stage_%s=%s\n' "$index" "$(stage_id_at "$index")"
    done
}

case "$MODE" in
    --plan)
        require_matrix
        print_plan "$(matrix_stage_count)"
        echo "hardware acceptance plan: read-only; nothing was written"
        ;;
    --guard)
        require_matrix
        guard_or_exit
        printf 'identity guard OK: device=%s model=%s serial=%s root=%s\n' \
            "$DEVICE" "$observed_model" "$observed_serial" "$observed_root"
        ;;
    --arm)
        if [[ -n "${CI:-}" ]]; then
            echo "hardware acceptance never arms in CI" >&2
            exit 1
        fi
        if [[ "$CONFIRM" != "ITEM-381" ]]; then
            echo "hardware acceptance requires HFOC_HARDWARE_ACCEPTANCE_CONFIRM=ITEM-381" >&2
            exit 1
        fi
        if [[ -z "$IMAGE" || ! -f "$IMAGE" ]]; then
            echo "hardware acceptance requires an existing HFOC_HARDWARE_ACCEPTANCE_IMAGE" >&2
            exit 1
        fi
        require_matrix
        guard_or_exit
        resolve_run_dir
        image_sha256=$(sha256sum "$IMAGE" | awk '{print $1}')
        started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
        started_compact=$(date -u +%Y%m%dT%H%M%SZ)
        audit_line="audit initiator=${INITIATOR} started_utc=${started_utc} device=${DEVICE} model=${observed_model} serial=${observed_serial} declared_model=${HFOC_PHYSICAL_SATA_EXPECTED_MODEL} declared_serial=${HFOC_PHYSICAL_SATA_EXPECTED_SERIAL} declared_root=${HFOC_PHYSICAL_SATA_EXPECTED_ROOT} image_sha256=${image_sha256}"
        mkdir -p "$RUN_DIR"
        {
            printf '%s\n' "$audit_line"
            printf 'hardware acceptance armed; awaiting the human physical run\n'
        } >>"$RUN_DIR/acceptance.log"
        write_manifest "$(matrix_stage_count)" "$started_compact"
        printf 'hardware acceptance armed: %s\n' "$MANIFEST"
        printf '%s\n' "$audit_line"
        echo "awaiting human physical run: install, observe every stage and record it with --record"
        exit 92
        ;;
    --record)
        resolve_run_dir
        [[ -f "$MANIFEST" ]] || {
            echo "hardware acceptance has no armed run: $MANIFEST" >&2
            exit 1
        }
        stage_count=$(sed -n 's/^stage_count=//p' "$MANIFEST")
        stage_index=
        for ((index = 1; index <= stage_count; ++index)); do
            if [[ "$(sed -n "s/^stage_${index}_id=//p" "$MANIFEST")" == "$RECORD_STAGE" ]]; then
                stage_index=$index
                break
            fi
        done
        if [[ -z "$stage_index" ]]; then
            echo "hardware acceptance has no stage ${RECORD_STAGE}" >&2
            exit 1
        fi
        case "$RECORD_STATUS" in
            PASS|DEGRADED)
                if [[ -z "$RECORD_EVIDENCE" ]]; then
                    echo "hardware acceptance requires evidence for ${RECORD_STATUS}" >&2
                    exit 1
                fi
                record_evidence=hardware
                record_reason=
                record_hardware_evidence=$RECORD_EVIDENCE
                ;;
            NOT_TESTED)
                if [[ -z "$RECORD_EVIDENCE" ]]; then
                    RECORD_EVIDENCE="the physical observation has not been recorded"
                fi
                record_evidence=none
                record_reason=$RECORD_EVIDENCE
                record_hardware_evidence=
                ;;
            *)
                echo "unsupported hardware acceptance status: ${RECORD_STATUS}" >&2
                exit 1
                ;;
        esac
        awk -v stage_no="$stage_index" \
            -v status="$RECORD_STATUS" \
            -v evidence="$record_evidence" \
            -v reason="$record_reason" \
            -v hardware_evidence="$record_hardware_evidence" '
            $0 ~ "^stage_" stage_no "_status=" {
                print "stage_" stage_no "_status=" status; next
            }
            $0 ~ "^stage_" stage_no "_evidence=" {
                print "stage_" stage_no "_evidence=" evidence; next
            }
            $0 ~ "^stage_" stage_no "_reason=" {
                print "stage_" stage_no "_reason=" reason; next
            }
            $0 ~ "^stage_" stage_no "_hardware_evidence=" {
                print "stage_" stage_no "_hardware_evidence=" hardware_evidence; next
            }
            { print }
        ' "$MANIFEST" >"$MANIFEST.tmp"
        mv "$MANIFEST.tmp" "$MANIFEST"
        printf 'recorded %s %s\n' "$RECORD_STAGE" "$RECORD_STATUS"
        ;;
    --report)
        resolve_run_dir
        [[ -f "$MANIFEST" ]] || {
            echo "hardware acceptance has no armed run: $MANIFEST" >&2
            exit 1
        }
        stage_count=$(sed -n 's/^stage_count=//p' "$MANIFEST")
        report=$RUN_DIR/report.txt
        run_finished_utc=$(date -u +%Y%m%dT%H%M%SZ)
        passed=0
        degraded=0
        not_tested=0
        failing=0
        {
            printf 'hfoc_hardware_acceptance_report_version=1\n'
            printf 'target_release_id=%s\n' "${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}"
            printf 'source_revision=%s\n' "$(sed -n 's/^source_revision=//p' "$MANIFEST")"
            printf 'acceptance_device=%s\n' "$(sed -n 's/^acceptance_device=//p' "$MANIFEST")"
            printf 'declared_model=%s\n' "$(sed -n 's/^declared_model=//p' "$MANIFEST")"
            printf 'declared_serial=%s\n' "$(sed -n 's/^declared_serial=//p' "$MANIFEST")"
            printf 'declared_root=%s\n' "$(sed -n 's/^declared_root=//p' "$MANIFEST")"
            printf 'observed_model=%s\n' "$(sed -n 's/^observed_model=//p' "$MANIFEST")"
            printf 'observed_serial=%s\n' "$(sed -n 's/^observed_serial=//p' "$MANIFEST")"
            printf 'observed_root=%s\n' "$(sed -n 's/^observed_root=//p' "$MANIFEST")"
            printf 'image_sha256=%s\n' "$(sed -n 's/^image_sha256=//p' "$MANIFEST")"
            printf 'audit_line=%s\n' "$(sed -n 's/^audit_line=//p' "$MANIFEST")"
            printf 'run_started_utc=%s\n' "$(sed -n 's/^run_started_utc=//p' "$MANIFEST")"
            printf 'run_finished_utc=%s\n' "$run_finished_utc"
            printf 'stage_count=%s\n' "$stage_count"
            for ((index = 1; index <= stage_count; ++index)); do
                status=$(sed -n "s/^stage_${index}_status=//p" "$MANIFEST")
                case "$status" in
                    PASS) passed=$((passed + 1)) ;;
                    DEGRADED) degraded=$((degraded + 1)) ;;
                    NOT_TESTED) not_tested=$((not_tested + 1)) ;;
                    *) failing=$((failing + 1)) ;;
                esac
                printf 'stage_%s_id=%s\n' "$index" "$(sed -n "s/^stage_${index}_id=//p" "$MANIFEST")"
                printf 'stage_%s_name=%s\n' "$index" "$(sed -n "s/^stage_${index}_name=//p" "$MANIFEST")"
                printf 'stage_%s_owner=%s\n' "$index" "$(sed -n "s/^stage_${index}_owner=//p" "$MANIFEST")"
                printf 'stage_%s_kind=%s\n' "$index" "$(sed -n "s/^stage_${index}_kind=//p" "$MANIFEST")"
                printf 'stage_%s_status=%s\n' "$index" "$status"
                printf 'stage_%s_evidence=%s\n' "$index" "$(sed -n "s/^stage_${index}_evidence=//p" "$MANIFEST")"
                printf 'stage_%s_reason=%s\n' "$index" "$(sed -n "s/^stage_${index}_reason=//p" "$MANIFEST")"
                printf 'stage_%s_hardware_evidence=%s\n' "$index" "$(sed -n "s/^stage_${index}_hardware_evidence=//p" "$MANIFEST")"
            done
            if (( failing != 0 )); then
                gate_status=REJECT
                gate_reason="${failing} hardware acceptance stages failed"
            elif (( not_tested != 0 )); then
                gate_status=INCOMPLETE
                gate_reason="${not_tested} hardware acceptance stages have not been observed"
            else
                gate_status=ACCEPT
                gate_reason="every hardware acceptance stage was observed"
            fi
            printf 'physical_run=%s\n' "$([[ "$gate_status" == "ACCEPT" ]] && echo YES || echo NO)"
            printf 'gate_status=%s\n' "$gate_status"
            printf 'gate_reason=%s\n' "$gate_reason"
        } >"$report"
        bash "$ROOT/scripts/verify-hardware-acceptance-report.sh" \
            "$report" "$MATRIX" >/dev/null || {
            echo "hardware acceptance report is malformed: $report" >&2
            exit 91
        }
        echo "Hardware acceptance report: $report"
        cat "$report"
        ;;
    *)
        usage
        ;;
esac
