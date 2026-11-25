/*
 * ITEM-552: the build identity the diagnostic report carries.
 *
 * The two values come from the build's generated header
 * (build/generated/hfoc-build-id.h, written from `git rev-parse --short=12
 * HEAD` and the VERSION file, see the Makefile).  ITEM-557 made that a header
 * rather than a -D flag so the compiler records it as a dependency of this
 * object: a stale object cannot keep publishing a revision of another checkout.
 * A build with no generated header - a host proof including this file - answers
 * "unknown", which is a fact the report can carry and a reader can see, not a
 * FALSE answer.
 */

#include "hfoc_build.h"

#if defined(__has_include)
#if __has_include("hfoc-build-id.h")
#include "hfoc-build-id.h"
#endif
#endif

#ifndef HFOC_BUILD_ID
#define HFOC_BUILD_ID "unknown"
#endif
#ifndef HFOC_VERSION_STRING
#define HFOC_VERSION_STRING "unknown"
#endif

const char *hfoc_build_id(void) {
    return HFOC_BUILD_ID;
}

const char *hfoc_version_string(void) {
    return HFOC_VERSION_STRING;
}

const char *hfoc_version_title(void) {
    return "HFOC OS " HFOC_VERSION_STRING;
}
