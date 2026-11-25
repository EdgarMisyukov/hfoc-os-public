#include "key_use.h"

#include <stdbool.h>
#include <stdint.h>

bool key_use_purpose_valid(uint32_t purpose)
{
    return purpose >= KEY_USE_PURPOSE_MIN &&
           purpose <= KEY_USE_PURPOSE_MAX;
}

bool key_use_accepted_purposes_valid(uint32_t accepted_purposes)
{
    return accepted_purposes != 0U &&
           (accepted_purposes & ~KEY_USE_ACCEPTED_PURPOSES_MASK) == 0U;
}

bool key_use_purpose_is_accepted(uint32_t accepted_purposes,
                                 uint32_t purpose)
{
    return key_use_purpose_valid(purpose) &&
           key_use_accepted_purposes_valid(accepted_purposes) &&
           (accepted_purposes & KEY_USE_PURPOSE_BIT(purpose)) != 0U;
}
