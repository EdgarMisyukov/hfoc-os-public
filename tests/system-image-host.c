#include "system_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SYSTEM_IMAGE_HOST_TEST

#define HASH_A "aaaaaaaa" "aaaaaaaa" "aaaaaaaa" "aaaaaaaa" \
               "aaaaaaaa" "aaaaaaaa" "aaaaaaaa" "aaaaaaaa"

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static void poison_output(struct system_image_manifest *manifest) {
    memset(manifest, 0xA5, sizeof(*manifest));
}

static bool output_unchanged(const struct system_image_manifest *manifest) {
    const unsigned char *bytes = (const unsigned char *)manifest;
    size_t index;

    for (index = 0U; index < sizeof(*manifest); ++index) {
        if (bytes[index] != 0xA5U) {
            return false;
        }
    }
    return true;
}

static enum system_image_status parse_manifest_variant(
    const char *manifest_version,
    const char *image_id,
    const char *arch,
    const char *min_kernel_abi,
    const char *max_kernel_abi,
    const char *component,
    struct system_image_manifest *manifest_out)
{
    char text[2048];
    size_t offset = 0U;
    int length;

#define APPEND_LINE(field) do { \
    if ((field) != NULL) { \
        length = snprintf(text + offset, sizeof(text) - offset, "%s\n", (field)); \
        if (length < 0 || (size_t)length >= sizeof(text) - offset) { \
            return SYSTEM_IMAGE_TOO_LONG; \
        } \
        offset += (size_t)length; \
    } \
} while (0)

    if (manifest_version != NULL) {
        char line[SYSTEM_IMAGE_LINE_MAX + 1U];
        length = snprintf(line, sizeof(line), "manifest_version=%s",
                          manifest_version);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            return SYSTEM_IMAGE_TOO_LONG;
        }
        APPEND_LINE(line);
    }
    if (image_id != NULL) {
        char line[SYSTEM_IMAGE_LINE_MAX + 1U];
        length = snprintf(line, sizeof(line), "image_id=%s", image_id);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            return SYSTEM_IMAGE_TOO_LONG;
        }
        APPEND_LINE(line);
    }
    if (arch != NULL) {
        char line[SYSTEM_IMAGE_LINE_MAX + 1U];
        length = snprintf(line, sizeof(line), "arch=%s", arch);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            return SYSTEM_IMAGE_TOO_LONG;
        }
        APPEND_LINE(line);
    }
    if (min_kernel_abi != NULL) {
        char line[SYSTEM_IMAGE_LINE_MAX + 1U];
        length = snprintf(line, sizeof(line), "min_kernel_abi=%s",
                          min_kernel_abi);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            return SYSTEM_IMAGE_TOO_LONG;
        }
        APPEND_LINE(line);
    }
    if (max_kernel_abi != NULL) {
        char line[SYSTEM_IMAGE_LINE_MAX + 1U];
        length = snprintf(line, sizeof(line), "max_kernel_abi=%s",
                          max_kernel_abi);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            return SYSTEM_IMAGE_TOO_LONG;
        }
        APPEND_LINE(line);
    }
    if (component != NULL) {
        char line[SYSTEM_IMAGE_LINE_MAX + 1U];
        length = snprintf(line, sizeof(line), "component=%s", component);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            return SYSTEM_IMAGE_TOO_LONG;
        }
        APPEND_LINE(line);
    }

#undef APPEND_LINE
    return system_image_manifest_parse(text, offset, manifest_out);
}

static void test_validation_and_too_long(void) {
    struct system_image_manifest output;
    char too_long[SYSTEM_IMAGE_INPUT_MAX + 1U];

    memset(&output, 0, sizeof(output));
    CHECK(system_image_manifest_parse(NULL, 0U, &output) == SYSTEM_IMAGE_NULL,
          "manifest parser rejects null input");
    CHECK(system_image_manifest_parse("", 0U, NULL) == SYSTEM_IMAGE_NULL,
          "manifest parser rejects null output");
    CHECK(system_image_manifest_parse(NULL, 0U, NULL) == SYSTEM_IMAGE_NULL,
          "manifest parser rejects null input and output");

    memset(too_long, 'x', sizeof(too_long));
    CHECK(system_image_manifest_parse(too_long, sizeof(too_long), &output) ==
              SYSTEM_IMAGE_TOO_LONG,
          "manifest parser rejects input over maximum length");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "manifest validation performs no allocation");
}

static void test_valid_manifest(void) {
    struct system_image_manifest output;
    const char *boot = "boot:1.0:" HASH_A;
    const char *kernel = "kernel:0.2.6:" HASH_A;
    const char *initramfs = "initramfs:1.0:" HASH_A;
    enum system_image_status status;

    poison_output(&output);
    status = parse_manifest_variant("1", "7", "x86_64", "1", "3", boot,
                                    &output);
    CHECK(status == SYSTEM_IMAGE_OK,
          "valid one-component manifest parses successfully");
    if (status != SYSTEM_IMAGE_OK) {
        return;
    }
    CHECK(output.manifest_version == SYSTEM_IMAGE_MANIFEST_VERSION &&
              output.image_id == 7U &&
              strcmp(output.arch, "x86_64") == 0 &&
              output.min_kernel_abi == 1U &&
              output.max_kernel_abi == 3U &&
              output.component_count == 1U,
          "valid manifest publishes exact header and count fields");
    CHECK(strcmp(output.components[0].name, "boot") == 0 &&
              strcmp(output.components[0].version, "1.0") == 0 &&
              strcmp(output.components[0].hash, HASH_A) == 0,
          "valid manifest publishes exact component fields");

    poison_output(&output);
    status = parse_manifest_variant("1", "7", "x86_64", "1", "3", kernel,
                                    &output);
    CHECK(status == SYSTEM_IMAGE_OK &&
              strcmp(output.components[0].version, "0.2.6") == 0,
          "component version may contain dots");

    poison_output(&output);
    status = parse_manifest_variant("1", "7", "x86_64", "1", "3", initramfs,
                                    &output);
    CHECK(status == SYSTEM_IMAGE_OK &&
              strcmp(output.components[0].name, "initramfs") == 0,
          "component name may contain letters and digits");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid manifest parse performs no allocation");
}

static void test_status_strings(void) {
    enum system_image_status status;

    for (status = SYSTEM_IMAGE_OK;
         status <= SYSTEM_IMAGE_INCOMPATIBLE_ABI; ++status) {
        const char *name = system_image_status_string(status);

        CHECK(name != NULL &&
                  (status == SYSTEM_IMAGE_OK ?
                       strcmp(name, "SYSTEM_IMAGE_OK") == 0 :
                       status == SYSTEM_IMAGE_NULL ?
                           strcmp(name, "SYSTEM_IMAGE_NULL") == 0 :
                           status == SYSTEM_IMAGE_TOO_LONG ?
                               strcmp(name, "SYSTEM_IMAGE_TOO_LONG") == 0 :
                               status == SYSTEM_IMAGE_BAD_VERSION ?
                                   strcmp(name, "SYSTEM_IMAGE_BAD_VERSION") == 0 :
                                   status == SYSTEM_IMAGE_MISSING_FIELD ?
                                       strcmp(name, "SYSTEM_IMAGE_MISSING_FIELD") == 0 :
                                       status == SYSTEM_IMAGE_DUPLICATE_FIELD ?
                                           strcmp(name, "SYSTEM_IMAGE_DUPLICATE_FIELD") == 0 :
                                           status == SYSTEM_IMAGE_UNKNOWN_FIELD ?
                                               strcmp(name, "SYSTEM_IMAGE_UNKNOWN_FIELD") == 0 :
                                               status == SYSTEM_IMAGE_MALFORMED_FIELD ?
                                                   strcmp(name, "SYSTEM_IMAGE_MALFORMED_FIELD") == 0 :
                                                   status == SYSTEM_IMAGE_OUT_OF_RANGE ?
                                                       strcmp(name, "SYSTEM_IMAGE_OUT_OF_RANGE") == 0 :
                                                       status == SYSTEM_IMAGE_EMPTY_COMPONENT_LIST ?
                                                           strcmp(name, "SYSTEM_IMAGE_EMPTY_COMPONENT_LIST") == 0 :
                                                           status == SYSTEM_IMAGE_TOO_MANY_COMPONENTS ?
                                                               strcmp(name, "SYSTEM_IMAGE_TOO_MANY_COMPONENTS") == 0 :
                                                               status == SYSTEM_IMAGE_DUPLICATE_COMPONENT ?
                                                                   strcmp(name, "SYSTEM_IMAGE_DUPLICATE_COMPONENT") == 0 :
                                                                   status == SYSTEM_IMAGE_UNSUPPORTED_ARCH ?
                                                                       strcmp(name, "SYSTEM_IMAGE_UNSUPPORTED_ARCH") == 0 :
                                                                       strcmp(name, "SYSTEM_IMAGE_INCOMPATIBLE_ABI") == 0),
              "manifest status string is exact for every defined status");
    }
    CHECK(system_image_status_string((enum system_image_status)99U) != NULL,
          "manifest status string handles undefined status");
}

static void test_duplicate_and_unknown_fields(void) {
    struct system_image_manifest output;
    const char *duplicate_header =
        "manifest_version=1\n"
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot:1.0:" HASH_A "\n";
    const char *duplicate_component =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot:1.0:" HASH_A "\n"
        "component=boot:2.0:" HASH_A "\n";
    const char *unknown =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot:1.0:" HASH_A "\n"
        "unknown_field=1\n";

    poison_output(&output);
    CHECK(system_image_manifest_parse(duplicate_header,
                                      strlen(duplicate_header), &output) ==
              SYSTEM_IMAGE_DUPLICATE_FIELD &&
              output_unchanged(&output),
          "duplicate header field fails without output publication");

    poison_output(&output);
    CHECK(system_image_manifest_parse(duplicate_component,
                                      strlen(duplicate_component), &output) ==
              SYSTEM_IMAGE_DUPLICATE_COMPONENT &&
              output_unchanged(&output),
          "duplicate component name fails without output publication");

    poison_output(&output);
    CHECK(system_image_manifest_parse(unknown, strlen(unknown), &output) ==
              SYSTEM_IMAGE_UNKNOWN_FIELD &&
              output_unchanged(&output),
          "unknown manifest field fails without output publication");
    CHECK(parse_manifest_variant("1", "7", "x86_64", "1", "3", NULL,
                                 &output) == SYSTEM_IMAGE_EMPTY_COMPONENT_LIST &&
              output_unchanged(&output),
          "manifest without components fails without output publication");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "duplicate and unknown manifest parsing performs no allocation");
}

static void test_missing_fields(void) {
    struct system_image_manifest output;
    const char *boot = "boot:1.0:" HASH_A;

    CHECK(parse_manifest_variant(NULL, "7", "x86_64", "1", "3", boot,
                                 &output) == SYSTEM_IMAGE_MISSING_FIELD,
          "missing manifest version fails");
    CHECK(parse_manifest_variant("1", NULL, "x86_64", "1", "3", boot,
                                 &output) == SYSTEM_IMAGE_MISSING_FIELD,
          "missing image id fails");
    CHECK(parse_manifest_variant("1", "7", NULL, "1", "3", boot,
                                 &output) == SYSTEM_IMAGE_MISSING_FIELD,
          "missing architecture fails");
    CHECK(parse_manifest_variant("1", "7", "x86_64", NULL, "3", boot,
                                 &output) == SYSTEM_IMAGE_MISSING_FIELD,
          "missing minimum kernel ABI fails");
    CHECK(parse_manifest_variant("1", "7", "x86_64", "1", NULL, boot,
                                 &output) == SYSTEM_IMAGE_MISSING_FIELD,
          "missing maximum kernel ABI fails");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "missing-field parsing performs no allocation");
}

static void test_malformed_lines(void) {
    struct system_image_manifest output;
    const char *no_delimiter =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component boot:1.0:" HASH_A "\n";
    const char *extra_delimiter =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot:1.0:" HASH_A "=extra\n";
    const char *blank_line =
        "\n"
        "manifest_version=1\n";
    const char *invalid_key =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component!=boot:1.0:" HASH_A "\n";
    const char *invalid_numeric =
        "manifest_version=1\n"
        "image_id=7x\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot:1.0:" HASH_A "\n";
    const char *component_no_colons =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot\n";
    const char *component_extra_colon =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot:1.0:" HASH_A ":extra\n";
    const char *component_empty_name =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=:1.0:" HASH_A "\n";
    const char *component_empty_version =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot::" HASH_A "\n";
    const char *component_empty_hash =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot:1.0:\n";
    const char *component_bad_name =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot@x:1.0:" HASH_A "\n";
    const char *component_bad_version =
        "manifest_version=1\n"
        "image_id=7\n"
        "arch=x86_64\n"
        "min_kernel_abi=1\n"
        "max_kernel_abi=3\n"
        "component=boot:1.0 1:" HASH_A "\n";
    char short_hash[65];
    char long_hash[66];
    char non_hex_hash[65];
    char short_component[SYSTEM_IMAGE_LINE_MAX + 1U];
    char long_component[SYSTEM_IMAGE_LINE_MAX + 1U];
    char non_hex_component[SYSTEM_IMAGE_LINE_MAX + 1U];
    int length;

    memset(short_hash, 'a', 63U);
    short_hash[63] = '\0';
    memset(long_hash, 'a', 65U);
    long_hash[65] = '\0';
    memset(non_hex_hash, 'g', 64U);
    non_hex_hash[64] = '\0';
    length = snprintf(short_component, sizeof(short_component),
                      "boot:1.0:%s", short_hash);
    CHECK(length >= 0 && (size_t)length < sizeof(short_component),
          "short hash component is representable");
    length = snprintf(long_component, sizeof(long_component),
                      "boot:1.0:%s", long_hash);
    CHECK(length >= 0 && (size_t)length < sizeof(long_component),
          "long hash component is representable");
    length = snprintf(non_hex_component, sizeof(non_hex_component),
                      "boot:1.0:%s", non_hex_hash);
    CHECK(length >= 0 && (size_t)length < sizeof(non_hex_component),
          "non-hex hash component is representable");

    poison_output(&output);
    CHECK(system_image_manifest_parse(no_delimiter, strlen(no_delimiter),
                                      &output) == SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "line without delimiter is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(extra_delimiter,
                                      strlen(extra_delimiter), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "line with extra delimiter is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(blank_line, strlen(blank_line), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "blank line is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(invalid_key, strlen(invalid_key),
                                      &output) == SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "invalid key character is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(invalid_numeric,
                                      strlen(invalid_numeric), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "non-numeric header value is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(component_no_colons,
                                      strlen(component_no_colons), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "component without colons is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(component_extra_colon,
                                      strlen(component_extra_colon), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "component with extra colon is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(component_empty_name,
                                      strlen(component_empty_name), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "component with empty name is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(component_empty_version,
                                      strlen(component_empty_version), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "component with empty version is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(component_empty_hash,
                                      strlen(component_empty_hash), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "component with empty hash is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(component_bad_name,
                                      strlen(component_bad_name), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "component with invalid name character is rejected");

    poison_output(&output);
    CHECK(system_image_manifest_parse(component_bad_version,
                                      strlen(component_bad_version), &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD &&
              output_unchanged(&output),
          "component with invalid version character is rejected");

    CHECK(parse_manifest_variant("1", "7", "x86_64", "1", "3",
                                 short_component, &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD,
          "component with short hash is rejected");
    CHECK(parse_manifest_variant("1", "7", "x86_64", "1", "3",
                                 long_component, &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD,
          "component with long hash is rejected");
    CHECK(parse_manifest_variant("1", "7", "x86_64", "1", "3",
                                 non_hex_component, &output) ==
              SYSTEM_IMAGE_MALFORMED_FIELD,
          "component with non-hex hash is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed-line parsing performs no allocation");
}

static void test_ranges_compatibility_and_capacity(void) {
    struct system_image_manifest output;
    const char *boot = "boot:1.0:" HASH_A;
    char too_many[4096];
    size_t offset = 0U;
    uint32_t index;
    int length;

    CHECK(parse_manifest_variant("2", "7", "x86_64", "1", "3", boot,
                                 &output) == SYSTEM_IMAGE_BAD_VERSION,
          "wrong manifest version fails");
    CHECK(parse_manifest_variant("1", "0", "x86_64", "1", "3", boot,
                                 &output) == SYSTEM_IMAGE_OUT_OF_RANGE,
          "zero image id is out of range");
    CHECK(parse_manifest_variant("1", "4097", "x86_64", "1", "3", boot,
                                 &output) == SYSTEM_IMAGE_OUT_OF_RANGE,
          "image id above maximum is out of range");
    CHECK(parse_manifest_variant("1", "7", "arm64", "1", "3", boot,
                                 &output) == SYSTEM_IMAGE_UNSUPPORTED_ARCH,
          "unsupported architecture fails");
    CHECK(parse_manifest_variant("1", "7", "x86_64", "0", "3", boot,
                                 &output) == SYSTEM_IMAGE_OUT_OF_RANGE,
          "minimum ABI below range fails");
    CHECK(parse_manifest_variant("1", "7", "x86_64", "256", "3", boot,
                                 &output) == SYSTEM_IMAGE_OUT_OF_RANGE,
          "minimum ABI above range fails");
    CHECK(parse_manifest_variant("1", "7", "x86_64", "1", "0", boot,
                                 &output) == SYSTEM_IMAGE_OUT_OF_RANGE,
          "maximum ABI below range fails");
    CHECK(parse_manifest_variant("1", "7", "x86_64", "1", "256", boot,
                                 &output) == SYSTEM_IMAGE_OUT_OF_RANGE,
          "maximum ABI above range fails");
    CHECK(parse_manifest_variant("1", "7", "x86_64", "4", "3", boot,
                                 &output) == SYSTEM_IMAGE_INCOMPATIBLE_ABI,
          "minimum ABI above maximum ABI fails");

    length = snprintf(too_many, sizeof(too_many),
                      "manifest_version=1\n"
                      "image_id=7\n"
                      "arch=x86_64\n"
                      "min_kernel_abi=1\n"
                      "max_kernel_abi=3\n");
    CHECK(length >= 0 && (size_t)length < sizeof(too_many),
          "too-many component header is representable");
    offset = (size_t)length;
    for (index = 0U; index <= SYSTEM_IMAGE_COMPONENT_MAX; ++index) {
        length = snprintf(too_many + offset, sizeof(too_many) - offset,
                          "component=c%02u:1.0:" HASH_A "\n", index);
        CHECK(length >= 0 && (size_t)length < sizeof(too_many) - offset,
              "too-many component line is representable");
        offset += (size_t)length;
    }
    CHECK(system_image_manifest_parse(too_many, offset, &output) ==
              SYSTEM_IMAGE_TOO_MANY_COMPONENTS,
          "component count above capacity fails");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "range and capacity parsing performs no allocation");
}

int main(void) {
    test_validation_and_too_long();
    test_valid_manifest();
    test_status_strings();
    test_duplicate_and_unknown_fields();
    test_missing_fields();
    test_malformed_lines();
    test_ranges_compatibility_and_capacity();

    if (failures != 0) {
        fprintf(stderr, "System image manifest host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("System image manifest host tests: ALL OK");
    return 0;
}

#endif
