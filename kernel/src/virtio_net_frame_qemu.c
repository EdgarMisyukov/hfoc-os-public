#ifdef HFOC_VIRTIO_NET_FRAME_QEMU_TEST

#include "arch/x86_64/io.h"
#include "network_device.h"
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
#include "ethernet_frame.h"
#include "hfoc_netif_input.h"
#include "hfoc_netif_output.h"
#include "ipv4_config.h"
#include "network_ipv4_profile.h"
#include "packet_pool.h"
#include "packet_pressure.h"
#include "packet_slice.h"
#include "virtio_net_mac_identity.h"
#endif
#include "pmm.h"
#include "serial.h"
#include "virtio_available_publication.h"
#include "virtio_descriptor_ownership.h"
#include "virtio_dma_region.h"
#include "virtio_net_feature_profile.h"
#include "virtio_net_interrupt.h"
#include "virtio_net_lifecycle.h"
#include "virtio_net_link_state.h"
#include "virtio_net_queue_composition.h"
#include "virtio_net_rx.h"
#include "virtio_net_tx.h"
#include "virtio_pci_capability.h"
#include "virtio_pci_device_status.h"
#include "virtio_pci_feature.h"
#include "virtio_pci_identity.h"
#include "virtio_queue_layout.h"
#include "virtio_used_consumption.h"
#include "vmm.h"

#ifdef HFOC_TCP_LISTENER_QEMU_TEST
#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp.h"
#ifdef HFOC_MBEDTLS_TLS_QEMU_TEST
#include "hfoc_mbedtls_alloc.h"
#include "hfoc_mbedtls_test_secret.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "psa/crypto_extra.h"
#include <stdarg.h>
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
#include "appliance_management_api.h"
#include "appliance_management_panel.h"
#include "appliance_management_reachability.h"
#include "appliance_network_api.h"
#include "appliance_service_api.h"
#include "appliance_health.h"
#include "appliance_health_api.h"
#include "appliance_system_api.h"
#include "audit.h"
#include "event_ring.h"
#include "http_error_matrix.h"
#include "http_parser.h"
#include "http_response.h"
#include "https_handler.h"
#include "https_health.h"
#include "https_peer_throttle.h"
#include "https_service_lifecycle.h"
#include "packet_pool.h"
#include "packet_pressure.h"
#ifdef HFOC_HTTPS_ENTROPY_FAIL_CLOSED_QEMU_TEST
#include "entropy_accumulator.h"
#include "entropy_diagnostics.h"
#include "entropy_health.h"
#include "entropy_restart.h"
#include "entropy_seed_fill.h"
#endif
#endif
#endif
#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
#include "hfoc_lwip_tcp_backlog.h"
#include "hfoc_lwip_tcp_close_policy.h"
#include "hfoc_lwip_tcp_deadline.h"
#include "hfoc_lwip_tcp_drain_restart.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"
#include "packet_pool.h"
#include "packet_pressure.h"
#endif
#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_time.h"
#include "ipv4_config.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
#include "lwip/priv/tcp_priv.h"
#endif
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST
void appliance_management_plane_qemu_test_init(void);
void appliance_management_reachability_qemu_test_init(void);
#endif

#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
#define VNF_FRAME_COUNT 9U
#define VNF_DIRECT_FRAME_COUNT 7U
#define VNF_PACKET_FRAME_COUNT \
    (VNF_FRAME_COUNT - VNF_DIRECT_FRAME_COUNT)
#define VNF_PACKET_POOL_COUNT 16U
#define VNF_PACKET_CAPACITY 128U
#define VNF_PACKET_HEADROOM 16U
#define VNF_PACKET_LENGTH 64U
#define VNF_PACKET_STORAGE_SIZE \
    (VNF_PACKET_POOL_COUNT * \
     (VNF_PACKET_CAPACITY + VNF_PACKET_HEADROOM))
#define VNF_PACKET_TX_ETHER_TYPE 0x88B8U
#define VNF_PACKET_INVALID_RX_COUNT 2U
#else
#define VNF_FRAME_COUNT 7U
#define VNF_DIRECT_FRAME_COUNT 7U
#endif
#ifndef VNF_TX_FRAME_COUNT
#define VNF_TX_FRAME_COUNT VNF_FRAME_COUNT
#endif
#define VNF_FRAME_LENGTH 60U
#define VNF_PROOF_STACK_SIZE 131072U
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
#define VNF_QUEUE_SIZE 32U
#define VNF_DMA_CAPACITY 32U
#else
#define VNF_QUEUE_SIZE 16U
#define VNF_DMA_CAPACITY 16U
#endif
#define VNF_RX_BUFFER_LENGTH 1536U
#define VNF_TX_HEADER_LENGTH 12U
#define VNF_TX_HEADER_STRIDE 16U
#define VNF_TX_FRAME_LENGTH 64U
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
#define VNF_PAYLOAD_PAGES 7U
#define VNF_TX_HEADER_BASE_OFFSET \
    (VNF_FRAME_COUNT * VNF_RX_BUFFER_LENGTH)
#define VNF_TX_FRAME_BASE_OFFSET \
    (VNF_TX_HEADER_BASE_OFFSET + \
     VNF_FRAME_COUNT * VNF_TX_HEADER_STRIDE)
#else
#define VNF_PAYLOAD_PAGES 4U
#define VNF_TX_HEADER_BASE_OFFSET 10752U
#define VNF_TX_FRAME_BASE_OFFSET 10880U
#endif
#define VNF_PAYLOAD_LENGTH (VNF_PAYLOAD_PAGES * 4096ULL)
#define VNF_QUEUE_REGION_LENGTH 4096U
#define VNF_DMA_ALIGNMENT 16U
#define VNF_MAX_USED_LENGTH 2048U
#define VNF_GENERATION 220U
#define VNF_ETHER_TYPE_RX 0x88B5U
#define VNF_ETHER_TYPE_TX 0x88B6U
#ifdef HFOC_TCP_LISTENER_QEMU_TEST
#define VNF_TCP_LISTENER_PORT 443U
#define VNF_TCP_MAX_CONNECTIONS 4U
#define VNF_TCP_IDLE_TIMEOUT_MS 100U
#define VNF_TCP_RESPONSE_PREFIX "HFOC-TCP-ECHO:"
#define VNF_TCP_PAYLOAD "HFOC-TCP-PAYLOAD"
#define VNF_TCP_TX_FRAME_OFFSET 12000U
#define VNF_TCP_TX_FRAME_SIZE 2048U
#define VNF_TCP_SETTLE_MS 500U
#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
#define VNF_NETRES_RESPONSE_PREFIX "HFOC-NETRES-ECHO:"
#define VNF_NETRES_PAYLOAD "HFOC-NETRES-PAYLOAD"
#define VNF_NETRES_KEEPALIVE "HFOC-NETRES-KEEP"
#define VNF_NETRES_PACKET_COUNT 8U
#define VNF_NETRES_PACKET_CAPACITY 64U
#define VNF_NETRES_PACKET_HEADROOM 0U
#define VNF_NETRES_PACKET_LENGTH 64U
#define VNF_NETRES_PACKET_STORAGE_SIZE \
    (VNF_NETRES_PACKET_COUNT * VNF_NETRES_PACKET_CAPACITY)
#define VNF_NETRES_IDLE_TIMEOUT_MS 500U
#define VNF_NETRES_DRAIN_TIMEOUT_MS 2000U
#define VNF_NETRES_BACKLOG_SETTLE_MS 5000U
#define VNF_NETRES_SETTLE_MS 500U
#endif
#undef VNF_TX_FRAME_LENGTH
#define VNF_TX_FRAME_LENGTH 1518U
#endif

#define VNF_COMMON_DEVICE_FEATURE_SELECT 0x00U
#define VNF_COMMON_DEVICE_FEATURE 0x04U
#define VNF_COMMON_DRIVER_FEATURE_SELECT 0x08U
#define VNF_COMMON_DRIVER_FEATURE 0x0CU
#define VNF_COMMON_DEVICE_STATUS 0x14U
#define VNF_COMMON_QUEUE_SELECT 0x16U
#define VNF_COMMON_QUEUE_SIZE 0x18U
#define VNF_COMMON_QUEUE_ENABLE 0x1CU
#define VNF_COMMON_QUEUE_NOTIFY_OFF 0x1EU
#define VNF_COMMON_QUEUE_DESC 0x20U
#define VNF_COMMON_QUEUE_DRIVER 0x28U
#define VNF_COMMON_QUEUE_DEVICE 0x30U

#define VNF_PCI_CONFIG_ADDRESS_PORT 0xCF8U
#define VNF_PCI_CONFIG_DATA_PORT 0xCFCU
#define VNF_PCI_CONFIG_ENABLE UINT32_C(0x80000000)
#define VNF_PCI_BAR_COUNT 6U
#define VNF_PCI_BAR_LENGTH UINT64_C(0x10000)

struct vnf_virtq_desc {
    volatile uint64_t addr;
    volatile uint32_t len;
    volatile uint16_t flags;
    volatile uint16_t next;
};

struct vnf_queue_mem {
    uint64_t physical;
    void *virtual_address;
    struct virtio_queue_layout_result layout;
    struct virtio_available_ring *available;
    struct virtio_used_ring *used;
    volatile struct vnf_virtq_desc *descriptors;
};

struct vnf_tx_notify_state {
    uint32_t calls;
    uint32_t last_slot;
    struct vnf_queue_mem *queue;
    uint64_t header_physical_base;
    uint64_t frame_physical_base;
    uint32_t frame_length;
};

static uint64_t vnf_hhdm;
static volatile uint8_t *vnf_common_base;
static volatile uint8_t *vnf_notify_base;
static volatile uint8_t *vnf_isr_base;
static volatile uint8_t *vnf_device_base;
static uint32_t vnf_notify_multiplier;
static uint32_t vnf_queue_notify_offs[2];
static uint64_t vnf_rx_region_physical;
static uint64_t vnf_tx_region_physical;
static uint64_t vnf_payload_physical;
static void *vnf_payload_virtual;
static struct vnf_queue_mem vnf_queues[2];
static struct vnf_tx_notify_state vnf_tx_notify_state;
static int vnf_io_context;
static struct virtio_net_interrupt *vnf_active_interrupt;
#ifdef HFOC_TCP_LISTENER_QEMU_TEST
struct vnf_tcp_connection_state {
    bool active;
    uint32_t index;
    uint32_t last_activity_ms;
    struct vnf_tcp_proof_state *proof;
    struct hfoc_lwip_tcp_connection connection;
    struct hfoc_lwip_tcp_conn_callbacks callbacks;
#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
    struct hfoc_lwip_tcp_pool_token token;
#endif
};

struct vnf_tcp_proof_state {
    struct hfoc_lwip_netif netif;
    struct hfoc_lwip_tcp_listener listener;
    struct hfoc_lwip_tcp_callback listener_callback;
    struct vnf_tcp_connection_state connections[VNF_TCP_MAX_CONNECTIONS];
    uint32_t active_count;
    uint32_t accept_count;
    uint32_t echo_count;
    uint32_t reset_count;
    uint32_t timeout_count;
    uint32_t backlog_ok;
#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_deadline_scheduler deadline;
    struct hfoc_lwip_tcp_backlog backlog;
    struct hfoc_lwip_tcp_quarantine quarantine;
    struct hfoc_lwip_tcp_close_policy close_policy;
    struct hfoc_lwip_tcp_drain_restart drain;
    struct packet_pool_descriptor packet_pool;
    struct packet_pressure packet_pressure;
    struct packet_pool_slot_descriptor
        packet_tokens[VNF_NETRES_PACKET_COUNT];
    uint32_t packet_pressure_reserved;
    uint32_t pool_full_ok;
    uint32_t backlog_pressure_ok;
    uint32_t backlog_drained_ok;
    uint32_t packet_pressure_ok;
    uint32_t restart_ok;
    uint32_t restarted_echo_ok;
    uint32_t balanced_pool_ok;
    uint32_t restart_live_ok;
    uint32_t backlog_drain_deadline_ms;
    bool backlog_drain_armed;
    bool restart_requested;
#endif
    struct virtio_net_tx *tx;
    struct virtio_net_queue_composition *composition;
    struct virtio_net_rx *rx;
    uint64_t tx_frame_physical;
    void *tx_frame_virtual;
    struct hfoc_lwip_alloc_snapshot alloc_start;
};

static struct vnf_tcp_proof_state vnf_tcp_proof;
#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
static uint8_t vnf_netres_packet_storage[
    VNF_NETRES_PACKET_STORAGE_SIZE];
#endif
#ifdef HFOC_MBEDTLS_TLS_QEMU_TEST
#define VNF_TLS_CONNECTION_COUNT 4U
#define VNF_TLS_RX_BUFFER_SIZE 4096U
#define VNF_TLS_RESPONSE "HFOC-TLS-QEMU-RESPONSE"
#define VNF_TLS_ENTROPY_MAGIC "HFOC-TLS-ENTROPY-DENY"
#define VNF_TLS_ENTROPY_MAGIC_LENGTH \
    (sizeof(VNF_TLS_ENTROPY_MAGIC) - 1U)
#define VNF_TLS_HANDSHAKE_STEP_LIMIT 64U
#define VNF_TLS_TIMEOUT_MS 20000U
#define VNF_TLS_SETTLE_MS 500U
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
#define VNF_HTTPS_RESPONSE_BUFFER_SIZE HTTP_RESPONSE_BUFFER_MAX
#define VNF_HTTPS_PLAINTEXT_CHUNK_SIZE 256U
#define VNF_HTTPS_PEER_ID 7U
#define VNF_HTTPS_PEER_ID_OTHER 8U
#define VNF_HTTPS_PEER_FAILURE_LIMIT 2U
#define VNF_HTTPS_PEER_WINDOW_TICKS 1000U
#define VNF_HTTPS_PEER_COOLDOWN_TICKS 1000U
#define VNF_HTTPS_LIFECYCLE_DRAIN_TIMEOUT_MS 100U
#define VNF_HTTPS_LIFECYCLE_PACKET_POOL_CAPACITY 16U
#define VNF_HTTPS_RESTART_SETTLE_MS 500U
#define VNF_HTTPS_PACKET_COUNT 8U
#define VNF_HTTPS_PACKET_CAPACITY 64U
#define VNF_HTTPS_PACKET_HEADROOM 0U
#define VNF_HTTPS_PACKET_LENGTH 64U
#define VNF_HTTPS_PACKET_STORAGE_SIZE \
    (VNF_HTTPS_PACKET_COUNT * VNF_HTTPS_PACKET_CAPACITY)
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
#define VNF_HTTPS_SOAK_ROUNDS 3U
#define VNF_HTTPS_SOAK_REQUESTS_PER_ROUND 8U
#define VNF_HTTPS_SOAK_ROUND_REQUESTS \
    (VNF_HTTPS_SOAK_ROUNDS * VNF_HTTPS_SOAK_REQUESTS_PER_ROUND)
#define VNF_HTTPS_SOAK_FINAL_REQUESTS 4U
#define VNF_HTTPS_SOAK_TOTAL_REQUESTS \
    (VNF_HTTPS_SOAK_ROUND_REQUESTS + VNF_HTTPS_SOAK_FINAL_REQUESTS)
#endif
#ifdef HFOC_HTTPS_ENTROPY_FAIL_CLOSED_QEMU_TEST
#define VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID 296U
#define VNF_HTTPS_ENTROPY_FAIL_BLOCK_COUNT 2U
#define VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH 32U
#define VNF_HTTPS_ENTROPY_FAIL_STALL_ITERATIONS 20000000U
#endif
#endif

struct vnf_tls_qemu_connection {
    bool active;
    bool ssl_setup;
    bool handshake_done;
    bool response_sent;
    bool close_notify_sent;
    bool closing;
    bool peer_closed;
    bool releasing;
    uint32_t index;
    uint32_t generation;
    uint32_t last_activity_ms;
    uint32_t allocator_id;
    struct tcp_pcb *pcb;
    uint8_t rx[VNF_TLS_RX_BUFFER_SIZE];
    size_t rx_length;
    size_t rx_offset;
    mbedtls_ssl_context ssl;
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    bool parser_started;
    bool request_line_done;
    bool request_parsed;
    /*
     * ITEM-445 step 2: the bounded sign-in body, collected after the headers
     * are complete and before the request is completed.
     */
    bool body_pending;
    uint32_t body_length;
    uint32_t body_expected;
    bool response_ready;
    bool response_head;
    bool response_http_sent;
    bool malformed_request;
    struct http_parser_token parser_token;
    enum http_parser_status parser_error;
    uint8_t response[VNF_HTTPS_RESPONSE_BUFFER_SIZE];
    uint8_t body[HTTP_FRAMING_BODY_MAX];
    uint32_t response_length;
    uint32_t response_offset;
#endif
};

struct vnf_tls_qemu_proof_state {
    struct hfoc_lwip_tcp_listener listener;
    struct hfoc_lwip_tcp_callback listener_callback;
    struct vnf_tls_qemu_connection connections[VNF_TLS_CONNECTION_COUNT];
    uint32_t active_count;
    uint32_t accept_count;
    uint32_t success_count;
    uint32_t malformed_count;
    uint32_t entropy_count;
    uint32_t recovery_count;
    uint32_t timeout_count;
    uint32_t settle_deadline_ms;
    bool settle_armed;
    struct hfoc_lwip_alloc_snapshot lwip_start;
    struct hfoc_mbedtls_alloc_snapshot mbedtls_start;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    mbedtls_ssl_config ssl_config;
    bool crypto_published;
    bool certificate_published;
    bool key_published;
    bool ssl_config_published;
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    struct http_parser parser;
    struct https_health_store health;
    struct https_peer_throttle_table throttle;
    struct https_service_lifecycle lifecycle;
    struct event_ring audit_ring;
    struct appliance_system_api_snapshot system_snapshot;
    struct appliance_network_api_snapshot network_snapshot;
    struct appliance_service_api_snapshot service_snapshot;
    struct appliance_health_api_snapshot health_snapshot;
    /*
     * ITEM-384 rendered panel. The fixture publishes one administrator
     * credential and keeps one deterministic bearer session open so the real
     * TLS client can prove the panel over the wire, including the
     * deny-by-default answer when no session is presented.
     */
    struct appliance_production_auth panel_auth;
    struct appliance_management_panel panel;
    struct appliance_admin_auth_proof panel_proof;
    uint8_t panel_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint64_t panel_session_expires;
    uint32_t panel_session_count;
    uint32_t panel_get_count;
    uint32_t panel_head_count;
    uint32_t panel_denied_count;
    uint32_t panel_method_count;
    /* ITEM-445 step 2: the browser sign-in proof counters. */
    uint32_t panel_sign_in_count;
    uint32_t panel_sign_out_count;
    uint32_t panel_cookie_get_count;
    uint32_t root_get_count;
    uint32_t root_head_count;
    uint32_t health_get_count;
    uint32_t health_head_count;
    uint32_t api_version_get_count;
    uint32_t api_version_head_count;
    uint32_t api_method_count;
    uint32_t api_not_found_count;
    uint32_t system_get_count;
    uint32_t system_head_count;
    uint32_t system_method_count;
    uint32_t network_config_get_count;
    uint32_t network_config_head_count;
    uint32_t network_status_get_count;
    uint32_t network_status_head_count;
    uint32_t network_method_count;
    uint32_t service_get_count;
    uint32_t service_head_count;
    uint32_t service_method_count;
    uint32_t health_api_get_count;
    uint32_t health_api_head_count;
    uint32_t health_api_method_count;
    uint32_t fifth_count;
    uint32_t peer_throttle_count;
    uint32_t restart_ok;
    uint32_t restart_live;
    uint32_t health_ok;
    uint32_t audit_ok;
    uint32_t route_count;
    bool restart_requested;
    bool proof_ready;
    struct packet_pool_descriptor packet_pool;
    struct packet_pressure packet_pressure;
    struct packet_pool_slot_descriptor
        packet_tokens[VNF_HTTPS_PACKET_COUNT];
    uint32_t packet_pressure_ok;
    uint32_t malformed_drained_ok;
    uint32_t slot_released_ok;
    uint32_t exhaustion_recovery_ok;
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    uint32_t soak_request_count;
    uint32_t soak_malformed_count;
    uint32_t soak_reset_count;
    uint32_t soak_restart_count;
    uint32_t soak_stale_callback_count;
#endif
#ifdef HFOC_HTTPS_ENTROPY_FAIL_CLOSED_QEMU_TEST
    uint32_t entropy_recovery_ok;
#endif
#endif
};

static struct vnf_tls_qemu_proof_state vnf_tls_qemu_proof;
static bool vnf_tls_qemu_rng_deny;
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
static uint8_t vnf_https_packet_storage[
    VNF_HTTPS_PACKET_STORAGE_SIZE];
static void vnf_https_qemu_require(bool condition, const char *message);
static void vnf_https_qemu_mark(const char *marker);
#ifdef HFOC_HTTPS_ENTROPY_FAIL_CLOSED_QEMU_TEST
static void vnf_https_entropy_fail_closed_matrix(void);
#endif
static void vnf_tls_qemu_setup_connection(
    struct vnf_tls_qemu_connection *connection);
#endif
#endif
#endif
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
struct vnf_packet_rx_state {
    struct hfoc_netif_input input;
    struct packet_pressure pressure;
    struct packet_pool_descriptor pool;
    struct packet_pool_slot_descriptor accepted[VNF_FRAME_COUNT];
    uint32_t accepted_count;
    uint32_t invalid_count;
};

struct vnf_packet_tx_state {
    struct hfoc_netif_output output;
    struct packet_pool_slot_descriptor slots[VNF_PACKET_FRAME_COUNT];
    struct hfoc_netif_output_submission
        submissions[VNF_PACKET_FRAME_COUNT];
    struct virtio_net_tx *tx;
    struct virtio_net_queue_composition *composition;
    uint64_t frame_physical_base;
    void *frame_virtual_base;
    uint32_t frame_length;
    uint32_t submitted_count;
    uint32_t backpressure_count;
    bool backpressure_pending;
};

static uint8_t vnf_packet_storage[VNF_PACKET_STORAGE_SIZE];
static struct vnf_packet_rx_state vnf_packet_rx_state;
static struct vnf_packet_tx_state vnf_packet_tx_state;
static struct virtio_net_mac_identity vnf_packet_mac_identity;
static struct network_ipv4_profile vnf_packet_ipv4_profile;
static struct network_device_selection vnf_packet_device_selection;
#endif
static uint8_t vnf_proof_stack[VNF_PROOF_STACK_SIZE]
    __attribute__((aligned(16)));

static void vnf_fail(const char *message) __attribute__((noreturn));
static void vnf_write_descriptor(
    struct vnf_queue_mem *queue,
    uint32_t index,
    uint64_t address,
    uint32_t length,
    uint16_t flags,
    uint16_t next);

static void vnf_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
        serial_write("[PACKET-BOUNDARY] failed: ");
#else
        serial_write("[VIRTIO-NET-FRAME] failed: ");
#endif
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void vnf_require(bool condition, const char *message)
{
    if (!condition) {
        vnf_fail(message);
    }
}

static void vnf_zero_bytes(void *destination, size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void vnf_copy_bytes(
    void *destination,
    const void *source,
    size_t length)
{
    const unsigned char *const source_bytes = source;
    unsigned char *const destination_bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination_bytes[index] = source_bytes[index];
    }
}

static uint16_t vnf_config_le16(
    const uint8_t *config,
    uint32_t offset)
{
    return (uint16_t)config[offset] |
           (uint16_t)((uint16_t)config[offset + 1U] << 8U);
}

static uint32_t vnf_config_le32(
    const uint8_t *config,
    uint32_t offset)
{
    return (uint32_t)config[offset] |
           ((uint32_t)config[offset + 1U] << 8U) |
           ((uint32_t)config[offset + 2U] << 16U) |
           ((uint32_t)config[offset + 3U] << 24U);
}

static void vnf_parse_qemu_capabilities(
    const uint8_t *config,
    const struct virtio_pci_bar_range *bars,
    struct virtio_pci_capability_parse_result *result)
{
    struct virtio_pci_capability_descriptor descriptors[
        VIRTIO_PCI_CAPABILITY_SLOT_COUNT] = {{0}};
    uint32_t capability_offset;
    uint32_t seen_mask = 0U;
    uint32_t iterations = 0U;
    uint32_t index;

    vnf_zero_bytes(result, sizeof(*result));
    capability_offset = vnf_config_le16(
        config, VIRTIO_PCI_CAPABILITY_POINTER_OFFSET);
    vnf_require(
        capability_offset >= VIRTIO_PCI_FIRST_CAPABILITY_OFFSET &&
            capability_offset < VIRTIO_PCI_CONFIG_SPACE_MAX,
        "QEMU capability pointer");

    while (capability_offset != 0U) {
        const uint8_t capability_id =
            config[capability_offset];
        const uint8_t next =
            config[capability_offset + 1U];

        vnf_require(iterations < 64U, "QEMU capability chain bound");
        ++iterations;
        vnf_require(
            capability_offset + 2U <= VIRTIO_PCI_CONFIG_SPACE_MAX,
            "QEMU capability header");

        if (capability_id == VIRTIO_PCI_CAPABILITY_ID_VENDOR) {
            const uint8_t type =
                config[capability_offset + 3U];
            const uint8_t bar =
                config[capability_offset + 4U];
            const uint32_t capability_length =
                config[capability_offset + 2U];
            const uint32_t range_offset = vnf_config_le32(
                config, capability_offset + 8U);
            const uint32_t range_length = vnf_config_le32(
                config, capability_offset + 12U);
            const uint32_t slot = (uint32_t)type - 1U;

            if (type >= VIRTIO_PCI_CAPABILITY_TYPE_COMMON &&
                type <= VIRTIO_PCI_CAPABILITY_TYPE_DEVICE) {
                const uint32_t minimum_length =
                    type == VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY ?
                        VIRTIO_PCI_NOTIFY_CAPABILITY_MIN_LENGTH :
                        VIRTIO_PCI_CAPABILITY_MIN_LENGTH;

                vnf_require(
                    capability_length >= minimum_length,
                    "QEMU capability length");
                vnf_require(
                    capability_offset + capability_length <=
                        VIRTIO_PCI_CONFIG_SPACE_MAX,
                    "QEMU capability bounds");
                vnf_require(
                    bar < VIRTIO_PCI_BAR_COUNT &&
                        bars[bar].present,
                    "QEMU capability BAR");
                vnf_require(
                    range_offset %
                        VIRTIO_PCI_CAPABILITY_ALIGNMENT == 0U &&
                    range_length %
                        VIRTIO_PCI_CAPABILITY_ALIGNMENT == 0U &&
                    range_length != 0U,
                    "QEMU capability alignment");
                vnf_require(
                    bars[bar].base <= UINT64_MAX - range_offset &&
                    bars[bar].base + range_offset <=
                        UINT64_MAX - range_length &&
                    range_offset + range_length <= bars[bar].length,
                    "QEMU capability range");
                vnf_require(
                    (seen_mask & (UINT32_C(1) << slot)) == 0U,
                    "QEMU duplicate capability");

                descriptors[slot].type = (uint8_t)type;
                descriptors[slot].bar = bar;
                descriptors[slot].offset = range_offset;
                descriptors[slot].length = range_length;
                if (type == VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY) {
                    descriptors[slot].notify_offset_multiplier =
                        vnf_config_le16(
                            config,
                            capability_offset +
                                VIRTIO_PCI_NOTIFY_OFFSET_MULTIPLIER_OFFSET);
                }
                seen_mask |= UINT32_C(1) << slot;
            } else {
                vnf_require(
                    type == 5U,
                    "unsupported QEMU capability type");
            }
        }

        if (next == 0U) {
            break;
        }
        vnf_require(
            next >= VIRTIO_PCI_FIRST_CAPABILITY_OFFSET &&
                next != capability_offset,
            "QEMU capability next");
        capability_offset = next;
    }

    vnf_require(
        seen_mask == VIRTIO_PCI_CAPABILITY_MASK,
        "QEMU capability completeness");
    result->published = true;
    result->version = VIRTIO_PCI_CAPABILITY_RESULT_VERSION;
    result->count = VIRTIO_PCI_CAPABILITY_SLOT_COUNT;
    for (index = 0U; index < VIRTIO_PCI_CAPABILITY_SLOT_COUNT;
         ++index) {
        result->descriptors[index] = descriptors[index];
    }
    result->snapshot.version = VIRTIO_PCI_CAPABILITY_SNAPSHOT_VERSION;
    result->snapshot.capability_mask = VIRTIO_PCI_CAPABILITY_MASK;
}

static void vnf_outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t vnf_inl(uint16_t port)
{
    uint32_t value;

    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t vnf_pci_config_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset)
{
    const uint32_t address =
        VNF_PCI_CONFIG_ENABLE |
        ((uint32_t)bus << 16U) |
        ((uint32_t)device << 11U) |
        ((uint32_t)function << 8U) |
        ((uint32_t)offset & 0xFCU);

    vnf_outl(VNF_PCI_CONFIG_ADDRESS_PORT, address);
    return vnf_inl(VNF_PCI_CONFIG_DATA_PORT);
}

static uint16_t vnf_pci_config_read16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset)
{
    const uint32_t value =
        vnf_pci_config_read32(bus, device, function, offset);

    return (uint16_t)((value >> ((offset & 3U) * 8U)) & 0xFFFFU);
}

static void vnf_pci_config_read_space(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t *config)
{
    uint32_t offset;

    for (offset = 0U; offset < 256U; offset += 4U) {
        const uint32_t value =
            vnf_pci_config_read32(bus, device, function, (uint8_t)offset);

        config[offset] = (uint8_t)(value & 0xFFU);
        config[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
        config[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
        config[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
    }
}

static uint32_t vnf_mmio_read32(
    volatile uint8_t *base,
    uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static uint64_t vnf_mmio_read64(
    volatile uint8_t *base,
    uint32_t offset)
{
    return *(volatile uint64_t *)(base + offset);
}

static uint16_t vnf_mmio_read16(
    volatile uint8_t *base,
    uint32_t offset)
{
    return *(volatile uint16_t *)(base + offset);
}

static uint8_t vnf_mmio_read8(
    volatile uint8_t *base,
    uint32_t offset)
{
    return *(volatile uint8_t *)(base + offset);
}

static void vnf_mmio_write32(
    volatile uint8_t *base,
    uint32_t offset,
    uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static void vnf_mmio_write16(
    volatile uint8_t *base,
    uint32_t offset,
    uint16_t value)
{
    *(volatile uint16_t *)(base + offset) = value;
}

static void vnf_mmio_write8(
    volatile uint8_t *base,
    uint32_t offset,
    uint8_t value)
{
    *(volatile uint8_t *)(base + offset) = value;
}

static void vnf_mmio_write64(
    volatile uint8_t *base,
    uint32_t offset,
    uint64_t value)
{
    *(volatile uint64_t *)(base + offset) = value;
}

static bool vnf_status_write(void *context, uint8_t value)
{
    (void)context;
    vnf_mmio_write8(vnf_common_base, VNF_COMMON_DEVICE_STATUS, value);
    return true;
}

static bool vnf_status_read(void *context, uint8_t *value)
{
    (void)context;
    *value = vnf_mmio_read8(vnf_common_base, VNF_COMMON_DEVICE_STATUS);
    return true;
}

static bool vnf_feature_read_offered(
    void *context,
    uint32_t select,
    uint32_t *value)
{
    (void)context;
    vnf_mmio_write32(
        vnf_common_base, VNF_COMMON_DEVICE_FEATURE_SELECT, select);
    *value = vnf_mmio_read32(vnf_common_base, VNF_COMMON_DEVICE_FEATURE);
    return true;
}

static bool vnf_feature_write_accepted(
    void *context,
    uint32_t select,
    uint32_t value)
{
    (void)context;
    vnf_mmio_write32(
        vnf_common_base, VNF_COMMON_DRIVER_FEATURE_SELECT, select);
    vnf_mmio_write32(vnf_common_base, VNF_COMMON_DRIVER_FEATURE, value);
    return true;
}

static bool vnf_feature_read_accepted(
    void *context,
    uint32_t select,
    uint32_t *value)
{
    (void)context;
    vnf_mmio_write32(
        vnf_common_base, VNF_COMMON_DRIVER_FEATURE_SELECT, select);
    *value = vnf_mmio_read32(vnf_common_base, VNF_COMMON_DRIVER_FEATURE);
    return true;
}

static void vnf_barrier_fence(void *context)
{
    (void)context;
    __asm__ volatile ("" : : : "memory");
}

static bool vnf_queue_reclaim(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    (void)context;
    (void)element;
    (void)token;
    return true;
}

static bool vnf_zeroize(
    void *context,
    uint64_t physical_base,
    uint64_t length)
{
    (void)context;
    vnf_zero_bytes(
        (void *)(uintptr_t)(vnf_hhdm + physical_base), (size_t)length);
    return true;
}

static bool vnf_link_read_config_status(
    void *context,
    uint32_t *status)
{
    (void)context;
    *status = (uint32_t)vnf_mmio_read16(vnf_device_base, 6U);
    return true;
}

static bool vnf_interrupt_read_status(void *context, uint8_t *status)
{
    (void)context;
    *status = vnf_mmio_read8(vnf_isr_base, 0U);
    return true;
}

static bool vnf_interrupt_has_rx_work(void *context)
{
    (void)context;
    return true;
}

static bool vnf_interrupt_has_tx_work(void *context)
{
    (void)context;
    return true;
}

static void vnf_interrupt_publish(
    void *context,
    const struct virtio_net_interrupt_work *work)
{
    (void)context;
    (void)work;
}

static bool vnf_tx_notify(
    void *context,
    const struct virtio_net_tx_submission *submission)
{
    struct vnf_tx_notify_state *state =
        (struct vnf_tx_notify_state *)context;

    vnf_require(
        state->queue != NULL &&
            submission->header_descriptor_index <
                state->queue->layout.queue_size &&
            submission->frame_descriptor_index <
                state->queue->layout.queue_size,
        "TX notify descriptor bounds");
    vnf_write_descriptor(
        state->queue,
        submission->header_descriptor_index,
        state->header_physical_base,
        VNF_TX_HEADER_LENGTH,
        1U,
        (uint16_t)submission->frame_descriptor_index);
    vnf_write_descriptor(
        state->queue,
        submission->frame_descriptor_index,
        state->frame_physical_base,
        state->frame_length,
        0U,
        0U);
    ++state->calls;
    state->last_slot = submission->slot_index;
    return true;
}

static bool vnf_notifications_quiesce(void *context)
{
    (void)context;
    return true;
}

static bool vnf_find_net_device(
    uint8_t *device_out,
    uint8_t *function_out,
    uint16_t *vendor_out,
    uint16_t *device_id_out)
{
    uint8_t device;

    for (device = 0U; device < 32U; ++device) {
        const uint16_t vendor =
            vnf_pci_config_read16(0U, device, 0U, 0U);
        const uint16_t device_id =
            vnf_pci_config_read16(0U, device, 0U, 2U);

        if (vendor == 0xFFFFU) {
            continue;
        }
        if (vendor == VIRTIO_PCI_VENDOR_ID &&
            (device_id == VIRTIO_PCI_DEVICE_ID_NET_MODERN ||
             device_id == VIRTIO_PCI_DEVICE_ID_NET_TRANSITIONAL)) {
            *device_out = device;
            *function_out = 0U;
            *vendor_out = vendor;
            *device_id_out = device_id;
            return true;
        }
    }
    return false;
}

static void vnf_make_bar_ranges(
    const uint8_t *config,
    uint8_t pci_device,
    struct virtio_pci_bar_range ranges[VNF_PCI_BAR_COUNT])
{
    uint32_t bar_index;

    (void)config;
    vnf_zero_bytes(ranges, sizeof(*ranges) * VNF_PCI_BAR_COUNT);
    for (bar_index = 0U; bar_index < VNF_PCI_BAR_COUNT; ++bar_index) {
        const uint32_t raw =
            vnf_pci_config_read32(0U, pci_device, 0U,
                                  (uint8_t)(0x10U + bar_index * 4U));
        uint64_t base;
        bool is_64;

        if (raw == 0U || (raw & 1U) != 0U) {
            continue;
        }
        base = (uint64_t)(raw & ~0xFU);
        is_64 = (raw & 0x6U) == 0x4U;
        ranges[bar_index].present = true;
        ranges[bar_index].base = base;
        ranges[bar_index].length = VNF_PCI_BAR_LENGTH;
        if (is_64 && bar_index + 1U < VNF_PCI_BAR_COUNT) {
            const uint32_t high =
                vnf_pci_config_read32(
                    0U, pci_device, 0U,
                    (uint8_t)(0x10U + (bar_index + 1U) * 4U));

            base |= (uint64_t)high << 32U;
            ranges[bar_index].base = base;
            ++bar_index;
        }
    }
}

static volatile uint8_t *vnf_map_bar(uint64_t physical_base)
{
    uint32_t page_index;

    for (page_index = 0U; page_index < 1U; ++page_index) {
        volatile void *mapped = NULL;
        const enum vmm_status status = vmm_map_mmio_page(
            physical_base + page_index * 4096U,
            vnf_hhdm,
            &mapped);

        vnf_require(status == VMM_OK, "BAR MMIO mapping");
    }
    return (volatile uint8_t *)(uintptr_t)(
        physical_base + vnf_hhdm);
}

static void vnf_build_expected_frame(
    uint8_t *frame,
    uint32_t frame_length,
    const uint8_t *destination,
    const uint8_t *source,
    uint16_t ether_type,
    const char *prefix,
    uint32_t index)
{
    size_t position = 0U;
    size_t prefix_length = strlen(prefix);
    size_t prefix_index;

    vnf_zero_bytes(frame, frame_length);
    vnf_copy_bytes(frame + position, destination, 6U);
    position += 6U;
    vnf_copy_bytes(frame + position, source, 6U);
    position += 6U;
    frame[position++] = (uint8_t)(ether_type >> 8U);
    frame[position++] = (uint8_t)(ether_type & 0xFFU);
    for (prefix_index = 0U; prefix_index < prefix_length;
         ++prefix_index) {
        frame[position++] = (uint8_t)prefix[prefix_index];
    }
    frame[position++] = (uint8_t)('0' + (index / 10U));
    frame[position++] = (uint8_t)('0' + (index % 10U));
}

static void vnf_write_descriptor(
    struct vnf_queue_mem *queue,
    uint32_t index,
    uint64_t address,
    uint32_t length,
    uint16_t flags,
    uint16_t next)
{
    volatile struct vnf_virtq_desc *descriptor =
        &queue->descriptors[index];

    descriptor->addr = address;
    descriptor->len = length;
    descriptor->flags = flags;
    descriptor->next = next;
}

static void vnf_program_queue(
    uint32_t queue_index,
    const struct vnf_queue_mem *queue)
{
    uint16_t notify_off;

    vnf_mmio_write16(vnf_common_base, VNF_COMMON_QUEUE_SELECT,
                     (uint16_t)queue_index);
    vnf_require(
        vnf_mmio_read16(vnf_common_base, VNF_COMMON_QUEUE_SELECT) ==
            queue_index,
        "queue select readback");
    notify_off =
        vnf_mmio_read16(vnf_common_base, VNF_COMMON_QUEUE_NOTIFY_OFF);
    vnf_mmio_write16(
        vnf_common_base, VNF_COMMON_QUEUE_SIZE,
        (uint16_t)queue->layout.queue_size);
    vnf_require(
        vnf_mmio_read16(vnf_common_base, VNF_COMMON_QUEUE_SIZE) ==
            queue->layout.queue_size,
        "queue size readback");
    vnf_mmio_write64(
        vnf_common_base, VNF_COMMON_QUEUE_DESC,
        queue->layout.region_base + queue->layout.descriptor.offset);
    vnf_mmio_write64(
        vnf_common_base, VNF_COMMON_QUEUE_DRIVER,
        queue->layout.region_base + queue->layout.available.offset);
    vnf_mmio_write64(
        vnf_common_base, VNF_COMMON_QUEUE_DEVICE,
        queue->layout.region_base + queue->layout.used.offset);
    vnf_queue_notify_offs[queue_index] = notify_off;
    vnf_mmio_write16(
        vnf_common_base, VNF_COMMON_QUEUE_ENABLE, 1U);
    vnf_require(
        vnf_mmio_read16(vnf_common_base, VNF_COMMON_QUEUE_ENABLE) == 1U,
        "queue enable readback");
}

static void vnf_notify_queue(uint32_t queue_index, uint16_t value)
{
    volatile uint16_t *notify_address = (volatile uint16_t *)(
        vnf_notify_base +
        vnf_queue_notify_offs[queue_index] * vnf_notify_multiplier);

    *notify_address = value;
}

static void vnf_wait_used_count(
    const struct virtio_used_ring *ring,
    uint32_t count)
{
    for (;;) {
        if ((uint32_t)ring->idx >= count) {
            return;
        }
        __asm__ volatile ("pause" : : : "memory");
    }
}

#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
static enum hfoc_netif_input_result vnf_packet_input_deliver(
    void *context,
    struct packet_slice *slice,
    const struct ethernet_frame_view *view)
{
    struct vnf_packet_rx_state *state =
        (struct vnf_packet_rx_state *)context;

    (void)slice;
    (void)view;
    vnf_require(
        state->accepted_count < VNF_FRAME_COUNT,
        "packet input accepted bound");
    state->accepted[state->accepted_count] = slice->slot;
    ++state->accepted_count;
    return HFOC_NETIF_INPUT_ACCEPTED;
}

static enum hfoc_netif_output_result vnf_packet_output_submit(
    void *context,
    struct packet_slice *slice,
    struct hfoc_netif_output_submission *submission)
{
    struct vnf_packet_tx_state *state =
        (struct vnf_packet_tx_state *)context;
    struct virtio_net_tx_frame frame;
    struct virtio_net_tx_submission tx_submission;
    enum virtio_net_tx_status tx_status;
    enum packet_slice_status slice_status;
    uint32_t index;
    uint64_t frame_physical;
    uint8_t *frame_virtual;

    if (state->backpressure_pending) {
        state->backpressure_pending = false;
        ++state->backpressure_count;
        return HFOC_NETIF_OUTPUT_BACKPRESSURE;
    }

    vnf_require(
        state->submitted_count < VNF_PACKET_FRAME_COUNT,
        "packet output submitted bound");
    index = state->submitted_count;
    frame_physical =
        state->frame_physical_base +
        (VNF_DIRECT_FRAME_COUNT + index) * state->frame_length;
    frame_virtual =
        (uint8_t *)state->frame_virtual_base +
        (VNF_DIRECT_FRAME_COUNT + index) * state->frame_length;
    slice_status = packet_slice_seek(slice, 0U);
    vnf_require(slice_status == PACKET_SLICE_OK, "packet TX seek");
    slice_status = packet_slice_read(
        slice, frame_virtual, state->frame_length);
    vnf_require(slice_status == PACKET_SLICE_OK, "packet TX copy");

    vnf_tx_notify_state.header_physical_base =
        state->tx->headers[VNF_DIRECT_FRAME_COUNT + index].physical_base;
    vnf_tx_notify_state.frame_physical_base = frame_physical;
    vnf_tx_notify_state.frame_length = state->frame_length;
    frame.data = frame_virtual;
    frame.physical_base = frame_physical;
    frame.length = state->frame_length;
    tx_status = virtio_net_tx_submit(
        state->tx, &frame, &tx_submission);
    vnf_require(tx_status == VIRTIO_NET_TX_OK, "packet TX submission");
    vnf_require(
        tx_submission.slot_index == VNF_DIRECT_FRAME_COUNT + index &&
        tx_submission.generation == state->tx->generation,
        "packet TX ownership");
    vnf_notify_queue(
        1U,
        (uint16_t)virtio_available_publication_visible_index(
            &state->composition->transmit.available));

    submission->slot = slice->slot.slot;
    submission->generation = slice->slot.generation;
    state->slots[index] = slice->slot;
    state->submissions[index] = *submission;
    ++state->submitted_count;
    return HFOC_NETIF_OUTPUT_SUBMITTED;
}

static void vnf_packet_pressure_refresh(
    uint32_t invalid_frame_count,
    uint32_t tx_backpressure_count)
{
    vnf_require(
        packet_pressure_begin_update(&vnf_packet_rx_state.pressure) ==
            PACKET_PRESSURE_OK &&
        packet_pressure_refresh_pool_counts(
            &vnf_packet_rx_state.pressure) == PACKET_PRESSURE_OK,
        "packet pressure refresh");
    if (invalid_frame_count != 0U) {
        vnf_require(
            packet_pressure_record_invalid_frame(
                &vnf_packet_rx_state.pressure,
                invalid_frame_count) == PACKET_PRESSURE_OK,
            "packet pressure invalid frame");
    }
    if (tx_backpressure_count != 0U) {
        vnf_require(
            packet_pressure_record_tx_backpressure(
                &vnf_packet_rx_state.pressure,
                tx_backpressure_count) == PACKET_PRESSURE_OK,
            "packet pressure TX backpressure");
    }
    vnf_require(
        packet_pressure_commit(&vnf_packet_rx_state.pressure) ==
            PACKET_PRESSURE_OK,
        "packet pressure commit");
}

static bool vnf_packet_ipv4_profile_active(void)
{
    const struct network_ipv4_config *active =
        network_ipv4_profile_active_config(&vnf_packet_ipv4_profile);
    const struct network_ipv4_port_policy *port =
        network_ipv4_profile_port_policy(&vnf_packet_ipv4_profile);

    return active != NULL && port != NULL &&
           active->schema_version == NETWORK_IPV4_CONFIG_VERSION &&
           active->address.octets[0] ==
               (uint8_t)NETWORK_IPV4_PROFILE_QEMU_ADDRESS_0 &&
           active->address.octets[1] ==
               (uint8_t)NETWORK_IPV4_PROFILE_QEMU_ADDRESS_1 &&
           active->address.octets[2] ==
               (uint8_t)NETWORK_IPV4_PROFILE_QEMU_ADDRESS_2 &&
           active->address.octets[3] ==
               (uint8_t)NETWORK_IPV4_PROFILE_QEMU_ADDRESS_3 &&
           active->prefix_length ==
               (uint8_t)NETWORK_IPV4_PROFILE_QEMU_PREFIX &&
           active->gateway.octets[0] ==
               (uint8_t)NETWORK_IPV4_PROFILE_QEMU_GATEWAY_0 &&
           active->gateway.octets[1] ==
               (uint8_t)NETWORK_IPV4_PROFILE_QEMU_GATEWAY_1 &&
           active->gateway.octets[2] ==
               (uint8_t)NETWORK_IPV4_PROFILE_QEMU_GATEWAY_2 &&
           active->gateway.octets[3] ==
               (uint8_t)NETWORK_IPV4_PROFILE_QEMU_GATEWAY_3 &&
           port->port == NETWORK_IPV4_PROFILE_QEMU_PORT;
}

static void vnf_packet_boundary_setup(
    const uint8_t guest_mac[6],
    const struct virtio_net_feature_profile *profile,
    const struct network_device_snapshot *device_snapshot,
    uint16_t pci_vendor,
    uint16_t pci_device_id,
    uint8_t pci_device,
    uint8_t pci_function)
{
    struct network_ipv4_config ipv4_config;
    struct network_ipv4_port_policy port_policy;
    struct hfoc_netif_input_adapter input_adapter;
    struct hfoc_netif_output_adapter output_adapter;

    (void)pci_vendor;
    (void)pci_device_id;
    (void)pci_device;
    (void)pci_function;
    serial_write("[PACKET-BOUNDARY] proof init\n");
    vnf_require(
        memcmp(device_snapshot->mac_address, guest_mac,
               NETWORK_DEVICE_MAC_LENGTH) == 0,
        "packet device MAC agreement");
    vnf_zero_bytes(&vnf_packet_rx_state, sizeof(vnf_packet_rx_state));
    vnf_zero_bytes(&vnf_packet_tx_state, sizeof(vnf_packet_tx_state));
    vnf_require(
        packet_pool_init(
            &vnf_packet_rx_state.pool,
            vnf_packet_storage,
            sizeof(vnf_packet_storage),
            VNF_PACKET_POOL_COUNT,
            VNF_PACKET_CAPACITY,
            VNF_PACKET_HEADROOM,
            VNF_PACKET_LENGTH) == PACKET_POOL_OK &&
        packet_pressure_init(
            &vnf_packet_rx_state.pressure,
            &vnf_packet_rx_state.pool) == PACKET_PRESSURE_OK,
        "packet pool publication");
    serial_write("[PACKET-BOUNDARY] pool\n");

    vnf_zero_bytes(&vnf_packet_device_selection,
                   sizeof(vnf_packet_device_selection));
    vnf_require(
        network_device_selection_init(
            &vnf_packet_device_selection,
            device_snapshot,
            VNF_GENERATION) == NETWORK_DEVICE_OK,
        "packet device selection");
    vnf_zero_bytes(&vnf_packet_mac_identity,
                   sizeof(vnf_packet_mac_identity));
    vnf_require(
        virtio_net_mac_identity_bind(
            &vnf_packet_mac_identity,
            &vnf_packet_device_selection,
            profile,
            VNF_GENERATION) == VIRTIO_NET_MAC_IDENTITY_OK,
        "packet MAC identity");
    vnf_require(
        network_ipv4_profile_qemu_fill(
            &ipv4_config, &port_policy) == NETWORK_IPV4_PROFILE_OK &&
        network_ipv4_profile_init(
            &vnf_packet_ipv4_profile,
            &ipv4_config,
            &vnf_packet_mac_identity,
            &port_policy,
            VNF_GENERATION) == NETWORK_IPV4_PROFILE_OK &&
        vnf_packet_ipv4_profile_active(),
        "static IPv4 profile activation");
    serial_write("[PACKET-BOUNDARY] identity/profile\n");

    input_adapter.deliver = vnf_packet_input_deliver;
    input_adapter.context = &vnf_packet_rx_state;
    vnf_require(
        hfoc_netif_input_init(
            &vnf_packet_rx_state.input,
            &vnf_packet_rx_state.pool,
            &input_adapter) == HFOC_NETIF_INPUT_OK,
        "packet input publication");
    output_adapter.submit = vnf_packet_output_submit;
    output_adapter.context = &vnf_packet_tx_state;
    vnf_require(
        hfoc_netif_output_init(
            &vnf_packet_tx_state.output,
            &vnf_packet_rx_state.pool,
            &output_adapter) == HFOC_NETIF_OUTPUT_OK,
        "packet output publication");
}

static void vnf_packet_process_rx_frame(
    const struct virtio_net_rx_frame *frame)
{
    static const uint16_t allowed_types[] = { VNF_ETHER_TYPE_RX };
    struct packet_pool_slot_descriptor descriptor;
    struct packet_slice slice;
    struct ethernet_frame_policy policy;
    struct ethernet_frame_view view;
    enum ethernet_frame_status frame_status;
    enum hfoc_netif_input_status input_status;
    enum hfoc_netif_input_result input_result;
    enum packet_slice_status slice_status;

    vnf_require(
        packet_pool_reserve(
            &vnf_packet_rx_state.pool,
            PACKET_POOL_DIRECTION_RX,
            &descriptor) == PACKET_POOL_OK &&
        packet_pool_commit(
            &vnf_packet_rx_state.pool,
            &descriptor) == PACKET_POOL_OK &&
        packet_slice_init(
            &slice,
            &vnf_packet_rx_state.pool,
            &descriptor,
            frame->length) == PACKET_SLICE_OK,
        "packet RX pool reservation");
    slice_status = packet_slice_write(
        &slice, frame->data, frame->length);
    vnf_require(slice_status == PACKET_SLICE_OK, "packet RX copy");
    slice_status = packet_slice_seek(&slice, 0U);
    vnf_require(slice_status == PACKET_SLICE_OK, "packet RX seek");

    vnf_zero_bytes(&policy, sizeof(policy));
    policy.version = ETHERNET_FRAME_POLICY_VERSION;
    policy.accept_unicast = true;
    policy.accept_broadcast = true;
    policy.accept_multicast = false;
    policy.allowed_ether_types = allowed_types;
    policy.allowed_ether_type_count = 1U;
    frame_status = ethernet_frame_validate(
        &slice, &policy, frame->length -
            ETHERNET_FRAME_HEADER_LENGTH, &view);
    if (frame_status != ETHERNET_FRAME_OK) {
        vnf_require(
            frame_status == ETHERNET_FRAME_DESTINATION_DENIED ||
            frame_status == ETHERNET_FRAME_ETHERTYPE_DENIED,
            "packet RX expected rejection");
        vnf_require(
            packet_pool_release(
                &vnf_packet_rx_state.pool,
                &descriptor) == PACKET_POOL_OK,
            "packet RX rejected release");
        ++vnf_packet_rx_state.invalid_count;
        return;
    }

    input_status = hfoc_netif_input_transfer(
        &vnf_packet_rx_state.input,
        &slice,
        &view,
        &input_result);
    vnf_require(
        input_status == HFOC_NETIF_INPUT_OK &&
        input_result == HFOC_NETIF_INPUT_ACCEPTED,
        "packet RX input transfer");
}

static void vnf_packet_submit_tx_frames(void)
{
    struct packet_pool_slot_descriptor descriptor;
    struct packet_slice slice;
    enum hfoc_netif_output_result output_result;
    enum packet_slice_status slice_status;
    uint32_t index;

    vnf_packet_tx_state.frame_physical_base =
        vnf_payload_physical + VNF_TX_FRAME_BASE_OFFSET;
    vnf_packet_tx_state.frame_virtual_base =
        (uint8_t *)vnf_payload_virtual + VNF_TX_FRAME_BASE_OFFSET;
    vnf_packet_tx_state.frame_length = VNF_TX_FRAME_LENGTH;
    vnf_packet_tx_state.backpressure_pending = true;

    for (index = 0U; index < VNF_PACKET_FRAME_COUNT; ++index) {
        uint8_t expected[VNF_TX_FRAME_LENGTH];

        vnf_require(
            packet_pool_reserve(
                &vnf_packet_rx_state.pool,
                PACKET_POOL_DIRECTION_TX,
                &descriptor) == PACKET_POOL_OK &&
            packet_pool_commit(
                &vnf_packet_rx_state.pool,
                &descriptor) == PACKET_POOL_OK &&
            packet_slice_init(
                &slice,
                &vnf_packet_rx_state.pool,
                &descriptor,
                VNF_TX_FRAME_LENGTH) == PACKET_SLICE_OK,
            "packet TX pool reservation");
        vnf_build_expected_frame(
            expected,
            VNF_TX_FRAME_LENGTH,
            (const uint8_t[6]){0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U},
            (const uint8_t[6]){0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U},
            VNF_PACKET_TX_ETHER_TYPE,
            "HFOC-PKT-",
            VNF_DIRECT_FRAME_COUNT + index);
        slice_status = packet_slice_write(
            &slice, expected, sizeof(expected));
        vnf_require(slice_status == PACKET_SLICE_OK, "packet TX fill");
        slice_status = packet_slice_seek(&slice, 0U);
        vnf_require(slice_status == PACKET_SLICE_OK, "packet TX seek");

        vnf_require(
            hfoc_netif_output_submit(
                &vnf_packet_tx_state.output,
                &slice,
                &output_result) == HFOC_NETIF_OUTPUT_OK,
            "packet output submit");
        if (output_result == HFOC_NETIF_OUTPUT_BACKPRESSURE) {
            vnf_require(
                hfoc_netif_output_submit(
                    &vnf_packet_tx_state.output,
                    &slice,
                    &output_result) == HFOC_NETIF_OUTPUT_OK,
                "packet output retry");
        }
        vnf_require(
            output_result == HFOC_NETIF_OUTPUT_SUBMITTED,
            "packet output submitted");
    }
    vnf_require(
        virtio_net_tx_submitted_count(vnf_packet_tx_state.tx) ==
            VNF_FRAME_COUNT &&
        hfoc_netif_output_pending_count(
            &vnf_packet_tx_state.output) == VNF_PACKET_FRAME_COUNT,
        "packet TX pressure submitted count");
    vnf_packet_pressure_refresh(0U, 0U);
    serial_write("[PACKET-BOUNDARY] tx ready\n");
}

static void vnf_packet_reclaim_tx_frames(void)
{
    uint32_t index;

    for (index = 0U; index < VNF_PACKET_FRAME_COUNT; ++index) {
        struct virtio_net_tx_completion completion;
        enum virtio_net_tx_status tx_status;
        enum hfoc_netif_output_status output_status;
        enum packet_pool_status pool_status;
        uint8_t expected[VNF_TX_FRAME_LENGTH];
        const uint8_t *frame_virtual =
            (const uint8_t *)vnf_packet_tx_state.frame_virtual_base +
            (VNF_DIRECT_FRAME_COUNT + index) *
                VNF_TX_FRAME_LENGTH;

        vnf_build_expected_frame(
            expected,
            VNF_TX_FRAME_LENGTH,
            (const uint8_t[6]){0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U},
            (const uint8_t[6]){0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U},
            VNF_PACKET_TX_ETHER_TYPE,
            "HFOC-PKT-",
            VNF_DIRECT_FRAME_COUNT + index);
        vnf_require(
            memcmp(frame_virtual, expected, sizeof(expected)) == 0,
            "packet TX exact bytes before reclaim");
        tx_status = virtio_net_tx_reclaim(
            vnf_packet_tx_state.tx, &completion);
        vnf_require(
            tx_status == VIRTIO_NET_TX_OK &&
            completion.slot_index == VNF_DIRECT_FRAME_COUNT + index &&
            completion.generation == vnf_packet_tx_state.tx->generation &&
            completion.length == VNF_TX_FRAME_LENGTH,
            "packet TX reclaim");
        output_status = hfoc_netif_output_complete(
            &vnf_packet_tx_state.output,
            &vnf_packet_tx_state.submissions[index]);
        vnf_require(
            output_status == HFOC_NETIF_OUTPUT_OK,
            "packet output completion");
        pool_status = packet_pool_release(
            &vnf_packet_rx_state.pool,
            &vnf_packet_tx_state.slots[index]);
        vnf_require(pool_status == PACKET_POOL_OK, "packet TX release");
    }
    vnf_require(
        virtio_net_tx_submitted_count(vnf_packet_tx_state.tx) == 0U &&
        virtio_net_tx_reclaimed_count(vnf_packet_tx_state.tx) ==
            VNF_FRAME_COUNT &&
        hfoc_netif_output_pending_count(
            &vnf_packet_tx_state.output) == 0U &&
        packet_pool_free_count(&vnf_packet_rx_state.pool) ==
            VNF_PACKET_POOL_COUNT - vnf_packet_rx_state.accepted_count,
        "packet TX balanced ownership");
    vnf_packet_pressure_refresh(0U, vnf_packet_tx_state.backpressure_count);
    serial_write("[PACKET-BOUNDARY] tx ok\n");
}

static void vnf_packet_release_rx_ownership(void)
{
    struct packet_pressure_snapshot snapshot;
    uint32_t index;

    for (index = 0U; index < vnf_packet_rx_state.accepted_count; ++index) {
        vnf_require(
            packet_pool_release(
                &vnf_packet_rx_state.pool,
                &vnf_packet_rx_state.accepted[index]) == PACKET_POOL_OK,
            "packet RX accepted release");
    }
    vnf_packet_rx_state.accepted_count = 0U;
    vnf_require(
        packet_pool_free_count(&vnf_packet_rx_state.pool) ==
            VNF_PACKET_POOL_COUNT &&
        packet_pool_reserved_count(&vnf_packet_rx_state.pool) == 0U &&
        packet_pool_active_count(&vnf_packet_rx_state.pool) == 0U,
        "packet balanced pool counts");
    vnf_packet_pressure_refresh(0U, 0U);
    vnf_require(
        packet_pressure_snapshot(
            &vnf_packet_rx_state.pressure,
            &snapshot) == PACKET_PRESSURE_OK &&
        snapshot.pool_free_count == VNF_PACKET_POOL_COUNT &&
        snapshot.pool_in_use_count == 0U &&
        snapshot.pool_high_water_count == VNF_FRAME_COUNT &&
        snapshot.invalid_frame_count == VNF_PACKET_INVALID_RX_COUNT &&
        snapshot.tx_backpressure_count ==
            vnf_packet_tx_state.backpressure_count &&
        snapshot.generation > 1U,
        "packet pressure final snapshot");
    serial_write("[PACKET-BOUNDARY] pressure\n");
    serial_write("[PACKET-BOUNDARY] balanced pool\n");
}
#endif

#ifdef HFOC_TCP_LISTENER_QEMU_TEST
static void vnf_tcp_fail(const char *message)
    __attribute__((noreturn));

static void vnf_tcp_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[TCP-LISTENER] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void vnf_tcp_require(bool condition, const char *message)
{
    if (!condition) {
        vnf_tcp_fail(message);
    }
}

static void vnf_tcp_mark(const char *marker)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(marker);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

static void vnf_tcp_repost_rx(
    struct virtio_net_rx *rx,
    uint32_t buffer_index)
{
    struct virtio_net_queue *queue = &rx->composition->receive;
    struct virtio_net_rx_buffer *buffer = &rx->buffers[buffer_index];
    struct virtio_descriptor_token descriptor;
    struct virtio_available_token available;
    uint16_t chain[1];

    vnf_tcp_require(
        buffer_index < rx->capacity && !buffer->posted,
        "TCP RX repost state");
    vnf_tcp_require(
        virtio_descriptor_ownership_reserve(
            &queue->descriptors,
            &descriptor) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
        "TCP RX descriptor reserve");
    vnf_tcp_require(
        virtio_descriptor_ownership_publish(
            &queue->descriptors,
            &descriptor) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
        "TCP RX descriptor publish");
    chain[0] = (uint16_t)descriptor.index;
    vnf_tcp_require(
        virtio_available_publication_prepare(
            &queue->available,
            chain,
            1U,
            &available) == VIRTIO_AVAILABLE_PUBLICATION_OK,
        "TCP RX available prepare");
    vnf_tcp_require(
        virtio_available_publication_publish(
            &queue->available,
            &available) == VIRTIO_AVAILABLE_PUBLICATION_OK,
        "TCP RX available publish");
    vnf_write_descriptor(
        &vnf_queues[0],
        descriptor.index,
        buffer->physical_base,
        buffer->length,
        2U,
        0U);
    buffer->posted = true;
    buffer->descriptor = descriptor;
    buffer->generation = descriptor.generation;
    ++rx->posted_count;
    vnf_notify_queue(
        0U,
        (uint16_t)virtio_available_publication_visible_index(
            &queue->available));
}

static void vnf_tcp_account_rx_completion(struct virtio_net_rx *rx)
{
    struct virtio_net_queue *queue = &rx->composition->receive;

    /*
     * The proof maintains the available publication count as active buffers.
     * Each used completion consumes one previously posted available slot, and
     * the used controller's cumulative count is not part of ring indexing.
     */
    vnf_tcp_require(
        queue->available.published_count > 0U,
        "TCP RX available active count");
    --queue->available.published_count;
    queue->used.consumed_count = 0U;
}

static const void *vnf_tcp_rx_payload_for_next(struct virtio_net_rx *rx)
{
    struct virtio_used_ring *ring = vnf_queues[0].used;
    struct virtio_used_consumption *used = &rx->composition->receive.used;
    uint32_t distance = (uint32_t)(uint16_t)(
        ring->idx - (uint16_t)used->expected_idx);
    uint32_t slot;
    uint32_t index;

    if (distance == 0U) {
        return NULL;
    }
    slot = used->expected_idx % rx->queue_size;
    for (index = 0U; index < rx->capacity; ++index) {
        const struct virtio_net_rx_buffer *buffer = &rx->buffers[index];

        if (buffer->posted &&
            buffer->descriptor.index == ring->ring[slot].id) {
            return (const void *)(uintptr_t)(
                vnf_hhdm + buffer->physical_base);
        }
    }
    vnf_tcp_fail("TCP RX used head ownership");
}

static void vnf_tcp_account_tx_reclaim(void)
{
    struct virtio_net_tx *tx = vnf_tcp_proof.tx;
    struct virtio_net_queue *queue = &vnf_tcp_proof.composition->transmit;

    vnf_tcp_require(
        tx->reclaimed_count > 0U &&
            queue->available.published_count > 0U,
        "TCP TX reclaim accounting");
    --tx->reclaimed_count;
    --queue->available.published_count;
    queue->used.consumed_count = 0U;
}

static void vnf_tcp_wait_tx_reclaim(void)
{
    struct virtio_net_tx_completion completion;

    while (vnf_tcp_proof.tx->submitted_count != 0U) {
        enum virtio_net_tx_status status = virtio_net_tx_reclaim(
            vnf_tcp_proof.tx, &completion);

        if (status == VIRTIO_NET_TX_USED_EMPTY) {
            __asm__ volatile ("pause" : : : "memory");
            continue;
        }
        vnf_tcp_require(status == VIRTIO_NET_TX_OK, "TCP TX reclaim");
        vnf_tcp_require(
            completion.generation == vnf_tcp_proof.tx->generation,
            "TCP TX reclaim generation");
        vnf_tcp_account_tx_reclaim();
    }
}

static err_t vnf_tcp_linkoutput(
    struct netif *netif,
    struct pbuf *frame)
{
    struct virtio_net_tx_frame tx_frame;
    struct virtio_net_tx_submission submission;
    enum virtio_net_tx_status status;
    uint32_t length;
    uint32_t frame_length;

    (void)netif;
    length = frame->tot_len;
    vnf_tcp_require(
        length > 0U && length <= VNF_TCP_TX_FRAME_SIZE,
        "TCP TX frame bounds");
    vnf_tcp_wait_tx_reclaim();
    vnf_tcp_require(
        pbuf_copy_partial(
            frame,
            vnf_tcp_proof.tx_frame_virtual,
            length,
            0U) == length,
        "TCP TX frame copy");
    frame_length = length;
    if (frame_length < VIRTIO_NET_RX_ETHERNET_FRAME_MIN) {
        memset(
            (uint8_t *)vnf_tcp_proof.tx_frame_virtual + length,
            0,
            VIRTIO_NET_RX_ETHERNET_FRAME_MIN - length);
        frame_length = VIRTIO_NET_RX_ETHERNET_FRAME_MIN;
    }

    tx_frame.data = vnf_tcp_proof.tx_frame_virtual;
    tx_frame.physical_base = vnf_tcp_proof.tx_frame_physical;
    tx_frame.length = frame_length;
    vnf_tx_notify_state.header_physical_base =
        vnf_tcp_proof.tx->headers[0].physical_base;
    vnf_tx_notify_state.frame_physical_base =
        vnf_tcp_proof.tx_frame_physical;
    vnf_tx_notify_state.frame_length = frame_length;
    status = virtio_net_tx_submit(
        vnf_tcp_proof.tx,
        &tx_frame,
        &submission);
    if (status == VIRTIO_NET_TX_QUEUE_FULL) {
        vnf_tcp_wait_tx_reclaim();
        status = virtio_net_tx_submit(
            vnf_tcp_proof.tx,
            &tx_frame,
            &submission);
    }
    vnf_tcp_require(status == VIRTIO_NET_TX_OK, "TCP TX submission");
    vnf_tcp_require(
        submission.generation == vnf_tcp_proof.tx->generation,
        "TCP TX submission generation");
    vnf_notify_queue(
        1U,
        (uint16_t)virtio_available_publication_visible_index(
            &vnf_tcp_proof.composition->transmit.available));
    return ERR_OK;
}

#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST

static err_t vnf_netres_conn_event(
    void *context,
    struct hfoc_lwip_tcp_connection *connection,
    enum hfoc_lwip_tcp_conn_event event,
    struct pbuf *p,
    uint16_t len,
    err_t err);

static void vnf_netres_backlog_tick(
    struct vnf_tcp_proof_state *proof);

static void vnf_netres_publish_connection(
    struct vnf_tcp_proof_state *proof,
    struct tcp_pcb *pcb)
{
    struct vnf_tcp_connection_state *connection_state = NULL;
    struct hfoc_lwip_tcp_deadline_policy deadline_policy;
    uint32_t now_ms = hfoc_lwip_time_now();
    uint32_t index;

    for (index = 0U; index < VNF_TCP_MAX_CONNECTIONS; ++index) {
        if (!proof->connections[index].active) {
            connection_state = &proof->connections[index];
            break;
        }
    }
    vnf_tcp_require(connection_state != NULL,
                    "network resource free pool connection");

    memset(connection_state, 0, sizeof(*connection_state));
    connection_state->active = false;
    connection_state->index = index;
    connection_state->proof = proof;
    connection_state->callbacks.event = vnf_netres_conn_event;
    connection_state->callbacks.context = connection_state;
    vnf_tcp_require(
        hfoc_lwip_tcp_pool_reserve(
            &proof->pool, &connection_state->token) ==
            HFOC_LWIP_TCP_POOL_OK,
        "network resource pool reserve");
    vnf_tcp_require(
        hfoc_lwip_tcp_pool_publish(
            &proof->pool, &connection_state->token, pcb,
            &connection_state->callbacks) == HFOC_LWIP_TCP_POOL_OK,
        "network resource pool publish");

    memset(&deadline_policy, 0, sizeof(deadline_policy));
    deadline_policy.version = HFOC_LWIP_TCP_DEADLINE_VERSION;
    deadline_policy.accept_timeout_ms = 10000U;
    deadline_policy.progress_timeout_ms = 10000U;
    deadline_policy.idle_timeout_ms = VNF_NETRES_IDLE_TIMEOUT_MS;
    deadline_policy.close_timeout_ms = 2000U;
    vnf_tcp_require(
        hfoc_lwip_tcp_deadline_attach(
            &proof->deadline, &connection_state->token,
            &deadline_policy, now_ms) ==
            HFOC_LWIP_TCP_DEADLINE_OK,
        "network resource deadline attach");
    vnf_tcp_require(
        hfoc_lwip_tcp_quarantine_attach(
            &proof->quarantine, &connection_state->token,
            &proof->pool.slots[index].connection,
            proof->pool.slots[index].connection.pcb) ==
            HFOC_LWIP_TCP_QUARANTINE_OK,
        "network resource quarantine attach");
    vnf_tcp_require(
        hfoc_lwip_tcp_close_policy_attach(
            &proof->close_policy, &proof->pool, &proof->quarantine,
            &connection_state->token) ==
            HFOC_LWIP_TCP_CLOSE_POLICY_OK,
        "network resource close policy attach");

    connection_state->active = true;
    ++proof->active_count;
    ++proof->accept_count;
    if (proof->restart_ok != 0U &&
        proof->accept_count > VNF_TCP_MAX_CONNECTIONS &&
        proof->active_count == 1U &&
        proof->restart_live_ok == 0U) {
        vnf_tcp_mark("[NET-RESOURCE] restart live");
        proof->restart_live_ok = 1U;
    }
}

static void vnf_netres_backlog_tick(
    struct vnf_tcp_proof_state *proof)
{
    uint32_t now_ms = hfoc_lwip_time_now();
    bool pressure;

    if (proof->backlog.pending_count != 0U &&
        !proof->backlog_drain_armed) {
        proof->backlog_drain_armed = true;
        proof->backlog_drain_deadline_ms =
            now_ms + VNF_NETRES_BACKLOG_SETTLE_MS;
    }
    pressure = proof->backlog.pending_count >=
        HFOC_LWIP_TCP_BACKLOG_CAPACITY;
    if (pressure && !proof->backlog_pressure_ok) {
        vnf_tcp_mark("[NET-RESOURCE] backlog pressure");
        proof->backlog_pressure_ok = 1U;
    }
    if (proof->backlog.pending_count != 0U &&
        (pressure ||
         (proof->backlog_drain_armed &&
          (int32_t)(now_ms - proof->backlog_drain_deadline_ms) >= 0))) {
        while (proof->backlog.pending_count != 0U &&
               proof->pool.published_count <
                   HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
            struct hfoc_lwip_tcp_backlog_token token;
            struct tcp_pcb *pcb = NULL;

            vnf_tcp_require(
                hfoc_lwip_tcp_backlog_drain(
                    &proof->backlog, &token, &pcb) ==
                    HFOC_LWIP_TCP_BACKLOG_OK &&
                pcb != NULL,
                "network resource backlog drain");
            vnf_netres_publish_connection(proof, pcb);
        }
    }
    if (proof->backlog_pressure_ok != 0U &&
        proof->backlog.pending_count == 0U &&
        proof->backlog_drained_ok == 0U) {
        vnf_tcp_mark("[NET-RESOURCE] backlog ok");
        proof->backlog_drained_ok = 1U;
    }
}

static void vnf_netres_release_connection(
    struct vnf_tcp_proof_state *proof,
    struct vnf_tcp_connection_state *connection_state)
{
    struct hfoc_lwip_tcp_pool_token token;

    if (!connection_state->active) {
        return;
    }
    token = connection_state->token;
    if (proof->pool.slots[token.index].state ==
        HFOC_LWIP_TCP_POOL_STATE_FREE) {
        connection_state->active = false;
        return;
    }
    vnf_tcp_require(
        hfoc_lwip_tcp_deadline_cancel(
            &proof->deadline, &token) ==
            HFOC_LWIP_TCP_DEADLINE_OK,
        "network resource deadline cancel");
    vnf_tcp_require(
        hfoc_lwip_tcp_close_policy_detach(
            &proof->close_policy, &proof->pool, &proof->quarantine,
            &token) == HFOC_LWIP_TCP_CLOSE_POLICY_OK,
        "network resource close policy detach");
    vnf_tcp_require(
        hfoc_lwip_tcp_quarantine_detach(
            &proof->quarantine, &token) ==
            HFOC_LWIP_TCP_QUARANTINE_OK,
        "network resource quarantine detach");
    vnf_tcp_require(
        hfoc_lwip_tcp_pool_release(
            &proof->pool, &token) == HFOC_LWIP_TCP_POOL_OK,
        "network resource pool release");
    connection_state->active = false;
    if (proof->active_count > 0U) {
        --proof->active_count;
    }
    vnf_netres_backlog_tick(proof);
}

static err_t vnf_netres_accept_pcb(
    void *context,
    struct tcp_pcb *pcb,
    uint32_t generation)
{
    struct vnf_tcp_proof_state *proof =
        (struct vnf_tcp_proof_state *)context;
    struct hfoc_lwip_tcp_backlog_token token;

    (void)generation;
    if (proof->backlog.pending_count >=
            HFOC_LWIP_TCP_BACKLOG_CAPACITY ||
        proof->pool.published_count ==
            HFOC_LWIP_TCP_POOL_SLOT_COUNT) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    vnf_tcp_require(
        hfoc_lwip_tcp_backlog_admit(
            &proof->backlog, pcb, &token) ==
            HFOC_LWIP_TCP_BACKLOG_OK,
        "network resource backlog admit");
    vnf_netres_backlog_tick(proof);
    return ERR_OK;
}

static void vnf_netres_packet_pressure(
    struct vnf_tcp_proof_state *proof)
{
    struct packet_pressure_snapshot snapshot;
    uint32_t index;

    for (index = 0U; index < VNF_NETRES_PACKET_COUNT; ++index) {
        vnf_tcp_require(
            packet_pool_reserve(
                &proof->packet_pool, PACKET_POOL_DIRECTION_RX,
                &proof->packet_tokens[index]) == PACKET_POOL_OK,
            "network resource packet pool reserve");
    }
    proof->packet_pressure_reserved = VNF_NETRES_PACKET_COUNT;
    vnf_tcp_require(
        packet_pressure_begin_update(&proof->packet_pressure) ==
            PACKET_PRESSURE_OK &&
        packet_pressure_refresh_pool_counts(
            &proof->packet_pressure) == PACKET_PRESSURE_OK &&
        packet_pressure_record_rx_drop(
            &proof->packet_pressure, 1U) == PACKET_PRESSURE_OK &&
        packet_pressure_record_tx_backpressure(
            &proof->packet_pressure, 2U) == PACKET_PRESSURE_OK &&
        packet_pressure_commit(&proof->packet_pressure) ==
            PACKET_PRESSURE_OK,
        "network resource packet pressure update");
    vnf_tcp_require(
        packet_pressure_snapshot(
            &proof->packet_pressure, &snapshot) == PACKET_PRESSURE_OK &&
        snapshot.pool_free_count == 0U &&
        snapshot.pool_in_use_count == VNF_NETRES_PACKET_COUNT &&
        snapshot.pool_high_water_count == VNF_NETRES_PACKET_COUNT &&
        snapshot.rx_drop_count == 1U &&
        snapshot.tx_backpressure_count == 2U,
        "network resource packet pressure high water");

    for (index = 0U; index < VNF_NETRES_PACKET_COUNT; ++index) {
        vnf_tcp_require(
            packet_pool_release(
                &proof->packet_pool,
                &proof->packet_tokens[index]) == PACKET_POOL_OK,
            "network resource packet pool release");
    }
    vnf_tcp_require(
        packet_pressure_begin_update(&proof->packet_pressure) ==
            PACKET_PRESSURE_OK &&
        packet_pressure_refresh_pool_counts(
            &proof->packet_pressure) == PACKET_PRESSURE_OK &&
        packet_pressure_commit(&proof->packet_pressure) ==
            PACKET_PRESSURE_OK,
        "network resource packet pressure final update");
    vnf_tcp_require(
        packet_pressure_snapshot(
            &proof->packet_pressure, &snapshot) == PACKET_PRESSURE_OK &&
        snapshot.pool_free_count == VNF_NETRES_PACKET_COUNT &&
        snapshot.pool_in_use_count == 0U &&
        snapshot.pool_high_water_count == VNF_NETRES_PACKET_COUNT,
        "network resource packet pressure final snapshot");
    proof->packet_pressure_ok = 1U;
    vnf_tcp_mark("[NET-RESOURCE] packet pressure ok");
}

static void vnf_netres_request_restart(
    struct vnf_tcp_proof_state *proof)
{
    uint32_t now_ms = hfoc_lwip_time_now();
    uint32_t index;

    vnf_tcp_require(
        hfoc_lwip_tcp_drain_restart_stop_admission(
            &proof->drain, now_ms) ==
            HFOC_LWIP_TCP_DRAIN_RESTART_OK,
        "network resource stop admission");
    vnf_tcp_require(
        hfoc_lwip_tcp_drain_restart_step(
            &proof->drain,
            now_ms + VNF_NETRES_DRAIN_TIMEOUT_MS + 1000U) ==
            HFOC_LWIP_TCP_DRAIN_RESTART_OK,
        "network resource drain");
    vnf_tcp_require(
        hfoc_lwip_tcp_drain_restart_finish_drain(
            &proof->drain) == HFOC_LWIP_TCP_DRAIN_RESTART_OK,
        "network resource finish drain");
    vnf_tcp_require(
        hfoc_lwip_tcp_drain_restart_restart(
            &proof->drain) == HFOC_LWIP_TCP_DRAIN_RESTART_OK,
        "network resource restart");
    proof->alloc_start = hfoc_lwip_alloc_snapshot();

    for (index = 0U; index < VNF_TCP_MAX_CONNECTIONS; ++index) {
        vnf_tcp_require(
            proof->pool.slots[index].state ==
                HFOC_LWIP_TCP_POOL_STATE_FREE,
            "network resource post-drain pool free");
        memset(&proof->connections[index], 0,
               sizeof(proof->connections[index]));
    }
    proof->active_count = 0U;
    proof->restart_ok = 1U;
    vnf_tcp_mark("[NET-RESOURCE] restart ok");
}

static err_t vnf_netres_conn_event(
    void *context,
    struct hfoc_lwip_tcp_connection *connection,
    enum hfoc_lwip_tcp_conn_event event,
    struct pbuf *p,
    uint16_t len,
    err_t err)
{
    struct vnf_tcp_connection_state *connection_state =
        (struct vnf_tcp_connection_state *)context;
    struct vnf_tcp_proof_state *proof = connection_state->proof;
    uint8_t payload[64];
    uint8_t response[64];
    size_t payload_length;
    size_t prefix_length;
    size_t response_length;

    (void)len;
    (void)err;
    switch (event) {
    case HFOC_LWIP_TCP_CONN_EVENT_ACCEPT:
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_RECEIVE:
        if (p == NULL) {
            break;
        }
        vnf_tcp_require(
            hfoc_lwip_tcp_quarantine_check(
                &proof->quarantine, &connection_state->token,
                connection, connection->pcb,
                HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
                HFOC_LWIP_TCP_QUARANTINE_KIND_RECEIVE) ==
                HFOC_LWIP_TCP_QUARANTINE_OK,
            "network resource quarantine receive");
        vnf_tcp_require(
            hfoc_lwip_tcp_deadline_touch(
                &proof->deadline, &connection_state->token,
                hfoc_lwip_time_now()) == HFOC_LWIP_TCP_DEADLINE_OK,
            "network resource deadline touch");
        payload_length = p->tot_len;
        vnf_tcp_require(
            payload_length > 0U &&
                payload_length <= sizeof(payload),
            "network resource receive bounds");
        vnf_tcp_require(
            pbuf_copy_partial(
                p, payload, payload_length, 0U) == payload_length,
            "network resource receive copy");
        pbuf_free(p);
        prefix_length = sizeof(VNF_NETRES_RESPONSE_PREFIX) - 1U;
        memcpy(response, VNF_NETRES_RESPONSE_PREFIX, prefix_length);
        memcpy(response + prefix_length, payload, payload_length);
        response_length = prefix_length + payload_length;
        if (connection->pcb != NULL &&
            tcp_write(
                connection->pcb, response, (u16_t)response_length,
                TCP_WRITE_FLAG_COPY) == ERR_OK) {
            tcp_recved(connection->pcb, (u16_t)payload_length);
            (void)tcp_output(connection->pcb);
            ++proof->echo_count;
        }
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_SENT:
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_POLL:
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_CLOSE:
        if (connection->pcb != NULL) {
            tcp_abort(connection->pcb);
            connection->pcb = NULL;
            connection->state = HFOC_LWIP_TCP_CONN_STATE_CLOSED;
        }
        vnf_netres_release_connection(proof, connection_state);
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_ERROR:
        if (err == ERR_RST && proof->reset_count == 0U) {
            vnf_tcp_mark("[NET-RESOURCE] reset ok");
        }
        if (err == ERR_RST) {
            ++proof->reset_count;
        }
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_ABORT:
        break;
    default:
        vnf_tcp_fail("network resource unknown event");
    }
    return ERR_OK;
}

static void vnf_netres_check_timeouts(void)
{
    uint32_t now_ms = hfoc_lwip_time_now();
    uint32_t index;

    for (index = 0U; index < VNF_TCP_MAX_CONNECTIONS; ++index) {
        struct vnf_tcp_connection_state *connection_state =
            &vnf_tcp_proof.connections[index];
        struct tcp_pcb *pcb = NULL;
        enum hfoc_lwip_tcp_deadline_status deadline_status;
        enum hfoc_lwip_tcp_deadline_kind expired_kind;
        enum hfoc_lwip_tcp_close_policy_status close_status;

        if (!connection_state->active) {
            continue;
        }
        pcb = vnf_tcp_proof.pool.slots[
            connection_state->token.index].connection.pcb;
        if (pcb == NULL) {
            continue;
        }
        deadline_status = hfoc_lwip_tcp_deadline_check(
            &vnf_tcp_proof.deadline,
            &connection_state->token, now_ms, &expired_kind);
        vnf_tcp_require(
            deadline_status == HFOC_LWIP_TCP_DEADLINE_EXPIRED ||
            deadline_status == HFOC_LWIP_TCP_DEADLINE_OK,
            "network resource deadline check");
        if (expired_kind == HFOC_LWIP_TCP_DEADLINE_KIND_NONE) {
            continue;
        }
        close_status = hfoc_lwip_tcp_close_policy_handle(
            &vnf_tcp_proof.close_policy,
            &vnf_tcp_proof.pool,
            &vnf_tcp_proof.quarantine,
            &connection_state->token,
            HFOC_LWIP_TCP_CLOSE_POLICY_REASON_TIMEOUT_ABORT);
        vnf_tcp_require(
            close_status == HFOC_LWIP_TCP_CLOSE_POLICY_ABORTED,
            "network resource timeout abort");
        ++vnf_tcp_proof.timeout_count;
        if (vnf_tcp_proof.timeout_count == 1U) {
            vnf_tcp_mark("[NET-RESOURCE] timeout ok");
        }
        vnf_netres_release_connection(
            &vnf_tcp_proof, connection_state);
    }
}

#endif

static void vnf_tcp_release_connection(
    struct vnf_tcp_connection_state *connection_state,
    bool reset)
{
    if (!connection_state->active) {
        return;
    }
    if (reset) {
        if (vnf_tcp_proof.active_count ==
                VNF_TCP_MAX_CONNECTIONS &&
            !vnf_tcp_proof.backlog_ok) {
            vnf_tcp_proof.backlog_ok = 1U;
            vnf_tcp_mark("[TCP-LISTENER] backlog ok");
        }
        ++vnf_tcp_proof.reset_count;
        if (vnf_tcp_proof.reset_count == 1U) {
            vnf_tcp_mark("[TCP-LISTENER] reset ok");
        }
    }
    connection_state->active = false;
    if (vnf_tcp_proof.active_count > 0U) {
        --vnf_tcp_proof.active_count;
    }
}

static err_t vnf_tcp_conn_event(
    void *context,
    struct hfoc_lwip_tcp_connection *connection,
    enum hfoc_lwip_tcp_conn_event event,
    struct pbuf *p,
    uint16_t len,
    err_t err)
{
#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
    return vnf_netres_conn_event(
        context, connection, event, p, len, err);
#else
    struct vnf_tcp_connection_state *connection_state =
        (struct vnf_tcp_connection_state *)context;
    struct vnf_tcp_proof_state *proof =
        connection_state->proof;
    uint8_t payload[64];
    uint8_t response[64];
    size_t payload_length;
    size_t prefix_length;
    size_t response_length;

    (void)len;
    (void)err;
    if (event != HFOC_LWIP_TCP_CONN_EVENT_POLL) {
        connection_state->last_activity_ms = hfoc_lwip_time_now();
    }

    switch (event) {
    case HFOC_LWIP_TCP_CONN_EVENT_ACCEPT:
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_RECEIVE:
        if (p == NULL) {
            break;
        }
        payload_length = p->tot_len;
        vnf_tcp_require(
            payload_length > 0U &&
                payload_length <= sizeof(payload),
            "TCP receive bounds");
        vnf_tcp_require(
            pbuf_copy_partial(
                p,
                payload,
                payload_length,
                0U) == payload_length,
            "TCP receive copy");
        pbuf_free(p);
        prefix_length = sizeof(VNF_TCP_RESPONSE_PREFIX) - 1U;
        memcpy(response, VNF_TCP_RESPONSE_PREFIX, prefix_length);
        memcpy(response + prefix_length, payload, payload_length);
        response_length = prefix_length + payload_length;
        if (connection->pcb != NULL) {
            if (tcp_write(
                    connection->pcb,
                    response,
                    (u16_t)response_length,
                    TCP_WRITE_FLAG_COPY) == ERR_OK) {
                tcp_recved(connection->pcb, (u16_t)payload_length);
                (void)tcp_output(connection->pcb);
                ++proof->echo_count;
                if (proof->echo_count == 1U) {
                    vnf_tcp_mark("[TCP-LISTENER] echo ok");
                }
            }
        }
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_SENT:
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_ERROR:
        vnf_tcp_release_connection(
            connection_state,
            err == ERR_RST);
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_POLL:
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_CLOSE:
        if (connection->pcb != NULL) {
            err_t close_err = tcp_close(connection->pcb);

            connection->pcb = NULL;
            vnf_tcp_require(close_err == ERR_OK, "TCP close");
        }
        vnf_tcp_release_connection(connection_state, false);
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_ABORT:
        vnf_tcp_release_connection(connection_state, false);
        break;
    default:
        vnf_tcp_fail("TCP unknown event");
    }
    return ERR_OK;
#endif
}

static err_t vnf_tcp_accept(
    void *context,
    struct tcp_pcb *pcb,
    uint32_t generation)
{
#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
    return vnf_netres_accept_pcb(context, pcb, generation);
#else
    struct vnf_tcp_proof_state *proof =
        (struct vnf_tcp_proof_state *)context;
    struct vnf_tcp_connection_state *connection_state = NULL;
    enum hfoc_lwip_tcp_conn_status status;
    uint32_t index;

    if (proof->active_count >= VNF_TCP_MAX_CONNECTIONS) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    for (index = 0U; index < VNF_TCP_MAX_CONNECTIONS; ++index) {
        if (!proof->connections[index].active) {
            connection_state = &proof->connections[index];
            break;
        }
    }
    if (connection_state == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    memset(connection_state, 0, sizeof(*connection_state));
    connection_state->active = true;
    connection_state->index = index;
    connection_state->proof = proof;
    connection_state->callbacks.event = vnf_tcp_conn_event;
    connection_state->callbacks.context = connection_state;
    status = hfoc_lwip_tcp_conn_accept(
        &connection_state->connection,
        pcb,
        generation,
        &connection_state->callbacks);
    if (status != HFOC_LWIP_TCP_CONN_OK) {
        connection_state->active = false;
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    connection_state->last_activity_ms = hfoc_lwip_time_now();
    ++proof->active_count;
    ++proof->accept_count;
    if (proof->accept_count == 1U) {
        vnf_tcp_mark("[TCP-LISTENER] accept ok");
    }
    return ERR_OK;
#endif
}

static void vnf_tcp_check_timeouts(void)
{
#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
    vnf_netres_check_timeouts();
#else
    uint32_t now_ms = hfoc_lwip_time_now();
    uint32_t index;

    if (vnf_tcp_proof.timeout_count != 0U) {
        return;
    }
    for (index = 0U; index < VNF_TCP_MAX_CONNECTIONS; ++index) {
        struct vnf_tcp_connection_state *connection_state =
            &vnf_tcp_proof.connections[index];

        if (!connection_state->active ||
            connection_state->connection.pcb == NULL) {
            continue;
        }
        if ((uint32_t)(now_ms -
                connection_state->last_activity_ms) >=
            VNF_TCP_IDLE_TIMEOUT_MS) {
            tcp_abort(connection_state->connection.pcb);
            ++vnf_tcp_proof.timeout_count;
            if (vnf_tcp_proof.timeout_count == 1U) {
                vnf_tcp_mark("[TCP-LISTENER] timeout ok");
            }
            vnf_tcp_release_connection(connection_state, false);
        }
    }
#endif
}

static void vnf_tcp_feed_rx_frame(
    const struct virtio_net_rx_frame *frame)
{
    struct pbuf *pbuf;

    vnf_tcp_require(
        frame->length >= VIRTIO_NET_RX_ETHERNET_FRAME_MIN &&
            frame->length <= VIRTIO_NET_RX_ETHERNET_FRAME_MAX,
        "TCP RX Ethernet bounds");
    pbuf = pbuf_alloc(PBUF_RAW, (u16_t)frame->length, PBUF_RAM);
    vnf_tcp_require(pbuf != NULL, "TCP RX pbuf allocation");
    vnf_tcp_require(
        frame->length <= (size_t)pbuf->len &&
            pbuf->payload != NULL,
        "TCP RX pbuf capacity");
    memcpy(pbuf->payload, frame->data, frame->length);
    (void)ethernet_input(pbuf, &vnf_tcp_proof.netif.netif);
}

static bool vnf_tcp_proof_complete(void)
{
    return vnf_tcp_proof.echo_count >= 2U &&
           vnf_tcp_proof.reset_count >= 1U &&
           vnf_tcp_proof.timeout_count >= 1U &&
           vnf_tcp_proof.backlog_ok != 0U &&
           vnf_tcp_proof.active_count == 0U;
}

static bool vnf_tcp_alloc_balanced(
    const struct hfoc_lwip_alloc_snapshot *before,
    const struct hfoc_lwip_alloc_snapshot *after)
{
    uint32_t pool_class;

    if (before == NULL || after == NULL ||
        before->version != after->version ||
        before->total_used_bytes != after->total_used_bytes ||
        before->total_free_bytes != after->total_free_bytes) {
        return false;
    }
    for (pool_class = 0U; pool_class < HFOC_LWIP_POOL_COUNT;
         ++pool_class) {
        const struct hfoc_lwip_alloc_pool_snapshot *before_pool =
            &before->pools[pool_class];
        const struct hfoc_lwip_alloc_pool_snapshot *after_pool =
            &after->pools[pool_class];

        if (before_pool->slot_size != after_pool->slot_size ||
            before_pool->slot_count != after_pool->slot_count ||
            before_pool->free_count != after_pool->free_count ||
            before_pool->used_count != after_pool->used_count) {
            return false;
        }
    }
    return true;
}

#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
static bool vnf_netres_alloc_balanced(
    const struct hfoc_lwip_alloc_snapshot *before,
    const struct hfoc_lwip_alloc_snapshot *after)
{
    uint32_t pool_class;

    if (before == NULL || after == NULL ||
        before->version != after->version) {
        return false;
    }
    if (before->total_used_bytes <
            HFOC_LWIP_POOL_SLOT_SIZE_TINY ||
        after->total_used_bytes !=
            before->total_used_bytes -
                HFOC_LWIP_POOL_SLOT_SIZE_TINY ||
        after->total_free_bytes !=
            before->total_free_bytes +
                HFOC_LWIP_POOL_SLOT_SIZE_TINY) {
        return false;
    }
    for (pool_class = 0U; pool_class < HFOC_LWIP_POOL_COUNT;
         ++pool_class) {
        const struct hfoc_lwip_alloc_pool_snapshot *before_pool =
            &before->pools[pool_class];
        const struct hfoc_lwip_alloc_pool_snapshot *after_pool =
            &after->pools[pool_class];

        if (before_pool->slot_size != after_pool->slot_size ||
            before_pool->slot_count != after_pool->slot_count) {
            return false;
        }
        if (pool_class == HFOC_LWIP_POOL_CLASS_TINY) {
            if (before_pool->free_count + 1U !=
                    after_pool->free_count ||
                before_pool->used_count !=
                    after_pool->used_count + 1U) {
                return false;
            }
        } else if (before_pool->free_count != after_pool->free_count ||
                   before_pool->used_count != after_pool->used_count) {
            return false;
        }
    }
    return true;
}
#endif

#ifdef HFOC_MBEDTLS_TLS_QEMU_TEST

void mbedtls_platform_zeroize(void *buffer, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

char *strchr(const char *text, int character)
{
    unsigned char wanted = (unsigned char)character;

    if (text == NULL) {
        return NULL;
    }
    for (;;) {
        if ((unsigned char)*text == wanted) {
            return (char *)text;
        }
        if (*text == '\0') {
            return NULL;
        }
        ++text;
    }
}

static void vnf_tls_qemu_fail(const char *message)
    __attribute__((noreturn));

static void vnf_tls_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[TLS-QEMU] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void vnf_tls_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        vnf_tls_qemu_fail(message);
    }
}

static void vnf_tls_qemu_mark(const char *marker)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(marker);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

static void vnf_tls_qemu_putc(
    char *buffer,
    size_t capacity,
    size_t *position,
    char value)
{
    if (*position < capacity) {
        buffer[*position] = value;
    }
    ++*position;
}

static void vnf_tls_qemu_puts(
    char *buffer,
    size_t capacity,
    size_t *position,
    const char *text,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        vnf_tls_qemu_putc(buffer, capacity, position, text[index]);
    }
}

static void vnf_tls_qemu_put_unsigned(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint64_t value,
    unsigned int base,
    bool uppercase)
{
    char digits[64];
    size_t count = 0U;

    do {
        unsigned int digit = (unsigned int)(value % base);

        digits[count++] = (char)(
            digit < 10U ? '0' + digit :
            (uppercase ? 'A' : 'a') + digit - 10U);
        value /= base;
    } while (value != 0U);

    while (count != 0U) {
        --count;
        vnf_tls_qemu_putc(buffer, capacity, position, digits[count]);
    }
}

static void vnf_tls_qemu_pad(
    char *buffer,
    size_t capacity,
    size_t *position,
    unsigned int width,
    bool left,
    char fill)
{
    unsigned int index;

    for (index = 0U; index < width; ++index) {
        vnf_tls_qemu_putc(buffer, capacity, position, fill);
    }
    (void)left;
}

static void vnf_tls_qemu_format_number(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint64_t value,
    bool negative,
    unsigned int base,
    bool uppercase,
    bool left,
    char fill,
    unsigned int width,
    int precision)
{
    char digits[64];
    size_t digit_count = 0U;
    unsigned int padding;

    do {
        unsigned int digit = (unsigned int)(value % base);

        digits[digit_count++] = (char)(
            digit < 10U ? '0' + digit :
            (uppercase ? 'A' : 'a') + digit - 10U);
        value /= base;
    } while (value != 0U);

    if (precision > (int)digit_count) {
        while (digit_count < (size_t)precision) {
            digits[digit_count++] = '0';
        }
    }
    if (negative) {
        vnf_tls_qemu_putc(buffer, capacity, position, '-');
    }
    padding = width > digit_count ? width - (unsigned int)digit_count : 0U;
    if (!left && fill == '0') {
        vnf_tls_qemu_pad(
            buffer, capacity, position, padding, false, fill);
        padding = 0U;
    }
    while (digit_count != 0U) {
        --digit_count;
        vnf_tls_qemu_putc(
            buffer, capacity, position, digits[digit_count]);
    }
    if (left) {
        vnf_tls_qemu_pad(
            buffer, capacity, position, padding, true, ' ');
    }
}

int hfoc_mbedtls_vsnprintf(
    char *s,
    size_t n,
    const char *format,
    va_list arguments);

int hfoc_mbedtls_snprintf(
    char *s,
    size_t n,
    const char *format,
    ...)
{
    va_list arguments;
    int result;

    va_start(arguments, format);
    result = hfoc_mbedtls_vsnprintf(s, n, format, arguments);
    va_end(arguments);
    return result;
}

int hfoc_mbedtls_vsnprintf(
    char *s,
    size_t n,
    const char *format,
    va_list arguments)
{
    size_t position = 0U;
    const char *cursor;

    if (s == NULL || (n != 0U && format == NULL)) {
        return 0;
    }

    for (cursor = format; *cursor != '\0'; ++cursor) {
        bool left = false;
        bool zero_pad = false;
        unsigned int width = 0U;
        int precision = -1;
        bool width_star = false;
        bool precision_star = false;
        char length = '\0';
        char conversion;

        if (*cursor != '%') {
            vnf_tls_qemu_putc(s, n, &position, *cursor);
            continue;
        }
        ++cursor;
        if (*cursor == '\0') {
            break;
        }
        if (*cursor == '%') {
            vnf_tls_qemu_putc(s, n, &position, '%');
            continue;
        }
        while (*cursor == '-' || *cursor == '0' || *cursor == '+' ||
               *cursor == ' ' || *cursor == '#') {
            if (*cursor == '-') {
                left = true;
            } else if (*cursor == '0') {
                zero_pad = true;
            }
            ++cursor;
        }
        if (*cursor == '*') {
            width_star = true;
            ++cursor;
        } else {
            while (*cursor >= '0' && *cursor <= '9') {
                width = width * 10U + (unsigned int)(*cursor - '0');
                ++cursor;
            }
        }
        if (*cursor == '.') {
            ++cursor;
            if (*cursor == '*') {
                precision_star = true;
                ++cursor;
            } else {
                precision = 0;
                while (*cursor >= '0' && *cursor <= '9') {
                    precision = precision * 10 + (*cursor - '0');
                    ++cursor;
                }
            }
        }
        if (*cursor == 'h' || *cursor == 'l' || *cursor == 'z' ||
            *cursor == 'j' || *cursor == 't') {
            length = *cursor;
            ++cursor;
            if (*cursor == 'l' && length == 'l') {
                length = 'L';
                ++cursor;
            }
        }
        if (*cursor == '\0') {
            break;
        }
        conversion = *cursor;
        if (width_star) {
            int star_width = va_arg(arguments, int);

            if (star_width < 0) {
                left = true;
                width = (unsigned int)(-star_width);
            } else {
                width = (unsigned int)star_width;
            }
        }
        if (precision_star) {
            precision = va_arg(arguments, int);
        }

        switch (conversion) {
        case 'c': {
            int value = va_arg(arguments, int);

            vnf_tls_qemu_putc(s, n, &position, (char)value);
            break;
        }
        case 's': {
            const char *text = va_arg(arguments, const char *);
            size_t text_length = 0U;
            unsigned int text_padding;

            if (text == NULL) {
                text = "(null)";
            }
            while (text[text_length] != '\0') {
                ++text_length;
            }
            if (precision >= 0 &&
                text_length > (size_t)precision) {
                text_length = (size_t)precision;
            }
            text_padding = width > text_length ?
                width - (unsigned int)text_length : 0U;
            if (!left) {
                vnf_tls_qemu_pad(
                    s, n, &position, text_padding, false, ' ');
                text_padding = 0U;
            }
            vnf_tls_qemu_puts(s, n, &position, text, text_length);
            if (left) {
                vnf_tls_qemu_pad(
                    s, n, &position, text_padding, true, ' ');
            }
            break;
        }
        case 'd':
        case 'i': {
            int64_t signed_value;
            uint64_t value;
            bool negative = false;

            if (length == 'l' || length == 'L') {
                signed_value = va_arg(arguments, int64_t);
            } else {
                signed_value = (int64_t)va_arg(arguments, int);
            }
            if (signed_value < 0) {
                negative = true;
                value = (uint64_t)(-signed_value);
            } else {
                value = (uint64_t)signed_value;
            }
            vnf_tls_qemu_format_number(
                s, n, &position, value, negative, 10U, false,
                left, zero_pad ? '0' : ' ', width, precision);
            break;
        }
        case 'u': {
            uint64_t value;

            if (length == 'l' || length == 'L') {
                value = va_arg(arguments, uint64_t);
            } else if (length == 'z') {
                value = (uint64_t)va_arg(arguments, size_t);
            } else {
                value = (uint64_t)va_arg(arguments, unsigned int);
            }
            vnf_tls_qemu_format_number(
                s, n, &position, value, false, 10U, false,
                left, zero_pad ? '0' : ' ', width, precision);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t value;

            if (length == 'l' || length == 'L') {
                value = va_arg(arguments, uint64_t);
            } else if (length == 'z') {
                value = (uint64_t)va_arg(arguments, size_t);
            } else {
                value = (uint64_t)va_arg(arguments, unsigned int);
            }
            vnf_tls_qemu_format_number(
                s, n, &position, value, false, 16U,
                conversion == 'X', left, zero_pad ? '0' : ' ',
                width, precision);
            break;
        }
        case 'p': {
            uintptr_t value =
                (uintptr_t)va_arg(arguments, void *);

            vnf_tls_qemu_puts(s, n, &position, "0x", 2U);
            vnf_tls_qemu_format_number(
                s, n, &position, (uint64_t)value, false, 16U,
                false, false, '0', 0U, -1);
            break;
        }
        default:
            vnf_tls_qemu_putc(s, n, &position, conversion);
            break;
        }
    }

    if (n != 0U) {
        if (position < n) {
            s[position] = '\0';
        } else {
            s[n - 1U] = '\0';
        }
    }
    return (int)position;
}

psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *context,
    uint8_t *output,
    size_t output_size,
    size_t *output_length)
{
    size_t index;

    (void)context;
    if (output == NULL || output_length == NULL ||
        (output_size != 0U && vnf_tls_qemu_rng_deny)) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    for (index = 0U; index < output_size; ++index) {
        output[index] = (uint8_t)(index * 37U + 11U);
    }
    *output_length = output_size;
    return PSA_SUCCESS;
}

static int vnf_tls_qemu_rng_cb(
    void *context,
    unsigned char *output,
    size_t length)
{
    size_t index;

    (void)context;
    if (output == NULL || vnf_tls_qemu_rng_deny) {
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        output[index] = (unsigned char)(index * 29U + 3U);
    }
    return 0;
}

static void vnf_tls_qemu_drain_entropy_magic(
    struct vnf_tls_qemu_connection *connection)
{
    size_t available;

    available = connection->rx_length - connection->rx_offset;
    while (available >= VNF_TLS_ENTROPY_MAGIC_LENGTH) {
        if (memcmp(
                &connection->rx[connection->rx_offset],
                VNF_TLS_ENTROPY_MAGIC,
                VNF_TLS_ENTROPY_MAGIC_LENGTH) == 0) {
            connection->rx_offset += VNF_TLS_ENTROPY_MAGIC_LENGTH;
            available -= VNF_TLS_ENTROPY_MAGIC_LENGTH;
            vnf_tls_qemu_rng_deny = true;
            continue;
        }
        break;
    }
    if (connection->rx_offset == connection->rx_length) {
        connection->rx_offset = 0U;
        connection->rx_length = 0U;
    }
}

static int vnf_tls_qemu_ssl_send(
    void *context,
    const unsigned char *buffer,
    size_t length)
{
    struct vnf_tls_qemu_connection *connection = context;
    err_t error;

    if (connection == NULL || connection->pcb == NULL ||
        buffer == NULL || length == 0U) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    error = tcp_write(
        connection->pcb,
        buffer,
        (u16_t)length,
        TCP_WRITE_FLAG_COPY);
    if (error == ERR_MEM) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (error != ERR_OK) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    error = tcp_output(connection->pcb);
    if (error == ERR_MEM) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (error != ERR_OK) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    return (int)length;
}

static int vnf_tls_qemu_ssl_recv(
    void *context,
    unsigned char *buffer,
    size_t length)
{
    struct vnf_tls_qemu_connection *connection = context;
    size_t available;
    size_t received;

    if (connection == NULL || buffer == NULL || length == 0U) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    available = connection->rx_length - connection->rx_offset;
    if (available == 0U) {
        if (connection->peer_closed) {
            return MBEDTLS_ERR_SSL_CONN_EOF;
        }
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    received = available < length ? available : length;
    memcpy(buffer, &connection->rx[connection->rx_offset], received);
    connection->rx_offset += received;
    if (connection->rx_offset == connection->rx_length) {
        connection->rx_offset = 0U;
        connection->rx_length = 0U;
    }
    return (int)received;
}

#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
static void vnf_https_qemu_abort_stale_pcbs(void)
{
    while (tcp_tw_pcbs != NULL) {
        tcp_abort(tcp_tw_pcbs);
    }
    for (;;) {
        struct tcp_pcb *pcb;
        struct tcp_pcb *stale = NULL;

        for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
            if (pcb->state != SYN_RCVD) {
                stale = pcb;
                break;
            }
        }
        if (stale == NULL) {
            break;
        }
        tcp_abort(stale);
    }
}
#endif

#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
static void vnf_https_qemu_detach_tcp_callbacks(
    struct vnf_tls_qemu_connection *connection);
#endif

static void vnf_tls_qemu_release_connection(
    struct vnf_tls_qemu_connection *connection)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;

    if (!connection->active) {
        return;
    }
    if (connection->releasing) {
        return;
    }
    connection->releasing = true;
    if (connection->pcb != NULL) {
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
        /* Closing PCBs can outlive release; quarantine before cleanup. */
        vnf_https_qemu_detach_tcp_callbacks(connection);
#endif
        if (connection->closing) {
            if (tcp_close(connection->pcb) != ERR_OK) {
                tcp_abort(connection->pcb);
            }
        } else {
            tcp_abort(connection->pcb);
        }
        connection->pcb = NULL;
    }
    if (connection->ssl_setup) {
        mbedtls_ssl_free(&connection->ssl);
        connection->ssl_setup = false;
    }
    if (connection->allocator_id != 0U) {
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
        vnf_https_qemu_require(
            hfoc_mbedtls_alloc_connection_exit(),
            "HTTPS allocator connection exit");
        vnf_https_qemu_require(
            hfoc_mbedtls_alloc_connection_close(
                connection->allocator_id),
            "HTTPS allocator connection close");
#else
        (void)hfoc_mbedtls_alloc_connection_exit();
        (void)hfoc_mbedtls_alloc_connection_close(
            connection->allocator_id);
#endif
        connection->allocator_id = 0U;
    }
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    if (connection->parser_started) {
        vnf_https_qemu_require(
            http_parser_release(
                &proof->parser,
                &connection->parser_token) == HTTP_PARSER_OK,
            "HTTPS parser release");
        connection->parser_started = false;
    }
#endif
    connection->active = false;
    connection->releasing = false;
    connection->handshake_done = false;
    connection->response_sent = false;
    connection->close_notify_sent = false;
    connection->rx_length = 0U;
    connection->rx_offset = 0U;
    if (proof->active_count != 0U) {
        --proof->active_count;
    }
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    if (proof->active_count == 0U) {
        vnf_https_qemu_abort_stale_pcbs();
    }
#endif
}

#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
static void vnf_https_qemu_fail(const char *message)
    __attribute__((noreturn));

static void vnf_https_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[HTTPS-QEMU] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void vnf_https_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        vnf_https_qemu_fail(message);
    }
}

static void vnf_https_qemu_mark(const char *marker)
{
    /* Host validation requires contiguous markers; IRQ logs may interleave. */
    uint64_t irq_flags = cpu_irq_save();

    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(marker);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    cpu_irq_restore(irq_flags);
}

#ifdef HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST
/*
 * ITEM-385 live management reachability gate.
 *
 * The appliance has a single network interface today (HFOC_LWIP_NETIF_NUM), so
 * the interface dimension of the policy is carried as a declared index and the
 * source address is read from the real accepted connection: the gate below
 * evaluates the same policy object the operator publishes, for the peer that
 * actually completed the TLS handshake. The acceptance flow additionally drives
 * a console-only publication so a denied peer is proved over the wire, not only
 * in the policy proof.
 */
#define VNF_REACHABILITY_INTERFACE \
    APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_MANAGEMENT

static struct appliance_management_reachability_selection
    vnf_reachability_selection;
static bool vnf_reachability_console_only;
static bool vnf_reachability_admitted_marked;
static bool vnf_reachability_denied_marked;
static bool vnf_reachability_console_only_denied;
static bool vnf_reachability_restored_marked;
static bool vnf_reachability_triggered;

/*
 * The management-plane acceptance flow asks the fixture to publish the
 * console-only policy with this dedicated request. It is a test-fixture input
 * only: it can make the appliance strictly more restrictive for the rest of the
 * flow and never less, and every other QEMU script that shares this image keeps
 * the admitting policy because it never sends this target.
 */
static const char vnf_reachability_trigger_target[] = "/mgmt-reach-proof";

static void vnf_https_qemu_reachability_publish(bool console_only)
{
    struct appliance_management_reachability_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = APPLIANCE_MANAGEMENT_REACHABILITY_VERSION;
    policy.mode = console_only
        ? APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY
        : APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE;
    policy.management_interface = VNF_REACHABILITY_INTERFACE;
    /* A zero management address admits the appliance's own interface. */
    memset(&vnf_reachability_selection, 0,
           sizeof(vnf_reachability_selection));
    vnf_https_qemu_require(
        appliance_management_reachability_selection_init(
            &vnf_reachability_selection, &policy) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_OK,
        "HTTPS reachability publication");
    vnf_reachability_console_only = console_only;
}

static bool vnf_https_qemu_reachability_trigger(
    const struct http_request_line *line)
{
    uint32_t index;
    const uint32_t length =
        (uint32_t)(sizeof(vnf_reachability_trigger_target) - 1U);

    if (vnf_reachability_triggered || line == NULL ||
        line->target == NULL ||
        line->target_length != length) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (line->target[index] !=
            (uint8_t)vnf_reachability_trigger_target[index]) {
            return false;
        }
    }
    vnf_reachability_triggered = true;
    vnf_https_qemu_reachability_publish(true);
    vnf_https_qemu_mark("[MGMT-REACH] console-only published ok");
    return true;
}

static bool vnf_https_qemu_reachability_admit(
    struct vnf_tls_qemu_connection *connection)
{
    struct appliance_management_reachability_peer peer;
    enum appliance_management_reachability_verdict verdict =
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_UNPUBLISHED;
    const ip4_addr_t *remote;
    const uint8_t *octets;

    /*
     * An unpublished policy refuses every network peer, exactly as the policy
     * evaluator does: the gate fails closed rather than open, so a composition
     * that forgot to publish is a visible outage instead of an open perimeter.
     */
    memset(&peer, 0, sizeof(peer));
    peer.interface_index = VNF_REACHABILITY_INTERFACE;
    if (connection->pcb != NULL) {
        remote = ip_2_ip4(&connection->pcb->remote_ip);
        if (remote != NULL) {
            octets = (const uint8_t *)&remote->addr;
            memcpy(peer.source_address, octets,
                   APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH);
        }
    }

    vnf_https_qemu_require(
        appliance_management_reachability_evaluate(
            &vnf_reachability_selection, &peer, &verdict) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_OK,
        "HTTPS reachability evaluation");
    if (appliance_management_reachability_verdict_allowed(verdict)) {
        if (vnf_reachability_console_only_denied &&
            !vnf_reachability_restored_marked) {
            vnf_reachability_restored_marked = true;
            vnf_https_qemu_mark(
                "[MGMT-REACH] console-only served again ok");
        }
        if (!vnf_reachability_admitted_marked) {
            vnf_reachability_admitted_marked = true;
            vnf_https_qemu_mark("[MGMT-REACH] live peer admitted ok");
        }
        return true;
    }
    if (!vnf_reachability_denied_marked) {
        vnf_reachability_denied_marked = true;
        vnf_https_qemu_mark("[MGMT-REACH] live peer denied ok");
    }
    if (vnf_reachability_console_only) {
        /* Fixture sequencing: restore the appliance policy after the proof. */
        vnf_reachability_console_only_denied = true;
        vnf_https_qemu_reachability_publish(false);
    }
    return false;
}
#endif

#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
static void vnf_https_qemu_note_stale_callback(
    const struct vnf_tls_qemu_connection *connection)
{
    if (connection != NULL && !connection->active) {
        ++vnf_tls_qemu_proof.soak_stale_callback_count;
    }
}

static void vnf_https_qemu_detach_tcp_callbacks(
    struct vnf_tls_qemu_connection *connection)
{
    struct tcp_pcb *pcb = connection->pcb;

    if (pcb != NULL) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_poll(pcb, NULL, 1U);
        tcp_err(pcb, NULL);
    }
}
#endif

#ifdef HFOC_HTTPS_ENTROPY_FAIL_CLOSED_QEMU_TEST
static void vnf_https_entropy_fill_block(
    uint8_t block[VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH],
    uint32_t generation,
    uint32_t index)
{
    uint32_t byte;

    for (byte = 0U; byte < VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH; ++byte) {
        block[byte] = (uint8_t)(
            index * 67U + byte * 13U + generation * 11U);
    }
}

static void vnf_https_entropy_setup_healthy(
    struct entropy_health *health,
    struct entropy_accumulator *accumulator,
    struct entropy_seed_fill *seed_fill,
    uint32_t generation,
    uint32_t source_id,
    uint32_t initial_tick,
    uint32_t max_stall_ticks)
{
    struct entropy_health_config health_config;
    struct entropy_accumulator_config accumulator_config;
    struct entropy_seed_fill_config seed_config;
    uint8_t blocks[
        VNF_HTTPS_ENTROPY_FAIL_BLOCK_COUNT]
        [VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH];
    uint8_t seed_output[VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH];
    uint32_t written;
    uint32_t index;

    memset(health, 0, sizeof(*health));
    memset(accumulator, 0, sizeof(*accumulator));
    memset(seed_fill, 0, sizeof(*seed_fill));
    memset(&health_config, 0, sizeof(health_config));
    health_config.version = ENTROPY_HEALTH_VERSION;
    health_config.block_length_min = 1U;
    health_config.block_length_max =
        VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH;
    health_config.max_stall_ticks = max_stall_ticks;
    vnf_https_qemu_require(
        entropy_health_init(
            health, &health_config, generation, initial_tick) ==
            ENTROPY_HEALTH_OK,
        "entropy fail-closed health init");

    memset(&accumulator_config, 0, sizeof(accumulator_config));
    accumulator_config.version = ENTROPY_ACCUMULATOR_VERSION;
    accumulator_config.generation = generation;
    accumulator_config.source_id = source_id;
    accumulator_config.block_length_min = 1U;
    accumulator_config.block_length_max =
        VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH;
    accumulator_config.max_stall_ticks = max_stall_ticks;
    vnf_https_qemu_require(
        entropy_accumulator_init(
            accumulator, &accumulator_config, initial_tick) ==
            ENTROPY_ACCUMULATOR_OK,
        "entropy fail-closed accumulator init");

    for (index = 0U; index < VNF_HTTPS_ENTROPY_FAIL_BLOCK_COUNT;
         ++index) {
        vnf_https_entropy_fill_block(
            blocks[index], generation, index);
        vnf_https_qemu_require(
            entropy_health_observe(
                health, generation, blocks[index],
                VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH,
                initial_tick + 1U + index) == ENTROPY_HEALTH_OK,
            "entropy fail-closed health observe");
        vnf_https_qemu_require(
            entropy_accumulator_mix(
                accumulator, generation, source_id, blocks[index],
                VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH,
                initial_tick + 1U + index) ==
                ENTROPY_ACCUMULATOR_OK,
            "entropy fail-closed accumulator mix");
    }
    vnf_https_qemu_require(
        entropy_health_ready(health) &&
            entropy_accumulator_accumulating(accumulator),
        "entropy fail-closed healthy chain");

    memset(&seed_config, 0, sizeof(seed_config));
    seed_config.version = ENTROPY_SEED_FILL_VERSION;
    seed_config.generation = generation;
    seed_config.source_id = source_id;
    seed_config.required_credited_bytes =
        VNF_HTTPS_ENTROPY_FAIL_BLOCK_COUNT *
        VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH;
    seed_config.seed_length = VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH;
    seed_config.fill_length_max = 8U;
    seed_config.fill_count_max = 4U;
    vnf_https_qemu_require(
        entropy_seed_fill_init(
            seed_fill, &seed_config, accumulator) ==
            ENTROPY_SEED_FILL_OK,
        "entropy fail-closed seed/fill init");

    written = 0U;
    vnf_https_qemu_require(
        entropy_seed_fill_seed(
            seed_fill, accumulator, seed_output,
            (uint32_t)sizeof(seed_output), &written) ==
            ENTROPY_SEED_FILL_OK &&
            written == VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH &&
            entropy_seed_fill_ready(seed_fill),
        "entropy fail-closed seed publication");
    memset(seed_output, 0, sizeof(seed_output));
    memset(blocks, 0, sizeof(blocks));
}

static void vnf_https_entropy_fail_closed_pause(void)
{
    volatile uint32_t count;

    for (count = 0U;
         count < VNF_HTTPS_ENTROPY_FAIL_STALL_ITERATIONS;
         ++count) {
        __asm__ volatile ("pause" : : : "memory");
    }
}

static void vnf_https_entropy_fail_closed_matrix(void)
{
    struct entropy_restart restart;
    struct entropy_restart_config restart_config;
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill seed_fill;
    struct entropy_diagnostics_snapshot snapshot;
    struct entropy_diagnostics_audit audit;
    uint8_t repeated[VNF_HTTPS_ENTROPY_FAIL_BLOCK_LENGTH];
    uint8_t support[ENTROPY_DIAGNOSTICS_SUPPORT_SIZE];
    char serial[ENTROPY_DIAGNOSTICS_SERIAL_MAX];
    size_t serial_length;
    size_t support_length;
    uint32_t byte;

    vnf_https_qemu_mark("[ENTROPY-FAIL-CLOSED] proof init");
    memset(&restart, 0, sizeof(restart));
    memset(&restart_config, 0, sizeof(restart_config));
    restart_config.version = ENTROPY_RESTART_VERSION;
    restart_config.restart_count_max = 8U;
    restart_config.min_restart_interval_ticks = 1U;
    vnf_https_qemu_require(
        entropy_restart_init(&restart, &restart_config) ==
            ENTROPY_RESTART_OK,
        "entropy fail-closed restart init");
    vnf_https_qemu_require(
        entropy_restart_activate(
            &restart, NULL, NULL, NULL,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 1U) ==
            ENTROPY_RESTART_MISSING_DEVICE &&
            entropy_restart_state(&restart) ==
                ENTROPY_RESTART_STATE_FAILED &&
            entropy_restart_fault(&restart) ==
                ENTROPY_RESTART_FAULT_MISSING_DEVICE,
        "entropy fail-closed missing device");
    vnf_https_qemu_mark(
        "[ENTROPY-FAIL-CLOSED] missing device denial");
    vnf_https_qemu_require(
        entropy_restart_restart(&restart, 2U) ==
            ENTROPY_RESTART_OK,
        "entropy fail-closed restart after missing");

    vnf_https_entropy_setup_healthy(
        &health, &accumulator, &seed_fill,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 10U, 100U);
    vnf_https_qemu_require(
        entropy_restart_activate(
            &restart, &health, &accumulator, &seed_fill,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 13U) ==
            ENTROPY_RESTART_OK &&
            entropy_restart_ready(&restart),
        "entropy fail-closed rejection baseline");
    vnf_https_entropy_fill_block(
        repeated, VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 1U);
    vnf_https_qemu_require(
        entropy_health_observe(
            &health, VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID,
            repeated, sizeof(repeated), 14U) ==
            ENTROPY_HEALTH_REPEATED_BLOCK,
        "entropy fail-closed repeated block");
    vnf_https_qemu_require(
        entropy_restart_check(&restart, 14U) ==
            ENTROPY_RESTART_REPEATED_OUTPUT &&
            entropy_restart_state(&restart) ==
                ENTROPY_RESTART_STATE_FAILED &&
            entropy_restart_fault(&restart) ==
                ENTROPY_RESTART_FAULT_REPEATED_OUTPUT,
        "entropy fail-closed rejection");
    vnf_https_qemu_mark("[ENTROPY-FAIL-CLOSED] rejection ok");
    vnf_https_qemu_require(
        entropy_restart_restart(&restart, 20U) ==
            ENTROPY_RESTART_OK,
        "entropy fail-closed restart after rejection");

    vnf_https_entropy_setup_healthy(
        &health, &accumulator, &seed_fill,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 1U,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 40U, 10U);
    vnf_https_qemu_require(
        entropy_restart_activate(
            &restart, &health, &accumulator, &seed_fill,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 1U,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 45U) ==
            ENTROPY_RESTART_OK &&
            entropy_restart_ready(&restart),
        "entropy fail-closed stall baseline");
    vnf_https_qemu_require(
        entropy_health_progress(&health, 100U) ==
            ENTROPY_HEALTH_STALLED,
        "entropy fail-closed health stall");
    vnf_https_qemu_require(
        entropy_restart_check(&restart, 100U) ==
            ENTROPY_RESTART_STALLED &&
            entropy_restart_state(&restart) ==
                ENTROPY_RESTART_STATE_FAILED &&
            entropy_restart_fault(&restart) ==
                ENTROPY_RESTART_FAULT_STALLED,
        "entropy fail-closed stall");
    vnf_https_qemu_mark("[ENTROPY-FAIL-CLOSED] stall ok");
    vnf_https_qemu_require(
        entropy_restart_restart(&restart, 110U) ==
            ENTROPY_RESTART_OK,
        "entropy fail-closed restart after stall");

    vnf_https_entropy_setup_healthy(
        &health, &accumulator, &seed_fill,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 2U,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 120U, 100U);
    vnf_https_qemu_require(
        entropy_restart_activate(
            &restart, &health, &accumulator, &seed_fill,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 2U,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 125U) ==
            ENTROPY_RESTART_OK &&
            entropy_restart_ready(&restart),
        "entropy fail-closed reset baseline");
    vnf_https_entropy_fill_block(
        repeated, VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 3U, 0U);
    vnf_https_qemu_require(
        entropy_health_observe(
            &health, VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 3U,
            repeated, sizeof(repeated), 126U) ==
            ENTROPY_HEALTH_DEVICE_RESET,
        "entropy fail-closed device reset");
    vnf_https_qemu_require(
        entropy_restart_check(&restart, 126U) ==
            ENTROPY_RESTART_DEVICE_RESET &&
            entropy_restart_state(&restart) ==
                ENTROPY_RESTART_STATE_FAILED &&
            entropy_restart_fault(&restart) ==
                ENTROPY_RESTART_FAULT_DEVICE_RESET,
        "entropy fail-closed reset");
    vnf_https_qemu_mark("[ENTROPY-FAIL-CLOSED] reset ok");
    vnf_https_qemu_require(
        entropy_restart_restart(&restart, 130U) ==
            ENTROPY_RESTART_OK,
        "entropy fail-closed restart after reset");

    vnf_https_entropy_setup_healthy(
        &health, &accumulator, &seed_fill,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 2U,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 140U, 100U);
    vnf_https_qemu_require(
        entropy_restart_activate(
            &restart, &health, &accumulator, &seed_fill,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 2U,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 145U) ==
            ENTROPY_RESTART_STALE_GENERATION,
        "entropy fail-closed stale generation");
    vnf_https_qemu_mark(
        "[ENTROPY-FAIL-CLOSED] stale generation denied");
    vnf_https_qemu_require(
        entropy_restart_stop(&restart, 150U) ==
            ENTROPY_RESTART_OK,
        "entropy fail-closed stop after stale");
    vnf_https_qemu_require(
        entropy_restart_restart(&restart, 155U) ==
            ENTROPY_RESTART_OK,
        "entropy fail-closed restart for recovery");
    vnf_https_qemu_mark("[ENTROPY-FAIL-CLOSED] listener closed");
    vnf_https_entropy_fail_closed_pause();
    vnf_https_qemu_mark(
        "[ENTROPY-FAIL-CLOSED] local operation healthy");

    vnf_https_entropy_setup_healthy(
        &health, &accumulator, &seed_fill,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 3U,
        VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 160U, 100U);
    vnf_https_qemu_require(
        entropy_restart_activate(
            &restart, &health, &accumulator, &seed_fill,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID + 3U,
            VNF_HTTPS_ENTROPY_FAIL_SOURCE_ID, 165U) ==
            ENTROPY_RESTART_OK &&
            entropy_restart_ready(&restart),
        "entropy fail-closed recovery generation");

    memset(&snapshot, 0, sizeof(snapshot));
    vnf_https_qemu_require(
        entropy_diagnostics_snapshot(
            &health, &accumulator, &seed_fill, &snapshot) ==
            ENTROPY_DIAGNOSTICS_OK,
        "entropy fail-closed diagnostics snapshot");
    memset(serial, 0, sizeof(serial));
    serial_length = 0U;
    vnf_https_qemu_require(
        entropy_diagnostics_serial(
            &snapshot, serial, sizeof(serial), &serial_length) ==
            ENTROPY_DIAGNOSTICS_OK &&
            serial_length < sizeof(serial),
        "entropy fail-closed serial bounds");
    memset(&audit, 0, sizeof(audit));
    vnf_https_qemu_require(
        entropy_diagnostics_audit(&snapshot, &audit) ==
            ENTROPY_DIAGNOSTICS_OK &&
            audit.version == ENTROPY_DIAGNOSTICS_VERSION &&
            audit.state == ENTROPY_DIAGNOSTICS_STATE_READY,
        "entropy fail-closed audit bounds");
    memset(support, 0, sizeof(support));
    support_length = 0U;
    vnf_https_qemu_require(
        entropy_diagnostics_support(
            &snapshot, support, sizeof(support), &support_length) ==
            ENTROPY_DIAGNOSTICS_OK &&
            support_length == ENTROPY_DIAGNOSTICS_SUPPORT_SIZE,
        "entropy fail-closed support bounds");
    serial_write(serial);
    vnf_https_qemu_mark("[ENTROPY-FAIL-CLOSED] evidence bounded");
    vnf_https_qemu_mark(
        "[ENTROPY-FAIL-CLOSED] recovery generation ok");
    vnf_https_qemu_require(
        entropy_restart_stop(&restart, 180U) ==
            ENTROPY_RESTART_OK,
        "entropy fail-closed recovery shutdown");
    vnf_tls_qemu_proof.entropy_recovery_ok = 1U;
    vnf_https_qemu_mark("[ENTROPY-FAIL-CLOSED] matrix ok");
    for (byte = 0U; byte < sizeof(repeated); ++byte) {
        repeated[byte] = 0U;
    }
    memset(support, 0, sizeof(support));
}
#endif

static bool vnf_https_lifecycle_stop_admission(
    void *context,
    uint32_t generation)
{
    (void)context;
    (void)generation;
    return true;
}

static bool vnf_https_lifecycle_drain_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    (void)context;
    (void)generation;
    (void)slot;
    return true;
}

static bool vnf_https_lifecycle_abort_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    (void)context;
    (void)generation;
    (void)slot;
    return true;
}

static bool vnf_https_lifecycle_zeroize_tls(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    (void)context;
    (void)generation;
    (void)count;
    return true;
}

static bool vnf_https_lifecycle_release_http(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    (void)context;
    (void)generation;
    (void)count;
    return true;
}

static bool vnf_https_lifecycle_release_packet(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    (void)context;
    (void)generation;
    (void)count;
    return true;
}

static bool vnf_https_lifecycle_restart_admission(
    void *context,
    uint32_t generation)
{
    (void)context;
    (void)generation;
    return true;
}

static void vnf_https_qemu_encode_response_cookie(
    struct vnf_tls_qemu_connection *connection,
    enum http_status_code status,
    enum http_content_type content_type,
    const uint8_t *body,
    uint32_t body_length,
    const uint8_t *set_cookie,
    uint32_t set_cookie_length,
    const uint8_t *location,
    uint32_t location_length)
{
    struct http_response response;
    enum http_response_status response_status;
    uint32_t written = 0U;

    memset(&response, 0, sizeof(response));
    response.status = status;
    response.content_type = content_type;
    response.body = body;
    response.body_length = body_length;
    response.set_cookie = set_cookie;
    response.set_cookie_length = set_cookie_length;
    response.location = location;
    response.location_length = location_length;
    if (connection->response_head) {
        response_status = http_response_encode_head(
            &response,
            connection->response,
            sizeof(connection->response),
            &written);
    } else {
        response_status = http_response_encode(
            &response,
            connection->response,
            sizeof(connection->response),
            &written);
    }
    vnf_https_qemu_require(
        response_status == HTTP_RESPONSE_OK &&
            written > 0U &&
            written <= sizeof(connection->response),
        "HTTPS response encode");
    connection->response_length = written;
    connection->response_offset = 0U;
    connection->response_ready = true;
}

static void vnf_https_qemu_encode_response(
    struct vnf_tls_qemu_connection *connection,
    enum http_status_code status,
    enum http_content_type content_type,
    const uint8_t *body,
    uint32_t body_length)
{
    vnf_https_qemu_encode_response_cookie(
        connection, status, content_type, body, body_length, NULL, 0U,
        NULL, 0U);
}

/*
 * ITEM-498: the QEMU witness answers a refusal with the same bodies the
 * production serving path builds, so the refusal proofs assert the shape a peer
 * really receives.  A target of NULL/0 is a pre-parse refusal: nothing is known
 * about the peer, so it gets the API (JSON) body.
 */
static void vnf_https_qemu_error_response(
    struct vnf_tls_qemu_connection *connection,
    enum http_status_code status,
    const uint8_t *target,
    size_t target_length)
{
    uint8_t body[HTTP_ERROR_BODY_MAX];
    enum http_content_type content_type = HTTP_CONTENT_TYPE_JSON;
    size_t body_length =
        http_error_matrix_json_body(status, body, sizeof(body));

    if (http_error_matrix_body_family(target, target_length) ==
        HTTP_ERROR_BODY_HTML) {
        size_t html_length =
            http_error_matrix_html_body(status, body, sizeof(body));

        if (html_length != 0U) {
            body_length = html_length;
            content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
        }
    }
    vnf_https_qemu_require(
        body_length != 0U, "HTTPS error response body");
    connection->response_head = false;
    vnf_https_qemu_encode_response(
        connection,
        status,
        content_type,
        body,
        (uint32_t)body_length);
}

static void vnf_https_qemu_record_peer_throttle(void)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    uint64_t blocked_until = 0U;
    uint64_t tick = (uint64_t)hfoc_lwip_time_now();

    vnf_https_qemu_require(
        https_peer_throttle_record_failure(
            &proof->throttle,
            VNF_HTTPS_PEER_ID,
            tick,
            &blocked_until) == HTTPS_PEER_THROTTLE_OK,
        "HTTPS peer throttle first failure");
    vnf_https_qemu_require(
        https_peer_throttle_record_failure(
            &proof->throttle,
            VNF_HTTPS_PEER_ID,
            tick + 1U,
            &blocked_until) == HTTPS_PEER_THROTTLE_PEER_BLOCKED,
        "HTTPS peer throttle second failure");
    vnf_https_qemu_require(
        https_peer_throttle_check(
            &proof->throttle,
            VNF_HTTPS_PEER_ID,
            tick + 2U) == HTTPS_PEER_THROTTLE_PEER_BLOCKED,
        "HTTPS peer throttle blocked");
    vnf_https_qemu_require(
        https_peer_throttle_check(
            &proof->throttle,
            VNF_HTTPS_PEER_ID_OTHER,
            tick + 2U) == HTTPS_PEER_THROTTLE_OK,
        "HTTPS unrelated peer remains allowed");
    vnf_https_qemu_require(
        https_peer_throttle_local_allowed(
            HTTPS_PEER_THROTTLE_LOCAL_HEALTH) ==
                HTTPS_PEER_THROTTLE_OK,
        "HTTPS local health remains allowed");
    proof->peer_throttle_count = 1U;
    vnf_https_qemu_mark("[HTTPS-QEMU] peer throttle ok");
}

static bool vnf_https_qemu_handle_api(
    struct vnf_tls_qemu_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct appliance_management_api_result api;
    enum appliance_management_api_status api_status;

    memset(&api, 0, sizeof(api));
    api_status = appliance_management_api_dispatch(
        policy->method,
        line->target,
        line->target_length,
        &api);
    if (api_status == APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT) {
        return false;
    }
    vnf_https_qemu_require(
        api_status == APPLIANCE_MANAGEMENT_API_OK ||
            api_status == APPLIANCE_MANAGEMENT_API_NOT_FOUND ||
            api_status == APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED,
        "HTTPS management API dispatch");
    connection->response_head =
        policy->method == HTTP_METHOD_HEAD;
    vnf_https_qemu_encode_response(
        connection,
        api.response.status,
        api.response.content_type,
        api.response.body,
        api.response.body_length);
    ++proof->route_count;

    if (api_status == APPLIANCE_MANAGEMENT_API_OK &&
        policy->method == HTTP_METHOD_GET &&
        proof->api_version_get_count == 0U) {
        proof->api_version_get_count = 1U;
        vnf_https_qemu_mark("[MGMT-API] version get ok");
    } else if (
        api_status == APPLIANCE_MANAGEMENT_API_OK &&
        policy->method == HTTP_METHOD_HEAD &&
        proof->api_version_head_count == 0U) {
        proof->api_version_head_count = 1U;
        vnf_https_qemu_mark("[MGMT-API] version head ok");
    } else if (
        api_status == APPLIANCE_MANAGEMENT_API_NOT_FOUND &&
        proof->api_not_found_count == 0U) {
        proof->api_not_found_count = 1U;
        vnf_https_qemu_mark("[MGMT-API] not found ok");
    } else if (
        api_status == APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED &&
        proof->api_method_count == 0U) {
        proof->api_method_count = 1U;
        vnf_https_qemu_mark("[MGMT-API] method denied ok");
    }
    return true;
}

static bool vnf_https_qemu_handle_health_api(
    struct vnf_tls_qemu_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct appliance_health_api_result health;
    enum appliance_health_api_status health_status;

    memset(&health, 0, sizeof(health));
    health_status = appliance_health_api_dispatch(
        policy->method,
        line->target,
        line->target_length,
        &proof->health_snapshot,
        &health);
    if (health_status == APPLIANCE_HEALTH_API_NOT_HEALTH) {
        return false;
    }
    vnf_https_qemu_require(
        health_status == APPLIANCE_HEALTH_API_OK ||
            health_status == APPLIANCE_HEALTH_API_METHOD_NOT_ALLOWED,
        "HTTPS health API dispatch");
    connection->response_head =
        policy->method == HTTP_METHOD_HEAD;
    vnf_https_qemu_encode_response(
        connection,
        health.response.status,
        health.response.content_type,
        health.response.body,
        health.response.body_length);
    ++proof->route_count;

    if (health_status == APPLIANCE_HEALTH_API_OK &&
        policy->method == HTTP_METHOD_GET &&
        proof->health_api_get_count == 0U) {
        proof->health_api_get_count = 1U;
        vnf_https_qemu_mark("[HEALTH-API] health get ok");
    } else if (
        health_status == APPLIANCE_HEALTH_API_OK &&
        policy->method == HTTP_METHOD_HEAD &&
        proof->health_api_head_count == 0U) {
        proof->health_api_head_count = 1U;
        vnf_https_qemu_mark("[HEALTH-API] health head ok");
    } else if (
        health_status == APPLIANCE_HEALTH_API_METHOD_NOT_ALLOWED &&
        proof->health_api_method_count == 0U) {
        proof->health_api_method_count = 1U;
        vnf_https_qemu_mark("[HEALTH-API] method denied ok");
    }
    return true;
}

static bool vnf_https_qemu_handle_system_api(
    struct vnf_tls_qemu_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct appliance_system_api_result system;
    enum appliance_system_api_status system_status;

    memset(&system, 0, sizeof(system));
    system_status = appliance_system_api_dispatch(
        policy->method,
        line->target,
        line->target_length,
        &proof->system_snapshot,
        &system);
    if (system_status == APPLIANCE_SYSTEM_API_NOT_SYSTEM) {
        return false;
    }
    vnf_https_qemu_require(
        system_status == APPLIANCE_SYSTEM_API_OK ||
            system_status == APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED,
        "HTTPS system API dispatch");
    connection->response_head =
        policy->method == HTTP_METHOD_HEAD;
    vnf_https_qemu_encode_response(
        connection,
        system.response.status,
        system.response.content_type,
        system.response.body,
        system.response.body_length);
    ++proof->route_count;

    if (system_status == APPLIANCE_SYSTEM_API_OK &&
        policy->method == HTTP_METHOD_GET &&
        proof->system_get_count == 0U) {
        proof->system_get_count = 1U;
        vnf_https_qemu_mark("[SYSTEM-API] system get ok");
    } else if (
        system_status == APPLIANCE_SYSTEM_API_OK &&
        policy->method == HTTP_METHOD_HEAD &&
        proof->system_head_count == 0U) {
        proof->system_head_count = 1U;
        vnf_https_qemu_mark("[SYSTEM-API] system head ok");
    } else if (
        system_status == APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED &&
        proof->system_method_count == 0U) {
        proof->system_method_count = 1U;
        vnf_https_qemu_mark("[SYSTEM-API] method denied ok");
    }
    return true;
}

static bool vnf_https_qemu_handle_service_api(
    struct vnf_tls_qemu_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct appliance_service_api_result service;
    enum appliance_service_api_status service_status;

    memset(&service, 0, sizeof(service));
    service_status = appliance_service_api_dispatch(
        policy->method,
        line->target,
        line->target_length,
        &proof->service_snapshot,
        &service);
    if (service_status == APPLIANCE_SERVICE_API_NOT_SERVICE) {
        return false;
    }
    vnf_https_qemu_require(
        service_status == APPLIANCE_SERVICE_API_OK ||
            service_status == APPLIANCE_SERVICE_API_METHOD_NOT_ALLOWED,
        "HTTPS service API dispatch");
    connection->response_head =
        policy->method == HTTP_METHOD_HEAD;
    vnf_https_qemu_encode_response(
        connection,
        service.response.status,
        service.response.content_type,
        service.response.body,
        service.response.body_length);
    ++proof->route_count;

    if (service_status == APPLIANCE_SERVICE_API_OK &&
        policy->method == HTTP_METHOD_GET &&
        proof->service_get_count == 0U) {
        proof->service_get_count = 1U;
        vnf_https_qemu_mark("[SERVICE-API] service get ok");
    } else if (
        service_status == APPLIANCE_SERVICE_API_OK &&
        policy->method == HTTP_METHOD_HEAD &&
        proof->service_head_count == 0U) {
        proof->service_head_count = 1U;
        vnf_https_qemu_mark("[SERVICE-API] service head ok");
    } else if (
        service_status == APPLIANCE_SERVICE_API_METHOD_NOT_ALLOWED &&
        proof->service_method_count == 0U) {
        proof->service_method_count = 1U;
        vnf_https_qemu_mark("[SERVICE-API] method denied ok");
    }
    return true;
}

static bool vnf_https_qemu_handle_network_api(
    struct vnf_tls_qemu_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct appliance_network_api_result network;
    enum appliance_network_api_status network_status;

    memset(&network, 0, sizeof(network));
    network_status = appliance_network_api_dispatch(
        policy->method,
        line->target,
        line->target_length,
        &proof->network_snapshot,
        &network);
    if (network_status == APPLIANCE_NETWORK_API_NOT_NETWORK) {
        return false;
    }
    vnf_https_qemu_require(
        network_status == APPLIANCE_NETWORK_API_OK ||
            network_status == APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED,
        "HTTPS network API dispatch");
    connection->response_head =
        policy->method == HTTP_METHOD_HEAD;
    vnf_https_qemu_encode_response(
        connection,
        network.response.status,
        network.response.content_type,
        network.response.body,
        network.response.body_length);
    ++proof->route_count;

    if (network_status == APPLIANCE_NETWORK_API_OK &&
        network.route == APPLIANCE_NETWORK_API_ROUTE_CONFIG &&
        policy->method == HTTP_METHOD_GET &&
        proof->network_config_get_count == 0U) {
        proof->network_config_get_count = 1U;
        vnf_https_qemu_mark("[NETWORK-API] config get ok");
    } else if (
        network_status == APPLIANCE_NETWORK_API_OK &&
        network.route == APPLIANCE_NETWORK_API_ROUTE_CONFIG &&
        policy->method == HTTP_METHOD_HEAD &&
        proof->network_config_head_count == 0U) {
        proof->network_config_head_count = 1U;
        vnf_https_qemu_mark("[NETWORK-API] config head ok");
    } else if (
        network_status == APPLIANCE_NETWORK_API_OK &&
        network.route == APPLIANCE_NETWORK_API_ROUTE_STATUS &&
        policy->method == HTTP_METHOD_GET &&
        proof->network_status_get_count == 0U) {
        proof->network_status_get_count = 1U;
        vnf_https_qemu_mark("[NETWORK-API] status get ok");
    } else if (
        network_status == APPLIANCE_NETWORK_API_OK &&
        network.route == APPLIANCE_NETWORK_API_ROUTE_STATUS &&
        policy->method == HTTP_METHOD_HEAD &&
        proof->network_status_head_count == 0U) {
        proof->network_status_head_count = 1U;
        vnf_https_qemu_mark("[NETWORK-API] status head ok");
    } else if (
        network_status == APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED &&
        proof->network_method_count == 0U) {
        proof->network_method_count = 1U;
        vnf_https_qemu_mark("[NETWORK-API] method denied ok");
    }
    return true;
}

static void vnf_https_qemu_handle_parser_error(
    struct vnf_tls_qemu_connection *connection)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    enum http_status_code status = HTTP_STATUS_400_BAD_REQUEST;
    enum http_error_matrix_status matrix_status;

    matrix_status = http_error_matrix_map_parser(
        connection->parser_error, &status);
    vnf_https_qemu_require(
        matrix_status == HTTP_ERROR_MATRIX_OK,
        "HTTPS parser error mapping");
    connection->malformed_request = true;
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    ++proof->soak_malformed_count;
    if (proof->soak_malformed_count == 1U) {
        vnf_https_qemu_mark("[HTTPS-RESTART-SOAK] malformed ok");
    }
#endif
    vnf_https_qemu_error_response(connection, status, NULL, 0U);
    if (proof->malformed_count == 0U) {
        proof->malformed_count = 1U;
        vnf_https_qemu_mark("[HTTPS-QEMU] malformed request ok");
    }
    if (proof->peer_throttle_count == 0U) {
        vnf_https_qemu_record_peer_throttle();
    }
}

static const struct appliance_management_web_ui_view
vnf_https_qemu_panel_view = {
    "node-316",
    "52:54:00:12:34:56",
    "hfoc-https",
    "management panel fixture ready",
    "HTTPS service QEMU fixture",
};

static void vnf_https_qemu_panel_fill(
    uint8_t *bytes,
    uint8_t value,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = value;
    }
}

/*
 * ITEM-445 step 2: the QEMU fixture's browser credential boundary. It mirrors
 * the production adapter's shape: the login and the password are compared with
 * the fixture literals, and a match issues the fixture session through the
 * same administrator boundary the bearer proofs use. The fixture introduces no
 * secret the production path does not have and no second session store.
 *
 * ITEM-506: the boundary carries the login too, so the fixture pins both halves
 * of the one account.
 */
#define VNF_HTTPS_QEMU_PANEL_LOGIN "operator"
#define VNF_HTTPS_QEMU_PANEL_PASSWORD "hfoc-fixture-secret"
#define VNF_HTTPS_QEMU_PANEL_PASSWORD_LENGTH 19U

static enum appliance_management_panel_credential_status
vnf_https_qemu_panel_authenticate(
    void *context,
    const uint8_t *user,
    uint32_t user_length,
    const uint8_t *secret,
    uint32_t secret_length,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out)
{
    struct vnf_tls_qemu_proof_state *proof = context;
    static const char fixture_login[] = VNF_HTTPS_QEMU_PANEL_LOGIN;
    static const char fixture[] = VNF_HTTPS_QEMU_PANEL_PASSWORD;
    uint32_t index;

    if (proof == NULL || user == NULL || secret == NULL ||
        token_out == NULL || session_id_out == NULL ||
        user_length != sizeof(VNF_HTTPS_QEMU_PANEL_LOGIN) - 1U ||
        secret_length != VNF_HTTPS_QEMU_PANEL_PASSWORD_LENGTH) {
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_DENIED;
    }
    for (index = 0U; index < user_length; ++index) {
        if (user[index] != (uint8_t)fixture_login[index]) {
            return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_DENIED;
        }
    }
    for (index = 0U; index < secret_length; ++index) {
        if (secret[index] != (uint8_t)fixture[index]) {
            return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_DENIED;
        }
    }
    if (appliance_admin_auth_authenticate_local(
            &proof->panel_auth.admin,
            &proof->panel_proof,
            proof->panel_token,
            now_tick,
            lifetime_ticks,
            session_id_out) != APPLIANCE_ADMIN_AUTH_OK) {
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_UNAVAILABLE;
    }
    memcpy(
        token_out,
        proof->panel_token,
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
    return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_OK;
}

/*
 * ITEM-384 fixture sign-in. It uses the existing administrator
 * authentication boundary with a caller-supplied proof and a caller-supplied
 * session token: the fixture generates no secret and the panel adds no
 * authentication factor. The session is re-issued when it expires so a long
 * proof run keeps a live session without weakening the lifetime.
 */
static void vnf_https_qemu_panel_open_session(uint64_t now_tick)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    uint32_t session_id = 0U;

    vnf_https_qemu_require(
        appliance_management_panel_login(
            &proof->panel,
            &proof->panel_proof,
            proof->panel_token,
            now_tick,
            APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
            &session_id) == APPLIANCE_MANAGEMENT_PANEL_OK,
        "HTTPS panel sign-in");
    proof->panel_session_expires =
        now_tick +
        (uint64_t)APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX;
    ++proof->panel_session_count;
}

static bool vnf_https_qemu_handle_panel(
    struct vnf_tls_qemu_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *body,
    uint32_t body_length)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct appliance_management_panel_request panel_request;
    struct appliance_management_panel_response panel_response;
    struct http_header_field field;
    enum appliance_management_panel_target_kind target_kind;
    enum appliance_management_web_ui_section section =
        APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD;
    enum appliance_management_panel_status panel_status;
    const uint8_t *authorization = NULL;
    const uint8_t *cookie = NULL;
    const uint8_t *content_type = NULL;
    uint32_t authorization_length = 0U;
    uint32_t cookie_length = 0U;
    uint32_t content_type_length = 0U;
    bool cookie_seen = false;
    bool content_type_seen = false;
    uint32_t header_count = 0U;
    uint32_t index;
    uint64_t now_tick;
    /*
     * The parser publishes a header as a name without the colon and a value
     * that keeps the optional whitespace after it. The administrator
     * authentication boundary consumes the canonical request line, so the
     * wire path rebuilds it here instead of teaching the boundary a second
     * header format.
     */
    uint8_t authorization_line[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    static const char authorization_name[] = "Authorization";
    static const char authorization_colon[] = "Authorization:";

    target_kind = appliance_management_panel_target_kind(
        line->target, line->target_length, &section);
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE) {
        return false;
    }
    /*
     * ITEM-445 step 2: a GET of `/` stays the fixed public sign-in document
     * served by the route table below, exactly as before this item.
     */
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN &&
        policy->method != HTTP_METHOD_POST) {
        return false;
    }
    if (!appliance_management_panel_ready(&proof->panel) &&
        !(target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_OUT ||
          (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN &&
           policy->method == HTTP_METHOD_POST))) {
        return false;
    }
    if (http_header_count(
            &proof->parser,
            &connection->parser_token,
            &header_count) != HTTP_PARSER_OK) {
        return false;
    }
    for (index = 0U; index < header_count; ++index) {
        const uint8_t *start;
        uint32_t length;

        memset(&field, 0, sizeof(field));
        if (http_header_field(
                &proof->parser,
                &connection->parser_token,
                index,
                &field) != HTTP_PARSER_OK) {
            continue;
        }
        if (field.name_length == sizeof(authorization_name) - 1U &&
            memcmp(
                field.name,
                authorization_name,
                sizeof(authorization_name) - 1U) == 0 &&
            field.value_length +
                    (uint32_t)(sizeof(authorization_colon) - 1U) <=
                sizeof(authorization_line)) {
            memcpy(
                authorization_line,
                authorization_colon,
                sizeof(authorization_colon) - 1U);
            memcpy(
                authorization_line + sizeof(authorization_colon) - 1U,
                field.value,
                field.value_length);
            authorization = authorization_line;
            authorization_length =
                (uint32_t)(sizeof(authorization_colon) - 1U) +
                field.value_length;
        }
        /*
         * ITEM-445 step 2: the browser sign-in material. A header seen twice
         * is not resolved by guessing: the value stays unpublished and the
         * request is treated as unsigned.
         */
        start = field.value;
        length = field.value_length;
        while (length != 0U &&
               (*start == (uint8_t)' ' || *start == (uint8_t)'\t')) {
            ++start;
            --length;
        }
        while (length != 0U &&
               (start[length - 1U] == (uint8_t)' ' ||
                start[length - 1U] == (uint8_t)'\t')) {
            --length;
        }
        if (field.name_length == 6U &&
            memcmp(field.name, "Cookie", 6U) == 0) {
            if (cookie_seen) {
                cookie = NULL;
                cookie_length = 0U;
            } else {
                cookie = start;
                cookie_length = length;
                cookie_seen = true;
            }
        }
        if (field.name_length == 12U &&
            memcmp(field.name, "Content-Type", 12U) == 0) {
            if (content_type_seen) {
                content_type = NULL;
                content_type_length = 0U;
            } else {
                content_type = start;
                content_type_length = length;
                content_type_seen = true;
            }
        }
    }
    now_tick = (uint64_t)hfoc_lwip_time_now();
    if (proof->panel_session_count == 0U ||
        proof->panel_session_expires <= now_tick) {
        vnf_https_qemu_panel_open_session(now_tick);
    }

    memset(&panel_request, 0, sizeof(panel_request));
    memset(&panel_response, 0, sizeof(panel_response));
    if (policy->method == HTTP_METHOD_POST) {
        panel_request.content_type = content_type;
        panel_request.content_type_length = content_type_length;
        panel_request.body = body;
        panel_request.body_length = body_length;
    } else if (authorization_length == 0U) {
        panel_request.cookie_header = cookie;
        panel_request.cookie_header_length = cookie_length;
    }
    panel_status = appliance_management_panel_dispatch(
        &proof->panel,
        policy->method,
        line->target,
        line->target_length,
        authorization,
        authorization_length,
        now_tick,
        NULL,
        &panel_request,
        &panel_response);
    if (panel_status == APPLIANCE_MANAGEMENT_PANEL_NOT_PANEL) {
        return false;
    }
    connection->response_head = policy->method == HTTP_METHOD_HEAD;
    vnf_https_qemu_encode_response_cookie(
        connection,
        panel_response.status,
        panel_response.content_type,
        panel_response.body,
        panel_response.body_length,
        panel_response.set_cookie,
        panel_response.set_cookie_length,
        panel_response.location,
        panel_response.location_length);
    ++proof->route_count;

    if (panel_status == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
        policy->method == HTTP_METHOD_GET &&
        proof->panel_get_count == 0U) {
        proof->panel_get_count = 1U;
        vnf_https_qemu_mark("[PANEL] section get ok");
    } else if (
        panel_status == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
        policy->method == HTTP_METHOD_HEAD &&
        proof->panel_head_count == 0U) {
        proof->panel_head_count = 1U;
        vnf_https_qemu_mark("[PANEL] section head ok");
    } else if (
        panel_status == APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED &&
        proof->panel_denied_count == 0U) {
        proof->panel_denied_count = 1U;
        vnf_https_qemu_mark("[PANEL] denied ok");
    } else if (
        panel_status == APPLIANCE_MANAGEMENT_PANEL_METHOD_NOT_ALLOWED &&
        proof->panel_method_count == 0U) {
        proof->panel_method_count = 1U;
        vnf_https_qemu_mark("[PANEL] method denied ok");
    } else if (
        panel_status == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
        policy->method == HTTP_METHOD_POST &&
        target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN &&
        proof->panel_sign_in_count == 0U) {
        proof->panel_sign_in_count = 1U;
        vnf_https_qemu_mark("[PANEL] sign in ok");
    } else if (
        panel_status == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
        policy->method == HTTP_METHOD_POST &&
        target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_OUT &&
        proof->panel_sign_out_count == 0U) {
        proof->panel_sign_out_count = 1U;
        vnf_https_qemu_mark("[PANEL] sign out ok");
    } else if (
        panel_status == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
        policy->method == HTTP_METHOD_GET &&
        authorization_length == 0U && cookie_length != 0U &&
        proof->panel_cookie_get_count == 0U) {
        proof->panel_cookie_get_count = 1U;
        vnf_https_qemu_mark("[PANEL] cookie get ok");
    }
    return true;
}

static void vnf_https_qemu_handle_complete(
    struct vnf_tls_qemu_connection *connection,
    const uint8_t *body,
    uint32_t body_length)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct http_framing_result framing;
    struct http_policy_result policy;
    struct http_request_line line;
    struct https_handler_result handler;
    enum http_parser_status parser_status;
    enum http_route_status route_status;
    enum http_route_id route = HTTP_ROUTE_NONE;
    enum http_status_code status;
    enum http_error_matrix_status matrix_status;

    memset(&framing, 0, sizeof(framing));
    memset(&policy, 0, sizeof(policy));
    memset(&line, 0, sizeof(line));
    memset(&handler, 0, sizeof(handler));

#ifdef HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST
    /*
     * ITEM-385: reachability is decided before any parsing, routing or
     * authorization work, for the peer that completed the handshake. A refused
     * peer is answered like an unknown route so the management plane does not
     * confirm its own existence.
     */
    if (!vnf_https_qemu_reachability_admit(connection)) {
        matrix_status = http_error_matrix_map_route(
            HTTP_ROUTE_NONE, &status);
        vnf_https_qemu_require(
            matrix_status == HTTP_ERROR_MATRIX_OK,
            "HTTPS reachability mapping");
        vnf_https_qemu_error_response(connection, status, NULL, 0U);
        return;
    }
#endif

    parser_status = http_framing_check(
        &proof->parser,
        &connection->parser_token,
        &framing);
    vnf_https_qemu_require(
        parser_status == HTTP_PARSER_OK,
        "HTTPS framing check");
    if (framing.body_denied || framing.upgrade_denied) {
        matrix_status = http_error_matrix_map_framing(
            &framing, &status);
        vnf_https_qemu_require(
            matrix_status == HTTP_ERROR_MATRIX_OK,
            "HTTPS framing mapping");
        vnf_https_qemu_error_response(connection, status, NULL, 0U);
        return;
    }

    parser_status = http_policy_check(
        &proof->parser,
        &connection->parser_token,
        &policy);
    vnf_https_qemu_require(
        parser_status == HTTP_PARSER_OK,
        "HTTPS policy check");
    parser_status = http_request_line_get(
        &proof->parser,
        &connection->parser_token,
        &line);
    vnf_https_qemu_require(
        parser_status == HTTP_PARSER_OK,
        "HTTPS request line get");
#ifdef HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST
    /*
     * ITEM-385 fixture input: the request that asks the appliance to publish
     * the console-only policy, so the acceptance flow can prove a live peer is
     * refused on the wire and served again once the policy returns. The request
     * itself falls through to the unknown-route response.
     */
    (void)vnf_https_qemu_reachability_trigger(&line);
#endif
    /*
     * ITEM-384: the rendered management panel is dispatched from its own
     * static target table before the fixed route table, exactly like the
     * /api/v1 modules are. A panel request with a method other than GET or
     * HEAD is answered by the panel itself.
     */
    if (vnf_https_qemu_handle_panel(
            connection, &policy, &line, body, body_length)) {
        return;
    }
    if (policy.reason == HTTP_POLICY_METHOD) {
        if (vnf_https_qemu_handle_health_api(
                connection, &policy, &line)) {
            return;
        }
        if (vnf_https_qemu_handle_service_api(
                connection, &policy, &line)) {
            return;
        }
        if (vnf_https_qemu_handle_network_api(
                connection, &policy, &line)) {
            return;
        }
        if (vnf_https_qemu_handle_system_api(
                connection, &policy, &line)) {
            return;
        }
        if (vnf_https_qemu_handle_api(
                connection, &policy, &line)) {
            return;
        }
        matrix_status = http_error_matrix_map_policy(
            policy.reason, &status);
        vnf_https_qemu_require(
            matrix_status == HTTP_ERROR_MATRIX_OK,
            "HTTPS policy mapping");
        vnf_https_qemu_error_response(
            connection, status, line.target, line.target_length);
        return;
    }
    if (!policy.allowed) {
        matrix_status = http_error_matrix_map_policy(
            policy.reason, &status);
        vnf_https_qemu_require(
            matrix_status == HTTP_ERROR_MATRIX_OK,
            "HTTPS policy mapping");
        vnf_https_qemu_error_response(
            connection, status, line.target, line.target_length);
        return;
    }
    if (vnf_https_qemu_handle_health_api(
            connection, &policy, &line)) {
        return;
    }
    if (vnf_https_qemu_handle_service_api(
            connection, &policy, &line)) {
        return;
    }
    if (vnf_https_qemu_handle_network_api(
            connection, &policy, &line)) {
        return;
    }
    if (vnf_https_qemu_handle_system_api(
            connection, &policy, &line)) {
        return;
    }
    if (vnf_https_qemu_handle_api(
            connection, &policy, &line)) {
        return;
    }
    route_status = http_route_match_line(&line, &route);
    vnf_https_qemu_require(
        route_status == HTTP_ROUTE_OK,
        "HTTPS route match");
    if (route == HTTP_ROUTE_NONE) {
        matrix_status = http_error_matrix_map_route(
            route, &status);
        vnf_https_qemu_require(
            matrix_status == HTTP_ERROR_MATRIX_OK,
            "HTTPS route mapping");
        vnf_https_qemu_error_response(
            connection, status, line.target, line.target_length);
        return;
    }

    vnf_https_qemu_require(
        https_handler_dispatch(
            &proof->parser,
            &connection->parser_token,
            &handler) == HTTPS_HANDLER_OK,
        "HTTPS handler dispatch");
    connection->response_head =
        handler.method == HTTP_METHOD_HEAD;
    vnf_https_qemu_encode_response(
        connection,
        handler.response.status,
        handler.response.content_type,
        handler.response.body,
        handler.response.body_length);

    ++proof->route_count;
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    ++proof->soak_request_count;
    if (proof->soak_request_count ==
        VNF_HTTPS_SOAK_REQUESTS_PER_ROUND) {
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] round 1 requests ok");
    } else if (
        proof->soak_request_count ==
        (2U * VNF_HTTPS_SOAK_REQUESTS_PER_ROUND)) {
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] round 2 requests ok");
    } else if (
        proof->soak_request_count ==
        (3U * VNF_HTTPS_SOAK_REQUESTS_PER_ROUND)) {
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] round 3 requests ok");
    } else if (
        proof->soak_request_count ==
        (VNF_HTTPS_SOAK_ROUND_REQUESTS + 1U)) {
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] link recovery ok");
    } else if (
        proof->soak_request_count ==
        VNF_HTTPS_SOAK_TOTAL_REQUESTS) {
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] final success");
    }
#endif
    if (route == HTTP_ROUTE_ROOT &&
        handler.method == HTTP_METHOD_GET &&
        proof->root_get_count == 0U) {
        proof->root_get_count = 1U;
        vnf_https_qemu_mark("[HTTPS-QEMU] root get ok");
    } else if (
        route == HTTP_ROUTE_ROOT &&
        handler.method == HTTP_METHOD_HEAD &&
        proof->root_head_count == 0U) {
        proof->root_head_count = 1U;
        vnf_https_qemu_mark("[HTTPS-QEMU] root head ok");
    } else if (
        route == HTTP_ROUTE_HEALTH &&
        handler.method == HTTP_METHOD_GET &&
        proof->health_get_count == 0U) {
        proof->health_get_count = 1U;
        vnf_https_qemu_mark("[HTTPS-QEMU] health get ok");
    } else if (
        route == HTTP_ROUTE_HEALTH &&
        handler.method == HTTP_METHOD_HEAD &&
        proof->health_head_count == 0U) {
        proof->health_head_count = 1U;
        vnf_https_qemu_mark("[HTTPS-QEMU] health head ok");
    }
}

static void vnf_https_qemu_feed_request(
    struct vnf_tls_qemu_connection *connection,
    const uint8_t *bytes,
    size_t length)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    size_t index;

    for (index = 0U; index < length; ++index) {
        enum http_parser_status status;
        bool complete = false;

        if (connection->request_parsed ||
            connection->malformed_request) {
            return;
        }
        /*
         * ITEM-445 step 2: once the headers are complete and framing published
         * a body, the rest of the input is that body until exactly
         * `body_expected` bytes have arrived; the request is completed only
         * then, so a body byte is never parsed as a second request.
         */
        if (connection->body_pending) {
            if (connection->body_length >= sizeof(connection->body)) {
                connection->parser_error = HTTP_PARSER_HEADER_OUT_OF_RANGE;
                vnf_https_qemu_handle_parser_error(connection);
                return;
            }
            connection->body[connection->body_length] = bytes[index];
            ++connection->body_length;
            if (connection->body_length == connection->body_expected) {
                connection->body_pending = false;
                connection->request_parsed = true;
                vnf_https_qemu_handle_complete(
                    connection,
                    connection->body,
                    connection->body_length);
                return;
            }
            continue;
        }
        if (!connection->request_line_done) {
            status = http_request_line_feed(
                &proof->parser,
                &connection->parser_token,
                bytes + index,
                1U,
                &complete);
            if (status != HTTP_PARSER_OK) {
                connection->parser_error = status;
                vnf_https_qemu_handle_parser_error(connection);
                return;
            }
            if (!complete) {
                continue;
            }
            {
                struct http_parser_progress progress;

                vnf_https_qemu_require(
                    http_parser_progress(
                        &proof->parser,
                        &connection->parser_token,
                        &progress) == HTTP_PARSER_OK,
                    "HTTPS request line progress");
                status = http_parser_advance(
                    &proof->parser,
                    &connection->parser_token,
                    HTTP_PARSER_STATE_HEADERS,
                    &(struct http_parser_progress){
                        0U,
                        progress.request_line_count,
                        progress.request_line_count,
                        0U,
                        progress.request_line_count
                    });
                if (status != HTTP_PARSER_OK) {
                    connection->parser_error = status;
                    vnf_https_qemu_handle_parser_error(connection);
                    return;
                }
                connection->request_line_done = true;
            }
            continue;
        }

        status = http_header_feed(
            &proof->parser,
            &connection->parser_token,
            bytes + index,
            1U,
            &complete);
        if (status != HTTP_PARSER_OK) {
            connection->parser_error = status;
            vnf_https_qemu_handle_parser_error(connection);
            return;
        }
        if (complete) {
            struct http_framing_result framing;

            /*
             * ITEM-445 step 2: framing decides whether this request carries a
             * body before the request is completed.
             */
            memset(&framing, 0, sizeof(framing));
            if (http_framing_check(
                    &proof->parser,
                    &connection->parser_token,
                    &framing) == HTTP_PARSER_OK &&
                framing.body_present && framing.content_length != 0U) {
                connection->body_pending = true;
                connection->body_length = 0U;
                connection->body_expected = framing.content_length;
                continue;
            }
            connection->request_parsed = true;
            vnf_https_qemu_handle_complete(connection, NULL, 0U);
            return;
        }
    }
}

static void vnf_https_qemu_send_response(
    struct vnf_tls_qemu_connection *connection)
{
    int result;

    if (!connection->response_ready ||
        connection->response_http_sent) {
        return;
    }
    result = mbedtls_ssl_write(
        &connection->ssl,
        connection->response + connection->response_offset,
        connection->response_length - connection->response_offset);
    if (result < 0) {
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;
        }
        vnf_https_qemu_require(false, "HTTPS response write");
    }
    vnf_https_qemu_require(
        result > 0 &&
            (uint32_t)result <=
                connection->response_length -
                    connection->response_offset,
        "HTTPS response progress");
    connection->response_offset += (uint32_t)result;
    if (connection->response_offset ==
        connection->response_length) {
        connection->response_http_sent = true;
    }
}

static void vnf_https_qemu_finish_connection(
    struct vnf_tls_qemu_connection *connection)
{
    int result;

    if (!connection->active || !connection->handshake_done) {
        return;
    }
    if (connection->request_parsed ||
        connection->malformed_request) {
        if (!connection->response_http_sent) {
            vnf_https_qemu_send_response(connection);
            return;
        }
    } else {
        return;
    }
    if (!connection->close_notify_sent) {
        result = mbedtls_ssl_close_notify(&connection->ssl);
        if (result != 0 && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            vnf_https_qemu_require(false, "HTTPS close notify");
        }
        if (result == 0) {
            connection->close_notify_sent = true;
        } else {
            return;
        }
    }
    connection->closing = true;
    vnf_tls_qemu_release_connection(connection);
}

static void vnf_https_qemu_step_connection(
    struct vnf_tls_qemu_connection *connection)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    uint8_t plaintext[VNF_HTTPS_PLAINTEXT_CHUNK_SIZE];
    int result;
    uint32_t step;

    if (!connection->active) {
        return;
    }
    if (!connection->ssl_setup) {
        if (connection->rx_length == connection->rx_offset) {
            return;
        }
        vnf_tls_qemu_setup_connection(connection);
    }
    vnf_tls_qemu_drain_entropy_magic(connection);
    if (!connection->handshake_done) {
        for (step = 0U; step < VNF_TLS_HANDSHAKE_STEP_LIMIT; ++step) {
            result = mbedtls_ssl_handshake(&connection->ssl);
            if (result == 0 ||
                result == MBEDTLS_ERR_SSL_WANT_READ ||
                result == MBEDTLS_ERR_SSL_WANT_WRITE) {
                break;
            }
        }
        if (result == 0) {
            connection->handshake_done = true;
            ++proof->success_count;
        } else if (
            result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;
        } else {
            vnf_https_qemu_require(false, "HTTPS TLS handshake");
        }
    }
    if (!connection->request_parsed &&
        !connection->malformed_request) {
        result = mbedtls_ssl_read(
            &connection->ssl,
            plaintext,
            sizeof(plaintext));
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;
        }
        if (result == MBEDTLS_ERR_SSL_CONN_EOF) {
            connection->closing = true;
            vnf_tls_qemu_release_connection(connection);
            return;
        }
        vnf_https_qemu_require(result > 0, "HTTPS plaintext read");
        vnf_https_qemu_feed_request(
            connection,
            plaintext,
            (size_t)result);
    }
    vnf_https_qemu_finish_connection(connection);
}

static void vnf_https_qemu_init_contracts(void)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct https_service_lifecycle_config config;
    struct https_service_lifecycle_ops ops;
    const struct appliance_identity *system_identity;
    struct appliance_network_config network_desired;
    struct appliance_network_diagnostics network_diagnostics;
    struct network_link_state_record network_link;
    struct network_ipv4_config active_ipv4;
    struct appliance_service_manager_snapshot service_manager;
    struct appliance_health_input health_input;

    memset(&proof->parser, 0, sizeof(proof->parser));
    memset(&proof->health, 0, sizeof(proof->health));
    memset(&proof->throttle, 0, sizeof(proof->throttle));
    memset(&proof->lifecycle, 0, sizeof(proof->lifecycle));
    memset(&proof->audit_ring, 0, sizeof(proof->audit_ring));
    memset(&proof->system_snapshot, 0, sizeof(proof->system_snapshot));
    memset(&proof->network_snapshot, 0, sizeof(proof->network_snapshot));
    memset(&proof->service_snapshot, 0, sizeof(proof->service_snapshot));
    memset(&proof->health_snapshot, 0, sizeof(proof->health_snapshot));
    memset(&network_desired, 0, sizeof(network_desired));
    memset(&network_diagnostics, 0, sizeof(network_diagnostics));
    memset(&network_link, 0, sizeof(network_link));
    memset(&active_ipv4, 0, sizeof(active_ipv4));
    memset(&service_manager, 0, sizeof(service_manager));
#ifdef HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST
    vnf_https_qemu_reachability_publish(false);
#endif
    system_identity = appliance_identity_get();
    vnf_https_qemu_require(
        system_identity != NULL,
        "HTTPS system identity available");
    vnf_https_qemu_require(
        appliance_system_api_snapshot_init(
            &proof->system_snapshot,
            system_identity,
            APPLIANCE_SYSTEM_STATE_OK,
            316U,
            APPLIANCE_SYSTEM_STATE_OK,
            APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
            "node-316",
            8U,
            0U,
            0U) == APPLIANCE_SYSTEM_API_OK,
        "HTTPS system snapshot init");
    active_ipv4.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    active_ipv4.address.octets[0] = 10U;
    active_ipv4.address.octets[2] = 2U;
    active_ipv4.address.octets[3] = 15U;
    active_ipv4.prefix_length = 24U;
    active_ipv4.gateway.octets[0] = 10U;
    active_ipv4.gateway.octets[2] = 2U;
    active_ipv4.gateway.octets[3] = 2U;
    active_ipv4.dns.octets[0] = 10U;
    active_ipv4.dns.octets[2] = 2U;
    active_ipv4.dns.octets[3] = 3U;
    network_link.version = NETWORK_LINK_STATE_VERSION;
    network_link.device_id = 1U;
    network_link.state = NETWORK_LINK_UP;
    network_link.generation = 7U;
    vnf_https_qemu_require(
        appliance_network_config_publish_static(
            &network_desired,
            "eth0",
            4U,
            &active_ipv4) == APPLIANCE_NETWORK_CONFIG_OK,
        "HTTPS network desired config publish");
    vnf_https_qemu_require(
        appliance_network_diagnostics_publish(
            &network_diagnostics,
            &network_desired,
            &network_link,
            &active_ipv4,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
            APPLIANCE_NETWORK_DIAGNOSTICS_OK,
        "HTTPS network diagnostics publish");
    vnf_https_qemu_require(
        appliance_network_api_snapshot_init(
            &proof->network_snapshot,
            &network_desired,
            APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED,
            &network_diagnostics) == APPLIANCE_NETWORK_API_OK,
        "HTTPS network snapshot init");
    service_manager.version = APPLIANCE_SERVICE_MANAGER_VERSION;
    service_manager.published = true;
    service_manager.state = APPLIANCE_SERVICE_MANAGER_STATE_RUNNING;
    service_manager.startup_policy =
        APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK;
    service_manager.generation = 1U;
    service_manager.configured_count = 1U;
    service_manager.active_count = 1U;
    service_manager.ready_count = 1U;
    service_manager.health_result = HEALTH_CHECK_RESULT_PASS;
    service_manager.health_fault = SERVICE_HEALTH_FAULT_NONE;
    vnf_https_qemu_require(
        appliance_service_api_snapshot_init(
            &proof->service_snapshot,
            &service_manager) == APPLIANCE_SERVICE_API_OK,
        "HTTPS service snapshot init");

    memset(&health_input, 0, sizeof(health_input));
    health_input.version = APPLIANCE_HEALTH_VERSION;
    health_input.generation = 328U;
    for (unsigned int __health_index = 0U;
         __health_index < APPLIANCE_HEALTH_DOMAIN_COUNT;
         ++__health_index) {
        static const enum appliance_health_domain __health_domains[
            APPLIANCE_HEALTH_DOMAIN_COUNT] = {
            APPLIANCE_HEALTH_DOMAIN_BOOT,
            APPLIANCE_HEALTH_DOMAIN_STORAGE,
            APPLIANCE_HEALTH_DOMAIN_CONFIGURATION,
            APPLIANCE_HEALTH_DOMAIN_NETWORK,
            APPLIANCE_HEALTH_DOMAIN_SERVICE,
            APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT,
        };
        health_input.domains[__health_index].version =
            APPLIANCE_HEALTH_VERSION;
        health_input.domains[__health_index].domain =
            __health_domains[__health_index];
        health_input.domains[__health_index].state =
            APPLIANCE_HEALTH_STATE_PASS;
        health_input.domains[__health_index].fault =
            APPLIANCE_HEALTH_FAULT_NONE;
        health_input.domains[__health_index].last_checked_ticks =
            (uint64_t)__health_index + 1U;
    }
    vnf_https_qemu_require(
        appliance_health_api_snapshot_init(
            &proof->health_snapshot,
            &health_input) == APPLIANCE_HEALTH_API_OK,
        "HTTPS health snapshot init");
    vnf_https_qemu_require(
        http_parser_init(&proof->parser) == HTTP_PARSER_OK,
        "HTTPS parser init");
    vnf_https_qemu_require(
        https_health_init(&proof->health) == HTTPS_HEALTH_OK,
        "HTTPS health init");
    vnf_https_qemu_require(
        event_ring_init(&proof->audit_ring) == EVENT_RING_OK,
        "HTTPS audit ring init");
    vnf_https_qemu_require(
        https_peer_throttle_init(
            &proof->throttle,
            HTTPS_PEER_THROTTLE_MAX_PEERS,
            VNF_HTTPS_PEER_FAILURE_LIMIT,
            VNF_HTTPS_PEER_WINDOW_TICKS,
            VNF_HTTPS_PEER_COOLDOWN_TICKS) ==
            HTTPS_PEER_THROTTLE_OK,
        "HTTPS peer throttle init");

    memset(&config, 0, sizeof(config));
    config.version = HTTPS_SERVICE_LIFECYCLE_VERSION;
    config.drain_timeout_ms = VNF_HTTPS_LIFECYCLE_DRAIN_TIMEOUT_MS;
    config.connection_capacity =
        HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
    config.tls_context_capacity =
        HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY;
    config.http_parser_capacity =
        HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY;
    config.packet_pool_capacity =
        VNF_HTTPS_LIFECYCLE_PACKET_POOL_CAPACITY;

    memset(&ops, 0, sizeof(ops));
    ops.stop_admission = vnf_https_lifecycle_stop_admission;
    ops.drain_connection = vnf_https_lifecycle_drain_connection;
    ops.abort_connection = vnf_https_lifecycle_abort_connection;
    ops.zeroize_tls = vnf_https_lifecycle_zeroize_tls;
    ops.release_http = vnf_https_lifecycle_release_http;
    ops.release_packet = vnf_https_lifecycle_release_packet;
    ops.restart_admission = vnf_https_lifecycle_restart_admission;
    ops.context = proof;

    /*
     * ITEM-384 panel fixture: publish the administrator credential, enable
     * the administrator boundary, initialize the panel and publish the
     * dynamic values the rendered pages show.
     */
    {
        struct appliance_admin_auth_credential credential;

        memset(&proof->panel_auth, 0, sizeof(proof->panel_auth));
        memset(&proof->panel, 0, sizeof(proof->panel));
        memset(&proof->panel_proof, 0, sizeof(proof->panel_proof));
        memset(&credential, 0, sizeof(credential));
        credential.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
        vnf_https_qemu_panel_fill(
            credential.verifier,
            0x5AU,
            sizeof(credential.verifier));
        proof->panel_proof.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
        vnf_https_qemu_panel_fill(
            proof->panel_proof.proof,
            0x5AU,
            sizeof(proof->panel_proof.proof));
        vnf_https_qemu_panel_fill(
            proof->panel_token,
            0x42U,
            sizeof(proof->panel_token));
        vnf_https_qemu_require(
            appliance_production_auth_init(&proof->panel_auth) ==
                APPLIANCE_PRODUCTION_AUTH_OK,
            "HTTPS panel auth init");
        vnf_https_qemu_require(
            appliance_production_auth_publish_credential(
                &proof->panel_auth,
                &credential) == APPLIANCE_PRODUCTION_AUTH_OK,
            "HTTPS panel credential publish");
        vnf_https_qemu_require(
            appliance_management_panel_init(
                &proof->panel,
                &proof->panel_auth) == APPLIANCE_MANAGEMENT_PANEL_OK,
            "HTTPS panel init");
        vnf_https_qemu_require(
            appliance_management_panel_set_view(
                &proof->panel,
                &vnf_https_qemu_panel_view) ==
                APPLIANCE_MANAGEMENT_PANEL_OK,
            "HTTPS panel view publish");
        /*
         * ITEM-382: publish the declared dependency inventory the payload
         * contract forces onto the image, so the wire proof serves the real
         * about/licenses page.
         */
        vnf_https_qemu_require(
            appliance_management_panel_publish_declared_licenses(
                &proof->panel) == APPLIANCE_MANAGEMENT_PANEL_OK,
            "HTTPS panel licenses publish");
        /*
         * ITEM-445 step 2: bind the fixture's browser credential boundary, so
         * the QEMU proof can post the form and read the cookie back.
         */
        {
            struct appliance_management_panel_credential credential;

            memset(&credential, 0, sizeof(credential));
            credential.context = proof;
            credential.authenticate = vnf_https_qemu_panel_authenticate;
            vnf_https_qemu_require(
                appliance_management_panel_set_credential(
                    &proof->panel, &credential) ==
                    APPLIANCE_MANAGEMENT_PANEL_OK,
                "HTTPS panel credential binding");
        }
    }
    vnf_https_qemu_require(
        https_service_lifecycle_init(
            &proof->lifecycle,
            &config,
            &ops) == HTTPS_SERVICE_LIFECYCLE_OK,
        "HTTPS lifecycle init");
    vnf_https_qemu_require(
        https_service_lifecycle_start(
            &proof->lifecycle,
            1U) == HTTPS_SERVICE_LIFECYCLE_OK,
        "HTTPS lifecycle start");
    memset(&proof->packet_pool, 0, sizeof(proof->packet_pool));
    memset(&proof->packet_pressure, 0, sizeof(proof->packet_pressure));
    vnf_https_qemu_require(
        packet_pool_init(
            &proof->packet_pool,
            vnf_https_packet_storage,
            sizeof(vnf_https_packet_storage),
            VNF_HTTPS_PACKET_COUNT,
            VNF_HTTPS_PACKET_CAPACITY,
            VNF_HTTPS_PACKET_HEADROOM,
            VNF_HTTPS_PACKET_LENGTH) == PACKET_POOL_OK,
        "HTTPS packet pool init");
    vnf_https_qemu_require(
        packet_pressure_init(
            &proof->packet_pressure,
            &proof->packet_pool) == PACKET_PRESSURE_OK,
        "HTTPS packet pressure init");
}

static void vnf_https_qemu_packet_pressure(void)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct packet_pressure_snapshot snapshot;
    uint32_t index;

    for (index = 0U; index < VNF_HTTPS_PACKET_COUNT; ++index) {
        vnf_https_qemu_require(
            packet_pool_reserve(
                &proof->packet_pool,
                PACKET_POOL_DIRECTION_RX,
                &proof->packet_tokens[index]) == PACKET_POOL_OK,
            "HTTPS packet pool reserve");
    }
    vnf_https_qemu_require(
        packet_pressure_begin_update(
            &proof->packet_pressure) == PACKET_PRESSURE_OK &&
        packet_pressure_refresh_pool_counts(
            &proof->packet_pressure) == PACKET_PRESSURE_OK &&
        packet_pressure_record_rx_drop(
            &proof->packet_pressure, 1U) == PACKET_PRESSURE_OK &&
        packet_pressure_record_tx_backpressure(
            &proof->packet_pressure, 2U) == PACKET_PRESSURE_OK &&
        packet_pressure_commit(
            &proof->packet_pressure) == PACKET_PRESSURE_OK,
        "HTTPS packet pressure update");
    vnf_https_qemu_require(
        packet_pressure_snapshot(
            &proof->packet_pressure,
            &snapshot) == PACKET_PRESSURE_OK &&
        snapshot.pool_free_count == 0U &&
        snapshot.pool_in_use_count == VNF_HTTPS_PACKET_COUNT &&
        snapshot.pool_high_water_count == VNF_HTTPS_PACKET_COUNT &&
        snapshot.rx_drop_count == 1U &&
        snapshot.tx_backpressure_count == 2U,
        "HTTPS packet pressure high water");

    for (index = 0U; index < VNF_HTTPS_PACKET_COUNT; ++index) {
        vnf_https_qemu_require(
            packet_pool_release(
                &proof->packet_pool,
                &proof->packet_tokens[index]) == PACKET_POOL_OK,
            "HTTPS packet pool release");
    }
    vnf_https_qemu_require(
        packet_pressure_begin_update(
            &proof->packet_pressure) == PACKET_PRESSURE_OK &&
        packet_pressure_refresh_pool_counts(
            &proof->packet_pressure) == PACKET_PRESSURE_OK &&
        packet_pressure_commit(
            &proof->packet_pressure) == PACKET_PRESSURE_OK,
        "HTTPS packet pressure final update");
    vnf_https_qemu_require(
        packet_pressure_snapshot(
            &proof->packet_pressure,
            &snapshot) == PACKET_PRESSURE_OK &&
        snapshot.pool_free_count == VNF_HTTPS_PACKET_COUNT &&
        snapshot.pool_in_use_count == 0U &&
        snapshot.pool_high_water_count == VNF_HTTPS_PACKET_COUNT,
        "HTTPS packet pressure final snapshot");
    proof->packet_pressure_ok = 1U;
    vnf_https_qemu_mark("[HTTPS-QEMU] packet pressure ok");
}

static void vnf_https_qemu_health_update(bool final)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct https_health_metrics metrics;
    struct https_health_snapshot snapshot;
    uint64_t sequence = 0U;

    memset(&metrics, 0, sizeof(metrics));
    metrics.version = HTTPS_HEALTH_VERSION;
    metrics.listener_state = final
        ? HTTPS_HEALTH_LISTENER_STOPPED
        : HTTPS_HEALTH_LISTENER_LISTENING;
    metrics.connection_count = proof->active_count;
    metrics.accepted_connections = proof->accept_count;
    metrics.rejected_connections = proof->fifth_count;
    metrics.failed_connections = proof->malformed_count;
    metrics.failure_counts[HTTPS_HEALTH_FAILURE_HTTP_PROTOCOL] =
        proof->malformed_count;
    metrics.failure_counts[HTTPS_HEALTH_FAILURE_PEER_ABUSE] =
        proof->peer_throttle_count;
    metrics.failure_counts[HTTPS_HEALTH_FAILURE_RESOURCE_EXHAUSTION] =
        proof->fifth_count;
    vnf_https_qemu_require(
        https_health_publish(
            &proof->health,
            &metrics,
            &proof->audit_ring,
            &sequence) == HTTPS_HEALTH_OK,
        "HTTPS health publish");
    vnf_https_qemu_require(
        https_health_snapshot(
            &proof->health,
            &snapshot) == HTTPS_HEALTH_OK &&
            snapshot.generation > 0U &&
            snapshot.high_water <=
                HTTPS_HEALTH_CONNECTION_CAPACITY &&
            snapshot.correlation_id > 0U,
        "HTTPS health snapshot");
    if (final) {
        proof->health_ok = 1U;
        vnf_https_qemu_mark("[HTTPS-QEMU] health ok");
        if (event_ring_count(&proof->audit_ring) >= 1U) {
            proof->audit_ok = 1U;
            vnf_https_qemu_mark("[HTTPS-QEMU] audit ok");
        }
    }
}

static void vnf_https_qemu_restart_service(void)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;
    struct https_service_lifecycle_snapshot snapshot;
    uint32_t now_ms = hfoc_lwip_time_now();
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    uint32_t soak_number = proof->soak_restart_count + 1U;
#endif

    vnf_https_qemu_require(
        https_service_lifecycle_stop_admission(
            &proof->lifecycle,
            now_ms) == HTTPS_SERVICE_LIFECYCLE_OK,
        "HTTPS lifecycle stop admission");
    vnf_https_qemu_require(
        https_service_lifecycle_step(
            &proof->lifecycle,
            now_ms + VNF_HTTPS_LIFECYCLE_DRAIN_TIMEOUT_MS + 1U) ==
            HTTPS_SERVICE_LIFECYCLE_OK,
        "HTTPS lifecycle drain");
    vnf_https_qemu_require(
        https_service_lifecycle_finish_drain(
            &proof->lifecycle) == HTTPS_SERVICE_LIFECYCLE_OK,
        "HTTPS lifecycle finish drain");
    vnf_https_qemu_require(
        https_service_lifecycle_restart(
            &proof->lifecycle) == HTTPS_SERVICE_LIFECYCLE_OK,
        "HTTPS lifecycle restart");
    vnf_https_qemu_require(
        https_service_lifecycle_snapshot(
            &proof->lifecycle,
            &snapshot) == HTTPS_SERVICE_LIFECYCLE_OK &&
            snapshot.phase ==
                HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED &&
            snapshot.restarted_count ==
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
                soak_number,
#else
                1U,
#endif
        "HTTPS lifecycle restart snapshot");
    proof->restart_ok = 1U;
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    proof->soak_restart_count = soak_number;
    proof->restart_requested = false;
    if (soak_number == 1U) {
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] restart 1 ok");
    } else if (soak_number == 2U) {
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] restart 2 ok");
    } else if (soak_number == 3U) {
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] restart 3 ok");
    }
#else
    proof->restart_requested = true;
    vnf_https_qemu_mark("[HTTPS-QEMU] restart ok");
#endif
}

static bool vnf_https_qemu_restart_ready(void)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;

#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    uint32_t round = proof->soak_restart_count + 1U;

    if (round > VNF_HTTPS_SOAK_ROUNDS) {
        return false;
    }
    return proof->soak_request_count >=
               (round * VNF_HTTPS_SOAK_REQUESTS_PER_ROUND) &&
           proof->soak_malformed_count >= round &&
           proof->soak_reset_count >= round &&
           proof->active_count == 0U;
#else
    return proof->root_get_count != 0U &&
           proof->root_head_count != 0U &&
           proof->health_get_count != 0U &&
           proof->health_head_count != 0U &&
           proof->malformed_count != 0U &&
           proof->fifth_count != 0U &&
           proof->peer_throttle_count != 0U &&
           proof->active_count == 0U &&
           !proof->restart_requested;
#endif
}

static bool vnf_https_qemu_proof_ready(void)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;

#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    return proof->soak_request_count >=
               VNF_HTTPS_SOAK_TOTAL_REQUESTS &&
           proof->soak_restart_count >= VNF_HTTPS_SOAK_ROUNDS &&
           proof->active_count == 0U;
#else
    return proof->root_get_count != 0U &&
           proof->root_head_count != 0U &&
           proof->health_get_count != 0U &&
           proof->health_head_count != 0U &&
           proof->malformed_count != 0U &&
           proof->fifth_count != 0U &&
           proof->peer_throttle_count != 0U &&
           proof->restart_ok != 0U &&
           proof->restart_live != 0U &&
           proof->active_count == 0U &&
#ifdef HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST
           /* Once the reachability flow has been asked for, the proof is not
            * complete until the wire proved both the refusal and the restored
            * service. */
           (!vnf_reachability_triggered ||
            vnf_reachability_restored_marked) &&
#endif
           true;
#endif
}

static bool vnf_https_qemu_proof_complete(void)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;

    return vnf_https_qemu_proof_ready() &&
           proof->health_ok != 0U &&
           proof->audit_ok != 0U;
}
#endif

static err_t vnf_tls_qemu_recv(
    void *context,
    struct tcp_pcb *pcb,
    struct pbuf *packet,
    err_t error)
{
    struct vnf_tls_qemu_connection *connection = context;
    size_t length;

#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    vnf_https_qemu_note_stale_callback(connection);
#endif
    (void)error;
    if (connection == NULL) {
        return ERR_ABRT;
    }
    if (packet == NULL) {
        connection->closing = true;
        connection->peer_closed = true;
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
        vnf_tls_qemu_release_connection(connection);
#endif
        return ERR_OK;
    }
    length = packet->tot_len;
    vnf_tls_qemu_require(
        length <= sizeof(connection->rx) - connection->rx_length,
        "TLS receive buffer capacity");
    vnf_tls_qemu_require(
        pbuf_copy_partial(
            packet,
            &connection->rx[connection->rx_length],
            length,
            0U) == length,
        "TLS receive copy");
    connection->rx_length += length;
    pbuf_free(packet);
    tcp_recved(pcb, (u16_t)length);
    connection->last_activity_ms = hfoc_lwip_time_now();
    return ERR_OK;
}

static err_t vnf_tls_qemu_sent(
    void *context,
    struct tcp_pcb *pcb,
    u16_t length)
{
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    vnf_https_qemu_note_stale_callback(
        (const struct vnf_tls_qemu_connection *)context);
#endif
    (void)context;
    (void)pcb;
    (void)length;
    return ERR_OK;
}

static void vnf_tls_qemu_error(
    void *context,
    err_t error)
{
    struct vnf_tls_qemu_connection *connection = context;
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;

#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    vnf_https_qemu_note_stale_callback(connection);
    if (connection == NULL) {
        return;
    }
    if (error == ERR_RST &&
        proof->soak_reset_count < VNF_HTTPS_SOAK_ROUNDS) {
        ++proof->soak_reset_count;
        if (proof->soak_reset_count == 1U) {
            vnf_https_qemu_mark("[HTTPS-RESTART-SOAK] reset ok");
        }
    }
#else
    (void)error;
#endif
    if (connection != NULL) {
        connection->closing = false;
        vnf_tls_qemu_release_connection(connection);
    }
}

static err_t vnf_tls_qemu_poll(
    void *context,
    struct tcp_pcb *pcb)
{
    struct vnf_tls_qemu_connection *connection = context;

#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    vnf_https_qemu_note_stale_callback(connection);
#endif
    (void)pcb;
    if (connection == NULL || !connection->active) {
        return ERR_OK;
    }
    if ((uint32_t)(hfoc_lwip_time_now() -
            connection->last_activity_ms) >=
        VNF_TLS_TIMEOUT_MS) {
        ++vnf_tls_qemu_proof.timeout_count;
        connection->closing = false;
        vnf_tls_qemu_release_connection(connection);
    }
    return ERR_OK;
}

static void vnf_tls_qemu_setup_connection(
    struct vnf_tls_qemu_connection *connection)
{
    uint32_t allocator_id = 0U;
    int result;

    vnf_tls_qemu_require(
        hfoc_mbedtls_alloc_connection_open(&allocator_id),
        "TLS allocator connection open");
    connection->allocator_id = allocator_id;
    vnf_tls_qemu_require(
        hfoc_mbedtls_alloc_connection_enter(allocator_id),
        "TLS allocator connection enter");
    mbedtls_ssl_init(&connection->ssl);
    result = mbedtls_ssl_setup(
        &connection->ssl,
        &vnf_tls_qemu_proof.ssl_config);
    if (result != 0) {
        vnf_tls_qemu_require(false, "TLS SSL setup");
    }
    mbedtls_ssl_set_bio(
        &connection->ssl,
        connection,
        vnf_tls_qemu_ssl_send,
        vnf_tls_qemu_ssl_recv,
        NULL);
    connection->ssl_setup = true;
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    connection->parser_token.index = connection->index;
    connection->parser_token.generation = 1U;
    vnf_https_qemu_require(
        http_parser_start(
            &vnf_tls_qemu_proof.parser,
            &connection->parser_token) == HTTP_PARSER_OK,
        "HTTPS parser start");
    connection->parser_started = true;
#endif
}

static err_t vnf_tls_qemu_accept(
    void *context,
    struct tcp_pcb *pcb,
    uint32_t generation)
{
    struct vnf_tls_qemu_proof_state *proof = context;
    struct vnf_tls_qemu_connection *connection = NULL;
    uint32_t index;

    (void)generation;
    if (proof->active_count >= VNF_TLS_CONNECTION_COUNT) {
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
        if (proof->fifth_count == 0U) {
            proof->fifth_count = 1U;
            vnf_https_qemu_health_update(false);
            vnf_https_qemu_mark("[HTTPS-QEMU] fifth denial ok");
        }
#endif
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    for (index = 0U; index < VNF_TLS_CONNECTION_COUNT; ++index) {
        if (!proof->connections[index].active) {
            connection = &proof->connections[index];
            break;
        }
    }
    if (connection == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    memset(connection, 0, sizeof(*connection));
    connection->active = true;
    connection->index = index;
    connection->generation = 1U;
    connection->pcb = pcb;
    connection->last_activity_ms = hfoc_lwip_time_now();
    tcp_arg(pcb, connection);
    tcp_recv(pcb, vnf_tls_qemu_recv);
    tcp_sent(pcb, vnf_tls_qemu_sent);
    tcp_poll(pcb, vnf_tls_qemu_poll, 1U);
    tcp_err(pcb, vnf_tls_qemu_error);
    ++proof->active_count;
    ++proof->accept_count;
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    if (proof->restart_ok == 0U &&
        proof->active_count == VNF_TLS_CONNECTION_COUNT &&
        proof->fifth_count == 0U) {
        vnf_https_qemu_mark("[HTTPS-QEMU] four held");
    }
    if (proof->restart_ok != 0U &&
        proof->restart_live == 0U) {
        proof->restart_live = 1U;
        vnf_https_qemu_mark("[HTTPS-QEMU] restart live");
    }
#endif
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    /* Defer TLS setup until ClientHello so fifth denial is pre-TLS. */
#else
    vnf_tls_qemu_setup_connection(connection);
#endif
    return ERR_OK;
}

static void vnf_tls_qemu_finish_connection(
    struct vnf_tls_qemu_connection *connection)
{
    static const char response[] = VNF_TLS_RESPONSE;
    int result;

    if (!connection->active || !connection->handshake_done) {
        return;
    }
    if (!connection->response_sent) {
        result = mbedtls_ssl_write(
            &connection->ssl,
            (const unsigned char *)response,
            sizeof(response) - 1U);
        if (result < 0) {
            return;
        }
        vnf_tls_qemu_require(
            (size_t)result == sizeof(response) - 1U,
            "TLS response write");
        connection->response_sent = true;
    }
    if (!connection->close_notify_sent) {
        result = mbedtls_ssl_close_notify(&connection->ssl);
        if (result != 0 && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            vnf_tls_qemu_require(false, "TLS close notify");
        }
        if (result == 0) {
            connection->close_notify_sent = true;
        } else {
            return;
        }
    }
    connection->closing = true;
    vnf_tls_qemu_release_connection(connection);
}

static void vnf_tls_qemu_handle_handshake_result(
    struct vnf_tls_qemu_connection *connection,
    int result)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;

    if (result == 0) {
        connection->handshake_done = true;
        ++proof->success_count;
        if (proof->success_count == 1U) {
            vnf_tls_qemu_mark("[TLS-QEMU] handshake ok");
        } else if (
            proof->success_count == 2U &&
            proof->malformed_count >= 1U &&
            proof->entropy_count >= 1U) {
            ++proof->recovery_count;
            vnf_tls_qemu_mark("[TLS-QEMU] recovery ok");
        }
        return;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ ||
        result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return;
    }
    if (vnf_tls_qemu_rng_deny && proof->entropy_count == 0U) {
        ++proof->entropy_count;
        vnf_tls_qemu_mark("[TLS-QEMU] entropy denial ok");
    } else if (proof->malformed_count == 0U) {
        ++proof->malformed_count;
        vnf_tls_qemu_mark("[TLS-QEMU] malformed handshake ok");
    }
    vnf_tls_qemu_rng_deny = false;
    connection->closing = false;
    vnf_tls_qemu_release_connection(connection);
}

static void vnf_tls_qemu_step_connection(
    struct vnf_tls_qemu_connection *connection)
{
    int result;
    uint32_t step;

    if (!connection->active) {
        return;
    }
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    vnf_https_qemu_step_connection(connection);
    return;
#endif
    if (!connection->ssl_setup) {
        return;
    }
    vnf_tls_qemu_drain_entropy_magic(connection);
    if (connection->handshake_done) {
        vnf_tls_qemu_finish_connection(connection);
        return;
    }
    for (step = 0U; step < VNF_TLS_HANDSHAKE_STEP_LIMIT; ++step) {
        result = mbedtls_ssl_handshake(&connection->ssl);
        if (result != MBEDTLS_ERR_SSL_WANT_READ &&
            result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            break;
        }
    }
    vnf_tls_qemu_handle_handshake_result(connection, result);
}

static bool vnf_tls_qemu_mbedtls_balanced(
    const struct hfoc_mbedtls_alloc_snapshot *before,
    const struct hfoc_mbedtls_alloc_snapshot *after)
{
    unsigned int pool_class;
    uint32_t index;

    if (before == NULL || after == NULL ||
        before->version != after->version ||
        before->total_used_bytes != after->total_used_bytes ||
        before->total_free_bytes != after->total_free_bytes ||
        before->total_high_water_bytes > after->total_high_water_bytes) {
        return false;
    }
    for (pool_class = 0U;
         pool_class < HFOC_MBEDTLS_POOL_COUNT;
         ++pool_class) {
        const struct hfoc_mbedtls_alloc_pool_snapshot *before_pool =
            &before->pools[pool_class];
        const struct hfoc_mbedtls_alloc_pool_snapshot *after_pool =
            &after->pools[pool_class];

        if (before_pool->slot_size != after_pool->slot_size ||
            before_pool->slot_count != after_pool->slot_count ||
            before_pool->free_count != after_pool->free_count ||
            before_pool->used_count != after_pool->used_count ||
            before_pool->high_water_count >
                after_pool->high_water_count) {
            return false;
        }
    }
    for (index = 0U; index < HFOC_MBEDTLS_CONNECTION_COUNT; ++index) {
        if (before->connections[index].active !=
                after->connections[index].active ||
            before->connections[index].id !=
                after->connections[index].id ||
            before->connections[index].allocation_count !=
                after->connections[index].allocation_count ||
            before->connections[index].used_bytes !=
                after->connections[index].used_bytes) {
            return false;
        }
    }
    return true;
}

static bool vnf_tls_qemu_proof_complete(void)
{
    return vnf_tls_qemu_proof.success_count >= 2U &&
           vnf_tls_qemu_proof.malformed_count >= 1U &&
           vnf_tls_qemu_proof.entropy_count >= 1U &&
           vnf_tls_qemu_proof.active_count == 0U;
}

static void vnf_tls_qemu_teardown(void)
{
    struct vnf_tls_qemu_proof_state *proof = &vnf_tls_qemu_proof;

    if (proof->key_published) {
        mbedtls_pk_free(&proof->key);
        proof->key_published = false;
    }
    if (proof->certificate_published) {
        mbedtls_x509_crt_free(&proof->certificate);
        proof->certificate_published = false;
    }
    if (proof->ssl_config_published) {
        mbedtls_ssl_config_free(&proof->ssl_config);
        proof->ssl_config_published = false;
    }
    if (proof->crypto_published) {
        mbedtls_psa_crypto_free();
        proof->crypto_published = false;
    }
}

static void vnf_tls_qemu_proof_run(
    struct virtio_net_rx *rx,
    struct virtio_net_tx *tx,
    struct virtio_net_queue_composition *composition)
{
    struct hfoc_lwip_netif *netif = &vnf_tcp_proof.netif;
    struct network_ipv4_config config;
    struct network_ipv4_port_policy port_policy;
    struct hfoc_lwip_alloc_snapshot lwip_end;
    struct hfoc_mbedtls_alloc_snapshot mbedtls_end;
    psa_status_t psa_status;
    uint32_t iterations = 0U;
    uint32_t index;
    int result;

    memset(&vnf_tls_qemu_proof, 0, sizeof(vnf_tls_qemu_proof));
    memset(&vnf_tcp_proof, 0, sizeof(vnf_tcp_proof));
#ifdef HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST
    appliance_management_plane_qemu_test_init();
    appliance_management_reachability_qemu_test_init();
#endif
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    vnf_https_qemu_init_contracts();
#endif
    vnf_tcp_proof.tx = tx;
    vnf_tcp_proof.composition = composition;
    vnf_tcp_proof.rx = rx;
    vnf_tcp_proof.tx_frame_physical =
        vnf_payload_physical + VNF_TCP_TX_FRAME_OFFSET;
    vnf_tcp_proof.tx_frame_virtual =
        (uint8_t *)vnf_payload_virtual + VNF_TCP_TX_FRAME_OFFSET;

    vnf_tls_qemu_require(
        hfoc_lwip_time_ready(),
        "TLS time ready");
    if (!hfoc_lwip_alloc_ready()) {
        vnf_tls_qemu_require(
            hfoc_lwip_alloc_init(),
            "TLS lwIP allocator init");
    }
    if (!hfoc_mbedtls_alloc_ready()) {
        vnf_tls_qemu_require(
            hfoc_mbedtls_alloc_init(),
            "TLS allocator init");
    }
    vnf_tls_qemu_proof.mbedtls_start =
        hfoc_mbedtls_alloc_snapshot();

    psa_status = psa_crypto_init();
    vnf_tls_qemu_require(
        psa_status == PSA_SUCCESS,
        "TLS PSA init");
    vnf_tls_qemu_proof.crypto_published = true;
    mbedtls_x509_crt_init(&vnf_tls_qemu_proof.certificate);
    mbedtls_pk_init(&vnf_tls_qemu_proof.key);
    mbedtls_ssl_config_init(&vnf_tls_qemu_proof.ssl_config);
    vnf_tls_qemu_proof.certificate_published = true;
    vnf_tls_qemu_proof.key_published = true;
    vnf_tls_qemu_proof.ssl_config_published = true;

    result = mbedtls_x509_crt_parse_der(
        &vnf_tls_qemu_proof.certificate,
        hfoc_mbedtls_test_secret_certificate(),
        hfoc_mbedtls_test_secret_certificate_length());
    vnf_tls_qemu_require(result == 0, "TLS certificate parse");
    result = mbedtls_pk_parse_key(
        &vnf_tls_qemu_proof.key,
        hfoc_mbedtls_test_secret_key(),
        hfoc_mbedtls_test_secret_key_length(),
        NULL,
        0U,
        vnf_tls_qemu_rng_cb,
        NULL);
    vnf_tls_qemu_require(result == 0, "TLS key parse");
    result = mbedtls_ssl_config_defaults(
        &vnf_tls_qemu_proof.ssl_config,
        MBEDTLS_SSL_IS_SERVER,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    vnf_tls_qemu_require(result == 0, "TLS SSL defaults");
    mbedtls_ssl_conf_rng(
        &vnf_tls_qemu_proof.ssl_config,
        vnf_tls_qemu_rng_cb,
        NULL);
    mbedtls_ssl_conf_min_tls_version(
        &vnf_tls_qemu_proof.ssl_config,
        MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(
        &vnf_tls_qemu_proof.ssl_config,
        MBEDTLS_SSL_VERSION_TLS1_3);
    result = mbedtls_ssl_conf_own_cert(
        &vnf_tls_qemu_proof.ssl_config,
        &vnf_tls_qemu_proof.certificate,
        &vnf_tls_qemu_proof.key);
    vnf_tls_qemu_require(result == 0, "TLS own certificate");

    memset(netif, 0, sizeof(*netif));
    netif->published = true;
    netif->live = true;
    netif->version = HFOC_LWIP_NETIF_VERSION;
    netif->generation = 1U;
    netif->netif.mtu = HFOC_LWIP_NETIF_MTU;
    netif->netif.flags = HFOC_LWIP_NETIF_FLAGS;
    netif->netif.hwaddr_len = NETWORK_DEVICE_MAC_LENGTH;
    memcpy(netif->netif.hwaddr,
           (const uint8_t[6]){0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U},
           NETWORK_DEVICE_MAC_LENGTH);
    netif->netif.name[0] = HFOC_LWIP_NETIF_NAME0;
    netif->netif.name[1] = HFOC_LWIP_NETIF_NAME1;
    netif->netif.num = HFOC_LWIP_NETIF_NUM;
    IP4_ADDR(&netif->netif.ip_addr, 10U, 0U, 2U, 15U);
    IP4_ADDR(&netif->netif.netmask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&netif->netif.gw, 10U, 0U, 2U, 2U);
    netif->netif.input = ethernet_input;
    netif->netif.output = etharp_output;
    netif->netif.linkoutput = vnf_tcp_linkoutput;
    netif->netif.state = &vnf_tcp_proof;
    netif_set_default(&netif->netif);
    netif_set_up(&netif->netif);
    netif_set_link_up(&netif->netif);

    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[1] = 0U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[1] = 0U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[1] = 0U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    memset(&port_policy, 0, sizeof(port_policy));
    port_policy.version = NETWORK_IPV4_PORT_POLICY_VERSION;
    port_policy.port = VNF_TCP_LISTENER_PORT;

    vnf_tls_qemu_proof.listener_callback.accept =
        vnf_tls_qemu_accept;
    vnf_tls_qemu_proof.listener_callback.context =
        &vnf_tls_qemu_proof;
    vnf_tls_qemu_require(
        hfoc_lwip_tcp_init(
            &vnf_tls_qemu_proof.listener,
            netif,
            &config,
            &port_policy,
            &vnf_tls_qemu_proof.listener_callback,
            1U) == HFOC_LWIP_TCP_OK,
        "TLS listener init");
    vnf_tls_qemu_proof.lwip_start =
        hfoc_lwip_alloc_snapshot();
    vnf_tls_qemu_require(
        hfoc_lwip_tcp_bind(
            &vnf_tls_qemu_proof.listener,
            &port_policy) == HFOC_LWIP_TCP_OK,
        "TLS listener bind");
    vnf_tls_qemu_require(
        hfoc_lwip_tcp_listen(
            &vnf_tls_qemu_proof.listener) == HFOC_LWIP_TCP_OK,
        "TLS listener listen");
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
#ifdef HFOC_HTTPS_ENTROPY_FAIL_CLOSED_QEMU_TEST
    vnf_https_entropy_fail_closed_matrix();
#endif
    vnf_https_qemu_mark("[HTTPS-QEMU] proof init");
    vnf_https_qemu_mark("[HTTPS-EXHAUSTION-MATRIX] harness ready");
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    vnf_https_qemu_mark("[HTTPS-RESTART-SOAK] proof init");
#endif
#else
    vnf_tls_qemu_mark("[TLS-QEMU] proof init");
#endif
    vnf_notify_queue(
        0U,
        (uint16_t)virtio_available_publication_visible_index(
            &composition->receive.available));
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    vnf_https_qemu_mark("[HTTPS-QEMU] listener live");
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    vnf_https_qemu_mark("[HTTPS-RESTART-SOAK] listener live");
#endif
#else
    vnf_tls_qemu_mark("[TLS-QEMU] listener live");
#endif
    cpu_sti();

    for (;;) {
        bool progress = false;
        const void *payload;
        struct virtio_net_rx_frame frame;

        payload = vnf_tcp_rx_payload_for_next(rx);
        while (payload != NULL) {
            enum virtio_net_rx_status status =
                virtio_net_rx_complete(rx, payload, &frame);

            vnf_tls_qemu_require(
                status == VIRTIO_NET_RX_OK,
                "TLS RX complete");
            vnf_tcp_feed_rx_frame(&frame);
            vnf_tcp_account_rx_completion(rx);
            vnf_tcp_repost_rx(rx, frame.buffer_index);
            sys_check_timeouts();
            progress = true;
            payload = vnf_tcp_rx_payload_for_next(rx);
        }

        while (tx->submitted_count != 0U) {
            struct virtio_net_tx_completion completion;
            enum virtio_net_tx_status status =
                virtio_net_tx_reclaim(tx, &completion);

            if (status == VIRTIO_NET_TX_USED_EMPTY) {
                break;
            }
            vnf_tls_qemu_require(
                status == VIRTIO_NET_TX_OK,
                "TLS TX reclaim");
            vnf_tls_qemu_require(
                completion.generation == tx->generation,
                "TLS TX completion generation");
            vnf_tcp_account_tx_reclaim();
            progress = true;
        }

        sys_check_timeouts();
        for (index = 0U;
             index < VNF_TLS_CONNECTION_COUNT;
             ++index) {
            struct vnf_tls_qemu_connection *connection =
                &vnf_tls_qemu_proof.connections[index];

            if (connection->active) {
                vnf_tls_qemu_step_connection(connection);
            }
        }

#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
        if (vnf_tls_qemu_proof.malformed_count != 0U &&
            vnf_tls_qemu_proof.peer_throttle_count != 0U &&
            vnf_tls_qemu_proof.active_count == 0U &&
            vnf_tls_qemu_proof.malformed_drained_ok == 0U) {
            vnf_tls_qemu_proof.malformed_drained_ok = 1U;
            vnf_https_qemu_mark(
                "[HTTPS-EXHAUSTION-MATRIX] malformed drained");
        }
        if (vnf_tls_qemu_proof.active_count ==
                VNF_TLS_CONNECTION_COUNT &&
            vnf_tls_qemu_proof.fifth_count != 0U &&
            vnf_tls_qemu_proof.packet_pressure_ok == 0U) {
            vnf_https_qemu_packet_pressure();
        }
        if (vnf_tls_qemu_proof.active_count ==
                (VNF_TLS_CONNECTION_COUNT - 1U) &&
            vnf_tls_qemu_proof.packet_pressure_ok != 0U &&
            vnf_tls_qemu_proof.root_get_count == 0U &&
            vnf_tls_qemu_proof.slot_released_ok == 0U) {
            vnf_tls_qemu_proof.slot_released_ok = 1U;
            vnf_https_qemu_mark("[HTTPS-EXHAUSTION-MATRIX] release ok");
        }
        if (vnf_tls_qemu_proof.active_count ==
                VNF_TLS_CONNECTION_COUNT &&
            vnf_tls_qemu_proof.accept_count >
                VNF_TLS_CONNECTION_COUNT &&
            vnf_tls_qemu_proof.packet_pressure_ok != 0U &&
            vnf_tls_qemu_proof.slot_released_ok != 0U &&
            vnf_tls_qemu_proof.exhaustion_recovery_ok == 0U) {
            vnf_tls_qemu_proof.exhaustion_recovery_ok = 1U;
            vnf_https_qemu_mark("[HTTPS-QEMU] exhaustion recovery ok");
        }
        if (vnf_https_qemu_restart_ready()) {
            vnf_https_qemu_restart_service();
        }
        if (vnf_https_qemu_proof_ready()) {
            vnf_https_qemu_mark("[HTTPS-QEMU] proof ready");
            vnf_https_qemu_health_update(true);
            break;
        }
#else
        if (vnf_tls_qemu_proof_complete()) {
            if (!vnf_tls_qemu_proof.settle_armed) {
                vnf_tls_qemu_proof.settle_armed = true;
                vnf_tls_qemu_proof.settle_deadline_ms =
                    hfoc_lwip_time_now() + VNF_TLS_SETTLE_MS;
            } else if (
                (int32_t)(
                    hfoc_lwip_time_now() -
                    vnf_tls_qemu_proof.settle_deadline_ms) >= 0) {
                break;
            }
        } else {
            vnf_tls_qemu_proof.settle_armed = false;
        }
#endif

        if (progress) {
            continue;
        }
        ++iterations;
        if ((iterations & 0xFFFFU) == 0U) {
            sys_check_timeouts();
        }
        __asm__ volatile ("pause" : : : "memory");
    }

    for (index = 0U;
         index < VNF_TLS_CONNECTION_COUNT;
         ++index) {
        if (vnf_tls_qemu_proof.connections[index].active) {
            vnf_tls_qemu_proof.connections[index].closing = false;
            vnf_tls_qemu_release_connection(
                &vnf_tls_qemu_proof.connections[index]);
        }
    }
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    /* Drain lwIP TIME-WAIT PCBs before the deterministic pool snapshot. */
    while (tcp_tw_pcbs != NULL) {
        tcp_abort(tcp_tw_pcbs);
    }
    {
        uint32_t teardown_deadline_ms =
            hfoc_lwip_time_now() + VNF_TLS_SETTLE_MS;

        while ((int32_t)(teardown_deadline_ms -
                hfoc_lwip_time_now()) > 0) {
            sys_check_timeouts();
        }
    }
#endif
    etharp_cleanup_netif(&vnf_tcp_proof.netif.netif);
    vnf_tls_qemu_require(
        hfoc_lwip_tcp_shutdown(
            &vnf_tls_qemu_proof.listener) == HFOC_LWIP_TCP_OK,
        "TLS listener shutdown");
    lwip_end = hfoc_lwip_alloc_snapshot();
    vnf_tls_qemu_require(
        vnf_tcp_alloc_balanced(
            &vnf_tls_qemu_proof.lwip_start,
            &lwip_end),
        "TLS lwIP allocator balance");
    vnf_tls_qemu_teardown();
    mbedtls_end = hfoc_mbedtls_alloc_snapshot();
    vnf_tls_qemu_require(
        vnf_tls_qemu_mbedtls_balanced(
            &vnf_tls_qemu_proof.mbedtls_start,
            &mbedtls_end),
        "TLS allocator balance");
#ifdef HFOC_HTTPS_SERVICE_QEMU_TEST
    vnf_https_qemu_require(
        http_parser_active_count(&vnf_tls_qemu_proof.parser) == 0U,
        "HTTPS parser balance");
    vnf_https_qemu_require(
        packet_pool_free_count(&vnf_tls_qemu_proof.packet_pool) ==
            VNF_HTTPS_PACKET_COUNT &&
        packet_pool_reserved_count(&vnf_tls_qemu_proof.packet_pool) == 0U &&
        packet_pool_active_count(&vnf_tls_qemu_proof.packet_pool) == 0U,
        "HTTPS packet pool balance");
    vnf_https_qemu_require(
        event_ring_count(&vnf_tls_qemu_proof.audit_ring) >= 1U &&
            vnf_tls_qemu_proof.health_ok != 0U &&
            vnf_tls_qemu_proof.audit_ok != 0U,
        "HTTPS health and audit evidence");
    vnf_https_qemu_mark("[HTTPS-QEMU] balanced pool");
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    {
        struct https_service_lifecycle_snapshot soak_snapshot;

        vnf_https_qemu_require(
            https_service_lifecycle_snapshot(
                &vnf_tls_qemu_proof.lifecycle,
                &soak_snapshot) == HTTPS_SERVICE_LIFECYCLE_OK &&
                soak_snapshot.phase ==
                    HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED &&
                soak_snapshot.restarted_count ==
                    VNF_HTTPS_SOAK_ROUNDS &&
                soak_snapshot.stopped_count ==
                    VNF_HTTPS_SOAK_ROUNDS &&
                soak_snapshot.active_connection_count ==
                    HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY,
            "HTTPS soak lifecycle counts");
        vnf_https_qemu_require(
            vnf_tls_qemu_proof.soak_request_count ==
                VNF_HTTPS_SOAK_TOTAL_REQUESTS &&
            vnf_tls_qemu_proof.soak_restart_count ==
                VNF_HTTPS_SOAK_ROUNDS,
            "HTTPS soak request/restart counts");
        vnf_https_qemu_require(
            vnf_tls_qemu_proof.soak_malformed_count ==
                VNF_HTTPS_SOAK_ROUNDS,
            "HTTPS soak malformed count");
        vnf_https_qemu_require(
            vnf_tls_qemu_proof.soak_reset_count ==
                VNF_HTTPS_SOAK_ROUNDS,
            "HTTPS soak reset count");
        vnf_https_qemu_require(
            vnf_tls_qemu_proof.soak_stale_callback_count == 0U,
            "HTTPS soak stale callback count");
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] no stale callback");
        vnf_https_qemu_mark(
            "[HTTPS-RESTART-SOAK] balanced counts");
    }
#endif
    if (vnf_tls_qemu_proof.slot_released_ok != 0U &&
        vnf_tls_qemu_proof.exhaustion_recovery_ok != 0U) {
        vnf_https_qemu_mark(
            "[HTTPS-EXHAUSTION-MATRIX] balanced counts");
    }
    vnf_https_qemu_mark("[HTTPS-QEMU] clean shutdown");
    vnf_https_qemu_mark("[HTTPS-QEMU] proof OK");
#ifdef HFOC_HTTPS_RESTART_SOAK_QEMU_TEST
    vnf_https_qemu_mark("[HTTPS-RESTART-SOAK] clean shutdown");
    vnf_https_qemu_mark("[HTTPS-RESTART-SOAK] proof OK");
#endif
#else
    vnf_tls_qemu_mark("[TLS-QEMU] balanced pool");
    vnf_tls_qemu_mark("[TLS-QEMU] clean shutdown");
    vnf_tls_qemu_mark("[TLS-QEMU] proof OK");
#endif
}

#endif

static void vnf_tcp_listener_proof(
    struct virtio_net_rx *rx,
    struct virtio_net_tx *tx,
    struct virtio_net_queue_composition *composition)
{
    struct hfoc_lwip_netif *netif = &vnf_tcp_proof.netif;
    struct network_ipv4_config config;
    struct network_ipv4_port_policy port_policy;
    struct hfoc_lwip_alloc_snapshot alloc_end;
    uint32_t iterations = 0U;
    uint32_t settle_deadline_ms = 0U;
    bool settle_armed = false;
    uint32_t index;

    memset(&vnf_tcp_proof, 0, sizeof(vnf_tcp_proof));
    vnf_tcp_proof.tx = tx;
    vnf_tcp_proof.composition = composition;
    vnf_tcp_proof.rx = rx;
    vnf_tcp_proof.tx_frame_physical =
        vnf_payload_physical + VNF_TCP_TX_FRAME_OFFSET;
    vnf_tcp_proof.tx_frame_virtual =
        (uint8_t *)vnf_payload_virtual + VNF_TCP_TX_FRAME_OFFSET;

    vnf_tcp_require(hfoc_lwip_time_ready(), "TCP time ready");
    if (!hfoc_lwip_alloc_ready()) {
        vnf_tcp_require(
            hfoc_lwip_alloc_init(),
            "TCP allocator init");
    }

    memset(netif, 0, sizeof(*netif));
    netif->published = true;
    netif->live = true;
    netif->version = HFOC_LWIP_NETIF_VERSION;
    netif->generation = 1U;
    netif->netif.mtu = HFOC_LWIP_NETIF_MTU;
    netif->netif.flags = HFOC_LWIP_NETIF_FLAGS;
    netif->netif.hwaddr_len = NETWORK_DEVICE_MAC_LENGTH;
    memcpy(netif->netif.hwaddr,
           (const uint8_t[6]){0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U},
           NETWORK_DEVICE_MAC_LENGTH);
    netif->netif.name[0] = HFOC_LWIP_NETIF_NAME0;
    netif->netif.name[1] = HFOC_LWIP_NETIF_NAME1;
    netif->netif.num = HFOC_LWIP_NETIF_NUM;
    IP4_ADDR(&netif->netif.ip_addr, 10U, 0U, 2U, 15U);
    IP4_ADDR(&netif->netif.netmask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&netif->netif.gw, 10U, 0U, 2U, 2U);
    netif->netif.input = ethernet_input;
    netif->netif.output = etharp_output;
    netif->netif.linkoutput = vnf_tcp_linkoutput;
    netif->netif.state = &vnf_tcp_proof;
    netif_set_default(&netif->netif);
    netif_set_up(&netif->netif);
    netif_set_link_up(&netif->netif);

    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[1] = 0U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[1] = 0U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[1] = 0U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    memset(&port_policy, 0, sizeof(port_policy));
    port_policy.version = NETWORK_IPV4_PORT_POLICY_VERSION;
    port_policy.port = VNF_TCP_LISTENER_PORT;

    vnf_tcp_proof.listener_callback.accept = vnf_tcp_accept;
    vnf_tcp_proof.listener_callback.context = &vnf_tcp_proof;
    vnf_tcp_require(
        hfoc_lwip_tcp_init(
            &vnf_tcp_proof.listener,
            netif,
            &config,
            &port_policy,
            &vnf_tcp_proof.listener_callback,
            1U) == HFOC_LWIP_TCP_OK,
        "TCP listener init");
    vnf_tcp_proof.alloc_start = hfoc_lwip_alloc_snapshot();
    vnf_tcp_require(
        hfoc_lwip_tcp_bind(
            &vnf_tcp_proof.listener,
            &port_policy) == HFOC_LWIP_TCP_OK,
        "TCP listener bind");
    vnf_tcp_require(
        hfoc_lwip_tcp_listen(
            &vnf_tcp_proof.listener) == HFOC_LWIP_TCP_OK,
        "TCP listener listen");
    serial_write("[TCP-LISTENER] proof init\n");
    vnf_notify_queue(
        0U,
        (uint16_t)virtio_available_publication_visible_index(
            &composition->receive.available));
    serial_write("[TCP-LISTENER] listener live\n");
    cpu_sti();

    for (;;) {
        bool progress = false;
        const void *payload;
        struct virtio_net_rx_frame frame;

        payload = vnf_tcp_rx_payload_for_next(rx);
        while (payload != NULL) {
            enum virtio_net_rx_status status = virtio_net_rx_complete(
                rx, payload, &frame);

            if (status != VIRTIO_NET_RX_OK) {
                {
                    const uint64_t serial_flags = serial_sequence_begin();
                    serial_write("[TCP-LISTENER] rx status: ");
                    serial_write(virtio_net_rx_status_string(status));
                    serial_write("\n");
                    serial_sequence_end(serial_flags);
                }
                vnf_tcp_fail("TCP RX complete");
            }
            vnf_tcp_feed_rx_frame(&frame);
            vnf_tcp_account_rx_completion(rx);
            vnf_tcp_repost_rx(rx, frame.buffer_index);
            sys_check_timeouts();
            vnf_tcp_check_timeouts();
            progress = true;
            payload = vnf_tcp_rx_payload_for_next(rx);
        }

        while (tx->submitted_count != 0U) {
            struct virtio_net_tx_completion completion;
            enum virtio_net_tx_status status =
                virtio_net_tx_reclaim(tx, &completion);

            if (status == VIRTIO_NET_TX_USED_EMPTY) {
                break;
            }
            vnf_tcp_require(status == VIRTIO_NET_TX_OK, "TCP TX reclaim");
            vnf_tcp_require(
                completion.generation == tx->generation,
                "TCP TX completion generation");
            vnf_tcp_account_tx_reclaim();
            progress = true;
        }

        sys_check_timeouts();
        vnf_tcp_check_timeouts();
        if (vnf_tcp_proof_complete()) {
            if (!settle_armed) {
                settle_armed = true;
                settle_deadline_ms =
                    hfoc_lwip_time_now() + VNF_TCP_SETTLE_MS;
            } else if (
                (int32_t)(hfoc_lwip_time_now() - settle_deadline_ms) >= 0) {
                break;
            }
        } else {
            settle_armed = false;
        }
        if (progress) {
            continue;
        }
        ++iterations;
        if ((iterations & 0xFFFFU) == 0U) {
            vnf_tcp_check_timeouts();
        }
        __asm__ volatile ("pause" : : : "memory");
    }

    for (index = 0U; index < VNF_TCP_MAX_CONNECTIONS; ++index) {
        struct vnf_tcp_connection_state *connection_state =
            &vnf_tcp_proof.connections[index];

        if (connection_state->active &&
            connection_state->connection.pcb != NULL) {
            tcp_abort(connection_state->connection.pcb);
            vnf_tcp_release_connection(connection_state, false);
        }
    }
    etharp_cleanup_netif(&netif->netif);
    vnf_tcp_require(
        hfoc_lwip_tcp_shutdown(&vnf_tcp_proof.listener) ==
            HFOC_LWIP_TCP_OK,
        "TCP listener shutdown");
    alloc_end = hfoc_lwip_alloc_snapshot();
    vnf_tcp_require(
        vnf_tcp_alloc_balanced(
            &vnf_tcp_proof.alloc_start,
            &alloc_end),
        "TCP allocator balance");
    vnf_tcp_mark("[TCP-LISTENER] balanced pool");
    vnf_tcp_mark("[TCP-LISTENER] clean shutdown");
    vnf_tcp_mark("[TCP-LISTENER] proof OK");
}

#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
static void vnf_netres_listener_proof(
    struct virtio_net_rx *rx,
    struct virtio_net_tx *tx,
    struct virtio_net_queue_composition *composition)
{
    struct hfoc_lwip_netif *netif = &vnf_tcp_proof.netif;
    struct network_ipv4_config config;
    struct network_ipv4_port_policy port_policy;
    struct hfoc_lwip_alloc_snapshot alloc_end;
    struct hfoc_lwip_tcp_drain_restart_config drain_config;
    uint32_t iterations = 0U;
    uint32_t settle_deadline_ms = 0U;
    bool settle_armed = false;
    uint32_t index;

    memset(&vnf_tcp_proof, 0, sizeof(vnf_tcp_proof));
    vnf_tcp_proof.tx = tx;
    vnf_tcp_proof.composition = composition;
    vnf_tcp_proof.rx = rx;
    vnf_tcp_proof.tx_frame_physical =
        vnf_payload_physical + VNF_TCP_TX_FRAME_OFFSET;
    vnf_tcp_proof.tx_frame_virtual =
        (uint8_t *)vnf_payload_virtual + VNF_TCP_TX_FRAME_OFFSET;

    vnf_tcp_require(hfoc_lwip_time_ready(), "network resource time ready");
    if (!hfoc_lwip_alloc_ready()) {
        vnf_tcp_require(
            hfoc_lwip_alloc_init(),
            "network resource allocator init");
    }

    memset(netif, 0, sizeof(*netif));
    netif->published = true;
    netif->live = true;
    netif->version = HFOC_LWIP_NETIF_VERSION;
    netif->generation = 1U;
    netif->netif.mtu = HFOC_LWIP_NETIF_MTU;
    netif->netif.flags = HFOC_LWIP_NETIF_FLAGS;
    netif->netif.hwaddr_len = NETWORK_DEVICE_MAC_LENGTH;
    memcpy(netif->netif.hwaddr,
           (const uint8_t[6]){0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U},
           NETWORK_DEVICE_MAC_LENGTH);
    netif->netif.name[0] = HFOC_LWIP_NETIF_NAME0;
    netif->netif.name[1] = HFOC_LWIP_NETIF_NAME1;
    netif->netif.num = HFOC_LWIP_NETIF_NUM;
    IP4_ADDR(&netif->netif.ip_addr, 10U, 0U, 2U, 15U);
    IP4_ADDR(&netif->netif.netmask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&netif->netif.gw, 10U, 0U, 2U, 2U);
    netif->netif.input = ethernet_input;
    netif->netif.output = etharp_output;
    netif->netif.linkoutput = vnf_tcp_linkoutput;
    netif->netif.state = &vnf_tcp_proof;
    netif_set_default(&netif->netif);
    netif_set_up(&netif->netif);
    netif_set_link_up(&netif->netif);

    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[1] = 0U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[1] = 0U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[1] = 0U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    memset(&port_policy, 0, sizeof(port_policy));
    port_policy.version = NETWORK_IPV4_PORT_POLICY_VERSION;
    port_policy.port = VNF_TCP_LISTENER_PORT;

    vnf_tcp_require(
        hfoc_lwip_tcp_pool_init(&vnf_tcp_proof.pool) ==
            HFOC_LWIP_TCP_POOL_OK,
        "network resource pool init");
    vnf_tcp_require(
        hfoc_lwip_tcp_deadline_init(&vnf_tcp_proof.deadline) ==
            HFOC_LWIP_TCP_DEADLINE_OK,
        "network resource deadline init");
    vnf_tcp_require(
        hfoc_lwip_tcp_backlog_init(&vnf_tcp_proof.backlog) ==
            HFOC_LWIP_TCP_BACKLOG_OK,
        "network resource backlog init");
    vnf_tcp_require(
        hfoc_lwip_tcp_quarantine_init(&vnf_tcp_proof.quarantine) ==
            HFOC_LWIP_TCP_QUARANTINE_OK,
        "network resource quarantine init");
    vnf_tcp_require(
        hfoc_lwip_tcp_close_policy_init(&vnf_tcp_proof.close_policy) ==
            HFOC_LWIP_TCP_CLOSE_POLICY_OK,
        "network resource close policy init");
    vnf_tcp_require(
        packet_pool_init(
            &vnf_tcp_proof.packet_pool,
            vnf_netres_packet_storage,
            sizeof(vnf_netres_packet_storage),
            VNF_NETRES_PACKET_COUNT,
            VNF_NETRES_PACKET_CAPACITY,
            VNF_NETRES_PACKET_HEADROOM,
            VNF_NETRES_PACKET_LENGTH) == PACKET_POOL_OK,
        "network resource packet pool init");
    vnf_tcp_require(
        packet_pressure_init(
            &vnf_tcp_proof.packet_pressure,
            &vnf_tcp_proof.packet_pool) == PACKET_PRESSURE_OK,
        "network resource packet pressure init");

    memset(&drain_config, 0, sizeof(drain_config));
    drain_config.version = HFOC_LWIP_TCP_DRAIN_RESTART_VERSION;
    drain_config.drain_timeout_ms = VNF_NETRES_DRAIN_TIMEOUT_MS;
    vnf_tcp_require(
        hfoc_lwip_tcp_drain_restart_init(
            &vnf_tcp_proof.drain, &drain_config) ==
            HFOC_LWIP_TCP_DRAIN_RESTART_OK,
        "network resource drain init");

    vnf_tcp_proof.listener_callback.accept = vnf_tcp_accept;
    vnf_tcp_proof.listener_callback.context = &vnf_tcp_proof;
    vnf_tcp_require(
        hfoc_lwip_tcp_init(
            &vnf_tcp_proof.listener,
            netif,
            &config,
            &port_policy,
            &vnf_tcp_proof.listener_callback,
            1U) == HFOC_LWIP_TCP_OK,
        "network resource listener init");
    vnf_tcp_require(
        hfoc_lwip_tcp_bind(
            &vnf_tcp_proof.listener,
            &port_policy) == HFOC_LWIP_TCP_OK,
        "network resource listener bind");
    vnf_tcp_require(
        hfoc_lwip_tcp_listen(
            &vnf_tcp_proof.listener) == HFOC_LWIP_TCP_OK,
        "network resource listener listen");
    vnf_tcp_require(
        hfoc_lwip_tcp_drain_restart_start(
            &vnf_tcp_proof.drain,
            &vnf_tcp_proof.listener,
            netif,
            &vnf_tcp_proof.pool,
            &vnf_tcp_proof.deadline,
            &vnf_tcp_proof.backlog,
            &vnf_tcp_proof.close_policy,
            &vnf_tcp_proof.quarantine) ==
            HFOC_LWIP_TCP_DRAIN_RESTART_OK,
        "network resource drain start");
    vnf_tcp_proof.alloc_start = hfoc_lwip_alloc_snapshot();
    serial_write("[NET-RESOURCE] proof init\n");
    vnf_notify_queue(
        0U,
        (uint16_t)virtio_available_publication_visible_index(
            &composition->receive.available));
    serial_write("[NET-RESOURCE] listener live\n");
    cpu_sti();

    for (;;) {
        bool progress = false;
        const void *payload;
        struct virtio_net_rx_frame frame;

        payload = vnf_tcp_rx_payload_for_next(rx);
        while (payload != NULL) {
            enum virtio_net_rx_status status = virtio_net_rx_complete(
                rx, payload, &frame);

            if (status != VIRTIO_NET_RX_OK) {
                {
                    const uint64_t serial_flags = serial_sequence_begin();
                    serial_write("[NET-RESOURCE] rx status: ");
                    serial_write(virtio_net_rx_status_string(status));
                    serial_write("\n");
                    serial_sequence_end(serial_flags);
                }
                vnf_tcp_fail("network resource RX complete");
            }
            vnf_tcp_feed_rx_frame(&frame);
            vnf_tcp_account_rx_completion(rx);
            vnf_tcp_repost_rx(rx, frame.buffer_index);
            sys_check_timeouts();
            vnf_netres_check_timeouts();
            progress = true;
            payload = vnf_tcp_rx_payload_for_next(rx);
        }

        while (tx->submitted_count != 0U) {
            struct virtio_net_tx_completion completion;
            enum virtio_net_tx_status status =
                virtio_net_tx_reclaim(tx, &completion);

            if (status == VIRTIO_NET_TX_USED_EMPTY) {
                break;
            }
            vnf_tcp_require(
                status == VIRTIO_NET_TX_OK,
                "network resource TX reclaim");
            vnf_tcp_require(
                completion.generation == tx->generation,
                "network resource TX completion generation");
            vnf_tcp_account_tx_reclaim();
            progress = true;
        }

        vnf_netres_backlog_tick(&vnf_tcp_proof);
        if (vnf_tcp_proof.pool.published_count ==
                HFOC_LWIP_TCP_POOL_SLOT_COUNT &&
            vnf_tcp_proof.echo_count >= 4U &&
            vnf_tcp_proof.pool_full_ok == 0U) {
            vnf_tcp_mark("[NET-RESOURCE] pool full");
            vnf_tcp_proof.pool_full_ok = 1U;
        }
        if (vnf_tcp_proof.pool_full_ok != 0U &&
            vnf_tcp_proof.packet_pressure_ok == 0U) {
            vnf_netres_packet_pressure(&vnf_tcp_proof);
        }
        vnf_netres_check_timeouts();
        if (vnf_tcp_proof.restart_requested == false &&
            vnf_tcp_proof.pool_full_ok != 0U &&
            vnf_tcp_proof.packet_pressure_ok != 0U &&
            vnf_tcp_proof.reset_count >= 1U &&
            vnf_tcp_proof.timeout_count >= 1U) {
            vnf_netres_request_restart(&vnf_tcp_proof);
            vnf_tcp_proof.restart_requested = true;
        }
        if (vnf_tcp_proof.restart_ok != 0U &&
            vnf_tcp_proof.restarted_echo_ok == 0U &&
            vnf_tcp_proof.accept_count > 4U &&
            vnf_tcp_proof.echo_count > 4U) {
            vnf_tcp_mark("[NET-RESOURCE] restarted echo ok");
            vnf_tcp_proof.restarted_echo_ok = 1U;
        }
        sys_check_timeouts();
        vnf_netres_check_timeouts();
        if (vnf_tcp_proof.restarted_echo_ok != 0U &&
            vnf_tcp_proof.active_count == 0U &&
            vnf_tcp_proof.pool.published_count == 0U) {
            if (!settle_armed) {
                settle_armed = true;
                settle_deadline_ms =
                    hfoc_lwip_time_now() + VNF_NETRES_SETTLE_MS;
            } else if (
                (int32_t)(hfoc_lwip_time_now() - settle_deadline_ms) >= 0) {
                break;
            }
        } else {
            settle_armed = false;
        }
        if (progress) {
            continue;
        }
        ++iterations;
        if ((iterations & 0xFFFFU) == 0U) {
            vnf_netres_check_timeouts();
        }
        __asm__ volatile ("pause" : : : "memory");
    }

    for (index = 0U; index < VNF_TCP_MAX_CONNECTIONS; ++index) {
        vnf_tcp_require(
            vnf_tcp_proof.pool.slots[index].state ==
                HFOC_LWIP_TCP_POOL_STATE_FREE,
            "network resource final pool slot free");
    }
    etharp_cleanup_netif(&netif->netif);
    alloc_end = hfoc_lwip_alloc_snapshot();
    vnf_tcp_require(
        hfoc_lwip_tcp_shutdown(&vnf_tcp_proof.listener) ==
            HFOC_LWIP_TCP_OK,
        "network resource listener shutdown");
    vnf_tcp_require(
        vnf_netres_alloc_balanced(
            &vnf_tcp_proof.alloc_start,
            &alloc_end),
        "network resource allocator balance");
    vnf_tcp_require(
        hfoc_lwip_tcp_pool_free_count(&vnf_tcp_proof.pool) == 4U &&
        hfoc_lwip_tcp_pool_reserved_count(&vnf_tcp_proof.pool) == 0U &&
        hfoc_lwip_tcp_pool_published_count(&vnf_tcp_proof.pool) == 0U &&
        hfoc_lwip_tcp_backlog_pending_count(
            &vnf_tcp_proof.backlog) == 0U &&
        hfoc_lwip_tcp_deadline_active_count(
            &vnf_tcp_proof.deadline) == 0U &&
        hfoc_lwip_tcp_close_policy_active_count(
            &vnf_tcp_proof.close_policy) == 0U &&
        hfoc_lwip_tcp_quarantine_active_count(
            &vnf_tcp_proof.quarantine) == 0U &&
        packet_pool_free_count(&vnf_tcp_proof.packet_pool) ==
            VNF_NETRES_PACKET_COUNT &&
        packet_pool_reserved_count(&vnf_tcp_proof.packet_pool) == 0U &&
        packet_pool_active_count(&vnf_tcp_proof.packet_pool) == 0U,
        "network resource exact ownership balance");
    vnf_tcp_mark("[NET-RESOURCE] balanced pool");
    vnf_tcp_mark("[NET-RESOURCE] clean shutdown");
    vnf_tcp_mark("[NET-RESOURCE] proof OK");
}
#endif
#endif

static void vnf_check_rx_frames(
    struct virtio_net_rx *rx,
    const struct virtio_used_ring *ring)
{
    uint32_t index;

#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
    serial_write("[PACKET-BOUNDARY] rx ready\n");
#endif
    vnf_wait_used_count(ring, VNF_FRAME_COUNT);
    for (index = 0U; index < VNF_FRAME_COUNT; ++index) {
        const struct virtio_net_rx_buffer *buffer =
            virtio_net_rx_buffer_at(rx, index);
        struct virtio_net_rx_frame frame;
        enum virtio_net_rx_status status;
        uint8_t expected[VNF_FRAME_LENGTH];
        const uint8_t *bytes;

        vnf_require(buffer != NULL, "RX buffer is posted before completion");
        bytes = (const uint8_t *)(uintptr_t)(
            vnf_hhdm + buffer->physical_base);
        status = virtio_net_rx_complete(rx, bytes, &frame);
        vnf_require(status == VIRTIO_NET_RX_OK, "RX completion");
        vnf_require(frame.length == VNF_FRAME_LENGTH, "RX frame length");
        if (index < VNF_DIRECT_FRAME_COUNT) {
            vnf_build_expected_frame(
                expected,
                VNF_FRAME_LENGTH,
                (const uint8_t[6]){0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U},
                (const uint8_t[6]){0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U},
                VNF_ETHER_TYPE_RX,
                "HFOC-RX-",
                index);
            vnf_require(
                memcmp(frame.data, expected, VNF_FRAME_LENGTH) == 0,
                "RX frame exact bytes");
        }
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
        vnf_packet_process_rx_frame(&frame);
#endif
    }
    vnf_require(
        virtio_net_rx_posted_count(rx) == 0U,
        "RX buffers balanced after completion");
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
    vnf_require(
        vnf_packet_rx_state.invalid_count == VNF_PACKET_INVALID_RX_COUNT &&
        vnf_packet_rx_state.accepted_count == VNF_DIRECT_FRAME_COUNT,
        "packet RX filtering and ownership");
    vnf_packet_pressure_refresh(
        vnf_packet_rx_state.invalid_count, 0U);
    serial_write("[PACKET-BOUNDARY] rx ok\n");
#endif
}

static void vnf_submit_tx_frames(
    struct virtio_net_tx *tx,
    struct virtio_net_queue_composition *composition)
{
    struct virtio_net_tx_frame frame;
    struct virtio_net_tx_submission submission;
    uint32_t index;

    for (index = 0U; index < VNF_DIRECT_FRAME_COUNT; ++index) {
        const uint64_t frame_physical =
            vnf_payload_physical + VNF_TX_FRAME_BASE_OFFSET +
            index * VNF_TX_FRAME_LENGTH;
        const uint8_t *frame_virtual =
            (const uint8_t *)vnf_payload_virtual +
            VNF_TX_FRAME_BASE_OFFSET + index * VNF_TX_FRAME_LENGTH;
        enum virtio_net_tx_status status;

        vnf_build_expected_frame(
            (uint8_t *)(uintptr_t)frame_virtual,
            VNF_TX_FRAME_LENGTH,
            (const uint8_t[6]){0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U},
            (const uint8_t[6]){0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U},
            VNF_ETHER_TYPE_TX,
            "HFOC-TX-",
            index);
        frame.data = frame_virtual;
        frame.physical_base = frame_physical;
        frame.length = VNF_TX_FRAME_LENGTH;
        vnf_tx_notify_state.header_physical_base =
            tx->headers[index].physical_base;
        vnf_tx_notify_state.frame_physical_base = frame_physical;
        vnf_tx_notify_state.frame_length = frame.length;
        status = virtio_net_tx_submit(tx, &frame, &submission);
        vnf_require(status == VIRTIO_NET_TX_OK, "TX submission");
        vnf_require(
            submission.slot_index == index &&
            submission.generation == tx->generation,
            "TX submission ownership");
        vnf_notify_queue(
            1U,
            (uint16_t)virtio_available_publication_visible_index(
                &composition->transmit.available));
    }
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
    vnf_packet_tx_state.tx = tx;
    vnf_packet_tx_state.composition = composition;
    vnf_packet_submit_tx_frames();
#endif
    vnf_require(
        virtio_net_tx_submitted_count(tx) == VNF_FRAME_COUNT,
        "TX pressure submitted count");
}

static void vnf_reclaim_tx_frames(
    struct virtio_net_tx *tx,
    const struct virtio_used_ring *ring)
{
    uint32_t index;

    vnf_wait_used_count(ring, VNF_FRAME_COUNT);
    for (index = 0U; index < VNF_DIRECT_FRAME_COUNT; ++index) {
        struct virtio_net_tx_completion completion;
        enum virtio_net_tx_status status;
        uint8_t expected[VNF_TX_FRAME_LENGTH];
        const uint8_t *frame_virtual =
            (const uint8_t *)vnf_payload_virtual +
            VNF_TX_FRAME_BASE_OFFSET + index * VNF_TX_FRAME_LENGTH;

        vnf_build_expected_frame(
            expected,
            VNF_TX_FRAME_LENGTH,
            (const uint8_t[6]){0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U},
            (const uint8_t[6]){0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U},
            VNF_ETHER_TYPE_TX,
            "HFOC-TX-",
            index);
        vnf_require(
            memcmp(frame_virtual, expected, VNF_TX_FRAME_LENGTH) == 0,
            "TX frame exact bytes before reclaim");
        status = virtio_net_tx_reclaim(tx, &completion);
        vnf_require(status == VIRTIO_NET_TX_OK, "TX reclaim");
        vnf_require(
            completion.slot_index == index &&
            completion.generation == tx->generation &&
            completion.length == VNF_TX_FRAME_LENGTH,
            "TX reclaim ownership");
    }
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
    vnf_packet_reclaim_tx_frames();
#endif
    vnf_require(
        virtio_net_tx_submitted_count(tx) == 0U &&
        virtio_net_tx_reclaimed_count(tx) == VNF_FRAME_COUNT,
        "TX buffers balanced after reclaim");
}

bool virtio_net_frame_qemu_ack_irq11(void)
{
    struct virtio_net_interrupt_work work;
    enum virtio_net_interrupt_status status;

    if (vnf_active_interrupt == NULL) {
        return false;
    }
    status = virtio_net_interrupt_handle(vnf_active_interrupt, &work);
    return status == VIRTIO_NET_INTERRUPT_OK ||
           status == VIRTIO_NET_INTERRUPT_EMPTY_WORK ||
           status == VIRTIO_NET_INTERRUPT_SPURIOUS;
}

static void vnf_frame_qemu_proof(uint64_t hhdm_offset)
{
    static const uint8_t guest_mac[6] =
        {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};
    static uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    uint8_t pci_device;
    uint8_t pci_function;
    uint16_t pci_vendor;
    uint16_t pci_device_id;
    static struct virtio_pci_bar_range bars[VNF_PCI_BAR_COUNT];
    static struct virtio_pci_capability_parse_result capabilities;
    static struct virtio_pci_identity identity;
    static struct virtio_pci_selection selection;
    static struct virtio_pci_device_status_io status_io;
    static struct virtio_pci_device_status_controller device_status;
    static struct virtio_pci_feature_io feature_io;
    static struct virtio_pci_feature_controller feature_controller;
    static struct virtio_pci_feature_policy feature_policy;
    static struct virtio_pci_feature_bitset pci_offered;
    enum virtio_pci_feature_status feature_status;
    static struct virtio_net_feature_bitset offered;
    enum virtio_net_feature_profile_status profile_status;
    static struct virtio_net_configuration net_configuration;
    static struct virtio_net_topology net_topology;
    static struct virtio_net_feature_profile profile;
    static struct virtio_queue_layout_policy layout_policy;
    static struct virtio_net_queue_config receive_config;
    static struct virtio_net_queue_config transmit_config;
    static struct virtio_net_queue_composition composition;
    static struct virtio_net_rx_buffer_descriptor
        rx_buffers[VNF_FRAME_COUNT];
    static struct virtio_net_rx_config rx_config;
    static struct virtio_net_rx rx;
    enum virtio_net_rx_status rx_status;
    static struct virtio_net_tx_header_descriptor
        tx_headers[VNF_FRAME_COUNT];
    static struct virtio_net_tx_config tx_config;
    static struct virtio_net_tx_notify tx_notify;
    static struct virtio_net_tx tx;
    static struct virtio_net_interrupt_io interrupt_io;
    static struct virtio_net_interrupt_direction receive_direction;
    static struct virtio_net_interrupt_direction transmit_direction;
    static struct virtio_net_interrupt_publication interrupt_publication;
    static struct virtio_net_interrupt interrupt;
    static struct network_device_snapshot device_snapshot;
    static struct network_link_state_record link_record;
    static struct virtio_net_link_readiness link_readiness;
    static struct virtio_net_link_io link_io;
    static struct virtio_net_link_state link_state;
    static struct virtio_net_notification_io notifications;
    static struct virtio_net_lifecycle lifecycle;
    uint64_t initial_free_pages;
    uint32_t index;

    vnf_hhdm = hhdm_offset;
    vnf_active_interrupt = NULL;
    vnf_zero_bytes(&composition, sizeof(composition));
    vnf_zero_bytes(&rx, sizeof(rx));
    vnf_zero_bytes(&tx, sizeof(tx));
    vnf_zero_bytes(&interrupt, sizeof(interrupt));
    vnf_zero_bytes(&link_state, sizeof(link_state));
    vnf_zero_bytes(&lifecycle, sizeof(lifecycle));
    serial_write("[VIRTIO-NET-FRAME] proof init\n");

    vnf_require(
        vnf_find_net_device(
            &pci_device, &pci_function, &pci_vendor, &pci_device_id),
        "virtio-net PCI device discovery");
    vnf_pci_config_read_space(0U, pci_device, pci_function, config);
    vnf_make_bar_ranges(config, pci_device, bars);
    vnf_parse_qemu_capabilities(config, bars, &capabilities);
    vnf_require(
        virtio_pci_capability_result_validate(&capabilities) ==
            VIRTIO_PCI_CAPABILITY_OK,
        "virtio capability parse");

    identity.version = VIRTIO_PCI_IDENTITY_VERSION;
    identity.vendor_id = pci_vendor;
    identity.device_id = pci_device_id;
    identity.bdf.bus = 0U;
    identity.bdf.device = pci_device;
    identity.bdf.function = pci_function;
    identity.modern =
        pci_device_id == VIRTIO_PCI_DEVICE_ID_NET_MODERN ? 1U : 0U;
    vnf_require(
        virtio_pci_selection_init(
            &selection, &identity,
            &capabilities.snapshot, VNF_GENERATION) == VIRTIO_PCI_OK,
        "virtio identity publication");

    vnf_common_base = vnf_map_bar(
        bars[capabilities.descriptors[0].bar].base +
        capabilities.descriptors[0].offset);
    vnf_notify_base = vnf_map_bar(
        bars[capabilities.descriptors[1].bar].base +
        capabilities.descriptors[1].offset);
    vnf_isr_base = vnf_map_bar(
        bars[capabilities.descriptors[2].bar].base +
        capabilities.descriptors[2].offset);
    vnf_device_base = vnf_map_bar(
        bars[capabilities.descriptors[3].bar].base +
        capabilities.descriptors[3].offset);
    vnf_notify_multiplier =
        capabilities.descriptors[1].notify_offset_multiplier;

    vnf_zero_bytes(&status_io, sizeof(status_io));
    status_io.write = vnf_status_write;
    status_io.read = vnf_status_read;
    status_io.context = &vnf_io_context;
    vnf_require(
        virtio_pci_device_status_init(
            &device_status, &status_io, VNF_GENERATION) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_reset(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_acknowledge(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_driver(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK,
        "device status driver sequence");

    vnf_zero_bytes(&feature_io, sizeof(feature_io));
    feature_io.read_offered = vnf_feature_read_offered;
    feature_io.write_accepted = vnf_feature_write_accepted;
    feature_io.read_accepted = vnf_feature_read_accepted;
    feature_io.context = &vnf_io_context;
    vnf_require(
        virtio_pci_feature_init(
            &feature_controller, &feature_io, VNF_GENERATION) ==
            VIRTIO_PCI_FEATURE_OK,
        "feature controller init");
    feature_policy.required.low = 0U;
    feature_policy.required.high =
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U);
    feature_policy.accepted = feature_policy.required;
    (void)vnf_feature_read_offered(
        &vnf_io_context, VIRTIO_PCI_FEATURE_SELECT_LO, &offered.low);
    (void)vnf_feature_read_offered(
        &vnf_io_context, VIRTIO_PCI_FEATURE_SELECT_HI, &offered.high);
    feature_status = virtio_pci_feature_negotiate(
        &feature_controller, &feature_policy);
    vnf_require(
        feature_status == VIRTIO_PCI_FEATURE_OK,
        "feature negotiation");
    vnf_require(
        virtio_pci_feature_observe(
            &feature_controller, &pci_offered) == VIRTIO_PCI_FEATURE_OK,
        "feature observed unchanged");

    vnf_zero_bytes(&net_configuration, sizeof(net_configuration));
    net_configuration.version = VIRTIO_NET_CONFIGURATION_VERSION;
    vnf_copy_bytes(net_configuration.mac_address, guest_mac,
                   VIRTIO_NET_MAC_LENGTH);
    net_configuration.status =
        (uint32_t)vnf_mmio_read16(vnf_device_base, 6U);
    vnf_zero_bytes(&net_topology, sizeof(net_topology));
    net_topology.max_virtqueue_pairs = 1U;
    net_topology.receive_queue_count = 1U;
    net_topology.transmit_queue_count = 1U;
    net_topology.control_queue_present = false;
    profile_status = virtio_net_feature_profile_activate(
            &profile, &identity, &offered,
            &net_configuration, &net_topology, VNF_GENERATION);
    vnf_require(
        profile_status == VIRTIO_NET_FEATURE_PROFILE_OK,
        "virtio-net feature profile");

    initial_free_pages = pmm_free_page_count();
    vnf_rx_region_physical = pmm_alloc_contiguous_pages(1U);
    vnf_tx_region_physical = pmm_alloc_contiguous_pages(1U);
    vnf_payload_physical = pmm_alloc_contiguous_pages(VNF_PAYLOAD_PAGES);
    vnf_require(
        vnf_rx_region_physical != 0U &&
        vnf_tx_region_physical != 0U &&
        vnf_payload_physical != 0U,
        "proof physical memory allocation");
    vnf_queues[0].physical = vnf_rx_region_physical;
    vnf_queues[0].virtual_address =
        (void *)(uintptr_t)(vnf_rx_region_physical + vnf_hhdm);
    vnf_queues[1].physical = vnf_tx_region_physical;
    vnf_queues[1].virtual_address =
        (void *)(uintptr_t)(vnf_tx_region_physical + vnf_hhdm);
    vnf_payload_virtual =
        (void *)(uintptr_t)(vnf_payload_physical + vnf_hhdm);
    vnf_zero_bytes(vnf_queues[0].virtual_address, VNF_QUEUE_REGION_LENGTH);
    vnf_zero_bytes(vnf_queues[1].virtual_address, VNF_QUEUE_REGION_LENGTH);
    vnf_zero_bytes(vnf_payload_virtual, (size_t)VNF_PAYLOAD_LENGTH);

    layout_policy.queue_size = VNF_QUEUE_SIZE;
    layout_policy.event_index = false;
    for (index = 0U; index < 2U; ++index) {
        const enum virtio_queue_layout_status layout_status =
            virtio_queue_layout_calculate(
                &layout_policy,
                vnf_queues[index].physical,
                VNF_QUEUE_REGION_LENGTH,
                &vnf_queues[index].layout);

        vnf_require(
            layout_status == VIRTIO_QUEUE_LAYOUT_OK,
            "queue layout");
        vnf_queues[index].available =
            (struct virtio_available_ring *)(
                (uintptr_t)vnf_queues[index].virtual_address +
                vnf_queues[index].layout.available.offset);
        vnf_queues[index].used =
            (struct virtio_used_ring *)(
                (uintptr_t)vnf_queues[index].virtual_address +
                vnf_queues[index].layout.used.offset);
        vnf_queues[index].descriptors =
            (volatile struct vnf_virtq_desc *)(
                (uintptr_t)vnf_queues[index].virtual_address +
                vnf_queues[index].layout.descriptor.offset);
    }

    vnf_require(
        virtio_pci_device_status_features_ok(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK,
        "features ok device status");
    vnf_program_queue(0U, &vnf_queues[0]);
    vnf_program_queue(1U, &vnf_queues[1]);
    vnf_require(
        virtio_pci_device_status_driver_ok(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_live(&device_status),
        "driver ok device status");

    vnf_zero_bytes(&receive_config, sizeof(receive_config));
    receive_config.direction = VIRTIO_NET_QUEUE_RECEIVE;
    receive_config.queue_index = 0U;
    receive_config.queue_size = VNF_QUEUE_SIZE;
    receive_config.event_index = false;
    receive_config.region_base = vnf_rx_region_physical;
    receive_config.region_length = VNF_QUEUE_REGION_LENGTH;
    receive_config.available_ring = vnf_queues[0].available;
    receive_config.used_ring = vnf_queues[0].used;
    receive_config.barrier.fence = vnf_barrier_fence;
    receive_config.barrier.context = &vnf_io_context;
    receive_config.reclaim.reclaim = vnf_queue_reclaim;
    receive_config.reclaim.context = &vnf_io_context;
    receive_config.max_used_length = VNF_MAX_USED_LENGTH;
    receive_config.dma_window.base = vnf_rx_region_physical;
    receive_config.dma_window.length = VNF_QUEUE_REGION_LENGTH;
    receive_config.dma_window.alignment = VNF_DMA_ALIGNMENT;
    receive_config.dma_zeroize.zeroize = vnf_zeroize;
    receive_config.dma_zeroize.context = &vnf_io_context;
    receive_config.dma_capacity = VNF_DMA_CAPACITY;

    vnf_zero_bytes(&transmit_config, sizeof(transmit_config));
    transmit_config.direction = VIRTIO_NET_QUEUE_TRANSMIT;
    transmit_config.queue_index = 1U;
    transmit_config.queue_size = VNF_QUEUE_SIZE;
    transmit_config.event_index = false;
    transmit_config.region_base = vnf_tx_region_physical;
    transmit_config.region_length = VNF_QUEUE_REGION_LENGTH;
    transmit_config.available_ring = vnf_queues[1].available;
    transmit_config.used_ring = vnf_queues[1].used;
    transmit_config.barrier.fence = vnf_barrier_fence;
    transmit_config.barrier.context = &vnf_io_context;
    transmit_config.reclaim.reclaim = vnf_queue_reclaim;
    transmit_config.reclaim.context = &vnf_io_context;
    transmit_config.max_used_length = VNF_MAX_USED_LENGTH;
    transmit_config.dma_window.base = vnf_tx_region_physical;
    transmit_config.dma_window.length = VNF_QUEUE_REGION_LENGTH;
    transmit_config.dma_window.alignment = VNF_DMA_ALIGNMENT;
    transmit_config.dma_zeroize.zeroize = vnf_zeroize;
    transmit_config.dma_zeroize.context = &vnf_io_context;
    transmit_config.dma_capacity = VNF_DMA_CAPACITY;

    vnf_require(
        virtio_net_queue_composition_activate(
            &composition, &profile,
            &receive_config, &transmit_config, VNF_GENERATION) ==
            VIRTIO_NET_QUEUE_COMPOSITION_OK,
        "queue composition");

    for (index = 0U; index < VNF_FRAME_COUNT; ++index) {
        rx_buffers[index].physical_base =
            vnf_payload_physical + index * VNF_RX_BUFFER_LENGTH;
        rx_buffers[index].length = VNF_RX_BUFFER_LENGTH;
    }
    vnf_zero_bytes(&rx_config, sizeof(rx_config));
    rx_config.buffers = rx_buffers;
    rx_config.buffer_count = VNF_FRAME_COUNT;
    rx_config.minimum_frame_capacity = 64U;
    rx_config.dma_window.base = vnf_payload_physical;
    rx_config.dma_window.length = VNF_PAYLOAD_LENGTH;
    rx_config.dma_window.alignment = VNF_DMA_ALIGNMENT;
    rx_config.dma_zeroize.zeroize = vnf_zeroize;
    rx_config.dma_zeroize.context = &vnf_io_context;
    rx_config.dma_capacity = VNF_DMA_CAPACITY;
    rx_status = virtio_net_rx_activate(&rx, &composition, &rx_config);
    vnf_require(
        rx_status == VIRTIO_NET_RX_OK,
        "RX activation");
    for (index = 0U; index < VNF_FRAME_COUNT; ++index) {
        const struct virtio_net_rx_buffer *buffer =
            virtio_net_rx_buffer_at(&rx, index);

        vnf_require(buffer != NULL, "RX posted buffer");
        vnf_write_descriptor(
            &vnf_queues[0],
            buffer->descriptor.index,
            buffer->physical_base,
            buffer->length,
            2U,
            0U);
    }

    for (index = 0U; index < VNF_FRAME_COUNT; ++index) {
        tx_headers[index].physical_base =
            vnf_payload_physical + VNF_TX_HEADER_BASE_OFFSET +
            index * VNF_TX_HEADER_STRIDE;
        tx_headers[index].length = VNF_TX_HEADER_LENGTH;
        tx_headers[index].alias =
            (void *)(uintptr_t)(
                (uint8_t *)vnf_payload_virtual +
                VNF_TX_HEADER_BASE_OFFSET +
                index * VNF_TX_HEADER_STRIDE);
    }
    vnf_zero_bytes(&tx_config, sizeof(tx_config));
    tx_config.headers = tx_headers;
    tx_config.header_count = VNF_FRAME_COUNT;
    tx_config.maximum_frame_length = VNF_TX_FRAME_LENGTH;
    tx_config.dma_window.base = vnf_payload_physical;
    tx_config.dma_window.length = VNF_PAYLOAD_LENGTH;
    tx_config.dma_window.alignment = VNF_DMA_ALIGNMENT;
    tx_config.dma_zeroize.zeroize = vnf_zeroize;
    tx_config.dma_zeroize.context = &vnf_io_context;
    tx_config.dma_capacity = VNF_DMA_CAPACITY;
    tx_notify.notify = vnf_tx_notify;
    tx_notify.context = &vnf_tx_notify_state;
    vnf_tx_notify_state.queue = &vnf_queues[1];
    vnf_require(
        virtio_net_tx_activate(&tx, &composition, &tx_config, &tx_notify) ==
            VIRTIO_NET_TX_OK,
        "TX activation");

    vnf_zero_bytes(&interrupt_io, sizeof(interrupt_io));
    interrupt_io.read_status = vnf_interrupt_read_status;
    interrupt_io.context = &vnf_io_context;
    receive_direction.has_work = vnf_interrupt_has_rx_work;
    receive_direction.context = &vnf_io_context;
    transmit_direction.has_work = vnf_interrupt_has_tx_work;
    transmit_direction.context = &vnf_io_context;
    interrupt_publication.publish = vnf_interrupt_publish;
    interrupt_publication.context = &vnf_io_context;
    vnf_require(
        virtio_net_interrupt_activate(
            &interrupt, &interrupt_io,
            &receive_direction, &transmit_direction,
            &interrupt_publication, VNF_GENERATION) ==
            VIRTIO_NET_INTERRUPT_OK,
        "interrupt activation");
    vnf_active_interrupt = &interrupt;

    vnf_zero_bytes(&device_snapshot, sizeof(device_snapshot));
    device_snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    device_snapshot.device_id = VIRTIO_NET_LINK_DEVICE_ID;
    device_snapshot.vendor_id = 0x1AF4U;
    device_snapshot.product_id = pci_device_id;
    device_snapshot.bus_address =
        ((uint32_t)pci_device << 3U) | pci_function;
    vnf_copy_bytes(device_snapshot.mac_address, guest_mac,
                   NETWORK_DEVICE_MAC_LENGTH);
    device_snapshot.capability_mask = VIRTIO_NET_LINK_CAPABILITY_MASK;
    vnf_zero_bytes(&link_record, sizeof(link_record));
    link_record.version = NETWORK_LINK_STATE_VERSION;
    link_record.device_id = VIRTIO_NET_LINK_DEVICE_ID;
    link_record.state = NETWORK_LINK_UP;
    link_record.generation = VNF_GENERATION;
    link_readiness.profile_published = true;
    link_readiness.queue_composition_published = true;
    link_readiness.interrupt_published = true;
    link_io.read_config_status = vnf_link_read_config_status;
    link_io.context = &vnf_io_context;
    vnf_require(
        virtio_net_link_state_activate(
            &link_state, &device_snapshot, &link_readiness,
            &link_io, &link_record, VNF_GENERATION, VNF_GENERATION) ==
            VIRTIO_NET_LINK_OK,
        "link state activation");

    notifications.quiesce = vnf_notifications_quiesce;
    notifications.context = &vnf_io_context;
    vnf_require(
        virtio_net_lifecycle_activate(
            &lifecycle, &link_state, &interrupt, &rx, &tx,
            &composition, &device_status, &notifications, VNF_GENERATION) ==
            VIRTIO_NET_LIFECYCLE_OK,
        "lifecycle activation");
    serial_write("[VIRTIO-NET-FRAME] device live\n");

#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
    vnf_packet_boundary_setup(
        guest_mac,
        &profile,
        &device_snapshot,
        pci_vendor,
        pci_device_id,
        pci_device,
        pci_function);
#endif
#ifdef HFOC_TCP_LISTENER_QEMU_TEST
#ifdef HFOC_NETWORK_RESOURCE_QEMU_TEST
    vnf_netres_listener_proof(&rx, &tx, &composition);
#elif defined(HFOC_HTTPS_SERVICE_QEMU_TEST)
    vnf_tls_qemu_proof_run(&rx, &tx, &composition);
#elif defined(HFOC_MBEDTLS_TLS_QEMU_TEST)
    vnf_tls_qemu_proof_run(&rx, &tx, &composition);
#else
    vnf_tcp_listener_proof(&rx, &tx, &composition);
#endif
#else
    vnf_notify_queue(
        0U,
        (uint16_t)virtio_available_publication_visible_index(
            &composition.receive.available));
    serial_write("[VIRTIO-NET-FRAME] rx ready\n");
    vnf_check_rx_frames(&rx, vnf_queues[0].used);
    serial_write("[VIRTIO-NET-FRAME] rx ok\n");

    vnf_submit_tx_frames(&tx, &composition);
    serial_write("[VIRTIO-NET-FRAME] tx ready\n");
    vnf_reclaim_tx_frames(&tx, vnf_queues[1].used);
    serial_write("[VIRTIO-NET-FRAME] tx ok\n");

#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
    vnf_packet_release_rx_ownership();
#endif
#endif
    vnf_require(
        virtio_net_lifecycle_stop(&lifecycle) ==
            VIRTIO_NET_LIFECYCLE_OK &&
        virtio_net_lifecycle_stopped(&lifecycle) &&
        virtio_net_lifecycle_stop(&lifecycle) ==
            VIRTIO_NET_LIFECYCLE_OK,
        "lifecycle reset shutdown");
    vnf_require(
        !virtio_net_rx_published(&rx) &&
        !virtio_net_tx_published(&tx) &&
        !virtio_net_queue_composition_published(&composition),
        "reset unpublished controllers");
    vnf_mmio_write16(vnf_common_base, VNF_COMMON_QUEUE_SELECT, 0U);
    vnf_mmio_write16(vnf_common_base, VNF_COMMON_QUEUE_ENABLE, 0U);
    vnf_mmio_write16(vnf_common_base, VNF_COMMON_QUEUE_SELECT, 1U);
    vnf_mmio_write16(vnf_common_base, VNF_COMMON_QUEUE_ENABLE, 0U);
    vnf_active_interrupt = NULL;
    serial_write("[VIRTIO-NET-FRAME] reset shutdown\n");
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
    serial_write("[PACKET-BOUNDARY] reset shutdown\n");
#endif

    (void)pmm_free_contiguous_pages(vnf_rx_region_physical, 1U);
    (void)pmm_free_contiguous_pages(vnf_tx_region_physical, 1U);
    (void)pmm_free_contiguous_pages(vnf_payload_physical,
                                    VNF_PAYLOAD_PAGES);
    vnf_rx_region_physical = 0U;
    vnf_tx_region_physical = 0U;
    vnf_payload_physical = 0U;
    vnf_require(
        pmm_free_page_count() == initial_free_pages,
        "balanced physical ownership");
#ifdef HFOC_PACKET_BOUNDARY_QEMU_TEST
    serial_write("[PACKET-BOUNDARY] clean shutdown\n");
    serial_write("[PACKET-BOUNDARY] proof OK\n");
#endif
    serial_write("[VIRTIO-NET-FRAME] balanced ownership\n");
    serial_write("[VIRTIO-NET-FRAME] clean shutdown\n");
    serial_write("[VIRTIO-NET-FRAME] proof OK\n");
}

void virtio_net_frame_qemu_test_init(uint64_t hhdm_offset)
{
    uint64_t *stack_top = (uint64_t *)(
        (uintptr_t)vnf_proof_stack + sizeof(vnf_proof_stack));
    uint64_t old_rsp;

    __asm__ volatile (
        "mov %%rsp, %0\n\t"
        "mov %1, %%rsp\n\t"
        "call *%2\n\t"
        "mov %0, %%rsp\n\t"
        : "=&r"(old_rsp)
        : "r"(stack_top), "r"(vnf_frame_qemu_proof), "D"(hhdm_offset)
        : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11",
          "memory", "cc");
}

#else
typedef int virtio_net_frame_qemu_translation_unit_guard;
#endif
