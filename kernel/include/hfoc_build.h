#ifndef HFOC_BUILD_H
#define HFOC_BUILD_H

/*
 * ITEM-552: what the diagnostic report says about the binary that produced it.
 *
 * `HFOC_BUILD_ID` is the revision the kernel was compiled from, passed by the
 * build (`git rev-parse --short=12 HEAD`) and "unknown" when the build has no
 * repository to ask - a kernel built from a tarball must still boot and still
 * answer, it just cannot name a revision.  The version string is the VERSION
 * file's value, passed the same way, so the report never disagrees with the
 * product the operator installed.
 *
 * The extractor prints both in its header, so a report carried to another
 * machine says which build wrote it without the reader having to guess.
 */

const char *hfoc_build_id(void);
const char *hfoc_version_string(void);
const char *hfoc_version_title(void);

#endif
