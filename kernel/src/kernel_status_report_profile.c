/*
 * ITEM-581: the status report on a medium that does not compile the
 * production owners.
 *
 * The console and the diagnostic export call the report on every medium: the
 * installed system prints it on demand and writes it into `diag save`, and the
 * diagnostic media must carry it too so the export proof and a field dump
 * always contain the same five records.  The production kernel owns the live
 * values (kernel.c); a medium without the production network or TLS owners has
 * no values to read, and this file makes that state explicit instead of
 * leaving the records out.
 *
 * The shape of every record is the one the production report prints, so a
 * reader parses one format: the fields exist and say UNAVAILABLE/NONE, and
 * `reason=` names the medium.  This file is compiled only when the production
 * owners are absent, and kernel.c's report is compiled only when they are
 * present, so exactly one definition of kernel_production_status_report()
 * exists in every profile.
 */
#include "kernel_status_report.h"

#include "appliance_panel_auth_trace.h"
#include "diag_export.h"

#if !defined(HFOC_PRODUCTION_NETWORK) || !defined(HFOC_PRODUCTION_TLS)

void kernel_production_status_report(kernel_status_sink_fn sink, void *context)
{
    if (sink == NULL) {
        return;
    }
    /*
     * The driver's view does not exist on this medium: no network backend is
     * compiled, so the device, readiness, backend, link, mode and stage are
     * the record's own UNAVAILABLE answer, and the reason names why.
     */
    sink(context,
         "[NET-STATUS] device=NONE readiness=UNAVAILABLE backend=NONE "
         "link=DOWN mode=NONE stage=0 reason=PROFILE_NO_PRODUCTION_NETWORK");
    /*
     * The addressing lifecycle has no wire owner to read: no lease, no DHCP
     * attempt and no exchange counters can exist, which is itself the
     * concrete state a reader needs instead of a missing record.
     */
    sink(context,
         "[NET-ADDR] state=UNAVAILABLE lease=NONE attempt=0/0 "
         "reason=PROFILE_NO_PRODUCTION_NETWORK");
    /*
     * The serving gate, the entropy owner and the administrator gate are
     * compiled only with the production TLS plane; the record keeps its
     * `v=`/`source=`/`state=`/`last=` fields so every medium yields the same
     * parse, and the reason names the missing plane.
     */
    sink(context, "[PANEL] v=0 state=UNAVAILABLE reason=PROFILE_NO_PRODUCTION_TLS");
    sink(context,
         "[RNG] source=NONE state=UNAVAILABLE stage=NONE cpuid_max=0 "
         "rdrand=0 rdseed=0 reason=PROFILE_NO_PRODUCTION_TLS");
    sink(context,
         "[ADMIN] state=UNAVAILABLE last=UNKNOWN "
         "reason=PROFILE_NO_PRODUCTION_TLS");
    /*
     * ITEM-617: the browser sign-in owner is absent on this medium, so the
     * record is the honest NOT_ATTEMPTED shape rather than a missing line.
     */
    appliance_panel_auth_trace_status_report(sink, context);
    /*
     * ITEM-615: the profile still links `diag save`, so its retained refusal
     * reaches the same shared composition.  The getter only reads RAM.
     */
    diag_failure_status_report(diag_export_last_failure(), sink, context);
}

#endif
