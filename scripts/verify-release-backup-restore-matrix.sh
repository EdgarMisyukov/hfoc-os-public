#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
MATRIX=${1:-$ROOT/release/release-backup-restore-matrix.txt}
RELEASE_ID=${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}
ARCHITECTURE=${HFOC_RELEASE_PROFILE_ARCH:-x86_64}
IMAGE_GENERATION=${HFOC_RELEASE_IMAGE_GENERATION:-1}
POLICY_VERSION=${HFOC_RELEASE_POLICY_VERSION:-1}
CONFIG_SCHEMA_VERSION=${HFOC_RELEASE_CONFIG_SCHEMA_VERSION:-1}
STATE_SCHEMA_VERSION=${HFOC_RELEASE_STATE_SCHEMA_VERSION:-1}
IMAGE_MANIFEST_VERSION=${HFOC_RELEASE_IMAGE_MANIFEST_VERSION:-1}
UPDATE_COMPATIBILITY_VERSION=${HFOC_RELEASE_UPDATE_COMPATIBILITY_VERSION:-1}
KERNEL_ABI_VERSION=${HFOC_RELEASE_KERNEL_ABI_VERSION:-1}
SERVICE_ABI_VERSION=${HFOC_RELEASE_SERVICE_ABI_VERSION:-1}
INTEGRATION_SCHEMA_VERSION=${HFOC_RELEASE_INTEGRATION_SCHEMA_VERSION:-1}
INTEGRATION_TRANSPORT_VERSION=${HFOC_RELEASE_INTEGRATION_TRANSPORT_VERSION:-1}
BACKUP_STREAM_FORMAT_VERSION=${HFOC_RELEASE_BACKUP_STREAM_FORMAT_VERSION:-1}
BACKUP_MANIFEST_VERSION=${HFOC_RELEASE_BACKUP_MANIFEST_VERSION:-1}
ALLOWED_IMAGE_IDS=${HFOC_RELEASE_BACKUP_ALLOWED_IMAGE_IDS:-1:2}
ALLOWED_CONFIG_SCHEMA_VERSIONS=${HFOC_RELEASE_BACKUP_ALLOWED_CONFIG_SCHEMA_VERSIONS:-1:1}
ALLOWED_STATE_SCHEMA_VERSIONS=${HFOC_RELEASE_BACKUP_ALLOWED_STATE_SCHEMA_VERSIONS:-1:1}
ALLOWED_BACKUP_STREAM_FORMAT_VERSIONS=${HFOC_RELEASE_BACKUP_ALLOWED_BACKUP_STREAM_FORMAT_VERSIONS:-1:1}
ALLOWED_BACKUP_MANIFEST_VERSIONS=${HFOC_RELEASE_BACKUP_ALLOWED_BACKUP_MANIFEST_VERSIONS:-1:1}

[[ -f "$MATRIX" ]] || {
    echo "Missing release backup/restore matrix: $MATRIX" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$MATRIX") -ne 1 ]]; then
        echo "Release backup/restore matrix is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$MATRIX")
    if [[ -z "$value" ]]; then
        echo "Release backup/restore matrix has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

require_positive() {
    local key=$1
    local value

    value=$(required_value "$key")
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "Release backup/restore matrix has invalid ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

compare_target() {
    local key=$1
    local expected=$2
    local actual

    actual=$(required_value "$key")
    [[ "$actual" == "$expected" ]] || {
        echo "Release backup/restore matrix ${key} mismatch" >&2
        exit 1
    }
}

parse_range() {
    local label=$1
    local value=$2
    local min_value
    local max_value

    IFS=':' read -r -a parts <<<"$value"
    min_value=${parts[0]:-}
    max_value=${parts[1]:-}
    if [[ ${#parts[@]} -ne 2 ||
          ! "$min_value" =~ ^[1-9][0-9]*$ ||
          ! "$max_value" =~ ^[1-9][0-9]*$ ||
          "$min_value" -gt "$max_value" ]]; then
        echo "Release backup/restore matrix has invalid ${label}" >&2
        exit 1
    fi

    case "$label" in
    allowed_source_image_ids)
        allowed_image_min=$min_value
        allowed_image_max=$max_value
        ;;
    allowed_source_config_schema_versions)
        allowed_config_min=$min_value
        allowed_config_max=$max_value
        ;;
    allowed_source_state_schema_versions)
        allowed_state_min=$min_value
        allowed_state_max=$max_value
        ;;
    allowed_source_backup_stream_format_versions)
        allowed_stream_min=$min_value
        allowed_stream_max=$max_value
        ;;
    allowed_source_backup_manifest_versions)
        allowed_manifest_min=$min_value
        allowed_manifest_max=$max_value
        ;;
    *)
        echo "Unknown release backup/restore range: ${label}" >&2
        exit 1
        ;;
    esac
}

matrix_version=$(require_positive release_backup_restore_matrix_version)
[[ "$matrix_version" == "1" ]] || {
    echo "Unsupported release backup/restore matrix version" >&2
    exit 1
}

path_count=$(require_positive path_count)
if (( path_count > 64 )); then
    echo "Release backup/restore matrix path count exceeds bound" >&2
    exit 1
fi
while IFS= read -r line; do
    if [[ "$line" =~ ^path_([0-9]+)_ ]]; then
        path_number=${BASH_REMATCH[1]}
        if (( path_number > path_count )); then
            echo "Release backup/restore matrix contains undeclared path ${path_number}" >&2
            exit 1
        fi
    fi
done < "$MATRIX"

compare_target target_release_id "$RELEASE_ID"
compare_target target_architecture "$ARCHITECTURE"
compare_target target_image_generation "$IMAGE_GENERATION"
compare_target target_policy_version "$POLICY_VERSION"
compare_target target_config_schema_version "$CONFIG_SCHEMA_VERSION"
compare_target target_state_schema_version "$STATE_SCHEMA_VERSION"
compare_target target_image_manifest_version "$IMAGE_MANIFEST_VERSION"
compare_target target_update_compatibility_version \
    "$UPDATE_COMPATIBILITY_VERSION"
compare_target target_kernel_abi_version "$KERNEL_ABI_VERSION"
compare_target target_service_abi_version "$SERVICE_ABI_VERSION"
compare_target target_integration_schema_version \
    "$INTEGRATION_SCHEMA_VERSION"
compare_target target_integration_transport_version \
    "$INTEGRATION_TRANSPORT_VERSION"
compare_target target_backup_stream_format_version \
    "$BACKUP_STREAM_FORMAT_VERSION"
compare_target target_backup_manifest_version "$BACKUP_MANIFEST_VERSION"

parse_range allowed_source_image_ids "$ALLOWED_IMAGE_IDS"
parse_range allowed_source_config_schema_versions \
    "$ALLOWED_CONFIG_SCHEMA_VERSIONS"
parse_range allowed_source_state_schema_versions \
    "$ALLOWED_STATE_SCHEMA_VERSIONS"
parse_range allowed_source_backup_stream_format_versions \
    "$ALLOWED_BACKUP_STREAM_FORMAT_VERSIONS"
parse_range allowed_source_backup_manifest_versions \
    "$ALLOWED_BACKUP_MANIFEST_VERSIONS"

range_fields=(
    source_config_schema_min
    source_config_schema_max
    source_state_schema_min
    source_state_schema_max
    target_config_schema_min
    target_config_schema_max
    target_state_schema_min
    target_state_schema_max
    source_backup_stream_format_min
    source_backup_stream_format_max
    source_backup_manifest_min
    source_backup_manifest_max
    target_backup_stream_format_min
    target_backup_stream_format_max
    target_backup_manifest_min
    target_backup_manifest_max
)

range_expected() {
    local field=$1

    case "$field" in
    source_config_schema_min|source_config_schema_max|target_config_schema_min|target_config_schema_max)
        printf '%s' "$CONFIG_SCHEMA_VERSION"
        ;;
    source_state_schema_min|source_state_schema_max|target_state_schema_min|target_state_schema_max)
        printf '%s' "$STATE_SCHEMA_VERSION"
        ;;
    source_backup_stream_format_min|source_backup_stream_format_max|target_backup_stream_format_min|target_backup_stream_format_max)
        printf '%s' "$BACKUP_STREAM_FORMAT_VERSION"
        ;;
    source_backup_manifest_min|source_backup_manifest_max|target_backup_manifest_min|target_backup_manifest_max)
        printf '%s' "$BACKUP_MANIFEST_VERSION"
        ;;
    *)
        echo "Unknown release backup/restore range field: $field" >&2
        exit 1
        ;;
    esac
}

range_allowed() {
    local field=$1
    local value=$2

    case "$field" in
    source_config_schema_min|source_config_schema_max)
        [[ "$value" -ge "$allowed_config_min" &&
           "$value" -le "$allowed_config_max" ]]
        ;;
    source_state_schema_min|source_state_schema_max)
        [[ "$value" -ge "$allowed_state_min" &&
           "$value" -le "$allowed_state_max" ]]
        ;;
    source_backup_stream_format_min|source_backup_stream_format_max)
        [[ "$value" -ge "$allowed_stream_min" &&
           "$value" -le "$allowed_stream_max" ]]
        ;;
    source_backup_manifest_min|source_backup_manifest_max)
        [[ "$value" -ge "$allowed_manifest_min" &&
           "$value" -le "$allowed_manifest_max" ]]
        ;;
    *)
        return 1
        ;;
    esac
}

check_path() {
    local index=$1
    local prefix="path_${index}_"
    local path_decision
    local field
    local actual
    local expected
    local all_compatible=yes
    local ids_valid=yes
    local path_backup
    local path_restore
    local path_rollback
    local source_image_id
    local target_image_id

    path_backup=$(required_value "${prefix}backup")
    path_restore=$(required_value "${prefix}restore")
    path_rollback=$(required_value "${prefix}rollback")
    source_image_id=$(required_value "${prefix}source_image_id")
    target_image_id=$(required_value "${prefix}target_image_id")

    [[ "$path_backup" == "yes" || "$path_backup" == "no" ]] || {
        echo "Release backup/restore matrix path ${index} has invalid backup" >&2
        exit 1
    }
    [[ "$path_restore" == "yes" || "$path_restore" == "no" ]] || {
        echo "Release backup/restore matrix path ${index} has invalid restore" >&2
        exit 1
    }
    [[ "$path_rollback" == "yes" || "$path_rollback" == "no" ]] || {
        echo "Release backup/restore matrix path ${index} has invalid rollback" >&2
        exit 1
    }
    [[ "$source_image_id" =~ ^[1-9][0-9]*$ &&
       "$target_image_id" =~ ^[1-9][0-9]*$ ]] || {
        echo "Release backup/restore matrix path ${index} has invalid image ID" >&2
        exit 1
    }

    path_decision=$(required_value "${prefix}decision")
    [[ "$path_decision" == "allowed" ||
       "$path_decision" == "rejected" ]] || {
        echo "Release backup/restore matrix path ${index} has invalid decision" >&2
        exit 1
    }

    if [[ "$path_decision" == "allowed" ]]; then
        [[ "$path_backup" == "yes" ||
           "$path_restore" == "yes" ||
           "$path_rollback" == "yes" ]] || {
            echo "Release backup/restore matrix allowed path ${index} has no action" >&2
            exit 1
        }
        for field in "${range_fields[@]}"; do
            actual=$(required_value "${prefix}${field}")
            expected=$(range_expected "$field")
            [[ "$actual" == "$expected" ]] || {
                echo "Release backup/restore matrix allowed path ${index} ${field} mismatch" >&2
                exit 1
            }
            if [[ "$field" == source_* ]] &&
                    ! range_allowed "$field" "$actual"; then
                echo "Release backup/restore matrix allowed path ${index} ${field} outside allowed range" >&2
                exit 1
            fi
        done
        if (( source_image_id < allowed_image_min ||
              source_image_id > allowed_image_max ||
              target_image_id < allowed_image_min ||
              target_image_id > allowed_image_max )); then
            echo "Release backup/restore matrix allowed path ${index} has unknown image ID" >&2
            exit 1
        fi
    else
        [[ "$path_backup" == "no" &&
           "$path_restore" == "no" &&
           "$path_rollback" == "no" ]] || {
            echo "Release backup/restore matrix rejected path ${index} enables an action" >&2
            exit 1
        }
        for field in "${range_fields[@]}"; do
            actual=$(required_value "${prefix}${field}")
            expected=$(range_expected "$field")
            if [[ "$actual" != "$expected" ]]; then
                all_compatible=no
            fi
            if [[ "$field" == source_* ]] &&
                    ! range_allowed "$field" "$actual"; then
                all_compatible=no
            fi
        done
        if (( source_image_id < allowed_image_min ||
              source_image_id > allowed_image_max ||
              target_image_id < allowed_image_min ||
              target_image_id > allowed_image_max )); then
            ids_valid=no
        fi
        if [[ "$all_compatible" == "yes" && "$ids_valid" == "yes" ]]; then
            echo "Release backup/restore matrix rejected path ${index} is actually compatible" >&2
            exit 1
        fi
    fi
}

for ((path_index = 1; path_index <= path_count; ++path_index)); do
    check_path "$path_index"
done

echo "Release backup/restore matrix verification: OK"
