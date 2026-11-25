/*
 * ITEM-506 host proof for the administrator credential model and its
 * persistent record.
 *
 * The proof runs the production credential module against the same in-memory
 * persistent secret store the enrollment proof uses, pins the PBKDF2 vectors
 * (computed independently), and checks the promises the item makes: policy,
 * salt and iteration count in the record, wrong password refused, the legacy
 * single-SHA-256 record refused as a credential and migrated only by an
 * explicit re-provisioning, and no plaintext password anywhere in the store.
 */

#include "appliance_admin_credential.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_secret_persistence.h"
#include "block_device.h"
#include "config_journal.h"
#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_ADMIN_CREDENTIAL_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 8U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_REGION_OFFSET 1U
#define TEST_REGION_COUNT 7U

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
    unsigned char storage[TEST_CAPACITY];
    size_t read_calls;
    size_t write_calls;
};

static void zero_bytes(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(void *destination, const void *source, size_t length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static void fill_bytes(void *destination, size_t length, unsigned char value)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = value;
    }
}

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void init_fixture(struct fixture_device *fixture)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    zero_bytes(fixture, sizeof(*fixture));
    copy_bytes(fixture->storage, magic, sizeof(magic));
    fixture->storage[8U] = DATA_REGION_FORMAT_VERSION;
    store_le64(fixture->storage + 16U, TEST_REGION_OFFSET);
    store_le64(fixture->storage + 24U, TEST_REGION_COUNT);
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->read_calls;
    copy_bytes(buffer, fixture->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->write_calls;
    copy_bytes(fixture->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(struct fixture_device *fixture)
{
    struct block_device device;

    zero_bytes(&device, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = fixture;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    return device;
}

static bool contains_bytes(const void *haystack,
                           size_t haystack_length,
                           const char *needle)
{
    const unsigned char *bytes = haystack;
    const size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length == 0U || needle_length > haystack_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= haystack_length; ++index) {
        if (memcmp(bytes + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool make_credential(
    const char *login,
    const char *password,
    struct appliance_admin_credential *credential_out)
{
    static const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH] = {
        0x10U, 0x32U, 0x54U, 0x76U, 0x98U, 0xbaU, 0xdcU, 0xfeU,
        0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0xcdU, 0xefU
    };

    return appliance_admin_credential_create(
               login, strlen(login), password, strlen(password), salt,
               APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN,
               credential_out) == APPLIANCE_ADMIN_CREDENTIAL_OK;
}

static bool load_secret_store(
    struct appliance_persistent_secret_config *config,
    struct block_device *device)
{
    zero_bytes(config, sizeof(*config));
    return appliance_persistent_secret_config_load(config, device) ==
           APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
}

static bool commit_credential(
    struct appliance_persistent_secret_config *config,
    const struct appliance_admin_credential *credential)
{
    return appliance_persistent_secret_config_commit_admin_credential(
               config, credential) ==
           APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
}

static bool credentials_equal(const struct appliance_admin_credential *left,
                              const struct appliance_admin_credential *right)
{
    return left->version == right->version &&
           left->iterations == right->iterations &&
           strcmp(left->login, right->login) == 0 &&
           memcmp(left->salt, right->salt,
                  APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH) == 0 &&
           memcmp(left->verifier, right->verifier,
                  APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH) == 0;
}

static void test_policy_constants(void)
{
    CHECK(APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MIN == 1U &&
          APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX == 32U,
          "login policy is 1..32");
    CHECK(APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MIN == 12U &&
          APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX == 128U,
          "password policy is 12..128");
    CHECK(APPLIANCE_ADMIN_CREDENTIAL_FAILURE_LIMIT == 5U,
          "five failed attempts are the lockout threshold");
    CHECK(APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH == 16U &&
          APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH == 32U,
          "the record carries a 16-byte salt and a 32-byte verifier");
    CHECK(APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT >=
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN &&
          APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT <=
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MAX,
          "the default iteration count is inside the downgrade guard");
}

static void test_login_policy(void)
{
    CHECK(appliance_admin_credential_validate_login("admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "a plain login is accepted");
    CHECK(appliance_admin_credential_validate_login("a.b_c-1", 7U) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the full login alphabet is accepted");
    {
        const char *thirty_two = "abcdefghijklmnopqrstuvwxyz012345";
        CHECK(strlen(thirty_two) == 32U &&
              appliance_admin_credential_validate_login(
                  thirty_two, 32U) == APPLIANCE_ADMIN_CREDENTIAL_OK,
              "a 32-character login is accepted");
    }
    CHECK(appliance_admin_credential_validate_login("", 0U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN,
          "an empty login is refused");
    CHECK(appliance_admin_credential_validate_login(
              "abcdefghijklmnopqrstuvwxyz0123456", 33U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN,
          "a 33-character login is refused");
    CHECK(appliance_admin_credential_validate_login("ad min", 6U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN,
          "a login with a space is refused");
    CHECK(appliance_admin_credential_validate_login("a:b", 3U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN,
          "a colon is refused because it breaks the record key");
    CHECK(appliance_admin_credential_validate_login("a=b", 3U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN,
          "an equals sign is refused because it breaks the record key");
    CHECK(appliance_admin_credential_validate_login("a/b", 3U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN,
          "a slash is refused");
    CHECK(appliance_admin_credential_validate_login("\xc3\xa9", 2U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN,
          "a non-ASCII login is refused");
    CHECK(appliance_admin_credential_validate_login(NULL, 0U) ==
              APPLIANCE_ADMIN_CREDENTIAL_NULL,
          "a null login is refused by name");
}

static void test_password_policy(void)
{
    static const char twelve[] = "twelve chars";
    static const char very_long[] =
        "012345678901234567890123456789012345678901234567890123456789"
        "012345678901234567890123456789012345678901234567890123456789"
        "01234567";
    static const char all_spaces[] = "            ";

    CHECK(appliance_admin_credential_validate_password(
              twelve, strlen(twelve), "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "a 12-character passphrase is accepted");
    CHECK(strlen(very_long) == 128U &&
          appliance_admin_credential_validate_password(
              very_long, strlen(very_long), "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "a 128-character passphrase is accepted");
    CHECK(appliance_admin_credential_validate_password(
              "short pass", 10U, "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD,
          "a password shorter than twelve is refused");
    CHECK(appliance_admin_credential_validate_password(
              all_spaces, 12U, "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD,
          "an all-space password is refused");
    CHECK(appliance_admin_credential_validate_password(
              "admin", 5U, "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD,
          "a password equal to the login is refused");
    CHECK(appliance_admin_credential_validate_password(
              "ADMIN", 5U, "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD,
          "a password equal to the login in another case is refused");
    CHECK(appliance_admin_credential_validate_password(
              "password", 8U, "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD,
          "a weak-list password is refused by name");
    CHECK(appliance_admin_credential_validate_password(
              "PASSWORD123", 11U, "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD,
          "the weak list is case-insensitive");
    CHECK(appliance_admin_credential_validate_password(
              "Hfoc-123", 8U, "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD,
          "the product name with digits is refused");
    CHECK(appliance_admin_credential_validate_password(
              "with\ttab", 8U, "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD,
          "a non-printable password is refused rather than trimmed");
    CHECK(appliance_admin_credential_validate_password(
              NULL, 0U, "admin", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_NULL,
          "a null password is refused by name");
    CHECK(appliance_admin_credential_password_weak("qwerty", 6U),
          "the weak helper sees the list directly");
    CHECK(!appliance_admin_credential_password_weak("twelve chars", 12U),
          "a passphrase is not in the weak list");
}

static void test_pbkdf2_vectors(void)
{
    /* Independently computed PBKDF2-HMAC-SHA256 vectors (RFC 7914 shape). */
    static const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH] = {
        's', 'a', 'l', 't', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U
    };
    static const uint8_t expected_10000
        [APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH] = {
        0x17U, 0x22U, 0x99U, 0xbeU, 0x25U, 0xdbU, 0x6fU, 0x9cU,
        0xfbU, 0xdeU, 0x4dU, 0x96U, 0x13U, 0xbdU, 0xbeU, 0x2fU,
        0x7fU, 0x2cU, 0x8aU, 0x6aU, 0x69U, 0xf8U, 0xbbU, 0x20U,
        0x9bU, 0x6dU, 0x7cU, 0x8eU, 0x51U, 0x98U, 0x97U, 0xd8U
    };
    static const uint8_t expected_200000
        [APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH] = {
        0x83U, 0x79U, 0x11U, 0xb8U, 0x28U, 0xf7U, 0x65U, 0xceU,
        0x9dU, 0x26U, 0x41U, 0x6cU, 0x2fU, 0x70U, 0x4dU, 0xf2U,
        0x0fU, 0x6fU, 0x14U, 0xeaU, 0x5fU, 0x31U, 0xc9U, 0x35U,
        0xd6U, 0xf6U, 0xd5U, 0xd4U, 0xcbU, 0xf2U, 0x67U, 0x25U
    };
    uint8_t verifier[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    uint8_t again[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    uint8_t zero_salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];

    zero_bytes(verifier, sizeof(verifier));
    CHECK(appliance_admin_credential_derive(
              "password", 8U, salt, 10000U, verifier) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "PBKDF2-HMAC-SHA256 derives");
    CHECK(memcmp(verifier, expected_10000, sizeof(verifier)) == 0,
          "the 10,000-iteration vector matches the independent computation");
    zero_bytes(again, sizeof(again));
    CHECK(appliance_admin_credential_derive(
              "password", 8U, salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT, again) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the default iteration count derives");
    CHECK(memcmp(again, expected_200000, sizeof(again)) == 0,
          "the 200,000-iteration default vector matches");
    CHECK(memcmp(verifier, again, sizeof(verifier)) != 0,
          "the iteration count changes the verifier");

    zero_bytes(zero_salt, sizeof(zero_salt));
    fill_bytes(again, sizeof(again), 0xcdU);
    CHECK(appliance_admin_credential_derive(
              "password", 8U, zero_salt, 10000U, again) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_SALT,
          "an all-zero salt is refused");
    CHECK(again[0] == 0U && again[31] == 0U &&
          again[7] == 0U && again[16] == 0U,
          "a refused derivation zeroes a pre-filled verifier");
    fill_bytes(again, sizeof(again), 0xcdU);
    CHECK(appliance_admin_credential_derive(
              "password", 8U, salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN - 1U, again) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS &&
          again[0] == 0U && again[31] == 0U,
          "an iteration count below the floor is refused and zeroes the verifier");
    fill_bytes(again, sizeof(again), 0xcdU);
    CHECK(appliance_admin_credential_derive(
              "password", 8U, salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MAX + 1U, again) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS &&
          again[0] == 0U && again[31] == 0U,
          "an iteration count above the ceiling is refused");
    fill_bytes(again, sizeof(again), 0xcdU);
    CHECK(appliance_admin_credential_derive(
              "", 0U, salt, 10000U, again) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD &&
          again[0] == 0U && again[31] == 0U,
          "an empty password is refused and zeroes the verifier");
}

/*
 * CI-SPEEDUP-4: the reference vectors across the HMAC block boundary.
 *
 * The optimized derivation normalises the key once and copies the prepared pad
 * states instead of hashing them per round, and a key longer than one block is
 * hashed once instead of on every round.  The four lengths below straddle that
 * boundary exactly (63 / 64 / 65) and go well past it (128); their expected
 * verifiers were computed independently with
 * `python3 -c 'hashlib.pbkdf2_hmac("sha256", ...)'`, not with this module.
 */
static void test_reference_across_the_block_boundary(void)
{
    static const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U
    };
    static const struct {
        size_t length;
        uint8_t expected[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    } vectors[] = {
        { 63U, {
            0x73U, 0xb7U, 0xa8U, 0x56U, 0xabU, 0x5cU, 0x51U, 0xbbU,
            0x2cU, 0x98U, 0xd1U, 0x2cU, 0xaaU, 0x2aU, 0x92U, 0x8aU,
            0x18U, 0x17U, 0x45U, 0x8dU, 0x46U, 0x91U, 0x88U, 0xa5U,
            0xddU, 0xf1U, 0x50U, 0x57U, 0xbbU, 0xabU, 0x8fU, 0x8fU
        } },
        { 64U, {
            0xb1U, 0x8eU, 0x59U, 0xc2U, 0xdaU, 0xafU, 0xb5U, 0x83U,
            0x31U, 0x3cU, 0x58U, 0x1aU, 0x6aU, 0x02U, 0x08U, 0x35U,
            0xc9U, 0xd6U, 0xd1U, 0x8dU, 0x95U, 0xa7U, 0xa1U, 0x6aU,
            0xf8U, 0xefU, 0x3cU, 0x94U, 0x37U, 0xa0U, 0x48U, 0xd1U
        } },
        { 65U, {
            0x36U, 0x4dU, 0x01U, 0xf0U, 0xacU, 0xa1U, 0x92U, 0x51U,
            0x24U, 0x6eU, 0xacU, 0xbfU, 0x20U, 0x81U, 0xc3U, 0x36U,
            0x18U, 0x2eU, 0xceU, 0x11U, 0xeaU, 0x12U, 0x12U, 0x1cU,
            0x8bU, 0x78U, 0xf3U, 0x28U, 0xb5U, 0xd1U, 0xffU, 0x51U
        } },
        { 128U, {
            0xb8U, 0x0eU, 0x73U, 0xa9U, 0xefU, 0xf5U, 0xcbU, 0x6fU,
            0x66U, 0x76U, 0xe6U, 0x69U, 0x7cU, 0xe4U, 0x2fU, 0xa2U,
            0xb9U, 0x35U, 0xf4U, 0x86U, 0x8dU, 0xf3U, 0x58U, 0x73U,
            0x0aU, 0xe0U, 0x05U, 0xe7U, 0x1dU, 0x34U, 0xafU, 0x13U
        } }
    };
    uint8_t password[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX];
    uint8_t verifier[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    size_t index;

    memset(password, 'p', sizeof(password));
    for (index = 0U; index < sizeof(vectors) / sizeof(vectors[0]); ++index) {
        char label[128];

        zero_bytes(verifier, sizeof(verifier));
        CHECK(appliance_admin_credential_derive(
                  (const char *)password, vectors[index].length, salt,
                  10000U, verifier) == APPLIANCE_ADMIN_CREDENTIAL_OK &&
              memcmp(verifier, vectors[index].expected,
                     sizeof(verifier)) == 0,
              "the PBKDF2 verifier matches the independent reference across "
              "the HMAC block boundary");
        if (memcmp(verifier, vectors[index].expected, sizeof(verifier)) != 0) {
            snprintf(label, sizeof(label),
                     "password length %zu differs from the reference",
                     vectors[index].length);
            CHECK(false, label);
        }
    }
}

/*
 * CI-SPEEDUP-4: a record written before the optimization still verifies.
 *
 * The value below was built from the independently computed verifier, exactly
 * as an installed appliance's record is, and it has to keep verifying through
 * the optimized derivation - same password accepted, wrong password and wrong
 * login refused with the same derivation cost.
 */
static void test_saved_record_still_verifies(void)
{
    static const char value[] =
        "pbkdf2-sha256$10000$"
        "0102030405060708090a0b0c0d0e0f10$"
        "88eff3a31216dbc4f7252a73abfd0727473d669b5ec9bbdcb042ab84ee8ccf22$"
        "operator";
    static const char password[] = "correct horse battery staple";
    struct appliance_admin_credential decoded;

    zero_bytes(&decoded, sizeof(decoded));
    CHECK(appliance_admin_credential_record_decode(
              value, strlen(value), &decoded) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the saved record decodes");
    CHECK(appliance_admin_credential_verify(
              &decoded, "operator", 8U, password, strlen(password)) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the saved record still verifies its password");
    CHECK(appliance_admin_credential_verify(
              &decoded, "operator", 8U, "correct horse battery stapl",
              strlen("correct horse battery stapl")) ==
              APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "a wrong password against the saved record is refused");
    CHECK(appliance_admin_credential_verify(
              &decoded, "operators", 9U, password, strlen(password)) ==
              APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "a wrong login against the saved record is refused, after the same "
          "derivation");
}

/*
 * CI-SPEEDUP-4: the work per derivation is named, not assumed.
 *
 * PBKDF2 spends two HMAC calls per round and each call is one inner plus one
 * outer compression once the pads are prepared, so a derivation costs two
 * compressions per round plus the two prepared pads - and one hashed key when
 * the password exceeds one block.  The bound below is the *old* shape (four per
 * round), so a regression that re-hashes the pads per round fails here even
 * before the exact count is read.
 */
static void test_derivation_compression_budget(void)
{
    static const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH] = {
        1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U
    };
    static const uint32_t iterations = 10000U;
    uint8_t password[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX];
    uint8_t verifier[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    unsigned long long compressions;

    memset(password, 'p', sizeof(password));

    sha256_host_test_reset_compressions();
    CHECK(appliance_admin_credential_derive(
              (const char *)password, 12U, salt, iterations, verifier) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "a short-password derivation runs");
    compressions = sha256_host_test_compressions();
    CHECK(compressions == 2ULL + 2ULL * iterations,
          "one derivation costs two compressions per round and two pads");
    CHECK(compressions < 4ULL * iterations,
          "a derivation no longer pays four compressions per round");

    sha256_host_test_reset_compressions();
    CHECK(appliance_admin_credential_derive(
              (const char *)password,
              APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX, salt, iterations,
              verifier) == APPLIANCE_ADMIN_CREDENTIAL_OK,
          "a 128-byte-password derivation runs");
    compressions = sha256_host_test_compressions();
    CHECK(compressions == 5ULL + 2ULL * iterations,
          "a key longer than one block is hashed once per derivation, "
          "not once per round");
}

static void test_create_and_verify(void)
{
    static const char password[] = "correct horse battery staple";
    struct appliance_admin_credential credential;
    struct appliance_admin_credential refused;
    uint8_t expected[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    static const uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH] = {
        1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U,
        15U, 16U
    };

    zero_bytes(&credential, sizeof(credential));
    CHECK(appliance_admin_credential_create(
              "operator", 8U, password, strlen(password), salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN, &credential) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "a credential is created from a valid login and passphrase");
    CHECK(strcmp(credential.login, "operator") == 0,
          "the created credential carries the login");
    CHECK(credential.iterations ==
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN,
          "the created credential carries the iteration count");
    CHECK(memcmp(credential.salt, salt,
                 APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH) == 0,
          "the created credential carries the salt");
    zero_bytes(expected, sizeof(expected));
    CHECK(appliance_admin_credential_derive(
              password, strlen(password), salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN, expected) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK &&
          memcmp(credential.verifier, expected, sizeof(expected)) == 0,
          "the stored verifier is the PBKDF2 output, not the password");
    CHECK(contains_bytes(&credential, sizeof(credential), password) == false,
          "the credential structure never holds the plaintext password");

    CHECK(appliance_admin_credential_verify(
              &credential, "operator", 8U, password, strlen(password)) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the right login and password verify");
    CHECK(appliance_admin_credential_verify(
              &credential, "operator", 8U, "wrong password!", 15U) ==
              APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "a wrong password is denied");
    CHECK(appliance_admin_credential_verify(
              &credential, "Operator", 8U, password, strlen(password)) ==
              APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "the login comparison is case-sensitive");
    CHECK(appliance_admin_credential_verify(
              &credential, "someoneelse", 11U, password,
              strlen(password)) == APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "a wrong login is denied");
    CHECK(appliance_admin_credential_verify(
              &credential, "operator", 8U, "short", 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "a too-short presented password is a denial, not a shape error");

    /* Creation refuses policy violations and leaves nothing behind. */
    zero_bytes(&refused, sizeof(refused));
    CHECK(appliance_admin_credential_create(
              "operator", 8U, "short", 5U, salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN, &refused) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_PASSWORD,
          "creation refuses a short password");
    CHECK(appliance_admin_credential_create(
              "operator", 8U, "password", 8U, salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN, &refused) ==
              APPLIANCE_ADMIN_CREDENTIAL_WEAK_PASSWORD,
          "creation refuses a weak password");
    CHECK(appliance_admin_credential_create(
              "bad login", 9U, password, strlen(password), salt,
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN, &refused) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN,
          "creation refuses a bad login");
    CHECK(refused.login[0] == '\0' && refused.iterations == 0U &&
          refused.verifier[0] == 0U && refused.verifier[31] == 0U,
          "a refused creation leaves the output zeroed");
}

static void test_record_codec(void)
{
    static const char password[] = "correct horse battery staple";
    struct appliance_admin_credential credential;
    struct appliance_admin_credential decoded;
    char value[APPLIANCE_ADMIN_CREDENTIAL_RECORD_VALUE_MAX + 1U];
    char small[8U];
    size_t value_length = 0U;
    size_t canonical_length;

    CHECK(make_credential("operator", password, &credential),
          "the codec fixture credential builds");
    zero_bytes(value, sizeof(value));
    CHECK(appliance_admin_credential_record_encode(
              &credential, value, sizeof(value), &value_length) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the credential encodes");
    CHECK(strncmp(value, "pbkdf2-sha256$10000$",
                  strlen("pbkdf2-sha256$10000$")) == 0,
          "the canonical value names the KDF and the iteration count");
    canonical_length = value_length;
    CHECK(strlen(value) == value_length,
          "the encoded length is the value length");
    CHECK(strstr(value, "operator") != NULL,
          "the canonical value carries the login");
    CHECK(strstr(value, password) == NULL,
          "the canonical value never carries the password");
    CHECK(value[value_length] == '\0',
          "the encoded value is NUL-terminated");

    zero_bytes(&decoded, sizeof(decoded));
    CHECK(appliance_admin_credential_record_decode(
              value, value_length, &decoded) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the encoded value decodes");
    CHECK(credentials_equal(&credential, &decoded),
          "the decode round trip is exact");
    CHECK(appliance_admin_credential_verify(
              &decoded, "operator", 8U, password, strlen(password)) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the decoded credential verifies the password");

    zero_bytes(&decoded, sizeof(decoded));
    CHECK(appliance_admin_credential_record_encode(
              &credential, small, sizeof(small), &value_length) ==
              APPLIANCE_ADMIN_CREDENTIAL_BUFFER_TOO_SMALL,
          "a short buffer is refused instead of truncated");
    CHECK(value_length == 0U &&
          appliance_admin_credential_record_decode(
              "pbkdf2-sha256$10000$", strlen("pbkdf2-sha256$10000$"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD,
          "a truncated value is refused");
    CHECK(appliance_admin_credential_record_decode(
              "scrypt$10000$000102030405060708090a0b0c0d0e0f$"
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
              "operator",
              strlen("scrypt$10000$000102030405060708090a0b0c0d0e0f$"
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
                     "operator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD,
          "a foreign KDF label is refused");
    CHECK(appliance_admin_credential_record_decode(
              "pbkdf2-sha256$09999$000102030405060708090a0b0c0d0e0f$"
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
              "operator",
              strlen("pbkdf2-sha256$09999$000102030405060708090a0b0c0d0e0f$"
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
                     "operator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD,
          "a non-canonical iteration field is refused");
    CHECK(appliance_admin_credential_record_decode(
              "pbkdf2-sha256$9999$000102030405060708090a0b0c0d0e0f$"
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
              "operator",
              strlen("pbkdf2-sha256$9999$000102030405060708090a0b0c0d0e0f$"
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
                     "operator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS,
          "an iteration count below the floor is refused by name");
    /*
     * A ten-digit field that wraps uint32_t must not land back inside the
     * allowed range: 4295067296 is 2^32 + 100000, and a wrapping parse would
     * accept it as 100000.
     */
    CHECK(appliance_admin_credential_record_decode(
              "pbkdf2-sha256$4295067296$000102030405060708090a0b0c0d0e0f$"
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
              "operator",
              strlen("pbkdf2-sha256$4295067296$000102030405060708090a0b0c0d0e0f$"
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
                     "operator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD,
          "an iteration field that would wrap uint32_t is refused");
    CHECK(appliance_admin_credential_record_decode(
              "pbkdf2-sha256$99999999999$000102030405060708090a0b0c0d0e0f$"
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
              "operator",
              strlen("pbkdf2-sha256$99999999999$000102030405060708090a0b0c0d0e0f$"
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
                     "operator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD,
          "an eleven-digit iteration field is refused");
    CHECK(appliance_admin_credential_record_decode(
              "pbkdf2-sha256$4000000000$000102030405060708090a0b0c0d0e0f$"
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
              "operator",
              strlen("pbkdf2-sha256$4000000000$000102030405060708090a0b0c0d0e0f$"
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
                     "operator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS,
          "a ten-digit in-range uint32 count above the ceiling is refused");
    CHECK(appliance_admin_credential_record_decode(
              "pbkdf2-sha256$10000$000102030405060708090A0B0C0D0E0F$"
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
              "operator",
              strlen("pbkdf2-sha256$10000$000102030405060708090A0B0C0D0E0F$"
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
                     "operator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_RECORD,
          "uppercase hex is refused instead of normalised");
    CHECK(appliance_admin_credential_record_decode(
              "pbkdf2-sha256$10000$00000000000000000000000000000000$"
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
              "operator",
              strlen("pbkdf2-sha256$10000$00000000000000000000000000000000$"
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
                     "operator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_SALT,
          "an all-zero salt is refused");
    CHECK(appliance_admin_credential_record_decode(
              "pbkdf2-sha256$10000$000102030405060708090a0b0c0d0e0f$"
              "0000000000000000000000000000000000000000000000000000000000000000$"
              "operator",
              strlen("pbkdf2-sha256$10000$000102030405060708090a0b0c0d0e0f$"
                     "0000000000000000000000000000000000000000000000000000000000000000$"
                     "operator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_VERIFIER,
          "an all-zero verifier is refused");
    CHECK(decoded.login[0] == '\0' && decoded.salt[0] == 0U &&
          decoded.verifier[0] == 0U && decoded.iterations == 0U,
          "a refused decode leaves no partially filled credential behind");
    CHECK(appliance_admin_credential_record_decode(
              "pbkdf2-sha256$10000$000102030405060708090a0b0c0d0e0f$"
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
              "op$erator",
              strlen("pbkdf2-sha256$10000$000102030405060708090a0b0c0d0e0f$"
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f$"
                     "op$erator"),
              &decoded) == APPLIANCE_ADMIN_CREDENTIAL_BAD_LOGIN,
          "a separator inside the login is refused");

    CHECK(appliance_admin_credential_record_is_legacy(
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
              64U),
          "the legacy 64-hex fingerprint is recognised");
    CHECK(appliance_admin_credential_record_is_legacy(
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde",
              63U) == false,
          "a 63-character value is not a legacy fingerprint");
    CHECK(appliance_admin_credential_record_is_legacy(
              "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
              64U) == false,
          "the legacy shape is the lower-case hex the old writer produced");
    zero_bytes(&decoded, sizeof(decoded));
    CHECK(appliance_admin_credential_record_decode(
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
              64U, &decoded) == APPLIANCE_ADMIN_CREDENTIAL_LEGACY_RECORD,
          "decoding a legacy value refuses it as a legacy record");
    CHECK(decoded.login[0] == '\0' && decoded.iterations == 0U,
          "a refused legacy decode leaves the output zeroed");

    CHECK(appliance_admin_credential_record_value_valid(
              (const uint8_t *)value, canonical_length),
          "the catalogue shape check accepts the structured value");
    CHECK(appliance_admin_credential_record_value_valid(
              (const uint8_t *)
                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
              64U),
          "the catalogue shape check accepts the legacy value");
    CHECK(appliance_admin_credential_record_value_valid(
              (const uint8_t *)"pbkdf2-sha256$10000$binary\x01", 27U) == false,
          "the catalogue shape check refuses a non-printable value");
}

static void test_store_round_trip_and_no_plaintext(void)
{
    static const char password[] = "correct horse battery staple";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_admin_credential credential;
    struct appliance_admin_credential read_back;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device),
          "an empty secret store loads");
    CHECK(appliance_persistent_secret_config_admin_kind(&secrets) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_NONE,
          "an empty store has no administrator record");
    zero_bytes(&read_back, sizeof(read_back));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &read_back) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_ABSENT,
          "an empty store reports the credential absent");
    CHECK(appliance_persistent_secret_config_admin(&secrets, NULL) == false,
          "an empty store has no legacy fingerprint either");

    CHECK(make_credential("operator", password, &credential),
          "the store fixture credential builds");
    /*
     * A caller's structure may carry junk after the login's terminator; the
     * committed head must still match what a fresh load produces.
     */
    fill_bytes(credential.login + strlen(credential.login) + 1U,
               sizeof(credential.login) - strlen(credential.login) - 1U,
               0x41U);
    CHECK(commit_credential(&secrets, &credential),
          "the credential commits to the store");
    CHECK(secrets.admin_credential_present &&
          secrets.admin_kind ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_PBKDF2_CREDENTIAL,
          "the committed head is the structured credential");
    CHECK(secrets.admin_credential.login[8] == '\0' &&
          secrets.admin_credential.login[9] == '\0' &&
          secrets.admin_credential.login[32] == '\0',
          "the committed head zeroes the login padding the caller carried");
    {
        struct appliance_admin_credential in_memory =
            secrets.admin_credential;

        CHECK(!secrets.admin_present,
              "a structured credential is not reported as a legacy fingerprint");
        CHECK(!contains_bytes(fixture.storage, sizeof(fixture.storage),
                              password),
              "the raw store never contains the plaintext password");
        CHECK(contains_bytes(fixture.storage, sizeof(fixture.storage),
                             "pbkdf2-sha256$"),
              "the raw store carries the canonical PBKDF2 value");

        CHECK(load_secret_store(&secrets, &device),
              "the store reloads after the credential commit");
        zero_bytes(&read_back, sizeof(read_back));
        CHECK(appliance_persistent_secret_config_admin_credential(
                  &secrets, &read_back) ==
                  APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
              "the structured credential reads back");
        CHECK(memcmp(&in_memory, &read_back, sizeof(in_memory)) == 0,
              "the committed head matches the reloaded credential byte for byte");
        CHECK(credentials_equal(&credential, &read_back),
              "the read-back credential is exact, salt and iterations included");
    }
    CHECK(appliance_persistent_secret_config_source(&secrets) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_COMMITTED,
          "the reloaded store reports a committed record");
    CHECK(read_back.iterations == APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN &&
          read_back.salt[0] == 0x10U && read_back.salt[15] == 0xefU,
          "the read-back record names its own salt and iteration count");
    CHECK(appliance_admin_credential_verify(
              &read_back, "operator", 8U, password, strlen(password)) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the read-back credential verifies the right password");
    CHECK(appliance_admin_credential_verify(
              &read_back, "operator", 8U, "wrong password!", 15U) ==
              APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "the read-back credential refuses a wrong password");
    CHECK(!contains_bytes(fixture.storage, sizeof(fixture.storage), password),
          "the reloaded raw store still never contains the password");
}

static void test_legacy_record_is_refused_and_migrated(void)
{
    static const char password[] = "correct horse battery staple";
    static const uint8_t legacy_fingerprint[32U] = {
        0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU,
        0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU,
        0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU,
        0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU
    };
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_admin_credential credential;
    struct appliance_admin_credential read_back;
    uint8_t stored[32U];

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device),
          "the legacy fixture store loads");
    CHECK(appliance_persistent_secret_config_commit_admin(
              &secrets, legacy_fingerprint) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the legacy fingerprint commits through the old writer");
    CHECK(secrets.admin_kind ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_LEGACY_FINGERPRINT &&
          secrets.admin_present && !secrets.admin_credential_present,
          "the head is the legacy fingerprint and not a credential");
    zero_bytes(stored, sizeof(stored));
    CHECK(appliance_persistent_secret_config_admin(&secrets, stored) &&
          memcmp(stored, legacy_fingerprint, sizeof(stored)) == 0,
          "the legacy fingerprint still reads through the old reader");

    CHECK(load_secret_store(&secrets, &device),
          "the legacy store reloads");
    CHECK(appliance_persistent_secret_config_admin_kind(&secrets) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_LEGACY_FINGERPRINT,
          "the reloaded head is the legacy fingerprint");
    zero_bytes(&read_back, sizeof(read_back));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &read_back) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD,
          "the legacy record is refused as a credential by name");
    CHECK(read_back.login[0] == '\0' && read_back.verifier[0] == 0U,
          "a refused legacy read leaves the output zeroed");

    /* Migration is an explicit re-provisioning, which appends a new head. */
    CHECK(make_credential("operator", password, &credential),
          "the migration credential builds");
    CHECK(commit_credential(&secrets, &credential),
          "the credential commits over the legacy record");
    CHECK(load_secret_store(&secrets, &device),
          "the migrated store reloads");
    CHECK(appliance_persistent_secret_config_admin_kind(&secrets) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_PBKDF2_CREDENTIAL,
          "the migrated head is the structured credential");
    CHECK(appliance_persistent_secret_config_admin(&secrets, stored) == false,
          "the migrated store no longer serves the legacy fingerprint");
    zero_bytes(&read_back, sizeof(read_back));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &read_back) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK &&
          credentials_equal(&credential, &read_back),
          "the migrated credential reads back exactly");
}

static void test_change_overwrites_the_head(void)
{
    static const char password[] = "correct horse battery staple";
    static const char new_password[] = "a brand new passphrase";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_admin_credential first;
    struct appliance_admin_credential second;
    struct appliance_admin_credential read_back;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device),
          "the change fixture store loads");
    CHECK(make_credential("operator", password, &first),
          "the first credential builds");
    CHECK(commit_credential(&secrets, &first),
          "the first credential commits");
    CHECK(appliance_admin_credential_create(
              "operator2", 9U, new_password, strlen(new_password),
              first.salt, APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_MIN,
              &second) == APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the replacement credential builds with the same salt");
    CHECK(commit_credential(&secrets, &second),
          "the replacement credential commits");
    CHECK(commit_credential(&secrets, &second),
          "re-committing the same credential is a no-op success");

    CHECK(load_secret_store(&secrets, &device),
          "the changed store reloads");
    zero_bytes(&read_back, sizeof(read_back));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &read_back) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the newest head reads back");
    CHECK(strcmp(read_back.login, "operator2") == 0,
          "the newest head carries the new login");
    CHECK(appliance_admin_credential_verify(
              &read_back, "operator2", 9U, new_password,
              strlen(new_password)) == APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the new password verifies");
    CHECK(appliance_admin_credential_verify(
              &read_back, "operator2", 9U, password,
              strlen(password)) == APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "the old password is refused");
    CHECK(appliance_admin_credential_verify(
              &read_back, "operator", 8U, password, strlen(password)) ==
              APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "the old login is refused");
}

static void test_corrupt_record_fails_closed(void)
{
    static const char bad_value[] = "pbkdf2-sha256$not-a-number";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct config_journal journal;
    char record[APPLIANCE_PERSISTENT_SECRET_CONFIG_RECORD_MAX];
    size_t record_length;
    struct appliance_admin_credential read_back;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device),
          "the corruption fixture store loads");
    record_length = strlen(APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX) +
                    strlen(bad_value);
    zero_bytes(record, sizeof(record));
    memcpy(record, APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX,
           strlen(APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX));
    memcpy(record + strlen(APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX),
           bad_value, strlen(bad_value));
    CHECK(config_journal_init(
              &journal, &device, TEST_REGION_OFFSET, TEST_REGION_COUNT) ==
              CONFIG_JOURNAL_OK,
          "the corruption fixture journal opens");
    CHECK(config_journal_append(&journal, record, record_length) ==
              CONFIG_JOURNAL_OK,
          "the malformed credential record lands in the journal");
    CHECK(load_secret_store(&secrets, &device),
          "a store with a malformed record still loads");
    CHECK(appliance_persistent_secret_config_source(&secrets) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_INVALID,
          "the malformed record is reported as an invalid default");
    zero_bytes(&read_back, sizeof(read_back));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &read_back) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_ABSENT,
          "a corrupted record yields no credential, not a partial one");
    CHECK(appliance_persistent_secret_config_admin_kind(&secrets) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_NONE,
          "a corrupted record yields no administrator kind");

    /* Binary bytes are refused by the catalogue shape check too. */
    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device),
          "the binary fixture store loads");
    {
        static const unsigned char binary_value[] = {
            'p', 'b', 'k', 'd', 'f', '2', '-', 's', 'h', 'a', '2', '5',
            '6', '$', 0x01U, 0x02U, 0xffU
        };

        record_length = strlen(APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX) +
                        sizeof(binary_value);
        zero_bytes(record, sizeof(record));
        memcpy(record, APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX,
               strlen(APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX));
        memcpy(record + strlen(APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX),
               binary_value, sizeof(binary_value));
        CHECK(config_journal_init(
                  &journal, &device, TEST_REGION_OFFSET, TEST_REGION_COUNT) ==
                  CONFIG_JOURNAL_OK &&
              config_journal_append(&journal, record, record_length) ==
                  CONFIG_JOURNAL_OK,
              "the binary record lands in the journal");
    }
    CHECK(load_secret_store(&secrets, &device),
          "a store with a binary record still loads");
    CHECK(appliance_persistent_secret_config_source(&secrets) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_INVALID,
          "a binary credential value is refused fail-closed");
}

static void test_status_names(void)
{
    CHECK(strcmp(appliance_admin_credential_status_string(
                     APPLIANCE_ADMIN_CREDENTIAL_LEGACY_RECORD),
                 "APPLIANCE_ADMIN_CREDENTIAL_LEGACY_RECORD") == 0,
          "the legacy refusal has a stable name");
    CHECK(strcmp(appliance_persistent_secret_config_status_string(
                     APPLIANCE_PERSISTENT_SECRET_CONFIG_ABSENT),
                 "APPLIANCE_PERSISTENT_SECRET_CONFIG_ABSENT") == 0,
          "the absent refusal has a stable name");
    CHECK(strcmp(appliance_persistent_secret_config_status_string(
                     APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD),
                 "APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD") == 0,
          "the store legacy refusal has a stable name");
    CHECK(strcmp(appliance_secret_persistence_handling_string(
                     APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER),
                 "APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER") == 0,
          "the catalogue handling names the PBKDF2 verifier");
    CHECK(appliance_admin_credential_status_string(
              (enum appliance_admin_credential_status)999) == NULL,
          "an unknown credential status has no name");
}

static bool all_zero_bytes(const void *bytes, size_t length)
{
    const unsigned char *cursor = bytes;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (cursor[index] != 0U) {
            return false;
        }
    }
    return true;
}

/*
 * ITEM-619: the resumable derivation is the production PBKDF2 core, not a
 * copy.  Draining it in bounded slices must produce the exact verifier the
 * one-shot `derive()` produces, and every slice must advance by at most the
 * requested number of rounds so a caller can bound the time it spends.
 */
static void test_resumable_derivation_matches_drain(void)
{
    static const char password[] = "correct horse battery staple";
    uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];
    uint8_t reference[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    struct appliance_admin_credential_kdf kdf;
    enum appliance_admin_credential_status status;
    size_t index;
    uint32_t previous;
    uint32_t slices = 0U;
    bool bounded = true;

    for (index = 0U; index < sizeof(salt); ++index) {
        salt[index] = (uint8_t)(0x40U + index);
    }
    CHECK(appliance_admin_credential_derive(
              password, sizeof(password) - 1U, salt, 10000U, reference) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the reference derivation completes");
    status = appliance_admin_credential_kdf_begin(
        &kdf, password, sizeof(password) - 1U, salt, 10000U);
    CHECK(status == APPLIANCE_ADMIN_CREDENTIAL_PENDING,
          "the resumable derivation starts pending");
    CHECK(appliance_admin_credential_kdf_pending(&kdf),
          "the job reports itself pending");
    previous = appliance_admin_credential_kdf_iterations_done(&kdf);
    CHECK(previous == 1U, "the start has spent exactly round 1");
    while (status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        status = appliance_admin_credential_kdf_step(&kdf, 7U);
        ++slices;
        if (status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
            const uint32_t done =
                appliance_admin_credential_kdf_iterations_done(&kdf);

            if (!(done > previous && done - previous <= 7U) ||
                done >= 10000U) {
                bounded = false;
            }
        }
        previous = appliance_admin_credential_kdf_iterations_done(&kdf);
    }
    CHECK(status == APPLIANCE_ADMIN_CREDENTIAL_OK,
          "draining the job finishes the derivation");
    CHECK(slices > 1U, "the derivation really took several slices");
    CHECK(bounded,
          "every slice advanced by at most the requested rounds");
    CHECK(appliance_admin_credential_kdf_iterations_done(&kdf) == 10000U,
          "the job spent every iteration, not fewer");
    CHECK(memcmp(reference, kdf.verifier, sizeof(reference)) == 0,
          "the sliced verifier equals the one-shot verifier");
    CHECK(all_zero_bytes(kdf.u, sizeof(kdf.u)) &&
              all_zero_bytes(kdf.t, sizeof(kdf.t)) &&
              all_zero_bytes(kdf.block_input, sizeof(kdf.block_input)) &&
              !kdf.prepared.ready,
          "completion zeroes the secret-derived intermediate state");
    appliance_admin_credential_kdf_cancel(&kdf);
}

/*
 * ITEM-619: the complete check as a job.  Correct credentials finish with OK,
 * a wrong password and a wrong login both finish with DENIED after the same
 * full derivation, and the answer is only decided once the job is drained.
 */
static void test_resumable_check_matches_verify(void)
{
    uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];
    struct appliance_admin_credential stored;
    struct appliance_admin_credential_check check;
    enum appliance_admin_credential_status status;
    size_t index;

    for (index = 0U; index < sizeof(salt); ++index) {
        salt[index] = (uint8_t)(0xA0U + index);
    }
    CHECK(appliance_admin_credential_create(
              "owner", 5U, "twelve chars plus more", 22U, salt, 10000U,
              &stored) == APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the job fixture credential is created");
    CHECK(appliance_admin_credential_verify(
              &stored, "owner", 5U, "twelve chars plus more", 22U) ==
              APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the synchronous verifier still accepts the pair");
    status = appliance_admin_credential_check_begin(
        &check, &stored, "owner", 5U, "twelve chars plus more", 22U);
    CHECK(status == APPLIANCE_ADMIN_CREDENTIAL_PENDING,
          "the check starts pending and does not decide early");
    CHECK(appliance_admin_credential_check_pending(&check),
          "the check reports itself pending");
    while (status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        status = appliance_admin_credential_check_step(&check, 13U);
    }
    CHECK(status == APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the drained check accepts the correct pair");
    CHECK(appliance_admin_credential_check_iterations_done(&check) == 10000U,
          "the accepted check spent every iteration");
    CHECK(all_zero_bytes(
              check.stored_verifier, sizeof(check.stored_verifier)),
          "the accepted check drops the stored verifier");
    appliance_admin_credential_check_cancel(&check);

    status = appliance_admin_credential_check_begin(
        &check, &stored, "owner", 5U, "wrong password value", 20U);
    while (status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        status = appliance_admin_credential_check_step(&check, 100U);
    }
    CHECK(status == APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "the drained check refuses a wrong password");
    CHECK(appliance_admin_credential_check_iterations_done(&check) == 10000U,
          "the wrong-password refusal still spent every iteration");
    appliance_admin_credential_check_cancel(&check);

    status = appliance_admin_credential_check_begin(
        &check, &stored, "Owner", 5U, "twelve chars plus more", 22U);
    while (status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        status = appliance_admin_credential_check_step(&check, 100U);
    }
    CHECK(status == APPLIANCE_ADMIN_CREDENTIAL_DENIED,
          "the drained check refuses a wrong login");
    CHECK(appliance_admin_credential_check_iterations_done(&check) == 10000U,
          "the wrong-login refusal derives as long as a wrong password");
    appliance_admin_credential_check_cancel(&check);
}

/*
 * ITEM-619: an abandoned job must not keep the submitted secret or any
 * derived state.  This is the disconnect/deadline path.
 */
static void test_resumable_cancel_zeroizes(void)
{
    uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];
    struct appliance_admin_credential stored;
    struct appliance_admin_credential_check check;
    struct appliance_admin_credential_kdf kdf;
    enum appliance_admin_credential_status status;
    size_t index;

    for (index = 0U; index < sizeof(salt); ++index) {
        salt[index] = (uint8_t)(0x11U + index);
    }
    CHECK(appliance_admin_credential_create(
              "owner", 5U, "another long passphrase", 23U, salt, 10000U,
              &stored) == APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the cancel fixture credential is created");
    status = appliance_admin_credential_check_begin(
        &check, &stored, "owner", 5U, "another long passphrase", 23U);
    CHECK(status == APPLIANCE_ADMIN_CREDENTIAL_PENDING,
          "the cancel fixture starts pending");
    (void)appliance_admin_credential_check_step(&check, 5U);
    appliance_admin_credential_check_cancel(&check);
    CHECK(!appliance_admin_credential_check_pending(&check),
          "a cancelled check is no longer pending");
    CHECK(check.result == APPLIANCE_ADMIN_CREDENTIAL_CANCELLED,
          "a cancelled check has the stable cancelled result");
    CHECK(appliance_admin_credential_check_iterations_done(&check) == 0U,
          "a cancelled check reports no rounds kept");
    CHECK(appliance_admin_credential_check_step(&check, 5U) ==
              APPLIANCE_ADMIN_CREDENTIAL_CANCELLED,
          "a cancelled check cannot be resumed");
    CHECK(appliance_admin_credential_check_begin(
              NULL, &stored, "owner", 5U, "another long passphrase", 23U) ==
              APPLIANCE_ADMIN_CREDENTIAL_NULL,
          "a null check job is refused");
    CHECK(appliance_admin_credential_kdf_begin(
              &kdf, "another long passphrase", 23U, salt, 10U) ==
              APPLIANCE_ADMIN_CREDENTIAL_BAD_ITERATIONS,
          "the resumable start keeps the iteration floor");
    CHECK(!kdf.started, "a refused start keeps no job state");
}


/*
 * ITEM-650: the serving poll used one fixed 64-round slice per 100 Hz wake.
 * 200000 iterations therefore needed 3125 wakes before the credential answer
 * could even be issued, while the derivation itself cost a fraction of that
 * wall time.  The next slice is now chosen from the measured cost of the
 * previous one, so a fast machine spends a bounded share of a turn deriving
 * and a slow machine keeps the proven minimum instead of overrunning the
 * liveness bound.
 *
 * The model below is arithmetic, not a clock: it feeds the same 7 ms per 64
 * rounds the installed LOGIN-TRACE measured and requires the 200000-round
 * job to be reachable in a bounded number of turns.  It does not shorten a
 * stored iteration count.
 */
static void test_adaptive_slice_budget(void)
{
    uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];
    struct appliance_admin_credential stored;
    struct appliance_admin_credential_check check;
    enum appliance_admin_credential_status status;
    uint32_t slice;
    uint32_t passes = 0U;
    uint32_t done;
    size_t index;

    CHECK(appliance_admin_credential_check_next_slice(0U, 0U) ==
              APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT,
          "an unmeasured slice starts at the proven minimum");
    CHECK(appliance_admin_credential_check_next_slice(64U, 0U) ==
              APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT,
          "an unmeasured elapsed time keeps the minimum");
    CHECK(appliance_admin_credential_check_next_slice(64U, 7000U) == 365U,
          "64 rounds measured at 7 ms grow toward a 40 ms slice");
    CHECK(appliance_admin_credential_check_next_slice(64U, 1U) ==
              APPLIANCE_ADMIN_CREDENTIAL_CHECK_SLICE_MAX_ROUNDS,
          "a very fast slice is clipped at the maximum");
    CHECK(appliance_admin_credential_check_next_slice(64U, 400000U) ==
              APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT,
          "a very slow slice is clipped at the minimum");
    CHECK(appliance_admin_credential_check_next_slice(4096U, 0U) ==
              APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT,
          "a completed maximum slice does not leak into the next job");
    CHECK(appliance_admin_credential_check_next_slice(4096U, 40000U) ==
              4096U, "a maximum slice at the target stays at the maximum");
    CHECK(appliance_admin_credential_check_next_slice(4096U, 0xFFFFFFFFU) ==
              APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT,
          "a maximum elapsed reading cannot underflow the slice");

    for (index = 0U; index < sizeof(salt); ++index) {
        salt[index] = (uint8_t)(0x60U + index);
    }
    CHECK(appliance_admin_credential_create(
              "owner", 5U, "adaptive slice passphrase", 24U, salt, 10000U,
              &stored) == APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the adaptive fixture credential is created");
    status = appliance_admin_credential_check_begin(
        &check, &stored, "owner", 5U, "adaptive slice passphrase", 24U);
    CHECK(status == APPLIANCE_ADMIN_CREDENTIAL_PENDING,
          "the adaptive fixture starts pending");
    slice = appliance_admin_credential_check_next_slice(0U, 0U);
    while (status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
        const uint32_t before =
            appliance_admin_credential_check_iterations_done(&check);
        uint64_t cost_us;

        status = appliance_admin_credential_check_step(&check, slice);
        done = appliance_admin_credential_check_iterations_done(&check);
        cost_us = ((uint64_t)(done - before) * 7000U) / 64U;
        slice = appliance_admin_credential_check_next_slice(
            done - before, (uint32_t)cost_us);
        ++passes;
        if (passes > 128U) {
            break;
        }
    }
    CHECK(status == APPLIANCE_ADMIN_CREDENTIAL_OK,
          "the adaptive drain accepts the correct pair");
    CHECK(passes <= 32U,
          "a measured 7 ms per 64 rounds drains 10000 rounds in <=32 turns");
    CHECK(appliance_admin_credential_check_iterations_done(&check) == 10000U,
          "the adaptive drain still spends every stored iteration");
    CHECK(slice >= APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT,
          "the chosen slice stays above the proven minimum");
    appliance_admin_credential_check_cancel(&check);
}

int main(void)
{
    test_policy_constants();
    test_login_policy();
    test_password_policy();
    test_pbkdf2_vectors();
    test_reference_across_the_block_boundary();
    test_saved_record_still_verifies();
    test_derivation_compression_budget();
    test_resumable_derivation_matches_drain();
    test_resumable_check_matches_verify();
    test_resumable_cancel_zeroizes();
    test_adaptive_slice_budget();
    test_create_and_verify();
    test_record_codec();
    test_store_round_trip_and_no_plaintext();
    test_legacy_record_is_refused_and_migrated();
    test_change_overwrites_the_head();
    test_corrupt_record_fails_closed();
    test_status_names();
    if (failures != 0) {
        fprintf(stderr, "appliance admin credential host test: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance admin credential host test: ALL OK\n");
    return 0;
}

#endif
