#include "nic_registry.h"

#include <stddef.h>
#include <string.h>

/*
 * ITEM-492: the registry and the description, implemented as values.
 *
 * Nothing here touches a PCI bus, a driver or a device: a backend is a name and
 * a claim, a device is an identity and a state, and every question this file
 * answers - who claims this id, what is this device's state, what does the
 * record say - is answered from those values.  The discovery half
 * (`nic_discovery.c`) is what fills them in from the machine.
 */

static struct nic_backend nic_registry_backends[NIC_REGISTRY_BACKENDS_MAX];
static size_t nic_registry_used;

/* One bounded line builder, the same shape the other records use. */
struct nic_line {
    char *text;
    size_t size;
    size_t length;
};

static void nic_line_reset(struct nic_line *line, char *text, size_t size)
{
    line->text = text;
    line->size = size;
    line->length = 0U;
    if (size != 0U) {
        text[0] = '\0';
    }
}

static void nic_line_append(struct nic_line *line, const char *text)
{
    size_t index = 0U;

    if (text == NULL) {
        return;
    }
    while (text[index] != '\0' && line->length + 1U < line->size) {
        line->text[line->length++] = text[index++];
    }
    line->text[line->length] = '\0';
}

static void nic_line_u64(struct nic_line *line, uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    if (value == 0U) {
        nic_line_append(line, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        char single[2U];

        single[0] = digits[--count];
        single[1] = '\0';
        nic_line_append(line, single);
    }
}

/* Four upper-case hex digits: a PCI id, printed the way the records print them. */
static void nic_line_hex16(struct nic_line *line, uint16_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[5U];
    size_t index;

    for (index = 0U; index < 4U; ++index) {
        text[index] = digits[(value >> ((3U - index) * 4U)) & 0xFU];
    }
    text[4] = '\0';
    nic_line_append(line, text);
}

/* Two lower-case hex digits: one byte of a MAC address or of a BDF. */
static void nic_line_hex8(struct nic_line *line, uint8_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[3U];

    text[0] = digits[(value >> 4U) & 0xFU];
    text[1] = digits[value & 0xFU];
    text[2] = '\0';
    nic_line_append(line, text);
}

void nic_registry_reset(void)
{
    memset(nic_registry_backends, 0, sizeof(nic_registry_backends));
    nic_registry_used = 0U;
}

static bool nic_registry_claim_valid(const struct nic_backend *backend)
{
    if (backend == NULL || backend->name == NULL || backend->name[0] == '\0') {
        return false;
    }
    if (backend->vendor_id == NIC_VENDOR_ANY) {
        /* Claiming every vendor is not a claim: it is a wildcard that would
         * make the registry order decide which driver a machine gets. */
        return false;
    }
    if (!backend->match_any_device &&
        backend->device_id == NIC_VENDOR_ANY) {
        return false;
    }
    return true;
}

static bool nic_registry_claim_equal(const struct nic_backend *left,
                                     const struct nic_backend *right)
{
    return left->vendor_id == right->vendor_id &&
           left->match_any_device == right->match_any_device &&
           (left->match_any_device ||
            left->device_id == right->device_id);
}

bool nic_registry_register(const struct nic_backend *backend)
{
    size_t index;

    if (!nic_registry_claim_valid(backend)) {
        return false;
    }
    if (nic_registry_used >= NIC_REGISTRY_BACKENDS_MAX) {
        return false;
    }
    /*
     * Two backends claiming one id would make the driver a machine gets a
     * property of the registration order.  Refuse the second one instead: an
     * ambiguous machine is a machine nobody can describe honestly.
     */
    for (index = 0U; index < nic_registry_used; ++index) {
        if (nic_registry_claim_equal(&nic_registry_backends[index], backend)) {
            return false;
        }
    }
    nic_registry_backends[nic_registry_used] = *backend;
    nic_registry_used += 1U;
    return true;
}

const struct nic_backend *nic_registry_match(uint16_t vendor_id,
                                             uint16_t device_id)
{
    size_t index;

    for (index = 0U; index < nic_registry_used; ++index) {
        const struct nic_backend *backend = &nic_registry_backends[index];

        if (backend->vendor_id != vendor_id) {
            continue;
        }
        if (backend->match_any_device || backend->device_id == device_id) {
            return backend;
        }
    }
    return NULL;
}

size_t nic_registry_count(void)
{
    return nic_registry_used;
}

const struct nic_backend *nic_registry_at(size_t index)
{
    if (index >= nic_registry_used) {
        return NULL;
    }
    return &nic_registry_backends[index];
}

void nic_device_set_identity(struct nic_device *device,
                             const struct nic_identity *identity)
{
    if (device == NULL) {
        return;
    }
    memset(device, 0, sizeof(*device));
    if (identity != NULL) {
        device->identity = *identity;
    }
    /* Nothing is known about this device yet, and the description says so: a
     * device with no driver is UNSUPPORTED until one binds it. */
    device->state = NIC_STATE_UNSUPPORTED;
    device->reason = nic_state_reason(NIC_STATE_UNSUPPORTED);
}

void nic_device_set_capability(struct nic_device *device,
                               const struct nic_backend *backend)
{
    if (device == NULL) {
        return;
    }
    if (backend == NULL) {
        device->driver = NULL;
        device->state = NIC_STATE_UNSUPPORTED;
        device->reason = nic_state_reason(NIC_STATE_UNSUPPORTED);
        return;
    }
    device->driver = backend->name;
    device->state = NIC_STATE_SUPPORTED;
    device->reason = nic_state_reason(NIC_STATE_SUPPORTED);
}

void nic_device_set_state(struct nic_device *device,
                          const struct nic_backend *backend,
                          bool bound,
                          const char *reason)
{
    if (device == NULL) {
        return;
    }
    if (backend == NULL) {
        device->driver = NULL;
        device->state = NIC_STATE_UNSUPPORTED;
        device->reason = reason != NULL ? reason
                                        : nic_state_reason(NIC_STATE_UNSUPPORTED);
        return;
    }
    device->driver = backend->name;
    if (bound) {
        device->state = NIC_STATE_AVAILABLE;
        device->reason = nic_state_reason(NIC_STATE_AVAILABLE);
        return;
    }
    /* A driver that claimed the device and then refused it is an error the
     * record has to carry: it is not the same machine as one with no driver. */
    device->state = NIC_STATE_ERROR;
    device->reason = reason != NULL ? reason : nic_state_reason(NIC_STATE_ERROR);
}

void nic_device_set_mac(struct nic_device *device,
                        const uint8_t mac[NIC_MAC_BYTES])
{
    if (device == NULL) {
        return;
    }
    if (mac == NULL) {
        device->mac_present = false;
        memset(device->mac, 0, NIC_MAC_BYTES);
        return;
    }
    memcpy(device->mac, mac, NIC_MAC_BYTES);
    device->mac_present = true;
}

const char *nic_state_name(enum nic_state state)
{
    switch (state) {
    case NIC_STATE_SUPPORTED:
        return "SUPPORTED";
    case NIC_STATE_AVAILABLE:
        return "AVAILABLE";
    case NIC_STATE_UNSUPPORTED:
        return "UNSUPPORTED";
    case NIC_STATE_ERROR:
        return "ERROR";
    case NIC_STATE_ABSENT:
    default:
        return "ABSENT";
    }
}

const char *nic_state_reason(enum nic_state state)
{
    switch (state) {
    case NIC_STATE_SUPPORTED:
        return "DRIVER_PRESENT";
    case NIC_STATE_AVAILABLE:
        return "OK";
    case NIC_STATE_UNSUPPORTED:
        return "NO_DRIVER";
    case NIC_STATE_ERROR:
        return "DRIVER_REFUSED";
    case NIC_STATE_ABSENT:
    default:
        return "NO_DEVICE";
    }
}

bool nic_state_is_available(enum nic_state state)
{
    return state == NIC_STATE_AVAILABLE;
}

size_t nic_device_record(const struct nic_device *device,
                         char *buffer,
                         size_t size)
{
    struct nic_line line;
    char record[NIC_RECORD_BYTES + 1U];

    if (device == NULL || buffer == NULL || size == 0U) {
        return 0U;
    }
    nic_line_reset(&line, record, sizeof(record));
    nic_line_append(&line, "[NIC] bdf=");
    nic_line_hex8(&line, device->identity.bus);
    nic_line_append(&line, ":");
    nic_line_hex8(&line, device->identity.device);
    nic_line_append(&line, ".");
    nic_line_hex8(&line, device->identity.function);
    nic_line_append(&line, " vendor=");
    nic_line_hex16(&line, device->identity.vendor_id);
    nic_line_append(&line, " device=");
    nic_line_hex16(&line, device->identity.device_id);
    nic_line_append(&line, " driver=");
    nic_line_append(&line, device->driver != NULL ? device->driver : "NONE");
    nic_line_append(&line, " mac=");
    if (device->mac_present) {
        size_t index;

        for (index = 0U; index < NIC_MAC_BYTES; ++index) {
            if (index != 0U) {
                nic_line_append(&line, ":");
            }
            nic_line_hex8(&line, device->mac[index]);
        }
    } else {
        /* Six zero bytes are a valid address; "NONE" is the absence of one. */
        nic_line_append(&line, "NONE");
    }
    nic_line_append(&line, " state=");
    nic_line_append(&line, nic_state_name(device->state));
    nic_line_append(&line, " reason=");
    nic_line_append(&line, device->reason != NULL ? device->reason
                                                  : nic_state_reason(device->state));
    if (line.length == 0U || line.length > NIC_RECORD_BYTES ||
        line.length + 1U > size) {
        return 0U;
    }
    memcpy(buffer, record, line.length + 1U);
    return line.length;
}

size_t nic_summary_record(const struct nic_device *devices,
                          size_t count,
                          char *buffer,
                          size_t size)
{
    struct nic_line line;
    char record[NIC_RECORD_BYTES + 1U];
    size_t supported = 0U;
    size_t available = 0U;
    size_t unsupported = 0U;
    size_t errors = 0U;
    size_t absent = 0U;
    size_t index;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    for (index = 0U; index < count; ++index) {
        switch (devices[index].state) {
        case NIC_STATE_SUPPORTED:
            supported += 1U;
            break;
        case NIC_STATE_AVAILABLE:
            available += 1U;
            break;
        case NIC_STATE_UNSUPPORTED:
            unsupported += 1U;
            break;
        case NIC_STATE_ERROR:
            errors += 1U;
            break;
        case NIC_STATE_ABSENT:
        default:
            absent += 1U;
            break;
        }
    }
    if (count == 0U) {
        absent = 1U;
    }
    nic_line_reset(&line, record, sizeof(record));
    nic_line_append(&line, "[NIC] devices=");
    nic_line_u64(&line, (uint64_t)count);
    nic_line_append(&line, " supported=");
    nic_line_u64(&line, (uint64_t)supported);
    nic_line_append(&line, " available=");
    nic_line_u64(&line, (uint64_t)available);
    nic_line_append(&line, " unsupported=");
    nic_line_u64(&line, (uint64_t)unsupported);
    nic_line_append(&line, " error=");
    nic_line_u64(&line, (uint64_t)errors);
    nic_line_append(&line, " absent=");
    nic_line_u64(&line, (uint64_t)absent);
    if (line.length == 0U || line.length > NIC_RECORD_BYTES ||
        line.length + 1U > size) {
        return 0U;
    }
    memcpy(buffer, record, line.length + 1U);
    return line.length;
}
