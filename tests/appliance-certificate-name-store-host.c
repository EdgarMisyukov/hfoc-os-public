/*
 * ITEM-507 increment 2 host proof for the durable name -> certificate/key
 * table.
 *
 * The store is the only path by which a certificate the operator declared for
 * a name reaches the next boot.  This proof drives that path against a memory
 * block device: a saved table loads back whole with the certificate and the
 * key bytes intact, a blank record is an empty table, an explicitly empty
 * table is committed rather than missing, a corrupt record of any kind is
 * refused as the empty table and never partially, a write the device does not
 * keep fails the save, and the model's own name rules are applied to whatever
 * comes off the disk.  It also pins the record's placement after the ITEM-503
 * certificate record and that no rendering ever exposes key material.
 */
#include "appliance_certificate_name_store.h"
#include "appliance_state_block_device.h"
#include "appliance_state_layout.h"
#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 512U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
/* The certificate store entry is the fourth default store: 3 * 64 blocks. */
#define TEST_CERTIFICATE_OFFSET 192U
#define TEST_CERTIFICATE_BLOCKS 64U
#define TEST_NAME_OFFSET \
    (TEST_CERTIFICATE_OFFSET + \
     APPLIANCE_CERTIFICATE_NAME_STORE_BLOCK_OFFSET)
#define TEST_NAME_BYTES \
    (APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS * TEST_BLOCK_SIZE)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct fixture_device {
    uint8_t storage[TEST_CAPACITY];
    bool fail_write;
    bool corrupt_after_write;
    size_t read_calls;
    size_t write_calls;
    size_t flush_calls;
};

static struct fixture_device fixture;

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *self = (struct fixture_device *)device->context;

    ++self->read_calls;
    if (offset > TEST_CAPACITY || length > TEST_CAPACITY - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, self->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *self = (struct fixture_device *)device->context;

    ++self->write_calls;
    if (offset > TEST_CAPACITY || length > TEST_CAPACITY - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    if (self->fail_write) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(self->storage + (size_t)offset, buffer, length);
    if (self->corrupt_after_write && length > 0U) {
        self->storage[(size_t)offset] ^= 0xFFU;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(struct block_device *device)
{
    struct fixture_device *self = (struct fixture_device *)device->context;

    ++self->flush_calls;
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(struct fixture_device *self)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = self;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static bool make_loader(
    struct appliance_state_loader *loader,
    struct block_device *device)
{
    memset(loader, 0, sizeof(*loader));
    if (appliance_state_layout_default(&loader->layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        return false;
    }
    if (appliance_state_block_device_bind_range(
            device, 0U, TEST_BLOCK_COUNT, &loader->partition) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    loader->ready = true;
    return true;
}

/* Deterministic certificate and key bytes: the test never needs real DER. */
static void fill_bytes(
    uint8_t *buffer,
    size_t length,
    uint8_t seed)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        buffer[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

/* The longest legal DNS name: 63 + 1 + 63 + 1 + 63 + 1 + 61 = 253 bytes. */
static size_t make_max_name(char *out)
{
    static const size_t labels[4] = {63U, 63U, 63U, 61U};
    size_t offset = 0U;
    size_t label;

    for (label = 0U; label < 4U; ++label) {
        if (label != 0U) {
            out[offset] = '.';
            ++offset;
        }
        memset(out + offset, (int)('a' + (int)label), labels[label]);
        offset += labels[label];
    }
    out[offset] = '\0';
    return offset;
}

static struct appliance_certificate_name_table make_two_name_table(void)
{
    struct appliance_certificate_name_table table;
    uint8_t certificate[423];
    uint8_t key[201];

    appliance_certificate_name_table_init(&table);
    fill_bytes(certificate, sizeof(certificate), 0x11U);
    fill_bytes(key, sizeof(key), 0x31U);
    (void)appliance_certificate_name_table_add(
        &table, "B.EXAMPLE", 9U, certificate, sizeof(certificate), key,
        sizeof(key));
    fill_bytes(certificate, sizeof(certificate), 0x51U);
    fill_bytes(key, sizeof(key), 0x71U);
    (void)appliance_certificate_name_table_add(
        &table, "a.example", 9U, certificate, sizeof(certificate), key,
        sizeof(key));
    return table;
}

static void test_layout_placement(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    const struct appliance_state_layout_entry *entry = NULL;

    CHECK(make_loader(&loader, &device), "default layout binds");
    CHECK(loader.layout.entry_count == 8U,
          "the default layout carries eight stores");
    CHECK(loader.layout.block_capacity == 512U,
          "the default layout reserves 512 blocks");
    CHECK(appliance_state_layout_find(
              &loader.layout,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
              APPLIANCE_STATE_STORE_ID_CERTIFICATE, &entry) ==
              APPLIANCE_STATE_LAYOUT_OK &&
              entry != NULL &&
              entry->block_offset == TEST_CERTIFICATE_OFFSET &&
              entry->block_count == TEST_CERTIFICATE_BLOCKS,
          "the certificate store keeps its own offset and size");
    CHECK(APPLIANCE_CERTIFICATE_NAME_STORE_BLOCK_OFFSET == 12U &&
              APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS == 32U &&
              TEST_NAME_OFFSET == 204U,
          "the name record starts after the ITEM-503 record, in the same store");
}

static void test_model_rules(void)
{
    struct appliance_certificate_name_table table;
    struct appliance_certificate_name_table full;
    const struct appliance_certificate_name *entry = NULL;
    uint8_t certificate[APPLIANCE_CERTIFICATE_NAME_DER_MAX];
    uint8_t key[APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX];
    char long_name[APPLIANCE_CERTIFICATE_NAME_MAX + 1U];
    char text[APPLIANCE_CERTIFICATE_NAME_RECORD_MAX];
    size_t length = 0U;
    size_t max_length = 0U;
    uint32_t index;

    fill_bytes(certificate, sizeof(certificate), 0x01U);
    fill_bytes(key, sizeof(key), 0x02U);
    appliance_certificate_name_table_init(&table);
    CHECK(table.count == 0U &&
              table.default_mode ==
                  APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE,
          "an empty table declares nothing and refuses unknown names");

    CHECK(appliance_certificate_name_table_add(
              &table, "B.EXAMPLE.", 10U, certificate, sizeof(certificate),
              key, sizeof(key)) == APPLIANCE_CERTIFICATE_NAME_OK,
          "a name is accepted, lowercased and stripped of the root dot");
    entry = NULL;
    CHECK(appliance_certificate_name_table_lookup(
              &table, "b.example", 9U, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              entry != NULL && strcmp(entry->name, "b.example") == 0 &&
              entry->certificate_length == sizeof(certificate) &&
              entry->key_length == sizeof(key),
          "the stored entry keeps the normalized name and both payloads");

    CHECK(appliance_certificate_name_table_add(
              &table, "b.example", 9U, certificate, sizeof(certificate),
              key, sizeof(key)) ==
              APPLIANCE_CERTIFICATE_NAME_DUPLICATE_NAME,
          "the same name in any case is a duplicate");
    CHECK(table.count == 1U, "a refused duplicate stores nothing");

    CHECK(appliance_certificate_name_table_lookup(
              &table, "*.example", 9U, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_BAD_NAME,
          "a wildcard is not a name this table can mean");
    CHECK(appliance_certificate_name_table_lookup(
              &table, "absent.example", 14U, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_NOT_FOUND &&
              entry == NULL,
          "an unknown name is NOT_FOUND, never the first entry");
    CHECK(appliance_certificate_name_table_lookup(
              &table, "", 0U, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_BAD_NAME,
          "an empty name is malformed");

    memset(long_name, 0, sizeof(long_name));
    max_length = make_max_name(long_name);
    CHECK(max_length == APPLIANCE_CERTIFICATE_NAME_MAX &&
              appliance_certificate_name_table_add(
                  &table, long_name, max_length, certificate,
                  sizeof(certificate), key, sizeof(key)) ==
                  APPLIANCE_CERTIFICATE_NAME_OK,
          "a 253-byte name is accepted");
    long_name[max_length] = 'a';
    CHECK(appliance_certificate_name_table_add(
              &table, long_name, max_length + 1U, certificate,
              sizeof(certificate), key, sizeof(key)) ==
              APPLIANCE_CERTIFICATE_NAME_NAME_TOO_LONG,
          "a 254-byte name is refused");
    long_name[max_length] = '\0';
    CHECK(appliance_certificate_name_table_remove(
              &table, long_name, max_length) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              table.count == 1U,
          "a name can be removed again");

    CHECK(appliance_certificate_name_table_remove(
              &table, "b.example", 9U) == APPLIANCE_CERTIFICATE_NAME_OK &&
              table.count == 0U,
          "the last name is removed");
    CHECK(appliance_certificate_name_table_lookup(
              &table, "b.example", 9U, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_NOT_FOUND,
          "a removed name is gone");
    /* The removed slot's key bytes are zeroed, not left under a dead name. */
    {
        bool zeroed = true;

        for (index = 0U; index < sizeof(table.entries[0].key_der); ++index) {
            if (table.entries[0].key_der[index] != 0U) {
                zeroed = false;
                break;
            }
        }
        CHECK(zeroed, "a deleted entry leaves no key material behind");
    }

    CHECK(appliance_certificate_name_table_add(
              &table, "keyless.example", 15U, certificate,
              sizeof(certificate), NULL, 0U) ==
              APPLIANCE_CERTIFICATE_NAME_BAD_KEY,
          "a name without a key is refused");
    CHECK(appliance_certificate_name_table_add(
              &table, "certless.example", 16U, NULL, 0U, key,
              sizeof(key)) == APPLIANCE_CERTIFICATE_NAME_BAD_CERTIFICATE,
          "a name without a certificate is refused");
    CHECK(appliance_certificate_name_table_add(
              &table, "bigcert.example", 15U, certificate,
              sizeof(certificate) + 1U, key, sizeof(key)) ==
              APPLIANCE_CERTIFICATE_NAME_BAD_CERTIFICATE,
          "a certificate over the DER bound is refused");
    CHECK(appliance_certificate_name_table_add(
              &table, "bigkey.example", 14U, certificate,
              sizeof(certificate), key, sizeof(key) + 1U) ==
              APPLIANCE_CERTIFICATE_NAME_BAD_KEY,
          "a key over the DER bound is refused");

    CHECK(appliance_certificate_name_table_add(
              &table, "b.example", 9U, certificate, sizeof(certificate), key,
              sizeof(key)) == APPLIANCE_CERTIFICATE_NAME_OK,
          "the table accepts a name again");
    CHECK(appliance_certificate_name_render(
              &table.entries[0], text, sizeof(text), &length) ==
              APPLIANCE_CERTIFICATE_NAME_OK,
          "an entry renders");
    CHECK(strstr(text, "name=b.example") != NULL &&
              strstr(text, "certificate_bytes=") != NULL &&
              strstr(text, "key=present") != NULL &&
              length < sizeof(text),
          "the rendering names the entry and the payload sizes");
    CHECK(strstr(text, "0x") == NULL && strlen(text) < 64U,
          "the rendering carries no payload bytes");
    CHECK(appliance_certificate_name_render(
              &table.entries[0], text, 8U, &length) ==
              APPLIANCE_CERTIFICATE_NAME_BUFFER_TOO_SMALL,
          "a rendering that does not fit is refused, never truncated");

    appliance_certificate_name_table_init(&full);
    for (index = 0U; index < APPLIANCE_CERTIFICATE_NAME_MAX_COUNT;
         ++index) {
        char name[16];

        memset(name, 0, sizeof(name));
        name[0] = (char)('a' + (char)index);
        memcpy(name + 1, ".example", 8U);
        CHECK(appliance_certificate_name_table_add(
                  &full, name, 9U, certificate,
                  APPLIANCE_CERTIFICATE_NAME_DER_MAX, key,
                  APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX) ==
                  APPLIANCE_CERTIFICATE_NAME_OK,
              "the table fills to its declared maximum");
    }
    CHECK(appliance_certificate_name_table_add(
              &full, "extra.example", 13U, certificate,
              sizeof(certificate), key, sizeof(key)) ==
              APPLIANCE_CERTIFICATE_NAME_TABLE_FULL,
          "a ninth name is refused");

    CHECK(appliance_certificate_name_table_set_default(
              &table, APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              table.default_mode ==
                  APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE,
          "the default-certificate declaration is configuration");
    CHECK(appliance_certificate_name_table_set_default(&table, 7U) ==
              APPLIANCE_CERTIFICATE_NAME_BAD_NAME &&
              table.default_mode ==
                  APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE,
          "an unknown default mode is refused and changes nothing");
    {
        struct appliance_certificate_name_table broken =
            make_two_name_table();

        broken.default_mode = 9U;
        CHECK(appliance_certificate_name_table_validate(&broken) ==
                  APPLIANCE_CERTIFICATE_NAME_BAD_NAME,
              "the model refuses a table with an unknown default mode");
    }
}

/*
 * ITEM-507 increment 3: the selection rule the handshake applies.  An exact
 * name (in any case) is EXACT, no name at all is the declared identity, and an
 * unknown name is DEFAULT only when the record says so and REFUSED otherwise.
 */
static void test_selection(void)
{
    struct appliance_certificate_name_table table = make_two_name_table();
    enum appliance_certificate_name_selection selection =
        APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED;
    const struct appliance_certificate_name *entry = NULL;

    CHECK(appliance_certificate_name_select(
              &table, "b.example", 9U, false, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT &&
              entry != NULL && strcmp(entry->name, "b.example") == 0,
          "an exact name selects its own certificate");
    CHECK(appliance_certificate_name_select(
              &table, "B.EXAMPLE", 9U, false, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT &&
              entry != NULL && strcmp(entry->name, "b.example") == 0,
          "an exact name selects in any case");

    /* REFUSE is the default: an undeclared name never borrows an identity. */
    selection = APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT;
    entry = table.entries;
    CHECK(appliance_certificate_name_select(
              &table, "absent.example", 14U, false, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_NOT_FOUND &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED &&
              entry == NULL,
          "an unknown name is refused, not given another identity");
    CHECK(appliance_certificate_name_select(
              &table, "*.example", 9U, false, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_BAD_NAME &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED,
          "a malformed name is refused");
    CHECK(appliance_certificate_name_select(
              &table, NULL, 0U, false, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_DEFAULT &&
              entry == NULL,
          "a client without SNI gets the declared identity");

    CHECK(appliance_certificate_name_table_set_default(
              &table, APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE) ==
              APPLIANCE_CERTIFICATE_NAME_OK,
          "the operator declares the default certificate");
    CHECK(appliance_certificate_name_select(
              &table, "absent.example", 14U, false, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_NOT_FOUND &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_DEFAULT &&
              entry == NULL,
          "a declared default is what an unknown name gets");
    CHECK(appliance_certificate_name_select(
              &table, "*.example", 9U, false, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_BAD_NAME &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_DEFAULT,
          "a malformed name follows the same declaration");
    CHECK(appliance_certificate_name_select(
              &table, "b.example", 9U, false, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT,
          "a declared default never shadows an exact name");
    CHECK(appliance_certificate_name_select(
              NULL, "b.example", 9U, false, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_NULL &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED,
          "a missing table refuses");

    /*
     * ITEM-507 increment 4: a name the route table carries must have its own
     * certificate.  The default certificate is not a substitute for a routed
     * name, so the refusal outranks the declaration.
     */
    CHECK(appliance_certificate_name_select(
              &table, "b.example", 9U, true, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT &&
              entry != NULL,
          "a routed name with its own certificate selects it");
    CHECK(appliance_certificate_name_select(
              &table, "absent.example", 14U, true, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_ROUTE_WITHOUT_CERTIFICATE &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED &&
              entry == NULL,
          "a routed name without a certificate is refused, default or not");
    CHECK(appliance_certificate_name_select(
              &table, "*.example", 9U, true, &selection, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_ROUTE_WITHOUT_CERTIFICATE &&
              selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED,
          "a malformed routed name is refused for the same reason");
    /*
     * And the same refusal holds when no default is declared at all.
     */
    {
        struct appliance_certificate_name_table strict =
            make_two_name_table();

        CHECK(appliance_certificate_name_select(
                  &strict, "absent.example", 14U, true, &selection, &entry) ==
                  APPLIANCE_CERTIFICATE_NAME_ROUTE_WITHOUT_CERTIFICATE &&
                  selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED,
              "the refusal does not depend on the default declaration");
    }
}

static void test_save_and_load_round_trip(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    struct appliance_certificate_name_store store;
    struct appliance_certificate_name_table table = make_two_name_table();
    struct appliance_certificate_name_table loaded;
    const struct appliance_certificate_name *entry = NULL;
    uint8_t expected_certificate[423];
    uint8_t expected_key[201];

    memset(&fixture, 0, sizeof(fixture));
    CHECK(make_loader(&loader, &device), "round trip: layout binds");
    appliance_certificate_name_store_init(&store);
    CHECK(appliance_certificate_name_store_load(&store, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK &&
              loaded.count == 0U &&
              appliance_certificate_name_store_source(&store) ==
                  APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_MISSING,
          "a blank store is an empty table, not a fault");
    CHECK(appliance_certificate_name_table_set_default(
              &table, APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE) ==
              APPLIANCE_CERTIFICATE_NAME_OK,
          "the operator declares the default certificate");
    CHECK(appliance_certificate_name_store_save(&store, &table) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK,
          "the table is written and read back");
    CHECK(appliance_certificate_name_store_available(&store),
          "a saved store is available");

    appliance_certificate_name_store_init(&store);
    CHECK(appliance_certificate_name_store_load(&store, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK &&
              loaded.count == 2U &&
              loaded.default_mode ==
                  APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE &&
              appliance_certificate_name_store_source(&store) ==
                  APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_COMMITTED,
          "the saved table loads back whole with its declaration");

    fill_bytes(expected_certificate, sizeof(expected_certificate), 0x11U);
    fill_bytes(expected_key, sizeof(expected_key), 0x31U);
    CHECK(appliance_certificate_name_table_lookup(
              &loaded, "b.example", 9U, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              entry != NULL &&
              entry->certificate_length == sizeof(expected_certificate) &&
              memcmp(entry->certificate_der, expected_certificate,
                     sizeof(expected_certificate)) == 0 &&
              entry->key_length == sizeof(expected_key) &&
              memcmp(entry->key_der, expected_key,
                     sizeof(expected_key)) == 0,
          "the certificate and the key survive the round trip byte for byte");
    CHECK(appliance_certificate_name_table_lookup(
              &loaded, "A.EXAMPLE", 9U, &entry) ==
              APPLIANCE_CERTIFICATE_NAME_OK &&
              entry != NULL && strcmp(entry->name, "a.example") == 0,
          "the lookup stays case-insensitive after the round trip");
}

static void test_empty_table_is_a_valid_record(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    struct appliance_certificate_name_store store;
    struct appliance_certificate_name_table empty;
    struct appliance_certificate_name_table loaded;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(make_loader(&loader, &device), "empty table: layout binds");
    appliance_certificate_name_table_init(&empty);
    appliance_certificate_name_store_init(&store);
    CHECK(appliance_certificate_name_store_load(&store, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK,
          "empty table: first load succeeds");
    CHECK(appliance_certificate_name_store_save(&store, &empty) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK,
          "an empty table is a legal record");
    appliance_certificate_name_store_init(&store);
    CHECK(appliance_certificate_name_store_load(&store, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK &&
              loaded.count == 0U &&
              loaded.default_mode ==
                  APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE &&
              appliance_certificate_name_store_source(&store) ==
                  APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_COMMITTED,
          "an explicitly empty table is committed, not missing");
}

static void test_corrupt_records_are_refused_whole(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    struct appliance_certificate_name_store writer;
    struct appliance_certificate_name_store reader;
    struct appliance_certificate_name_table table = make_two_name_table();
    struct appliance_certificate_name_table loaded;
    size_t offset = TEST_NAME_OFFSET * TEST_BLOCK_SIZE;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(make_loader(&loader, &device), "corrupt: layout binds");
    appliance_certificate_name_store_init(&writer);
    CHECK(appliance_certificate_name_store_load(&writer, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK,
          "corrupt: the writer binds the store");
    CHECK(appliance_certificate_name_store_save(&writer, &table) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK,
          "corrupt: a good record is stored first");

    /*
     * A byte of the first entry's name changes: the length structure still
     * walks, so the checksum is what has to catch it.
     */
    fixture.storage[offset + 28U] ^= 0x01U;
    appliance_certificate_name_store_init(&reader);
    CHECK(appliance_certificate_name_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CHECKSUM &&
              loaded.count == 0U &&
              appliance_certificate_name_store_source(&reader) ==
                  APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_INVALID,
          "a body that disagrees with its checksum is refused whole");

    fixture.storage[offset + 28U] ^= 0x01U;
    fixture.storage[offset] ^= 0xFFU;
    appliance_certificate_name_store_init(&reader);
    CHECK(appliance_certificate_name_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT &&
              loaded.count == 0U,
          "a record whose magic is gone is refused");

    memset(&fixture, 0, sizeof(fixture));
    (void)appliance_certificate_name_store_save(&writer, &table);
    fixture.storage[offset + 4U] = 9U;
    appliance_certificate_name_store_init(&reader);
    CHECK(appliance_certificate_name_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT &&
              loaded.count == 0U,
          "a record of another version is refused");

    memset(&fixture, 0, sizeof(fixture));
    (void)appliance_certificate_name_store_save(&writer, &table);
    fixture.storage[offset + 8U] = 200U;
    appliance_certificate_name_store_init(&reader);
    CHECK(appliance_certificate_name_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT &&
              loaded.count == 0U,
          "a count beyond the model's limit is refused");

    memset(&fixture, 0, sizeof(fixture));
    (void)appliance_certificate_name_store_save(&writer, &table);
    /* The first entry's name length becomes longer than the record holds. */
    fixture.storage[offset + 20U] = 0xFFU;
    fixture.storage[offset + 21U] = 0x0FU;
    appliance_certificate_name_store_init(&reader);
    CHECK(appliance_certificate_name_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT &&
              loaded.count == 0U,
          "a length that overruns the record is refused before it is read");

    memset(&fixture, 0, sizeof(fixture));
    (void)appliance_certificate_name_store_save(&writer, &table);
    /* The first entry's certificate length goes to zero. */
    fixture.storage[offset + 22U] = 0U;
    fixture.storage[offset + 23U] = 0U;
    appliance_certificate_name_store_init(&reader);
    CHECK(appliance_certificate_name_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT &&
              loaded.count == 0U,
          "a zero-length payload is refused");
}

static void test_failed_writes_never_apply(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    struct appliance_certificate_name_store store;
    struct appliance_certificate_name_table table = make_two_name_table();
    struct appliance_certificate_name_table loaded;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(make_loader(&loader, &device), "write failure: layout binds");
    appliance_certificate_name_store_init(&store);
    CHECK(appliance_certificate_name_store_load(&store, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK,
          "write failure: the store binds");
    fixture.fail_write = true;
    CHECK(appliance_certificate_name_store_save(&store, &table) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_WRITE_FAILED,
          "a write the device refuses fails the save");
    fixture.fail_write = false;
    appliance_certificate_name_store_init(&store);
    CHECK(appliance_certificate_name_store_load(&store, &loader, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK &&
              loaded.count == 0U &&
              appliance_certificate_name_store_source(&store) ==
                  APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_MISSING,
          "the refused write left no record behind");

    CHECK(appliance_certificate_name_store_save(&store, &table) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_OK,
          "a later save takes the record");
    fixture.corrupt_after_write = true;
    CHECK(appliance_certificate_name_store_save(&store, &table) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_WRITE_FAILED,
          "a read-back that disagrees fails the save");
    fixture.corrupt_after_write = false;
}

static void test_unavailable_store(void)
{
    struct appliance_certificate_name_store store;
    struct appliance_certificate_name_table table = make_two_name_table();
    struct appliance_certificate_name_table loaded;

    appliance_certificate_name_store_init(&store);
    CHECK(appliance_certificate_name_store_load(&store, NULL, &loaded) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_STORE_UNAVAILABLE &&
              loaded.count == 0U &&
              appliance_certificate_name_store_source(&store) ==
                  APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_UNAVAILABLE,
          "no state volume means no stored names, honestly reported");
    CHECK(appliance_certificate_name_store_save(&store, &table) ==
              APPLIANCE_CERTIFICATE_NAME_STORE_STORE_UNAVAILABLE,
          "a commit cannot reach a store that was never bound");
}

int main(void)
{
    test_layout_placement();
    test_model_rules();
    test_save_and_load_round_trip();
    test_empty_table_is_a_valid_record();
    test_corrupt_records_are_refused_whole();
    test_failed_writes_never_apply();
    test_unavailable_store();
    test_selection();
    if (failures != 0) {
        fprintf(stderr,
                "appliance certificate name store host tests: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance certificate name store host tests: ALL OK\n");
    return 0;
}
