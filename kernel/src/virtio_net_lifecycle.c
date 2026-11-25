#include "virtio_net_lifecycle.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_net_lifecycle_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_NET_LIFECYCLE_GENERATION_MIN &&
           generation <= VIRTIO_NET_LIFECYCLE_GENERATION_MAX;
}

static bool virtio_net_lifecycle_state_valid(
    enum virtio_net_lifecycle_state state)
{
    return state == VIRTIO_NET_LIFECYCLE_STATE_ACTIVE ||
           state == VIRTIO_NET_LIFECYCLE_STATE_STOPPED;
}

static bool virtio_net_lifecycle_notifications_valid(
    const struct virtio_net_notification_io *notifications)
{
    return notifications != NULL &&
           notifications->quiesce != NULL &&
           notifications->context != NULL;
}

static enum virtio_net_lifecycle_status
virtio_net_lifecycle_controller_validate(
    const struct virtio_net_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return VIRTIO_NET_LIFECYCLE_NULL;
    }
    if (!lifecycle->published) {
        return VIRTIO_NET_LIFECYCLE_UNPUBLISHED;
    }
    if (lifecycle->version != VIRTIO_NET_LIFECYCLE_VERSION) {
        return VIRTIO_NET_LIFECYCLE_INVALID_VERSION;
    }
    if (!virtio_net_lifecycle_generation_valid(lifecycle->generation)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_GENERATION;
    }
    if (!virtio_net_lifecycle_state_valid(lifecycle->state)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_STATE;
    }
    if (lifecycle->state == VIRTIO_NET_LIFECYCLE_STATE_STOPPED) {
        return VIRTIO_NET_LIFECYCLE_OK;
    }
    if (!virtio_net_lifecycle_notifications_valid(
            &lifecycle->notifications)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS;
    }
    if (lifecycle->link_state == NULL ||
        !virtio_net_link_state_published(lifecycle->link_state)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_LINK_STATE;
    }
    if (lifecycle->interrupt == NULL ||
        !virtio_net_interrupt_published(lifecycle->interrupt)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_INTERRUPT;
    }
    if (lifecycle->composition == NULL ||
        !virtio_net_queue_composition_published(
            lifecycle->composition)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_COMPOSITION;
    }
    if (lifecycle->rx == NULL ||
        !virtio_net_rx_published(lifecycle->rx)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_RX;
    }
    if (lifecycle->tx == NULL ||
        !virtio_net_tx_published(lifecycle->tx)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_TX;
    }
    if (lifecycle->device_status == NULL ||
        !lifecycle->device_status->published ||
        lifecycle->device_status->version !=
            VIRTIO_PCI_DEVICE_STATUS_VERSION) {
        return VIRTIO_NET_LIFECYCLE_INVALID_DEVICE_STATUS;
    }
    if (lifecycle->link_state->generation != lifecycle->generation ||
        lifecycle->interrupt->generation != lifecycle->generation ||
        lifecycle->rx->generation != lifecycle->generation ||
        lifecycle->tx->generation != lifecycle->generation ||
        lifecycle->composition->generation != lifecycle->generation ||
        lifecycle->device_status->generation != lifecycle->generation) {
        return VIRTIO_NET_LIFECYCLE_GENERATION_MISMATCH;
    }
    if (!virtio_pci_device_status_live(lifecycle->device_status)) {
        return VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE;
    }
    return VIRTIO_NET_LIFECYCLE_OK;
}

enum virtio_net_lifecycle_status virtio_net_lifecycle_activate(
    struct virtio_net_lifecycle *lifecycle,
    struct virtio_net_link_state *link_state,
    struct virtio_net_interrupt *interrupt,
    struct virtio_net_rx *rx,
    struct virtio_net_tx *tx,
    struct virtio_net_queue_composition *composition,
    struct virtio_pci_device_status_controller *device_status,
    const struct virtio_net_notification_io *notifications,
    uint32_t generation)
{
    if (lifecycle == NULL ||
        link_state == NULL ||
        interrupt == NULL ||
        rx == NULL ||
        tx == NULL ||
        composition == NULL ||
        device_status == NULL ||
        notifications == NULL) {
        return VIRTIO_NET_LIFECYCLE_NULL;
    }
    if (lifecycle->published) {
        return VIRTIO_NET_LIFECYCLE_ALREADY_PUBLISHED;
    }
    if (lifecycle->version != 0U) {
        return VIRTIO_NET_LIFECYCLE_INVALID_VERSION;
    }
    if (!virtio_net_lifecycle_generation_valid(generation)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_GENERATION;
    }
    if (!virtio_net_lifecycle_notifications_valid(notifications)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS;
    }
    if (!virtio_net_link_state_published(link_state)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_LINK_STATE;
    }
    if (!virtio_net_interrupt_published(interrupt)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_INTERRUPT;
    }
    if (!virtio_net_queue_composition_published(composition)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_COMPOSITION;
    }
    if (!virtio_net_rx_published(rx)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_RX;
    }
    if (!virtio_net_tx_published(tx)) {
        return VIRTIO_NET_LIFECYCLE_INVALID_TX;
    }
    if (!device_status->published ||
        device_status->version != VIRTIO_PCI_DEVICE_STATUS_VERSION) {
        return VIRTIO_NET_LIFECYCLE_INVALID_DEVICE_STATUS;
    }
    if (link_state->generation != generation ||
        interrupt->generation != generation ||
        rx->generation != generation ||
        tx->generation != generation ||
        composition->generation != generation ||
        device_status->generation != generation) {
        return VIRTIO_NET_LIFECYCLE_GENERATION_MISMATCH;
    }
    if (!virtio_pci_device_status_live(device_status)) {
        return VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE;
    }

    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->published = true;
    lifecycle->version = VIRTIO_NET_LIFECYCLE_VERSION;
    lifecycle->generation = generation;
    lifecycle->state = VIRTIO_NET_LIFECYCLE_STATE_ACTIVE;
    lifecycle->link_state = link_state;
    lifecycle->interrupt = interrupt;
    lifecycle->rx = rx;
    lifecycle->tx = tx;
    lifecycle->composition = composition;
    lifecycle->device_status = device_status;
    lifecycle->notifications = *notifications;
    return VIRTIO_NET_LIFECYCLE_OK;
}

enum virtio_net_lifecycle_status virtio_net_lifecycle_stop(
    struct virtio_net_lifecycle *lifecycle)
{
    struct virtio_net_link_readiness lost_readiness;
    const struct network_link_selection *selection;
    struct network_link_state_record failed_state;
    enum virtio_net_lifecycle_status status;
    enum virtio_net_link_status link_status;

    status = virtio_net_lifecycle_controller_validate(lifecycle);
    if (status != VIRTIO_NET_LIFECYCLE_OK) {
        return status;
    }
    if (lifecycle->state == VIRTIO_NET_LIFECYCLE_STATE_STOPPED) {
        return VIRTIO_NET_LIFECYCLE_OK;
    }

    if (!lifecycle->notifications.quiesce(
            lifecycle->notifications.context)) {
        return VIRTIO_NET_LIFECYCLE_NOTIFICATIONS_FAILED;
    }

    selection = virtio_net_link_state_selection(lifecycle->link_state);
    if (selection == NULL) {
        return VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED;
    }
    memset(&lost_readiness, 0, sizeof(lost_readiness));
    memset(&failed_state, 0, sizeof(failed_state));
    failed_state.version = NETWORK_LINK_STATE_VERSION;
    failed_state.device_id = selection->link.device_id;
    failed_state.state = NETWORK_LINK_FAILED;
    failed_state.generation = lifecycle->generation;
    link_status = virtio_net_link_state_transition(
        lifecycle->link_state,
        &lost_readiness,
        &failed_state,
        selection->generation);
    if (link_status != VIRTIO_NET_LINK_OK) {
        return VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED;
    }

    if (virtio_net_rx_teardown(lifecycle->rx) != VIRTIO_NET_RX_OK) {
        return VIRTIO_NET_LIFECYCLE_RX_TEARDOWN_FAILED;
    }
    if (virtio_net_tx_teardown(lifecycle->tx) != VIRTIO_NET_TX_OK) {
        return VIRTIO_NET_LIFECYCLE_TX_TEARDOWN_FAILED;
    }
    if (virtio_pci_device_status_reset(lifecycle->device_status) !=
        VIRTIO_PCI_DEVICE_STATUS_OK) {
        return VIRTIO_NET_LIFECYCLE_DEVICE_RESET_FAILED;
    }
    if (virtio_net_queue_composition_teardown(
            lifecycle->composition) !=
        VIRTIO_NET_QUEUE_COMPOSITION_OK) {
        return VIRTIO_NET_LIFECYCLE_QUEUE_TEARDOWN_FAILED;
    }

    lifecycle->state = VIRTIO_NET_LIFECYCLE_STATE_STOPPED;
    lifecycle->link_state = NULL;
    lifecycle->interrupt = NULL;
    lifecycle->rx = NULL;
    lifecycle->tx = NULL;
    lifecycle->composition = NULL;
    lifecycle->device_status = NULL;
    return VIRTIO_NET_LIFECYCLE_OK;
}

bool virtio_net_lifecycle_published(
    const struct virtio_net_lifecycle *lifecycle)
{
    return lifecycle != NULL &&
           lifecycle->published &&
           lifecycle->version == VIRTIO_NET_LIFECYCLE_VERSION &&
           virtio_net_lifecycle_generation_valid(lifecycle->generation) &&
           virtio_net_lifecycle_state_valid(lifecycle->state);
}

bool virtio_net_lifecycle_stopped(
    const struct virtio_net_lifecycle *lifecycle)
{
    return virtio_net_lifecycle_published(lifecycle) &&
           lifecycle->state == VIRTIO_NET_LIFECYCLE_STATE_STOPPED;
}

uint32_t virtio_net_lifecycle_generation(
    const struct virtio_net_lifecycle *lifecycle)
{
    if (!virtio_net_lifecycle_published(lifecycle)) {
        return 0U;
    }
    return lifecycle->generation;
}

const char *virtio_net_lifecycle_status_string(
    enum virtio_net_lifecycle_status status)
{
    switch (status) {
    case VIRTIO_NET_LIFECYCLE_OK:
        return "VIRTIO_NET_LIFECYCLE_OK";
    case VIRTIO_NET_LIFECYCLE_NULL:
        return "VIRTIO_NET_LIFECYCLE_NULL";
    case VIRTIO_NET_LIFECYCLE_INVALID_VERSION:
        return "VIRTIO_NET_LIFECYCLE_INVALID_VERSION";
    case VIRTIO_NET_LIFECYCLE_INVALID_GENERATION:
        return "VIRTIO_NET_LIFECYCLE_INVALID_GENERATION";
    case VIRTIO_NET_LIFECYCLE_ALREADY_PUBLISHED:
        return "VIRTIO_NET_LIFECYCLE_ALREADY_PUBLISHED";
    case VIRTIO_NET_LIFECYCLE_UNPUBLISHED:
        return "VIRTIO_NET_LIFECYCLE_UNPUBLISHED";
    case VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS:
        return "VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS";
    case VIRTIO_NET_LIFECYCLE_INVALID_LINK_STATE:
        return "VIRTIO_NET_LIFECYCLE_INVALID_LINK_STATE";
    case VIRTIO_NET_LIFECYCLE_INVALID_INTERRUPT:
        return "VIRTIO_NET_LIFECYCLE_INVALID_INTERRUPT";
    case VIRTIO_NET_LIFECYCLE_INVALID_RX:
        return "VIRTIO_NET_LIFECYCLE_INVALID_RX";
    case VIRTIO_NET_LIFECYCLE_INVALID_TX:
        return "VIRTIO_NET_LIFECYCLE_INVALID_TX";
    case VIRTIO_NET_LIFECYCLE_INVALID_COMPOSITION:
        return "VIRTIO_NET_LIFECYCLE_INVALID_COMPOSITION";
    case VIRTIO_NET_LIFECYCLE_INVALID_DEVICE_STATUS:
        return "VIRTIO_NET_LIFECYCLE_INVALID_DEVICE_STATUS";
    case VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE:
        return "VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE";
    case VIRTIO_NET_LIFECYCLE_GENERATION_MISMATCH:
        return "VIRTIO_NET_LIFECYCLE_GENERATION_MISMATCH";
    case VIRTIO_NET_LIFECYCLE_NOTIFICATIONS_FAILED:
        return "VIRTIO_NET_LIFECYCLE_NOTIFICATIONS_FAILED";
    case VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED:
        return "VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED";
    case VIRTIO_NET_LIFECYCLE_RX_TEARDOWN_FAILED:
        return "VIRTIO_NET_LIFECYCLE_RX_TEARDOWN_FAILED";
    case VIRTIO_NET_LIFECYCLE_TX_TEARDOWN_FAILED:
        return "VIRTIO_NET_LIFECYCLE_TX_TEARDOWN_FAILED";
    case VIRTIO_NET_LIFECYCLE_DEVICE_RESET_FAILED:
        return "VIRTIO_NET_LIFECYCLE_DEVICE_RESET_FAILED";
    case VIRTIO_NET_LIFECYCLE_QUEUE_TEARDOWN_FAILED:
        return "VIRTIO_NET_LIFECYCLE_QUEUE_TEARDOWN_FAILED";
    case VIRTIO_NET_LIFECYCLE_INVALID_STATE:
        return "VIRTIO_NET_LIFECYCLE_INVALID_STATE";
    default:
        return NULL;
    }
}
