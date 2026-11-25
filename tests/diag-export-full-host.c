/*
 * ITEM-590 host proof: the operator's `diag save` on a journal that is FULL.
 *
 * The owner's Toshiba L875 booted the installed system with the diagnostic
 * stick attached, the journal bound its reserved FAT12 file, and the machine's
 * own telemetry filled it to 512/512 slots in about twenty-three seconds.  The
 * console then refused the operator's one command with `DIAG FAILED
 * reason=NO_MEDIUM step=MEDIUM_PROBE`, and the stick carried no `[DIAG] BEGIN`
 * or `[DIAG] END` at all - the export never reached the rewind that exists
 * exactly for this case.
 *
 * The cause is one predicate: `install_journal_platform_ready()` answers
 * READY only, so a journal that bound its placement and used every slot it
 * declares - a healthy FULL journal, whose stream the export can restart - was
 * indistinguishable from a medium that failed.  This witness drives the real
 * production chain (`install_journal_platform`, the journal core, the
 * preallocated FAT12 writer, `diag_export_save()`) on a RAM disk carrying the
 * same reservation the medium's build writes:
 *
 *   * background telemetry fills the journal to FULL, exactly as the loop's
 *     per-second records did on the laptop;
 *   * `diag save` must recover the room through the journal's own rewind and
 *     write a fresh session, not refuse the medium;
 *   * two saves in a row must produce two different, complete sessions;
 *   * a write that fails and a flush that fails must be refused by name, with
 *     the step the operator reads on the console;
 *   * a medium that has actually failed is still refused at MEDIUM_PROBE -
 *     FULL is not a failure, and a failure is not FULL.
 *
 * The finished image is handed to the operator's own Debian extractor
 * (`scripts/hfoc-diag-read.py`), so the proof ends where the operator's does:
 * a `HFOC-DIAG-<session>.txt` whose BEGIN/END pair, build identity, record
 * count and CRC the reader verified itself.
 */
#include "diag_export.h"
#include "appliance_panel_auth_trace.h"
#include "kernel_login_trace.h"

#include "block_device.h"
#include "install_journal_platform.h"
#include "install_medium.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BPS 512U
#define SPF 9U
#define RESERVED 1U
#define NFAT 2U
#define ROOT_ENTRIES 240U
#define SPC 2U
#define TEST_SECTORS 6144U
#define PARTITION_LBA 2048U
#define ROOT_SECTORS ((ROOT_ENTRIES * 32U + BPS - 1U) / BPS)
#define ROOT_LBA (RESERVED + NFAT * SPF)
#define DATA_LBA (ROOT_LBA + ROOT_SECTORS)
#define HFOC_CLUSTER 2U
#define FILE_FIRST 3U
/* 16 clusters of two slots each: a journal the telemetry fills quickly and
   whose capacity is still large enough to hold the report after the rewind. */
#define FILE_CLUSTERS 16U
#define JOURNAL_SLOTS (FILE_CLUSTERS * SPC)
#define EOC 0x0FFFU
#define REPORT_RECORDS 10U
#define IMAGE_DIR "build/test-artifacts/diag-export-full"
#define IMAGE_PATH IMAGE_DIR "/stick.img"
#define REPORT_DIR IMAGE_DIR "/report"
#define EXTRACT_LOG IMAGE_DIR "/extract.log"
/* The build identity the proof compiles in: a BEGIN record that names it is a
   report the extractor can attribute to this proof, not to a stale tree. */
#ifndef HFOC_BUILD_ID
#define HFOC_BUILD_ID "host-590"
#endif

static unsigned checks;
static unsigned failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));          \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------ the stubs --- */

static char serial_text[4096];
static size_t serial_length;

void serial_write(const char *text) {
    const size_t length = text != NULL ? strlen(text) : 0U;

    if (length == 0U || serial_length + length >= sizeof(serial_text)) {
        return;
    }
    memcpy(&serial_text[serial_length], text, length);
    serial_length += length;
    serial_text[serial_length] = '\0';
}

void serial_write_u64(uint64_t value) {
    char text[24];

    snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    serial_write(text);
}

void serial_write_hex(uint64_t value) {
    char text[24];

    snprintf(text, sizeof(text), "0x%llX", (unsigned long long)value);
    serial_write(text);
}

uint64_t serial_sequence_begin(void) {
    return 0U;
}

void serial_sequence_end(uint64_t flags) {
    (void)flags;
}

/* The TSC-to-microsecond divider the trace uses; the proof's values are chosen
   so every rendered field stays visible and the record still fits one slot. */
uint64_t timer_cycles_to_us(uint64_t cycles) {
    return cycles / 1000U;
}

/* The journal's own status lines reach the durable store on the machine; here
   they are kept so a refusal that is named can be told from a silence. */
static char store_text[8192];
static size_t store_length;

void appliance_boot_log_store_observe(const char *record, size_t length) {
    if (record == NULL || length == 0U ||
        store_length + length + 1U >= sizeof(store_text)) {
        return;
    }
    if (store_length != 0U) {
        store_text[store_length++] = '\n';
    }
    memcpy(&store_text[store_length], record, length);
    store_length += length;
    store_text[store_length] = '\0';
}

/* Both controllers answer as a machine with no USB medium: the export's
   evidence is then the network/panel composition below, which is exactly the
   evidence the owner's report had to carry. */
bool ehci_storage_medium_status(const char **state, const char **reason) {
    if (state != NULL) {
        *state = "UNCHECKED";
    }
    if (reason != NULL) {
        *reason = "NOT_PROBED";
    }
    return false;
}

bool xhci_storage_medium_status(const char **state, const char **reason) {
    if (state != NULL) {
        *state = "UNCHECKED";
    }
    if (reason != NULL) {
        *reason = "NOT_PROBED";
    }
    return false;
}

bool ehci_storage_medium_publish(void) {
    return false;
}

bool xhci_storage_medium_publish(void) {
    return false;
}

const char *ehci_failure_primary(void) {
    return "NONE";
}

const char *ehci_status_string(enum ehci_status status) {
    (void)status;
    return "NONE";
}

enum ehci_status ehci_status(void) {
    return 0;
}

const char *ehci_detail(void) {
    return "NONE";
}

const char *ehci_failure_text(void) {
    return "";
}

const char *ehci_start_evidence(void) {
    return "pci=00:1D.0 start=0x0/0x0";
}

const char *ehci_controllers_text(void) {
    return "00:1D.0=NO_DEVICE/NO_CONNECTED_PORT";
}

bool ehci_storage_fault_text(char *buffer, size_t size) {
    (void)buffer;
    (void)size;
    return false;
}

bool xhci_storage_fault_text(char *buffer, size_t size) {
    (void)buffer;
    (void)size;
    return false;
}

void ehci_diagnostics_report(void (*sink)(void *context, const char *text),
                             void *context) {
    if (sink != NULL) {
        sink(context, "[EHCI] pci=00:1D.0 state=OK ports=2 devices=0");
    }
}

void xhci_diagnostics_report(void (*sink)(void *context, const char *text),
                             void *context) {
    if (sink != NULL) {
        sink(context, "[XHCI] state=UNAVAILABLE reason=NO_DEVICE pci=00:14.0");
    }
}

/* The production status report the export composes: the same three records
   name the installed machine's network, address and panel, so the extracted
   session is recognisable as this machine's own evidence. */
void kernel_production_status_report(void (*sink)(void *context,
                                                  const char *line),
                                     void *context) {
    if (sink == NULL) {
        return;
    }
    sink(context,
         "[NET-STATUS] device=PRESENT readiness=DEGRADED backend=RTL8168 "
         "link=DOWN mode=DHCP stage=40 reason=LINK_DOWN");
    sink(context,
         "[NET-ADDR] state=WAITING_FOR_LINK lease=NONE attempt=0/0 "
         "reason=NONE");
    sink(context, "[PANEL] v=1 state=AWAITING_NETWORK listener=down tls=OFF");
    /* ITEM-613: the same public host-key record the production status report
       composes.  The fixture is a real P-256 OpenSSH blob and its independently
       computed SHA256 fingerprint; the extractor must accept it and the
       known_hosts candidate must name the record's own address/port. */
    sink(context,
         "[PRODUCTION-SSH-HOSTKEY] v=1 state=READY "
         "algorithm=ecdsa-sha2-nistp256 "
         "blob=AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBA4XbH6KtmYa4QZo2V2VL+5d+OtVsDy2tSV/yba15z2yoG2ol67GhN2TR6Q6Ylj0kIyo1Dn/17tXULjsxdWTyYY= "
         "fingerprint=SHA256:Lc8UsC2OrEcPGz9MqeDS2DKOa4gzVdhYsQpSuWz+0QE "
         "address=192.0.2.13 port=22 build=host-590");
    /* ITEM-614: the shared report carries the stored production login record
       through the same module the kernel calls, so a real snapshot/save and
       the operator's extractor prove the export, not a serial marker. */
    kernel_login_trace_status_report(sink, context);
    /* ITEM-617: the browser sign-in record travels through the same shared
       composition; the fixture below finishes one attempt before the save. */
    appliance_panel_auth_trace_status_report(sink, context);
    /* ITEM-615: the retained `diag save` refusal travels through the same
       shared composition.  This is the real renderer and the real record the
       production status report publishes. */
    diag_failure_status_report(diag_export_last_failure(), sink, context);
}

/* ------------------------------------------------------------- the medium --- */

struct ram_medium {
    uint8_t *bytes;
    uint64_t size;
    unsigned writes;
    unsigned flushes;
    /* A record write is refused while the header's own commit point still
       reaches the medium: the write failure the operator must see named. */
    bool fail_data_writes;
    /* The commit point (naming the full capacity) and the rewind (naming slot
       1) flush outside the stream; the session's final flush names a record
       inside it, which is the flush this case refuses. */
    bool flush_fails_inside_stream;
    uint32_t header_offset;
    uint64_t first_write;
    uint64_t last_write;
};

static struct ram_medium medium;

static enum block_device_status ram_read(struct block_device *device,
                                         uint64_t offset, void *buffer,
                                         size_t length) {
    struct ram_medium *ram = device->context;

    if (offset + length > ram->size) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, &ram->bytes[offset], length);
    return BLOCK_DEVICE_OK;
}

static uint32_t ram_header_write_slot(const struct ram_medium *ram) {
    const uint8_t *slot = &ram->bytes[ram->header_offset];

    return (uint32_t)slot[21] | ((uint32_t)slot[22] << 8) |
           ((uint32_t)slot[23] << 16) | ((uint32_t)slot[24] << 24);
}

static enum block_device_status ram_write(struct block_device *device,
                                          uint64_t offset,
                                          const void *buffer, size_t length) {
    struct ram_medium *ram = device->context;

    if (offset + length > ram->size) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    if (ram->fail_data_writes && offset > ram->header_offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(&ram->bytes[offset], buffer, length);
    ram->writes += 1U;
    if (ram->first_write == 0U || offset < ram->first_write) {
        ram->first_write = offset;
    }
    if (offset + length > ram->last_write) {
        ram->last_write = offset + length;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status ram_flush(struct block_device *device) {
    struct ram_medium *ram = device->context;

    ram->flushes += 1U;
    if (ram->flush_fails_inside_stream) {
        const uint32_t write_slot = ram_header_write_slot(ram);

        if (write_slot > 1U && write_slot < JOURNAL_SLOTS) {
            return BLOCK_DEVICE_IO_ERROR;
        }
    }
    return BLOCK_DEVICE_OK;
}

static void put_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
    p[2] = (uint8_t)((value >> 16) & 0xFFU);
    p[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t crc32_bytes(const uint8_t *bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1) ^
                  (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static uint64_t cluster_offset(uint32_t cluster) {
    return (uint64_t)(DATA_LBA + (cluster - 2U) * SPC) * BPS;
}

static void fat_set(uint8_t *img, uint32_t cluster, uint16_t value) {
    for (uint32_t copy = 0U; copy < NFAT; ++copy) {
        uint64_t base = (uint64_t)(RESERVED + copy * SPF) * BPS;
        uint32_t off = cluster + cluster / 2U;
        uint16_t word = le16(&img[base + off]);

        if (cluster % 2U == 0U) {
            word = (uint16_t)((word & 0xF000U) | (value & 0x0FFFU));
        } else {
            word = (uint16_t)((word & 0x000FU) | ((value & 0x0FFFU) << 4));
        }
        put_le16(&img[base + off], word);
    }
}

static void make_entry(uint8_t *entry, const char *name8, const char *ext3,
                       uint8_t attr, uint32_t cluster, uint32_t size) {
    memset(entry, 0, 32U);
    memset(entry, ' ', 11U);
    memcpy(entry, name8, strlen(name8));
    memcpy(&entry[8], ext3, strlen(ext3));
    entry[11] = attr;
    put_le16(&entry[26], (uint16_t)cluster);
    put_le32(&entry[28], size);
}

/* The header the medium's build writes: the build reserves the file, the
   kernel only advances the commit point. */
static void header_put(uint8_t *slot, uint32_t capacity_slots,
                       uint32_t write_slot) {
    memset(slot, 0, BPS);
    memcpy(slot, INSTALL_JOURNAL_MAGIC, INSTALL_JOURNAL_MAGIC_BYTES);
    put_le32(&slot[9], INSTALL_JOURNAL_VERSION);
    put_le32(&slot[13], INSTALL_JOURNAL_SLOT_BYTES);
    put_le32(&slot[17], capacity_slots);
    put_le32(&slot[21], write_slot);
    put_le32(&slot[36], crc32_bytes(slot, 36U));
}

static void volume_build(uint8_t *volume) {
    volume[510] = 0x55U;
    volume[511] = 0xAAU;
    put_le16(&volume[11], BPS);
    volume[13] = SPC;
    put_le16(&volume[14], RESERVED);
    volume[16] = NFAT;
    put_le16(&volume[17], ROOT_ENTRIES);
    put_le16(&volume[19], (uint16_t)TEST_SECTORS);
    volume[21] = 0xF8U;
    put_le16(&volume[22], SPF);
    put_le16(&volume[24], 63U);
    put_le16(&volume[26], 255U);
    put_le32(&volume[28], 140U);
    volume[38] = 0x29U;
    memcpy(&volume[54], "FAT12   ", 8U);

    fat_set(volume, 0U, 0x0FF8U);
    fat_set(volume, 1U, EOC);
    fat_set(volume, HFOC_CLUSTER, EOC);
    for (uint32_t i = 0U; i < FILE_CLUSTERS; ++i) {
        uint16_t value = (i + 1U == FILE_CLUSTERS)
                             ? EOC
                             : (uint16_t)(FILE_FIRST + i + 1U);

        fat_set(volume, FILE_FIRST + i, value);
    }
    make_entry(&volume[ROOT_LBA * BPS], "HFOC", "", 0x10U, HFOC_CLUSTER, 0U);
    {
        uint8_t *dir = &volume[cluster_offset(HFOC_CLUSTER)];

        make_entry(&dir[0], ".", "", 0x10U, HFOC_CLUSTER, 0U);
        make_entry(&dir[32], "..", "", 0x10U, 0U, 0U);
        /* Preallocated: the directory size is the whole reservation, so the
           header's write_slot is the commit point. */
        make_entry(&dir[64], "INSTALL", "LOG", 0x20U, FILE_FIRST,
                   JOURNAL_SLOTS * BPS);
    }
    header_put(&volume[cluster_offset(FILE_FIRST)], JOURNAL_SLOTS, 1U);
}

static void image_refresh(void) {
    uint8_t *entry;

    memset(medium.bytes, 0, (size_t)medium.size);
    medium.bytes[510] = 0x55U;
    medium.bytes[511] = 0xAAU;
    entry = &medium.bytes[446];
    entry[4] = 0x01U; /* FAT12 */
    put_le32(&entry[8], PARTITION_LBA);
    put_le32(&entry[12], TEST_SECTORS);
    volume_build(&medium.bytes[(uint64_t)PARTITION_LBA * BPS]);
}

static void medium_reset(void);

/* A fresh machine for each case: the seam initialises once at boot and never
   rebinds a journal it already bound, so a case that reused the previous
   binding would be running against the previous medium. */
static void machine_reset(void) {
    install_journal_platform_init();
    medium_reset();
}

static void medium_reset(void) {
    free(medium.bytes);
    memset(&medium, 0, sizeof(medium));
    medium.size = (uint64_t)(PARTITION_LBA + TEST_SECTORS) * BPS;
    medium.bytes = calloc(1U, (size_t)medium.size);
    if (medium.bytes == NULL) {
        fprintf(stderr, "cannot allocate the RAM medium\n");
        exit(2);
    }
    medium.header_offset = (uint32_t)((uint64_t)PARTITION_LBA * BPS +
                                      cluster_offset(FILE_FIRST));
    image_refresh();
}

/* The placement keeps the device *pointer*, so the block view it was bound
   with must outlive the bind call: this is the machine's own block view, not a
   temporary. */
static struct block_device medium_block;

static struct block_device *medium_device(void) {
    memset(&medium_block, 0, sizeof(medium_block));
    medium_block.block_size = BPS;
    medium_block.block_count = medium.size / BPS;
    medium_block.context = &medium;
    medium_block.read_fn = ram_read;
    medium_block.write_fn = ram_write;
    medium_block.flush_fn = ram_flush;
    return &medium_block;
}

/* Bind the reserved file the way the installed machine does: the loader named
   the partition, so no table is scanned. */
static bool medium_bind(void) {
    struct install_medium_verdict verdict;
    struct block_device *device = medium_device();

    memset(&verdict, 0, sizeof(verdict));
    verdict.result = INSTALL_MEDIUM_MATCH;
    verdict.reason = "OK";
    verdict.partition_index = 1U;
    verdict.partition_type = 0x01U;
    verdict.first_lba = PARTITION_LBA;
    verdict.length_lba = TEST_SECTORS;
    return install_journal_platform_bind_medium(device, &verdict);
}

static bool image_write(const char *path) {
    FILE *handle = fopen(path, "wb");
    size_t written;

    if (handle == NULL) {
        return false;
    }
    written = fwrite(medium.bytes, 1U, (size_t)medium.size, handle);
    fclose(handle);
    return written == (size_t)medium.size;
}

/* ------------------------------------------------------------- the cases --- */

static enum install_journal_state journal_state(void) {
    struct install_journal_status status;

    install_journal_platform_status(&status);
    return status.state;
}

/* The loop's per-second telemetry, as the machine published it: distinct lines
   so the journal's slots carry the shape the laptop's did. */
static unsigned telemetry_fill_to_full(void) {
    unsigned published = 0U;

    while (journal_state() == INSTALL_JOURNAL_READY && published < 4096U) {
        char line[200];

        snprintf(line, sizeof(line),
                 "[LOOP-PHASES] turns=%u console_us=%u xhci_us=%u input_us=0 "
                 "net_us=%u svc_us=%u journal_us=%u store_us=%u other_us=0 "
                 "work_us=%u",
                 published + 1U, published, published * 2U, published * 3U,
                 published * 4U, published * 5U, published * 6U,
                 published * 7U);
        install_journal_platform_observe(line, strlen(line));
        ++published;
    }
    return published;
}

static bool extractor_run(void) {
    char command[512];
    int result;

    snprintf(command, sizeof(command),
             "python3 scripts/hfoc-diag-read.py --disk '%s' --output '%s' "
             ">'%s' 2>&1",
             IMAGE_PATH, REPORT_DIR, EXTRACT_LOG);
    result = system(command);
    if (result == -1) {
        return false;
    }
    return (result >> 8) == 0;
}

static bool extract_log_contains(const char *needle) {
    char line[512];
    FILE *handle = fopen(EXTRACT_LOG, "rb");

    if (handle == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), handle) != NULL) {
        if (strstr(line, needle) != NULL) {
            fclose(handle);
            return true;
        }
    }
    fclose(handle);
    return false;
}

static bool file_contains(const char *path, const char *needle) {
    char line[512];
    FILE *handle = fopen(path, "rb");

    if (handle == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), handle) != NULL) {
        if (strstr(line, needle) != NULL) {
            fclose(handle);
            return true;
        }
    }
    fclose(handle);
    return false;
}

static const char *snapshot_line_starting_with(const char *prefix) {
    const uint32_t records = diag_export_snapshot_records();

    for (uint32_t index = 0U; index < records; ++index) {
        const char *line = diag_export_snapshot_line(index);

        if (line != NULL && strncmp(line, prefix, strlen(prefix)) == 0) {
            return line;
        }
    }
    return NULL;
}

static void login_trace_commit(bool ok, uint32_t stage, uint32_t iterations) {
    struct kernel_login_trace trace;

    memset(&trace, 0, sizeof(trace));
    trace.ok = ok;
    trace.stage = stage;
    trace.account_iterations = iterations;
    trace.account_cycles = 123456000U;
    trace.seed_cycles = 654321000U;
    trace.publish_cycles = 1000U;
    trace.entropy_cycles = 2000U;
    trace.total_cycles = 777777000U;
    trace.seed.slot_reads = 2U;
    trace.seed.slot_writes = 1U;
    trace.seed.opens = 2U;
    trace.seed.seals = 1U;
    trace.seed.readbacks = 1U;
    trace.seed.kdf_calls = 3U;
    trace.seed.kdf_iterations = 60000U;
    trace.seed.read_cycles = 500000U;
    trace.seed.open_cycles = 1000000U;
    trace.seed.seal_cycles = 1000000U;
    trace.seed.write_cycles = 500000U;
    trace.seed.readback_cycles = 500000U;
    kernel_login_trace_begin();
    kernel_login_trace_commit(&trace);
}

static void test_login_trace_cold_is_honest(void) {
    char line[KERNEL_LOGIN_TRACE_LINE_MAX];
    const char *record;

    CHECK(kernel_login_trace_snapshot_render(line, sizeof(line)) != 0U,
          "a cold boot renders an explicit login-trace state");
    CHECK(strstr(line, "available=0") != NULL &&
              strstr(line, "state=NOT_ATTEMPTED") != NULL,
          "no attempt is NOT_ATTEMPTED, not fabricated numbers");
    CHECK(strstr(line, " ok=") == NULL && strstr(line, " total_us=") == NULL,
          "a cold-boot record claims no attempt result or timing");

    CHECK(diag_export_snapshot_compose() != 0U,
          "the real snapshot composer runs before any login");
    record = snapshot_line_starting_with("[LOGIN-TRACE]");
    CHECK(record != NULL,
          "the composed snapshot carries the honest login-trace record");
    CHECK(record != NULL && strstr(record, "state=NOT_ATTEMPTED") != NULL,
          "the snapshot says NOT_ATTEMPTED before any login");
    CHECK(record != NULL && strstr(record, " ok=") == NULL,
          "the cold snapshot invents no login result");
    /* ITEM-615: the retained refusal is a shared record from the first boot,
       and before any save it is honestly empty rather than a stale failure. */
    record = snapshot_line_starting_with("[DIAG-FAIL]");
    CHECK(record != NULL &&
              strstr(record, "state=NO_FAILURE") != NULL &&
              strstr(record, "reason=NOT_ATTEMPTED") != NULL,
          "the cold snapshot publishes NO_FAILURE/NOT_ATTEMPTED");
}

static bool collect_fragment(void *context, const char *fragment) {
    char *buffer = context;
    const size_t used = strlen(buffer);

    if (used + strlen(fragment) + 1U >= KERNEL_LOGIN_TRACE_LINE_MAX) {
        return false;
    }
    memcpy(buffer + used, fragment, strlen(fragment) + 1U);
    return true;
}

static void test_login_trace_shared_renderer_and_bounds(void) {
    struct kernel_login_trace trace;
    char line[KERNEL_LOGIN_TRACE_LINE_MAX];
    char fragments[KERNEL_LOGIN_TRACE_LINE_MAX];
    size_t length;

    memset(&trace, 0, sizeof(trace));
    trace.ok = false;
    trace.stage = 2U;
    trace.account_iterations = 200000U;
    trace.account_cycles = UINT64_MAX;
    trace.seed_cycles = UINT64_MAX;
    trace.publish_cycles = UINT64_MAX;
    trace.entropy_cycles = UINT64_MAX;
    trace.total_cycles = UINT64_MAX;
    trace.seed.slot_reads = UINT32_MAX;
    trace.seed.slot_writes = UINT32_MAX;
    trace.seed.opens = UINT32_MAX;
    trace.seed.seals = UINT32_MAX;
    trace.seed.readbacks = UINT32_MAX;
    trace.seed.kdf_calls = UINT32_MAX;
    trace.seed.kdf_iterations = UINT32_MAX;
    trace.seed.read_cycles = UINT64_MAX;
    trace.seed.open_cycles = UINT64_MAX;
    trace.seed.seal_cycles = UINT64_MAX;
    trace.seed.write_cycles = UINT64_MAX;
    trace.seed.readback_cycles = UINT64_MAX;

    login_trace_commit(true, 4U, 200000U);
    length = kernel_login_trace_snapshot_render(line, sizeof(line));
    CHECK(length > 0U && length < sizeof(line),
          "a completed trace fits the declared line bound");
    CHECK(length <= DIAG_RECORD_TEXT_MAX,
          "a normal completed trace fits one diagnostic record");
    CHECK(strstr(line, "available=1") != NULL &&
              strstr(line, " ok=1") != NULL &&
              strstr(line, " stage=4") != NULL,
          "the completed record names validity, result and stage");
    CHECK(strstr(line, " account_us=") != NULL &&
              strstr(line, " account_iterations=200000") != NULL &&
              strstr(line, " seed_kdf_iterations=60000") != NULL &&
              strstr(line, " seed_readback_us=") != NULL &&
              strstr(line, " total_us=") != NULL,
          "the completed record carries every measured field");

    memset(fragments, 0, sizeof(fragments));
    CHECK(kernel_login_trace_emit_state(collect_fragment, fragments),
          "the fragment renderer emits the same stored record");
    CHECK(strcmp(line, fragments) == 0,
          "serial/journal fragments and the snapshot line are identical");

    fragments[0] = '\0';
    CHECK(kernel_login_trace_emit(
              KERNEL_LOGIN_TRACE_COMPLETED, &trace, 7U, collect_fragment,
              fragments),
          "an explicit maximum-width record renders");
    CHECK(strstr(fragments, "stage=2") != NULL &&
              strstr(fragments, "seed_kdf_iterations=4294967295") != NULL,
          "maximum-width values are not truncated");
    CHECK(strlen(fragments) < KERNEL_LOGIN_TRACE_LINE_MAX,
          "even maximum-width values fit the declared bound");
    CHECK(kernel_login_trace_snapshot_render(line, 64U) == 0U,
          "a capacity that cannot hold every field is refused, not truncated");

    kernel_login_trace_begin();
    CHECK(kernel_login_trace_snapshot_render(line, sizeof(line)) != 0U &&
              strstr(line, "state=IN_PROGRESS") != NULL &&
              strstr(line, " ok=") == NULL &&
              strstr(line, " total_us=") == NULL,
          "an attempt in progress never presents the previous success");
    login_trace_commit(false, 1U, 0U);
    CHECK(kernel_login_trace_snapshot_render(line, sizeof(line)) != 0U &&
              strstr(line, " ok=0") != NULL &&
              strstr(line, " stage=1") != NULL,
          "a failed attempt replaces the previous success as the latest result");
    login_trace_commit(true, 4U, 200000U);
}

static void test_full_journal_saves_a_verified_session(void) {
    struct diag_export_session session;
    struct install_journal_status status;
    enum diag_export_result result;
    char path[256];
    unsigned published;

    machine_reset();
    CHECK(medium_bind(), "the reserved FAT12 file binds");
    CHECK(install_journal_platform_ready(),
          "a fresh journal is READY before the telemetry runs");

    published = telemetry_fill_to_full();
    install_journal_platform_status(&status);
    CHECK(published > 0U && status.state == INSTALL_JOURNAL_FULL,
          "the telemetry fills the journal to FULL");
    CHECK(status.reason != NULL && strcmp(status.reason, "CAPACITY") == 0,
          "FULL names capacity, not an I/O failure");
    CHECK(install_journal_platform_written() == JOURNAL_SLOTS,
          "FULL means every declared slot was written");
    CHECK(install_journal_platform_capacity_slots() == JOURNAL_SLOTS,
          "the declared capacity is the reservation's own");
    CHECK(!install_journal_platform_ready(),
          "a FULL journal is not READY - that is the state the export used to "
          "mistake for a missing medium");
    CHECK(install_journal_platform_saveable(),
          "a FULL journal is still a placement the export can restart");

    login_trace_commit(true, 4U, 200000U);
    appliance_panel_auth_trace_begin(1000U);
    appliance_panel_auth_trace_commit(APPLIANCE_PANEL_AUTH_RESULT_OK, 22000U);
    result = diag_export_save(&session);
    CHECK(result == DIAG_EXPORT_OK,
          "diag save recovers the room through the rewind and saves");
    if (result != DIAG_EXPORT_OK) {
        printf("  save refused: reason=%s step=%s\n",
               diag_export_last_reason(),
               diag_export_last_failure() != NULL
                   ? diag_export_last_failure()->step
                   : "NONE");
    }
    CHECK(session.id == 1U, "the first save is session 1");
    CHECK(session.records == REPORT_RECORDS,
          "the session carries the medium, both controllers, the "
          "network/panel records and the finished login trace");
    CHECK(install_journal_platform_sequence() == 1U,
          "the header's persisted counter is the session's identity");
    CHECK(journal_state() == INSTALL_JOURNAL_READY,
          "a saved session leaves the journal READY for the next one");

    CHECK(image_write(IMAGE_PATH),
          "the finished image is written for the extractor");
    CHECK(extractor_run(), "the operator's extractor verifies the journal");
    CHECK(extract_log_contains("verified"),
          "the extractor reports the session as verified");
    snprintf(path, sizeof(path), "%s/HFOC-DIAG-%08X.txt", REPORT_DIR,
             session.id);
    CHECK(file_contains(path, "[DIAG] id=00000001 state=BEGIN"),
          "the extracted report carries the session's BEGIN record");
    CHECK(file_contains(path, "[DIAG] id=00000001 state=END"),
          "the extracted report carries the session's END record");
    CHECK(file_contains(path, "build=host-590"),
          "the extracted BEGIN names the build this proof compiled");
    CHECK(file_contains(path, "state=END records=10 crc="),
          "the extracted END carries the record count the export verified");
    CHECK(file_contains(path, "[LOGIN-TRACE] v=1 build=host-590"),
          "the extracted file carries the production login trace");
    CHECK(file_contains(path, "[PANEL-AUTH] v=1"),
          "the extracted file carries the browser sign-in record");
    CHECK(file_contains(path, "state=COMPLETED") &&
              file_contains(path, "result=OK") &&
              file_contains(path, "duration_ms=21000"),
          "the saved panel trace is the finished attempt, not a placeholder");
    CHECK(file_contains(path, "[DIAG-FAIL] state=NO_FAILURE"),
          "a successful save carries the honest empty refusal record");
    CHECK(file_contains(path, "available=1"),
          "the saved trace is a finished attempt, not a cold-boot placeholder");
    CHECK(file_contains(path, " ok=1 ") && file_contains(path, "total_us="),
          "the saved trace carries the measured result and duration");
    CHECK(!file_contains(path, "state=NOT_ATTEMPTED") &&
              !file_contains(path, "state=IN_PROGRESS"),
          "the saved trace does not present a placeholder as the measurement");
    CHECK(file_contains(path, "[NET-STATUS] device=PRESENT"),
          "the extracted report carries the machine's network answer");
    CHECK(file_contains(path, "journal=FULL"),
          "the session's medium record names the FULL journal it recovered");
    snprintf(path, sizeof(path), "%s/HFOC-SSH-HOST-%08X.pub", REPORT_DIR,
             session.id);
    CHECK(file_contains(path, "ecdsa-sha2-nistp256 AAAA"),
          "the extractor writes the OpenSSH public key");
    snprintf(path, sizeof(path), "%s/HFOC-SSH-HOST-%08X.fingerprint",
             REPORT_DIR, session.id);
    CHECK(file_contains(path, "SHA256:Lc8UsC2OrEcPGz9MqeDS2DKOa4gzVdhYsQpSuWz+0QE"),
          "the extractor writes the independently checked fingerprint");
    snprintf(path, sizeof(path), "%s/HFOC-SSH-HOST-%08X.known_hosts.candidate",
             REPORT_DIR, session.id);
    CHECK(file_contains(path, "192.0.2.13 ecdsa-sha2-nistp256 AAAA"),
          "the extractor writes the known_hosts candidate for the record's address");
    CHECK(extract_log_contains("hostkey=SHA256:Lc8UsC2OrEcPGz9MqeDS2DKOa4gzVdhYsQpSuWz+0QE"),
          "the extractor reports the host key it verified");
}

static void test_two_saves_after_full_are_two_sessions(void) {
    struct diag_export_session first;
    struct diag_export_session second;
    char path[256];

    machine_reset();
    CHECK(medium_bind(), "the reserved FAT12 file binds for the second case");
    (void)telemetry_fill_to_full();
    CHECK(journal_state() == INSTALL_JOURNAL_FULL, "the journal is FULL again");
    login_trace_commit(true, 4U, 200000U);

    CHECK(diag_export_save(&first) == DIAG_EXPORT_OK,
          "the first save after the fill is verified");
    CHECK(diag_export_save(&second) == DIAG_EXPORT_OK,
          "the second save after the fill is verified");
    CHECK(first.id == 1U && second.id == 2U,
          "the two saves carry different session identifiers");
    CHECK(first.records == second.records,
          "both sessions carry the same composition");
    CHECK(install_journal_platform_sequence() == 2U,
          "the persisted counter advanced once per save");

    CHECK(image_write(IMAGE_PATH),
          "the image with two sessions is written");
    CHECK(extractor_run(), "the extractor reads the second session");
    CHECK(extract_log_contains("session 00000002"),
          "the extractor reports the newest session");
    snprintf(path, sizeof(path), "%s/HFOC-DIAG-%08X.txt", REPORT_DIR,
             second.id);
    CHECK(file_contains(path, "[DIAG] id=00000002 state=BEGIN"),
          "the extracted report is the second session's");
    CHECK(file_contains(path, "[DIAG] id=00000002 state=END"),
          "the second session's END is complete");
    CHECK(file_contains(path, "[LOGIN-TRACE]") &&
              file_contains(path, "available=1"),
          "the second save carries the same finished login trace");
    snprintf(path, sizeof(path), "%s/HFOC-DIAG-%08X.txt", REPORT_DIR, first.id);
    CHECK(file_contains(path, "[DIAG] id=00000001 state=BEGIN"),
          "the first session's own report is still a complete file");
    CHECK(file_contains(path, "[LOGIN-TRACE]") &&
              file_contains(path, "available=1"),
          "the first save also carried the finished login trace");
}

static void test_a_write_failure_is_named(void) {
    struct diag_export_session session;
    enum diag_export_result result;
    const char *record;

    machine_reset();
    CHECK(medium_bind(), "the reserved FAT12 file binds for the write failure");
    (void)telemetry_fill_to_full();
    login_trace_commit(true, 4U, 200000U);
    medium.fail_data_writes = true;
    result = diag_export_save(&session);
    CHECK(result == DIAG_EXPORT_WRITE,
          "a record write that fails is a WRITE refusal");
    CHECK(strcmp(diag_export_last_reason(), "WRITE") == 0,
          "the console's reason is the write, not the medium");
    CHECK(diag_export_last_failure() != NULL &&
              strcmp(diag_export_last_failure()->step, "BEGIN_WRITE") == 0,
          "the refusal names BEGIN_WRITE as the step that failed");
    CHECK(journal_state() == INSTALL_JOURNAL_UNAVAILABLE,
          "the journal names the failed placement after the refused write");
    CHECK(!install_journal_platform_saveable(),
          "a failed medium is not a FULL journal the export may restart");
    CHECK(!install_journal_platform_ready(), "a failed medium is not READY");
    CHECK(diag_export_snapshot_compose() != 0U,
          "the composer still runs after a failed save");
    record = snapshot_line_starting_with("[LOGIN-TRACE]");
    CHECK(record != NULL && strstr(record, "available=1") != NULL,
          "a failed save leaves the finished trace for the next snapshot");
    record = snapshot_line_starting_with("[DIAG-FAIL]");
    CHECK(record != NULL && strstr(record, "state=RECORDED") != NULL &&
              strstr(record, "valid=1") != NULL &&
              strstr(record, "reason=WRITE") != NULL &&
              strstr(record, "step=BEGIN_WRITE") != NULL,
          "the next snapshot names the retained write refusal");
    CHECK(record != NULL && strstr(record, " commit=") != NULL &&
              strstr(record, " storage=") != NULL,
          "the shared record carries the commit and storage receipts");
    if (record != NULL) {
        char before[DIAG_FAILURE_STATUS_MAX];

        snprintf(before, sizeof(before), "%s", record);
        CHECK(diag_export_snapshot_compose() != 0U,
              "a second composition still succeeds");
        record = snapshot_line_starting_with("[DIAG-FAIL]");
        CHECK(record != NULL && strcmp(record, before) == 0,
              "reading the record does not clear or rewrite the first cause");
    }

    /* The distinction the item is about, from the other side: the same
       predicate that accepts FULL must refuse a medium that failed. */
    result = diag_export_save(&session);
    CHECK(result == DIAG_EXPORT_NO_MEDIUM,
          "the export refuses a medium that failed");
    CHECK(strcmp(diag_export_last_reason(), "NO_MEDIUM") == 0,
          "the refusal is named NO_MEDIUM");
    CHECK(diag_export_last_failure() != NULL &&
              strcmp(diag_export_last_failure()->step, "MEDIUM_PROBE") == 0,
          "the refusal names the medium probe as its stage");
}

static void test_a_flush_failure_is_named(void) {
    struct diag_export_session session;
    enum diag_export_result result;

    machine_reset();
    CHECK(medium_bind(), "the reserved FAT12 file binds for the flush failure");
    (void)telemetry_fill_to_full();
    /* The commit point flushes a header that still names the full capacity and
       the rewind one that names slot 1; the session's final flush is the first
       one naming a record inside the stream. */
    medium.flush_fails_inside_stream = true;
    result = diag_export_save(&session);
    CHECK(result == DIAG_EXPORT_WRITE,
          "a flush that fails is a WRITE refusal");
    CHECK(strcmp(diag_export_last_reason(), "WRITE") == 0,
          "the flush refusal keeps the write as the cause");
    CHECK(diag_export_last_failure() != NULL &&
              strcmp(diag_export_last_failure()->step, "FLUSH") == 0,
          "the refusal names FLUSH as the step that failed");
    CHECK(journal_state() == INSTALL_JOURNAL_UNAVAILABLE,
          "a failed flush leaves the journal unavailable");
}

int main(void) {
    if (system("mkdir -p " IMAGE_DIR " >/dev/null 2>&1") == -1) {
        fprintf(stderr, "cannot create %s\n", IMAGE_DIR);
        return 2;
    }
    test_login_trace_cold_is_honest();
    test_login_trace_shared_renderer_and_bounds();
    test_full_journal_saves_a_verified_session();
    test_two_saves_after_full_are_two_sessions();
    test_a_write_failure_is_named();
    test_a_flush_failure_is_named();

    if (failures != 0U) {
        printf("\n%u FAILURES of %u checks\n", failures, checks);
        return 1;
    }
    printf("diag export after a FULL journal host test: %u checks ALL OK\n",
           checks);
    return 0;
}
