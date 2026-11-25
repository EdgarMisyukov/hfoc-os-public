#ifndef HFOC_LWIP_ARCH_CC_H
#define HFOC_LWIP_ARCH_CC_H

/*
 * Minimal lwIP compiler/architecture mapping for the HFOC port.
 *
 * lwIP pulls this header through arch.h. Diagnostics and assertions are
 * disabled so vendored lwIP headers can be used by the host static gate and
 * later freestanding kernel sources without pulling libc symbols.
 */

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NOASSERT
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { } while (0)
#define LWIP_NO_CTYPE_H 1

#endif
