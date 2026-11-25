#ifndef HFOC_EHCI_ENGINE_H
#define HFOC_EHCI_ENGINE_H

/*
 * ITEM-559: the controller's own verdict on a transfer, separated from the
 * descriptor classifier.
 *
 * The hardware run after ITEM-558 printed `result=OTHER index=0/3
 * qtd=0x00080E80 usbcmd=0x00080070 usbsts=0x0000F018`: every descriptor was
 * still Active, no descriptor carried an error bit, and the register pair says
 * the controller had halted with a Host System Error (USBSTS.HSE).  The qTD
 * classifier can only describe what the descriptors say, so it answered
 * "OTHER" - which is why the operator's line named a stage (`DEVICE8`) and a
 * verdict that explains nothing.  The actual failure was in the schedule: the
 * controller was not running, the transfer never executed, and the driver's
 * wait loop kept polling a descriptor that could never retire.
 *
 * This module is that half of the path, with the registers and the clock
 * substituted:
 *
 *   * link the transfer head behind the asynchronous list head;
 *   * poll the last descriptor's Active bit while watching the controller's
 *     state - Host System Error, halt, and a stopped asynchronous schedule end
 *     the wait with a name instead of burning the budget;
 *   * capture USBCMD/USBSTS (and the port's PORTSC) *before* anything is
 *     unlinked, so the snapshot is the failure state and not the state after
 *     the stop;
 *   * classify the descriptors through the existing pure classifier;
 *   * unlink the head, ring the async-advance doorbell and wait for the
 *     acknowledgement; a doorbell that is never acknowledged stops the engine,
 *     and that is named too.
 *
 * The driver (kernel/src/ehci.c) passes MMIO and the timer; the host proof
 * (tests/ehci-engine-host.c) passes a scripted register file and a fake clock,
 * so the schedule transitions, the wait's verdict and the snapshot's position
 * are executed by the same code the machine runs.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ehci.h"

/* Why the transfer did not complete: the controller's answer, not the
   descriptor's.  NONE means the schedule was healthy and the descriptor
   classification stands on its own. */
enum ehci_engine_cause {
    EHCI_ENGINE_CAUSE_NONE = 0,
    /* The budget expired with the descriptor still Active and no controller
       error: the transfer made no progress. */
    EHCI_ENGINE_CAUSE_WAIT_EXPIRED,
    /* USBSTS.HSE: the controller lost host memory or the bus, and halted. */
    EHCI_ENGINE_CAUSE_HOST_SYSTEM_ERROR,
    /* USBSTS.HCH with USBCMD.RS clear: the controller is halted. */
    EHCI_ENGINE_CAUSE_HALTED,
    /* USBCMD.ASE is set but USBSTS.ASS is clear: the async schedule stopped. */
    EHCI_ENGINE_CAUSE_ASYNC_STOPPED,
    /* The async-advance doorbell was never acknowledged: the controller may
       still hold the queue head. */
    EHCI_ENGINE_CAUSE_ASYNC_ADVANCE,
    /* A previous run left the engine stalled; the pool must not be touched. */
    EHCI_ENGINE_CAUSE_ENGINE_STALLED
};

/*
 * ITEM-564: the cleanup is its own verdict.
 *
 * The hardware run after ITEM-563 reported `result=TIMEOUT cause=ASYNC_ADVANCE
 * quiesced=0 stalled=1`: the transfer had timed out with every descriptor still
 * Active, and then the stop's own failure (a doorbell the controller never
 * acknowledged) overwrote the transfer's cause.  A cleanup verdict is a fact
 * about the schedule, not about the device, and it must not replace the reason
 * the device's transfer failed; both travel separately.
 */
enum ehci_engine_cleanup {
    EHCI_ENGINE_CLEANUP_NONE = 0,
    /* The controller confirmed it advanced past the removed head. */
    EHCI_ENGINE_CLEANUP_ACKNOWLEDGED,
    /* The asynchronous schedule was not running: there is no advance to wait
       for, and the head is out of a list the controller is not walking. */
    EHCI_ENGINE_CLEANUP_NOT_RUNNING,
    /* The doorbell bit did not stick: the controller never accepted it. */
    EHCI_ENGINE_CLEANUP_REFUSED,
    /* The doorbell was rung and never answered. */
    EHCI_ENGINE_CLEANUP_IGNORED
};

const char *ehci_engine_cleanup_string(enum ehci_engine_cleanup cleanup);

/* The largest descriptor count one transfer can carry (the driver's bound). */
#define EHCI_ENGINE_MAX_QTD 8U
/* The poll bound used when the platform has no clock (the driver always has
   one; the fallback keeps a broken timer from turning the wait into a hang). */
#define EHCI_ENGINE_SPIN_WAIT 4000000U

struct ehci_engine_ops {
    void *context;
    /* One operational register, at an offset from the operational base. */
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);
    /* One queue element's token dword, as the controller left it. */
    uint32_t (*qtd_token)(void *context, uint32_t index);
    /* Link (`true`) the transfer head behind the list head, or unlink it. */
    void (*link_transfer_head)(void *context, bool linked);
    void (*barrier)(void *context);
    uint64_t (*deadline)(void *context, uint32_t microseconds);
    bool (*expired)(void *context, uint64_t deadline);
    void (*pause)(void *context);
    /* Offset of the selected port's PORTSC, or zero when no port is selected.
       Read with the failure snapshot, before the stop. */
    uint32_t portsc_offset;
};

/* What one transfer left behind, for the report's detail. */
struct ehci_engine_result {
    enum ehci_transfer_result transfer; /* the descriptor classifier's verdict */
    enum ehci_engine_cause cause;       /* the controller's verdict */
    uint32_t failed_index;              /* the descriptor the classifier named */
    uint32_t usbcmd;                    /* captured before the unlink */
    uint32_t usbsts;
    uint32_t portsc;
    /* ITEM-564: the stop's own facts, kept beside the transfer's. */
    enum ehci_engine_cleanup cleanup;
    uint32_t doorbell;          /* the word written to USBCMD */
    uint32_t doorbell_readback; /* what USBCMD read back after the write */
    /* ITEM-566: the register words at the three points that decide who changed
       the schedule: before the head was linked, at the first refusal, and
       after the stop.  RS/ASE/ASS/HCH are read from these words. */
    uint32_t usbcmd_before_link;
    uint32_t usbsts_before_link;
    uint32_t usbcmd_first_refusal;
    uint32_t usbsts_first_refusal;
    uint32_t usbcmd_after;      /* USBCMD after the stop */
    uint32_t usbsts_after;      /* USBSTS after the stop */
    uint32_t ass_waited_us;     /* how much of the start-up window was used */
    bool stale_iaa;             /* an IAA from an earlier transfer was cleared */
    bool schedule_running;      /* ASE and ASS both read set before the stop */
    bool ass_seen;              /* ASS was observed at least once */
    bool halted_confirmed;      /* HCHalted was read back after commanding it */
    bool restarted;             /* the schedule was re-armed after the stop */
    bool restart_confirmed;     /* ASS came back after the re-arm */
    bool quiesced; /* the controller confirmed it left the head */
    bool stalled;  /* future transfers must not rewrite the pool */
};

/*
 * The window a schedule that was just enabled is given to report its status:
 * USBSTS follows USBCMD at frame boundaries (EHCI 1.0 section 4.8), so one
 * frame of tolerance is the specification's, and two frames keep a slow
 * controller from being declared stopped.
 */
#define EHCI_ENGINE_ASS_TRANSITION_US 2000U

/* "NONE", "WAIT_EXPIRED", "HSE", "HALTED", "ASYNC_STOPPED", "ASYNC_ADVANCE",
   "STALLED" - the short names the snapshot and the operator's line carry. */
const char *ehci_engine_cause_string(enum ehci_engine_cause cause);

/*
 * Run one transfer through the asynchronous schedule.  `qtd_count` descriptors
 * were built by the caller and the last one is the completion witness.
 */
void ehci_engine_run(const struct ehci_engine_ops *ops,
                     uint32_t qtd_count,
                     uint32_t budget_us,
                     uint32_t iaa_budget_us,
                     bool engine_stalled,
                     struct ehci_engine_result *result);

#endif
