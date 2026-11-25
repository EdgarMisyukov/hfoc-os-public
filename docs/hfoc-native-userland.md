# Native HFOC userland (ITEM-626)

This is the native user runtime and the external programs the ordinary rootfs
carries under `/bin`. It is **not** Linux, not Bash and not a POSIX
certification: the ABI is HFOC's own, and the programs are static ELF64 images
built from this tree with no libc, no dynamic loader and no new third-party
source.

## ABI and error rule

The user header is `user/libhfoc/hfoc.h`; `tests/user-runtime-host.c` compares
every number, flag and error value with `kernel/include/syscall.h` at compile
time, so the two cannot drift silently.

The legacy calls keep their published numbers and meanings:
`WRITE=0`, `YIELD=1`, `EXIT=2`, then the ITEM-624 file calls and `EXEC=15`.
ITEM-626 appends `RESULT=16` and makes `EXIT` accept a status in the low byte
of `arg0`; the old test calls that passed zero are unchanged.

The kernel historically returns small positive error codes (`1..16`). A byte
count, a descriptor or a seek result can be a small positive number too, so a
program cannot tell `5` bytes from `ENOENT` by the value alone. Every file and
exec call now records its error in the calling process, and `RESULT` returns
that error without clearing it. The runtime reports a negative value
(`-HFOC_ENOENT`, ...) only when a result is in the ambiguous `0..16` range and
`RESULT` says the call failed. Legacy callers that never ask `RESULT` see
exactly the old values.

`user/libhfoc/syscall.c` declares `rcx`, `r11`, `r8`, `r9`, `r10` and memory
as clobbered by the `syscall` instruction. The kernel restores `rcx`, `r11`,
`rdi`, `rsi` and `rdx`; it does not preserve `r8..r10`, so a wrapper that kept
a value in one of those registers across the `RESULT` query read kernel
garbage. The QEMU proof caught this as a read on descriptor 0.

## Layout and build

`user/crt0.S` publishes the SysV-style initial stack (`argc`, `argv`, `NULL`,
`envp`, `NULL`) as `hfoc_argc`, `hfoc_argv`, `hfoc_envp` and `environ`, calls
`main`, and exits with its return value. `user/linker.ld` emits one RX segment
and one RW/NX segment at `0x400000`; the ITEM-625 loader accepts that shape.

`make iso` stages `rootfs/` and copies `build/user/bin/*.elf` to
`build/rootfs-stage/bin/<name>` before packing `hfoc-rootfs.arc`. The test
image (`make test-user-init`) uses the same programs plus the ITEM-625
fixtures and a generated `/etc/big` for the ENOSPC case.

## Programs and supported flags

| program | supported arguments |
| --- | --- |
| `echo` | `-n`; other options fail with status 2 |
| `pwd` | none |
| `ls` | `-l`; otherwise names |
| `cat` | files, or stdin when none |
| `mkdir` | one or more paths |
| `rm` | `-f`; files, not directories |
| `cp` | `SOURCE DESTINATION` |
| `mv` | `SOURCE DESTINATION` within one mount |
| `wc` | `-l`, `-w`, `-c`; files or stdin |
| `grep` | `-i`, `-n`; `PATTERN [FILE...]` |
| `true`, `false` | none |
| `hfoc` | no argument, `env`, `exec PATH [ARG...]` |

Unsupported options fail instead of being ignored. Multi-file `grep` prefixes
the file name; `grep` exits 0 on a match, 1 without one and 2 on an error.
`hfoc exec` is an `exec` (not a fork); a missing path exits 127 and a path that
exists but is not a loadable native image exits 126, which is the mapping the
later `sh` uses.

## Bounds

- path 1023 bytes + NUL, name 255 bytes + NUL (UTF-8 bytes, no locale folding);
- one read/write call up to 64 KiB, one file up to 64 KiB;
- 64 descriptors per process; descriptors 0/1/2 are the typed streams;
- a fixed 32 KiB bump arena inside the program, never the kernel heap;
- the rootfs archive and the VFS keep the ITEM-622/623 limits: 256 nodes,
  64 data blocks of 4 KiB (256 KiB), 64 archive entries;
- an exit status is one byte.

## Proof

`make test-user-init` boots the test image and first runs the ITEM-625
lifecycle matrix, then `[USER-UTIL] start`. The parent spawns each program
through the production ELF loader and syscall entry and reaps its real status:
`mkdir`, `cp`, `cat`, `wc`, `grep`, `mv`, a second `cp`, `rm`, the read-only
and missing-path refusals, `echo`, `true`, `false`, `pwd`, `ls`, `hfoc env`,
`hfoc exec` (success, 127, 126), a UTF-8 directory/file roundtrip, and an
ENOSPC copy. The parent reads `/tmp/utildir/second` and the UTF-8 file back
through the boot VFS, so printed text alone cannot pass. A free-page and
scheduler count is taken before and after the whole phase. The ordinary
production ISO carries the same `/bin` programs; the installed-boot proof
keeps checking the populated rootfs.

Not in this item: a shell, pipes, redirection, job control, a PTY/SSH session
and a persistent filesystem. Those are ITEM-628..633.  ITEM-628 adds the
native shell; ITEM-629 adds the session-owned TTY and the enrolled owner's
interactive SSH shell.  Persistence, jobs/signals and the final acceptance
remain later items; this is not a Linux binary ABI or POSIX certification.
