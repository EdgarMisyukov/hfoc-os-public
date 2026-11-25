# Persistent workspace binding (ITEM-635)

ITEM-635 closes the ITEM-631 installed-workspace blocker and binds the declared
HFOCFS image on an ordinary installed boot.  It does not change the partition
table, the eight fixed state stores, the host-seed companion slots, the
sealed-state custody block or any host disk.

## Root cause of the ITEM-631 blocker

The ITEM-631 RCA blamed a single AHCI read for making PS/2 input disappear.
That was wrong.  A QEMU i8042 trace of the same installed image shows every
injected key arriving and being read by the kernel:

```
ps2_keyboard_event ... qcode 44 down 1 ... set 2 xlate 1
ps2_put_keycode ... keycode 0x4b
pckbd_kbd_read_status 0x1d
ps2_read_data ...
pckbd_kbd_read_data 0x26          # 'l' in the guest's translated set
```

The real collision was the allocation itself.  The state layout ends at
partition block 512, and `appliance_host_seed_store_present()` reads its two
companion slots at block 512 and block 513.  ITEM-631 started the HFOCFS image
at block 512, so a first-use format wrote an `HFOCFS1` superblock over the
first host-seed slot.  The presence check then reported a host seed that did
not exist, `console_start_entropy_unlock()` started the entropy login prompt,
and the update proof's `language` command became the login name.  The serial
log shows the next prompt:

```
Логин: <IRQ1 burst> Пароль: ...
```

That is why the proof timed out waiting for `ЯЗЫК`: the keys were delivered,
but the wrong conversation consumed them.  Zeroing blocks 512 and 513 of the
same artifact and re-running the proof prints `ЯЗЫК: RU`, which pins the
collision rather than an AHCI or PS/2 defect.

## Corrected layout

| Partition blocks | Use |
|---|---|
| 0..511 | eight fixed appliance state stores (unchanged) |
| 512..513 | two host-seed companion slots (reserved, never touched) |
| 514..33281 | 16 MiB HFOCFS workspace image (32768 sectors) |
| 33282..65534 | free / future allocation |
| 65535 | sealed-state custody block (unchanged) |

`HFOCFS_WORKSPACE_OFFSET_BLOCKS` is derived from
`APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS` plus
`APPLIANCE_HOST_SEED_STATE_SLOT_COUNT`; compile-time assertions refuse a
future edit that would move the image back over the host-seed slots or past
the custody block.

## First use and failure boundary

`hfocfs_workspace_bind` reads the first 512 bytes of the declared range at
block 514:

* `HFOCFS1` — mount the existing image;
* all zero — format and mount a new empty generation-1 image (the production
  first-use path passes `format_empty=true`);
* anything else — return `REFUSED` and write nothing.

The host-seed slots are outside the bound range, so no workspace write can
change their presence check.

## Production bridge

An ordinary installed boot now:

1. creates `/home` on the writable root before the read-only initramfs is
   mounted at `/` (the archive does not carry `/home`; no separate mount
   record is added, so the existing `[VFS] mounts=3` record is unchanged);
2. binds the declared range at block 514;
3. formats an all-zero first use and mounts an existing HFOCFS image;
4. imports the persisted tree at `/home/owner`;
5. publishes the commit hook used by file close/create/remove/rename.

The hook is re-entrancy guarded and the commit is synchronous; there is no
background flush thread and no change to timer/IRQ/scheduler ownership.  A
failed sync reloads the last durable generation and returns an error to the
caller instead of claiming success.

## Evidence

- `make test-hfocfs-workspace-host`: sentinels cover the state prefix
  (0..511), both host-seed slots (512..513) and the custody block; a first-use
  format leaves the host-seed slots zero (or a non-zero sentinel unchanged)
  and writes `HFOCFS1` at block 514; the refused/too-small cases still write
  nothing.
- `make test-hfocfs-workspace-qemu`: two QEMU boots share one 32 MiB ide-hd;
  boot 1 writes `/home/owner/phase1.txt`, boot 2 verifies it and writes
  `phase2.txt`; the host then checks the state prefix and host-seed slots are
  byte-identical, HFOCFS starts at block 514, and decodes both files
  independently.
- `make test-production-installer-update-qemu`: the `settings-installed`
  console answers the language command in the preserved language and does not
  print the `Пароль:` prompt, which is the installed-boot proof that the
  workspace no longer fabricates a host seed.  The same test checks the
  staged installed kernel, the state-preserving update and the refusal cases.

## Acceptance gate repairs in the same change

The branch CI exposed two pre-existing QEMU gate failures that also reproduce
on the current `main`:

* `test-scheduler-wait` started its 5-tick sleep before the LAPIC timer
  runtime was active, so the PIT could complete the sleep before the LAPIC
  timer EOI.  The waiting loop is compiled only into the
  `HFOC_SCHEDULER_WAIT_QEMU_TEST` kernel; production scheduling and IRQ
  ownership are unchanged.
* `test-production-network-qemu` could read the console status report after an
  IRQ trace split the `[RNG]` line.  The console status sink now holds the
  serial sequence around the whole line, exactly as the language answers do;
  the record contents are unchanged.

Both repairs restore the existing proofs rather than weakening them.

## Explicit non-claims

No physical SSD/USB layout change is made by this item.  No partition table or
state-layout version changes, no reuse of encrypted secret slots, and no
encryption claim for HFOCFS.  The physical Toshiba update and the owner's
filesystem/reboot acceptance remain owner-run and OPEN; QEMU is not a Toshiba
hardware proof.
