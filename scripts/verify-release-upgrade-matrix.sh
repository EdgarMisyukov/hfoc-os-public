#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
MATRIX=${1:-$ROOT/release/release-upgrade-matrix.txt}
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
ALLOWED_IMAGE_IDS=${HFOC_RELEASE_UPGRADE_ALLOWED_IMAGE_IDS:-1:2}

[[ -f "$MATRIX" ]] || {
    echo "Missing release upgrade matrix: $MATRIX" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$MATRIX") -ne 1 ]]; then
        echo "Release upgrade matrix is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$MATRIX")
    if [[ -z "$value" ]]; then
        echo "Release upgrade matrix has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

require_positive() {
    local key=$1
    local value

    value=$(required_value "$key")
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "Release upgrade matrix has invalid ${key}" >&2
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
        echo "Release upgrade matrix ${key} mismatch" >&2
        exit 1
    }
}

matrix_version=$(require_positive release_upgrade_matrix_version)
[[ "$matrix_version" == "1" ]] || {
    echo "Unsupported release upgrade matrix version" >&2
    exit 1
}

path_count=$(require_positive path_count)
if (( path_count > 64 )); then
    echo "Release upgrade matrix path count exceeds bound" >&2
    exit 1
fi
while IFS= read -r line; do
    if [[ "$line" =~ ^path_([0-9]+)_ ]]; then
        path_number=${BASH_REMATCH[1]}
        if (( path_number > path_count )); then
            echo "Release upgrade matrix contains undeclared path ${path_number}" >&2
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

IFS=':' read -r -a allowed_range <<<"$ALLOWED_IMAGE_IDS"
if [[ ${#allowed_range[@]} -ne 2 ||
      ! "${allowed_range[0]}" =~ ^[1-9][0-9]*$ ||
      ! "${allowed_range[1]}" =~ ^[1-9][0-9]*$ ||
      "${allowed_range[0]}" -gt "${allowed_range[1]}" ]]; then
    echo "Invalid allowed source image ID range" >&2
    exit 1
fi
id_min=${allowed_range[0]}
id_max=${allowed_range[1]}

range_fields=(
    source_schema_min
    source_schema_max
    target_schema_min
    target_schema_max
    source_kernel_abi_min
    source_kernel_abi_max
    target_kernel_abi_min
    target_kernel_abi_max
    source_service_abi_min
    source_service_abi_max
    target_service_abi_min
    target_service_abi_max
    source_policy_generation_min
    source_policy_generation_max
    target_policy_generation_min
    target_policy_generation_max
)

range_expected() {
    local field=$1

    case "$field" in
    source_schema_min|source_schema_max|target_schema_min|target_schema_max)
        printf '%s' "$CONFIG_SCHEMA_VERSION"
        ;;
    source_kernel_abi_min|source_kernel_abi_max|target_kernel_abi_min|target_kernel_abi_max)
        printf '%s' "$KERNEL_ABI_VERSION"
        ;;
    source_service_abi_min|source_service_abi_max|target_service_abi_min|target_service_abi_max)
        printf '%s' "$SERVICE_ABI_VERSION"
        ;;
    source_policy_generation_min|source_policy_generation_max|target_policy_generation_min|target_policy_generation_max)
        printf '%s' "$POLICY_VERSION"
        ;;
    *)
        echo "Unknown release upgrade range field: $field" >&2
        exit 1
        ;;
    esac
}

path_decision=""
path_forward=""
path_rollback=""
source_image_id=""
target_image_id=""

check_path() {
    local index=$1
    local prefix="path_${index}_"
    local field
    local actual
    local expected
    local all_compatible=yes
    local ids_valid=yes

    path_decision=$(required_value "${prefix}decision")
    path_forward=$(required_value "${prefix}forward")
    path_rollback=$(required_value "${prefix}rollback")
    source_image_id=$(required_value "${prefix}source_image_id")
    target_image_id=$(required_value "${prefix}target_image_id")

    [[ "$path_decision" == "allowed" || "$path_decision" == "rejected" ]] || {
        echo "Release upgrade matrix path ${index} has invalid decision" >&2
        exit 1
    }
    [[ "$path_forward" == "yes" || "$path_forward" == "no" ]] || {
        echo "Release upgrade matrix path ${index} has invalid forward" >&2
        exit 1
    }
    [[ "$path_rollback" == "yes" || "$path_rollback" == "no" ]] || {
        echo "Release upgrade matrix path ${index} has invalid rollback" >&2
        exit 1
    }
    [[ "$source_image_id" =~ ^[1-9][0-9]*$ &&
       "$target_image_id" =~ ^[1-9][0-9]*$ ]] || {
        echo "Release upgrade matrix path ${index} has invalid image ID" >&2
        exit 1
    }

    if [[ "$path_decision" == "allowed" ]]; then
        [[ "$path_forward" == "yes" || "$path_rollback" == "yes" ]] || {
            echo "Release upgrade matrix allowed path ${index} has no action" >&2
            exit 1
        }
        for field in "${range_fields[@]}"; do
            actual=$(required_value "${prefix}${field}")
            expected=$(range_expected "$field")
            [[ "$actual" == "$expected" ]] || {
                echo "Release upgrade matrix allowed path ${index} ${field} mismatch" >&2
                exit 1
            }
        done
        if (( source_image_id < id_min || source_image_id > id_max ||
              target_image_id < id_min || target_image_id > id_max )); then
            echo "Release upgrade matrix allowed path ${index} has unknown image ID" >&2
            exit 1
        fi
    else
        [[ "$path_forward" == "no" && "$path_rollback" == "no" ]] || {
            echo "Release upgrade matrix rejected path ${index} enables an action" >&2
            exit 1
        }
        for field in "${range_fields[@]}"; do
            actual=$(required_value "${prefix}${field}")
            expected=$(range_expected "$field")
            if [[ "$actual" != "$expected" ]]; then
                all_compatible=no
            fi
        done
        if (( source_image_id < id_min || source_image_id > id_max ||
              target_image_id < id_min || target_image_id > id_max )); then
            ids_valid=no
        fi
        if [[ "$all_compatible" == "yes" && "$ids_valid" == "yes" ]]; then
            echo "Release upgrade matrix rejected path ${index} is actually compatible" >&2
            exit 1
        fi
    fi
}

for ((path_index = 1; path_index <= path_count; ++path_index)); do
    check_path "$path_index"
done

echo "Release upgrade matrix verification: OK"
