/*
 * ITEM-504 increment 6 host proof for the durable route table.
 *
 * The store is the only path by which a table the operator configured reaches
 * the next boot.  This proof drives that path against a memory block device:
 * a saved table loads back whole, a blank record is an empty table, a corrupt
 * record of any kind is refused as the empty table and never partially, a
 * write that the device does not keep (a failed write or a read-back that
 * disagrees) fails the save, and the model's own rules are applied to whatever
 * comes off the disk.  It also pins the layout the store lives in and the
 * restore path into the route composition.
 */
#include "appliance_route_config.h"
#include "appliance_route_store.h"
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
#define TEST_ROUTE_OFFSET 448U
#define TEST_ROUTE_BLOCKS 64U

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

static struct appliance_route_table make_two_route_table(void)
{
    struct appliance_route_table table;

    appliance_route_table_init(&table);
    (void)appliance_route_table_add(&table, "B.EXAMPLE", 9U, "192.0.2.10",
                                    10U, "443", 3U, true,
                                    APPLIANCE_ROUTE_SOURCE_PANEL);
    (void)appliance_route_table_add(&table, "a.example", 9U, "198.51.100.7",
                                    12U, "8443", 4U, false,
                                    APPLIANCE_ROUTE_SOURCE_DECLARATION);
    return table;
}

static const struct appliance_route *route_named(
    const struct appliance_route_table *table,
    const char *name)
{
    uint32_t index;

    for (index = 0U; index < table->count; ++index) {
        if (strcmp(table->routes[index].name, name) == 0) {
            return &table->routes[index];
        }
    }
    return NULL;
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
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              APPLIANCE_STATE_STORE_ID_SYSTEM_ROUTES, &entry) ==
              APPLIANCE_STATE_LAYOUT_OK &&
              entry != NULL && entry->block_offset == TEST_ROUTE_OFFSET &&
              entry->block_count == TEST_ROUTE_BLOCKS,
          "the route store is appended after the seven existing stores");
}

/*
 * The boot record quotes the model's own rendering of the restored table, so
 * the separator the renderer writes between routes is part of what the record
 * says; a proof that greps the serial log has to be able to rely on it.
 */
static void test_rendered_table_separator(void)
{
    struct appliance_route_table table = make_two_route_table();
    char text[APPLIANCE_ROUTE_LIST_MAX];
    size_t length = 0U;

    CHECK(appliance_route_table_render(&table, text, sizeof(text), &length) ==
              APPLIANCE_ROUTE_OK,
          "the restored table renders");
    CHECK(strstr(text, "routes=2; name=b.example") != NULL,
          "the rendering separates routes with a semicolon and a space");
}

static void test_save_and_load_round_trip(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    struct appliance_route_store store;
    struct appliance_route_table table = make_two_route_table();
    struct appliance_route_table loaded;
    const struct appliance_route *route;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(make_loader(&loader, &device), "round trip: layout binds");
    appliance_route_store_init(&store);
    CHECK(appliance_route_store_load(&store, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_OK &&
              loaded.count == 0U &&
              appliance_route_store_source(&store) ==
                  APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_MISSING,
          "a blank store is an empty table, not a fault");
    CHECK(appliance_route_store_save(&store, &table) ==
              APPLIANCE_ROUTE_STORE_OK,
          "the table is written and read back");
    CHECK(appliance_route_store_available(&store),
          "a saved store is available");

    appliance_route_store_init(&store);
    CHECK(appliance_route_store_load(&store, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_OK &&
              loaded.count == 2U &&
              appliance_route_store_source(&store) ==
                  APPLIANCE_ROUTE_STORE_SOURCE_COMMITTED,
          "the saved table loads back whole");
    route = route_named(&loaded, "b.example");
    CHECK(route != NULL && route->backend_port == 443U &&
              route->tls_to_backend &&
              route->source == APPLIANCE_ROUTE_SOURCE_PANEL &&
              route->backend[0] == 192U && route->backend[3] == 10U,
          "the panel route keeps its name, backend, port, TLS and source");
    route = route_named(&loaded, "a.example");
    CHECK(route != NULL && route->backend_port == 8443U &&
              !route->tls_to_backend &&
              route->source == APPLIANCE_ROUTE_SOURCE_DECLARATION,
          "the declared route keeps its own values");
}

/*
 * A store the installer or the ITEM-502 erase rebuilt is an empty data region:
 * the route record is written over the region header only when a table is
 * committed, so a valid region header is "no routes yet", not corruption.
 */
static void test_rebuilt_region_is_an_empty_table(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    struct appliance_route_store store;
    struct appliance_route_table loaded;
    size_t base = TEST_ROUTE_OFFSET * TEST_BLOCK_SIZE;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(make_loader(&loader, &device), "rebuilt region: layout binds");
    memcpy(fixture.storage + base, "HFOCDATA", 8U);
    fixture.storage[base + 8U] = 1U;
    fixture.storage[base + 16U] = 1U;   /* block_offset */
    fixture.storage[base + 24U] = TEST_ROUTE_BLOCKS - 1U;
    appliance_route_store_init(&store);
    CHECK(appliance_route_store_load(&store, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_OK &&
              loaded.count == 0U &&
              appliance_route_store_source(&store) ==
                  APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_MISSING,
          "a rebuilt empty data region is an empty table, not corruption");
}

static void test_empty_table_is_a_valid_record(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    struct appliance_route_store store;
    struct appliance_route_table empty;
    struct appliance_route_table loaded;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(make_loader(&loader, &device), "empty table: layout binds");
    appliance_route_table_init(&empty);
    appliance_route_store_init(&store);
    CHECK(appliance_route_store_load(&store, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_OK,
          "empty table: first load succeeds");
    CHECK(appliance_route_store_save(&store, &empty) ==
              APPLIANCE_ROUTE_STORE_OK,
          "an empty table is a legal record");
    appliance_route_store_init(&store);
    CHECK(appliance_route_store_load(&store, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_OK &&
              loaded.count == 0U &&
              appliance_route_store_source(&store) ==
                  APPLIANCE_ROUTE_STORE_SOURCE_COMMITTED,
          "an explicitly empty table is committed, not missing");
}

static void test_corrupt_records_are_refused_whole(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    struct appliance_route_store writer;
    struct appliance_route_store reader;
    struct appliance_route_table table = make_two_route_table();
    struct appliance_route_table loaded;
    size_t offset = TEST_ROUTE_OFFSET * TEST_BLOCK_SIZE;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(make_loader(&loader, &device), "corrupt: layout binds");
    appliance_route_store_init(&writer);
    CHECK(appliance_route_store_load(&writer, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_OK,
          "corrupt: the writer binds the store");
    CHECK(appliance_route_store_save(&writer, &table) ==
              APPLIANCE_ROUTE_STORE_OK,
          "corrupt: a good record is stored first");

    /* A body byte changes: the checksum is what has to catch it. */
    fixture.storage[offset + 20U] ^= 0x01U;
    appliance_route_store_init(&reader);
    CHECK(appliance_route_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_RECORD_CHECKSUM &&
              loaded.count == 0U &&
              appliance_route_store_source(&reader) ==
                  APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_INVALID,
          "a body that disagrees with its checksum is refused whole");

    fixture.storage[offset + 20U] ^= 0x01U;
    fixture.storage[offset] ^= 0xFFU;
    appliance_route_store_init(&reader);
    CHECK(appliance_route_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_RECORD_CORRUPT &&
              loaded.count == 0U,
          "a record whose magic is gone is refused");

    memset(&fixture, 0, sizeof(fixture));
    (void)appliance_route_store_save(&writer, &table);
    fixture.storage[offset + 4U] = 9U;
    appliance_route_store_init(&reader);
    CHECK(appliance_route_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_RECORD_CORRUPT &&
              loaded.count == 0U,
          "a record of another version is refused");

    memset(&fixture, 0, sizeof(fixture));
    (void)appliance_route_store_save(&writer, &table);
    fixture.storage[offset + 8U] = 200U;
    appliance_route_store_init(&reader);
    CHECK(appliance_route_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_RECORD_CORRUPT &&
              loaded.count == 0U,
          "a count beyond the model's limit is refused");

    memset(&fixture, 0, sizeof(fixture));
    (void)appliance_route_store_save(&writer, &table);
    /* The first route's name length becomes longer than the record holds. */
    fixture.storage[offset + 16U] = 0xFFU;
    fixture.storage[offset + 17U] = 0x0FU;
    appliance_route_store_init(&reader);
    CHECK(appliance_route_store_load(&reader, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_RECORD_CORRUPT &&
              loaded.count == 0U,
          "a length that overruns the record is refused before it is read");
}

static void test_failed_writes_never_apply(void)
{
    struct appliance_state_loader loader;
    struct block_device device = make_device(&fixture);
    struct appliance_route_store store;
    struct appliance_route_table table = make_two_route_table();
    struct appliance_route_table loaded;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(make_loader(&loader, &device), "write failure: layout binds");
    appliance_route_store_init(&store);
    CHECK(appliance_route_store_load(&store, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_OK,
          "write failure: the store binds");
    fixture.fail_write = true;
    CHECK(appliance_route_store_save(&store, &table) ==
              APPLIANCE_ROUTE_STORE_WRITE_FAILED,
          "a write the device refuses fails the save");
    fixture.fail_write = false;
    appliance_route_store_init(&store);
    CHECK(appliance_route_store_load(&store, &loader, &loaded) ==
              APPLIANCE_ROUTE_STORE_OK &&
              loaded.count == 0U &&
              appliance_route_store_source(&store) ==
                  APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_MISSING,
          "the refused write left no record behind");

    CHECK(appliance_route_store_save(&store, &table) ==
              APPLIANCE_ROUTE_STORE_OK,
          "a later save takes the record");
    fixture.corrupt_after_write = true;
    CHECK(appliance_route_store_save(&store, &table) ==
              APPLIANCE_ROUTE_STORE_WRITE_FAILED,
          "a read-back that disagrees fails the save");
    fixture.corrupt_after_write = false;
}

static void test_unavailable_store(void)
{
    struct appliance_route_store store;
    struct appliance_route_table table = make_two_route_table();
    struct appliance_route_table loaded;

    appliance_route_store_init(&store);
    CHECK(appliance_route_store_load(&store, NULL, &loaded) ==
              APPLIANCE_ROUTE_STORE_STORE_UNAVAILABLE &&
              loaded.count == 0U &&
              appliance_route_store_source(&store) ==
                  APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_UNAVAILABLE,
          "no state volume means no stored routes, honestly reported");
    CHECK(appliance_route_store_save(&store, &table) ==
              APPLIANCE_ROUTE_STORE_STORE_UNAVAILABLE,
          "a commit cannot reach a store that was never bound");
}

static void test_restore_into_the_composition(void)
{
    struct appliance_route_config config;
    struct appliance_route_table table = make_two_route_table();
    struct appliance_route_table broken;

    appliance_route_config_init(&config);
    CHECK(appliance_route_config_restore(&config, &table) ==
              APPLIANCE_ROUTE_CONFIG_OK &&
              appliance_route_config_applied(&config)->count == 2U,
          "a stored table becomes the applied table");

    appliance_route_table_init(&broken);
    (void)appliance_route_table_add(&broken, "a.example", 9U, "192.0.2.10",
                                    10U, "443", 3U, false,
                                    APPLIANCE_ROUTE_SOURCE_PANEL);
    /* A table a store read could carry and the model must refuse: duplicate. */
    broken.routes[1] = broken.routes[0];
    broken.count = 2U;
    CHECK(appliance_route_config_restore(&config, &broken) ==
              APPLIANCE_ROUTE_CONFIG_REFUSED &&
              appliance_route_config_applied(&config)->count == 2U,
          "a table the model refuses leaves the applied table untouched");
    CHECK(appliance_route_config_candidate(&config) == NULL,
          "a restore stages no candidate a commit could apply");
}

int main(void)
{
    test_layout_placement();
    test_save_and_load_round_trip();
    test_rendered_table_separator();
    test_rebuilt_region_is_an_empty_table();
    test_empty_table_is_a_valid_record();
    test_corrupt_records_are_refused_whole();
    test_failed_writes_never_apply();
    test_unavailable_store();
    test_restore_into_the_composition();
    if (failures != 0) {
        fprintf(stderr, "appliance route store host tests: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance route store host tests: ALL OK\n");
    return 0;
}
