#ifndef HFOC_KEY_USE_H
#define HFOC_KEY_USE_H

#include <stdbool.h>
#include <stdint.h>

#define KEY_USE_PURPOSE_IMAGE 1U
#define KEY_USE_PURPOSE_UPDATE 2U
#define KEY_USE_PURPOSE_BACKUP 3U
#define KEY_USE_PURPOSE_INTEGRATION 4U
#define KEY_USE_PURPOSE_RECOVERY 5U
#define KEY_USE_PURPOSE_MIN KEY_USE_PURPOSE_IMAGE
#define KEY_USE_PURPOSE_MAX KEY_USE_PURPOSE_RECOVERY
#define KEY_USE_PURPOSE_BIT(purpose) \
    (1U << ((uint32_t)(purpose) - 1U))
#define KEY_USE_ACCEPTED_PURPOSES_MASK \
    (KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE) | \
     KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_UPDATE) | \
     KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_BACKUP) | \
     KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_INTEGRATION) | \
     KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_RECOVERY))
#define KEY_USE_ACCEPTED_PURPOSES_ALL KEY_USE_ACCEPTED_PURPOSES_MASK

bool key_use_purpose_valid(uint32_t purpose);
bool key_use_accepted_purposes_valid(uint32_t accepted_purposes);
bool key_use_purpose_is_accepted(uint32_t accepted_purposes,
                                 uint32_t purpose);

#endif
