# HFOC OS shell acceptance (ITEM-634)

This is the software acceptance for the native owner shell.  QEMU is not a
Toshiba hardware proof; the owner-run installed-system step is listed at the
end and remains separate.

## Run

```
make test-appliance-ssh-qemu
# or, keeping the evidence under diag/item-634/:
./scripts/hfoc-shell-acceptance.sh
```

The proof starts the production SSH fixture in QEMU with a forwarded port,
accepts the pinned stock OpenSSH client on one real pty, and drives the
production `/bin/sh` and `/bin` programs.

## Matrix actually exercised

| Check | Evidence |
|---|---|
| prompt and session-owned TTY | `hfoc$` prompt, clean EOF/exit |
| external programs and argv | `echo`, `wc`, `cat`, `ls`, `cp`, `mv`, `rm`, `sh` |
| file create/read/list/copy/rename/delete | `note` -> `copy` -> `renamed` -> `ls` -> `rm` -> `cat note` |
| pipeline and redirection | `/bin/echo pipeline \| /bin/wc -l`, `> note`, `2>&1` |
| script file | `/tmp/sh-script` executed by `/bin/sh` |
| exit status | `false; echo status=$?` -> 1, `true` -> 0 |
| background job | `sleep 2 &`, `jobs` Running, `wait` |
| terminal signals | Ctrl-Z -> Stopped, `bg` -> Running, `fg` -> Ctrl-C -> prompt |
| shell survives | new prompt and `echo job-done` after the interrupted job |
| second shell session | a second stock-OpenSSH `-tt` shell after the first closes, `echo second-session-ok` |
| SSH refusals kept | subsystem and direct-tcpip requests still refused |
| host key | pinned fingerprint matches the handshake |

## Installed update preservation

`make test-production-installer-update-qemu` runs the installer, language,
update and SSH-key-update media against QEMU disks and checks that an update
preserves the installed state.  It includes the workspace binding: the
`settings-installed` phase must answer the language command as `ЯЗЫК: RU` and
must not print the `Пароль:` prompt.

ITEM-637 adds the file half on the installed disk:
`make test-installed-persistence-qemu` installs from the management medium,
applies the management-scope test-key update, creates the owner account on the
first installed boot, writes `/home/owner/persist-marker` through the real
production SSH shell, and requires the same bytes after a reboot and after a
second state-preserving update.  The same run pins the host key against the
fingerprint the guest publishes, refuses a client key that is not the owner's,
and checks HFOCFS at persistence block 514 with host-seed slots 512-513 left
alone.  See `docs/hfoc-installed-persistence-637.md`.  This is QEMU evidence;
the physical Toshiba run is still owner-run and OPEN.

## Persistent workspace — corrected by ITEM-635

ITEM-631's HFOCFS workspace module, host proof and two-boot QEMU image proof
are merged.  ITEM-635 found the installed-boot blocker was not AHCI/PS2: the
workspace started at partition block 512, the first host-seed companion slot,
so its `HFOCFS1` superblock made an absent host seed look present and the
console started the entropy login prompt.  That prompt consumed the typed
`language` line as a login name (`Пароль:` followed).  The workspace now starts
at block 514, after the two host-seed slots; the production binding and the
file-syscall commit hook are on.  The detailed evidence is in
`docs/hfocfs-workspace-635.md`.  ITEM-637 now proves the installed-image file
path in QEMU (write, reboot, update, reboot).  The physical Toshiba
filesystem/reboot step is still owner-run and OPEN; a QEMU proof is not a
hardware proof.

## Owner hardware step

The owner updates the Toshiba SSD with the state-preserving owner-key update
ISO (declared identity, inside token, first entry, installed config, both
kernels, expected build and SHA-256; USB identity/backup/standard write/full
readback/settle/separate verify-only).  After boot, the physical SSH shell,
file/reboot persistence, panel and wheel criteria are collected on that exact
build.  The owner never writes the host raw disks and the diagnostic
DataTraveler is never formatted.
