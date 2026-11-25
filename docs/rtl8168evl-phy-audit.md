<!-- SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial -->
# RTL8168evl PHY audit (ITEM-593)

Latest correction: [ITEM-597 page readback evidence](#item-597-page-readback-evidence)
supersedes the earlier assumption that a selected vendor page must echo.

Scope: Toshiba L875, PCI 10ec:8168 rev 7, XID 02c9. The verified export
from build cb7e799c10b3 (session 00000001, 62 records, CRC 3AFBCF3E) shows
working PHY access and advertisements, but no observed link or completed
negotiation. The code comparison below identifies an initialization gap;
the export alone does not establish the physical cause of link failure.

## Reference and applicability

The primary reference is **Linux v6.12**, not a moving master branch:

* [r8169_main.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_main.c):
  the 7c8/2c8 XID match selects VER_34 and rtl8168e-3.fw;
  rtl8169_up, rtl8169_init_phy, rtl_hw_start_8168e_2 and r8169_apply_firmware.
* [r8169_phy_config.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_phy_config.c):
  VER_34 selects rtl8168e_2_hw_phy_config, including its
  r8168d_modify_extpage, r8168d_phy_param and rtl8168f_config_eee_phy helpers.
* [phy_device.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/phy/phy_device.c):
  genphy_resume, genphy_soft_reset and phy_poll_reset specify distinct power
  resume and initiated BMCR reset, 50 ms polling, 600 ms bound, then 1 ms.

Source SHA256, respectively:
`c7d8ba27d0266db93d5177388b95a688d825ec7244e674a5fd2f2f10d74a0417`,
`3462237a279ecaf29fc14d5021d68e6aeb45e0123493e3e9e200c64c1944b84b`,
`6b6045a98fa2084a02f61cf400f984a1cb1d5c531745cf9625cf5982f13a77ff`.

This change independently implements the hardware register operations in
HFOC's bounded, error-returning PHYAR interface. No Linux function bodies,
firmware interpreter or firmware bytes are copied or relicensed. Register
addresses, masks and required order are the interoperability facts used.

## Comparison

| Reference stage | HFOC at cb7e799c | ITEM-593 action / deliberate difference |
| --- | --- | --- |
| MAC identification and reset | Correct EVL profile; ChipCmd reset | Retained; this is not PHY reset |
| PHY resume before vendor tuning | Power-down cleared only with final AN command | Clear BMCR power-down before tuning; preserve original BMCR |
| Firmware before vendor tuning | Skipped | Still explicitly SKIPPED; see dependency below |
| Delay cap | Absent | Extended page ac, register 18 = 0006 |
| Channel estimation | Absent | Page 3, register 09 = a20f |
| Indirect green parameters | Absent | Page 5, index 05 / data 06: 8b5b=9222, 8b6d=8000, 8b76=8000 |
| Corner performance | Absent | Page 5, selector 8b80, register 17 OR 0006 (not indirect register 06) |
| Auto speed down | Absent | Extended page 2d register 18 OR 0010; page 0 register 14 OR 8000 |
| 10M waveform / two-pair detection | Absent | Indirect 8b86 OR 0001; 8b85 OR 4000 |
| EEE PHY helper | Only MAC-side EEE | Extended page 20 register 15 OR 0100; indirect 8b85 OR 2000 |
| Green feature | Absent | Page 3 register 19 OR 0001 and 10 OR 0400; page 5 register 01 OR 0100 |
| PHY soft reset after tuning | Only waits for an existing RESET | Initiate BMCR RESET, bounded wait and post-reset settle before advertisements |
| EPHY / clock / MAC start | Added by ITEM-592 | Retained, before final AN start; PHY preparation precedes these stages |
| Advertise / restart / observe | Page 0, registers 4/9, read-back | Retained after reset; command result and observed completion reported separately |
| Cold-start timing | Fixed-cycle fallback before console calibration | Locally measure TSC through existing timer API before PHYAR; retain microsecond and reset wait budgets |
| Page handling | Page 0 only | Explicit extended-page and indirect selectors; restore and verify page 0 on success and failure |

The initial MAC reset quiesces HFOC's polling device before PHY preparation.
Linux also performs MAC cleanup/reset around hardware start. PHY tuning and
its own reset precede EPHY/clock/MAC enable and final advertisement/restart.
Other profiles retain their previous paths; none receives EVL tuning.
The 001cc915 PHY uses genphy_resume in Linux v6.12 realtek.c; the 20 ms
rtlgen_resume delay of other PHY models does not apply. MAC-only callers
may explicitly omit PHY state, yielding SKIPPED stages; production supplies
it and never uses that mode.
All PHYAR operations retain their 20 us post-operation pause, including
failure. Firmware absence is not replaced with guessed firmware writes.

## Firmware dependency and license

[linux-firmware 20241017 WHENCE](https://gitlab.com/kernel-firmware/linux-firmware/-/blob/20241017/WHENCE)
lists rtl_nic/rtl8168e-3.fw version 0.0.4 under the r8169 section. That
section has its **own** Realtek 2011–2013 permission to distribute firmware
data in hexadecimal or equivalent form with its copyright notice. It does
not refer to LICENCE.rtlwifi_firmware.txt (the subsequent r8152 section does).
WHENCE SHA256:
`7851ae8e04d71ccfc741fd2df450ef5724c4a172a0a282f69e8a8ce44877ba84`.
The Linux driver's GPL-2.0-only license is not the firmware's license.

The actual unmodified rtl8168e-3.fw from that tag was inspected outside the
repository: 3,872 bytes, SHA256
`1287fdd82ff2f60d75c9970c108b49cfd11456ad3fe29c9666b8539a35777dbf`,
header `rtl8168e-3_0.0.4 03/27/12`, byte checksum zero, 943 encoded
instructions. Its static stream contains 913 write instructions, page-5
parameter/data programming and conditional PHY polling; its last instruction
writes BMCR 9200 (reset/restart). These are encoded-instruction counts, not a
claim about the number of executed bus operations. No instructions were run
on hardware or installed into HFOC.

Linux's [r8169_firmware.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_firmware.c)
interprets this PHY register instruction stream; this is a PHY MCU
patch/configuration dependency, not a host executable. When
requesting firmware fails, rtl_request_firmware leaves rtl_fw null;
r8169_apply_firmware then returns without writes, and vendor configuration,
reset and negotiation continue. Thus firmware is optional to that software
startup path. This is evidence that trying initialization without the patch
is supported, not proof that this Toshiba will link without it.

No blob is added. Full firmware-equivalent initialization and physical link
remain unproven. If fresh hardware evidence requires the patch, the prepared
delivery approach is an independently implemented bounded instruction
interpreter (header/checksum/bounds/opcode validation, bounded branches and
polling, first-error retention and page recovery), the pinned unmodified
3,872-byte image with the digest above, its own Realtek notice,
SBOM/license inventory and both installer/installed payload coverage. That
requires a separate firmware-delivery decision and review; relabeling the
blob as HFOC or copying the GPL interpreter is not that solution.

## Proof boundary

The host model exercises rtl8168_cold_boot, the production entry point,
with separate page, extended-page and indirect register storage. Its
literal expected writes derive from the reference above. It checks order,
masked-bit preservation, intermediate refusals, page recovery and bounded
reset waiting. It cannot prove analog behavior or firmware sufficiency.
Virtio/e1000 QEMU tests establish integration non-regression only.

Hardware acceptance stays OPEN until a fresh verified Toshiba export from
the delivered build shows link=UP, aneg=1 and lease=ACQUIRED. Panel testing
follows address acquisition and separately checks entropy and account state.


The adapter reports an identified RTL probe even if a failed startup never
activated its backend. The host journal/export proof compiles the real x86
adapter and verifies this refusal through its public PHY-info function,
then through diag save and the CRC-checking operator reader. Diagnosis must
not depend on successful NIC activation.


## Production cold-start timing

Network initialization runs before kernel.c starts the console cycle-clock
calibration. The old NIC delay callback used the same 200,000-cycle fallback
for every request, so a requested 50 ms PHY reset poll was not a 50 ms wait.
An actual-adapter clock-model regression reproduces this independently of
PHY register storage: the previous callback fails its requested-duration
assertion while the PHY recipe tests alone pass.

Before installing the PHYAR callback, the NIC measures and caches a local
TSC rate against a checked form of the existing IRQ-independent 100 ms PIT
wait. The checked result reports a bounded stall and restores the configured
timer on either outcome; a nonzero TSC delta alone cannot establish that the
reference interval completed. The old void callers retain their behavior.
The two small checked APIs are the only timing-utility addition needed by
this NIC; no interrupt mask, routing or timer architecture changes. A failed
reference refuses the PHY before any PHYAR command. PHY operations retain
the checked local rate even if the later console clock reports another rate. Microsecond conversion
rounds up. Zero-duration waits do nothing; calibration is cached rather than
repeated for each operation. The 20 us PHYAR settle, 25 us poll and 50 ms PHY
reset poll requests are measured durations on this path.

The reference wait's production loop is also executed in the host proof with
modeled PIT ports: requested crystal counts complete, a stopped counter
returns refusal after 100,000 unchanged samples, and the old void wrapper
keeps its existing bounded behavior. A second 50 ms delay assertion excludes
calibration time, so calibration cannot hide an omitted requested wait.

Every completed BMSR read is retained immediately, even if a subsequent LPA
or identity read fails. Such a failure invalidates the complete current
snapshot; it must not erase an already observed historical link/AN bit.
The production cold-start and refresh refusal paths both have regressions
for this distinction.

During required QEMU export verification, the preflight incorrectly refused an
ELF that did contain the expected build ID: strings received SIGPIPE (141)
after grep -q found the ID (0). The check now reads the same binary bytes in
one process. A regression executes the real preflight with a large matching
kernel and with an absent ID, stopping at a deliberately invalid journal
before QEMU can start. Identity and journal refusals remain enforced; this
is a verification correctness fix, not a CI optimization or relaxed gate.


## New hardware evidence and first-transaction diagnostics (ITEM-594)

The delivered 8d7947c3aeb3 build was run on Toshiba. Session00000001 has
62 records and CRC377971F9, independently checked again on USB12345678; its
text matches the owner's HFOC-DIAG-00000001.txt in the older-named directory.
MAC reset and PHY resume passed, phy_tune refused with RTL8168_IO_ERROR.
PHY reset, EPHY, clock, MAC, advertisements and AN start were SKIPPED. This
physical run does not satisfy the network criterion. It also does not name
the particular vendor operation or establish firmware as the cause.

ITEM-594 preserves the first failed transaction before page cleanup, using
values from the existing command/poll path. It adds no hardware reads,
writes, retries, polls or delay changes. NET-PHY-FAIL has these meanings:

| Field | Meaning |
| --- | --- |
| step | Vendor action1..15 in the ordered table above;0 is standard PHY access |
| phase / op / reg | Recipe substep, bus read/write direction and actual MII register |
| page_req / selector_req | Intended addressing, not a claim it was accepted; NONE means no selector request |
| reason | MMIO command write/read failure, PHYAR timeout, page mismatch or BMCR reset timeout |
| cmd / last / polls | Submitted command, last successful completion-poll sample (UNREAD if none), number of poll attempts including a failed callback |
| expected / observed | Requested write/comparison value or NONE; low16 from the separate payload read or UNREAD |
| restore / restore_page | Cleanup after that first failure: OK, FAILED or SKIPPED, and its own actually read page or UNREAD |

A last PHYAR value on timeout is raw handshake evidence, not a completed
PHY register read. A later refresh retains the first transaction and cleanup
verdict. A successful PHY has no failure record. The bounded line uses one
composer in network/boot publishing and the canonical diagnostic snapshot.
The production-path model distinguishes page mismatch, command failure,
uncompleted PHYAR, failed poll reads and a second failure during cleanup;
all vendor read/write refusals retain their exact action. The real FAT12
journal/diag-save/CRC-reader proof includes the new record. Physical link,
AN completion, DHCP and firmware sufficiency still require hardware evidence.

## PHYAR completion and payload reads (ITEM-595)

The fresh a9c16e5ce7b7 Toshiba session stopped at the first page-7 selector
verification. Its completion poll was `last=0x801F0000`, with the busy flag and
register selector present but low16 zero; cleanup restored page 0. This run
does not prove that a second PHYAR read would have returned page 7.

Linux v6.12 `r8169_mdio_read` performs the bounded completion check and then
reads `PHYAR` again for the payload. HFOC previously returned the low16 value
captured by the completion poll itself. ITEM-595 aligns the read path with the
reference: the completion sample remains in `transaction.last`, while
`transaction.observed` is the low16 value from the separate payload read.
That read and its 20 us settle occur only after successful completion. A
failed payload read is `MMIO_READ_FAILED`; it preserves the completion sample,
leaves `observed` unverified and still performs the settle.

The host models report completion and payload as separate MMIO responses. The
completion sample carries stale low16 data, so the old production path fails
the page check while the corrected path reaches page 7. The production
cold-start model also drives a payload-read MMIO failure and verifies page-0
recovery. The page-mismatch guard, poll budget, write path, EPHY path and other
supported profiles are unchanged. Physical link and firmware sufficiency
remain hardware questions.

## Page-selection evidence after hardware refusal (ITEM-596)

The fresh Toshiba export from **310ea2caf168** (session00000001,61 records,
CRC4F2F1CE7) still refuses step1 PAGE/READ: requested page7, completion
sample801f0000 after2 polls, separate payload low16=0, page0 cleanupOK.
Thus ITEM-595's protocol correction is present but did not resolve the
hardware mismatch. Selector1e=ac and delay-cap register18 were not reached.
The physical cause remains unknown; firmwareSKIPPED is not a diagnosis.

The additional source review gives these boundaries:

| Reference stage | Actual HFOC path at310ea2ca | Finding |
| --- | --- | --- |
| VER_34 MDIO | Ordinary PHYAR, bounded poll then separate payload,20us settle | Matches v6.12; OCP cached pages of VER_40+ do not apply |
| MAC reset / early initialization | ChipCmd reset before PHY preparation | Distinct from PHY reset; v6.12 early OOB initialization has no VER_34 case |
| phy_init_hw / resume | Select page0, keep BMCR0, wait existing RESET, clear power-down | RTL8211E001cc915 has no soft_reset callback; its GMII config_init needs no RGMII delay writes |
| Firmware / special tuning | FirmwareSKIPPED; page7 write/read then selector and vendor settings | First page verification refuses before any vendor parameter; firmware sufficiency unproven |
| r8168d_modify_extpage | Write1f=7, verify1f, then1e=selector | Linux saves the old page and writes the new one, without HFOC's post-write comparison; no evidence yet justifies bypassing that comparison |
| genphy_soft_reset / advertisements / AN | Initiated PHY reset after tuning, then EPHY/clock/MAC and advertisement/restart | Currently SKIPPED because tuning refused; not evidence of a reset timeout or successful AN |

Alongside the pinned v6.12 files, see
[realtek.c at v6.12](https://github.com/torvalds/linux/blob/v6.12/drivers/net/phy/realtek.c)
for RTL8211E callbacks and rtl821x_read_page/write_page. The
[2019 helper patch](https://lists.openwall.net/netdev/2019/11/09/142) removes
the older4→7 /2→0 transitions, including the VER_34 delay-cap sequence;
they are absent in v6.12. Their historical presence alone does not justify
adding them back as required hardware initialization. Realtek's standalone
[RTL8211E/EG datasheet Rev1.6](https://datasheet.lcsc.com/datasheet/pdf/4db0b5f392a00cbc87420d89cf1d417f.pdf?productCode=C90735),
section8.4.22 table44 (printed page43), describes bits2:0 of1f as RW.
Its applicability to every detail of the integrated NIC PHY is unproven;
it does not establish zero as a normal readback of selected page7.
PDF SHA256:452eb038a4cf18dbda94be65cc884883290418b4bc616e735719ae302086d5b2.

ITEM-596 closes an evidence gap, not the link failure. Previously the page
write's completed transaction was overwritten by its verification read.
Now a refused verification freezes that preceding write's submitted command,
actual last completion sample and poll count, alongside the full separate
read payload when available. Cleanup and later refreshes cannot replace this
pair. NET-PHY-PAGE reports write_cmd/write_last/write_polls/read_payload in
network, boot journal and the canonical diag-save snapshot. UNREAD means no
sample was obtained. A completed write handshake still does not establish
that the PHY selected the bank. NET-PHY-FAIL retains the original semantics.
No additional PHY/MMIO reads, writes, retries or delays are introduced, and
the strict page-mismatch refusal remains. Firmware policy is unchanged.

The production cold-start regression with a refused page selection fails on
the previous implementation because the pair is absent from publication,
real FAT12 journal/diag save and CRC-verified export. It verifies unchanged
command/sample counts, skipped MAC/reset/AN, page0 recovery and persistence
across successful and failed refresh. Opaque upper payload bits deliberately
differ from the completion sample to reject synthesized raw evidence; their
test value is not a claim about a physical status bit. Payload failure stays
UNREAD, unrelated register failures produce no stale pair, and oversized
records are refused whole. Existing independent v6.12 recipe expectations,
bounded failures, timing and other-profile tests remain active.

A fresh hardware file is still necessary to inspect the write side. Neither
this diagnostic addition nor host/QEMU success establishes why page7 reads
zero, or establishes link/AN/DHCP success. The hardware criterion remains OPEN.


## ITEM-597 page readback evidence

The owner ran the separate atomic Linux probe on SatelliteL875-B6M,
PCI10ec8168rev07/XID2c9/PHY001cc915, kernel6.12.107+deb13-amd64.
TXT toshiba-linux-page-FUuaQ6Jk.txt,150712bytes,SHA256
`a4308c86bec1467a4e4b04bf3c0d66397a7447132df57dfca4710a108ea853b9`:
select3 completed, read1f=0000 but bank3reg09=a20f; select0 then reg09=0300.
Select7 also read1f=0000; that alone does not independently prove bank7.
All16reads/4page-onlywrites completed, cleanup controls matched, module
unloaded and Linux1000Full/carrier1/IPv4 remained. No policy bypass occurred;
Linux accepted the external unsigned module with its normal taint warning.

The primary v6.12 chain confirms the unsupported HFOC precondition:
[phy-core.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/phy/phy-core.c)
(blob1f98b6a96c15355e5c318de0f161e426639c483c), phy_select_page saves the old
page and writes the new page without requiring a new echo; phy_write_paged
then writes the target register and restores the old page.
[realtek.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/phy/realtek.c)
(blob166f6a7283731e2d3536214b7cda481d7cc7b9ac) maps RTL8211E page access
to ordinary reads/writes of1f. VER34's register recipe is unchanged above.

| Reference / hardware fact | Previous HFOC path | ITEM-597 result |
| --- | --- | --- |
| Write new vendor page; no echo requirement | Every selected page must echo | Only EVL vendor pages permit the observed zero response |
| Page3 data changes despite selector0 | First page7/read0 refuses before delay cap | All15 actions, PHY reset and AN command become reachable in the zero-readback model |
| Zero does not prove bank acceptance | Failure pair retained only on mismatch | First completed non-echo pair/count published as EVL_ZERO_UNVERIFIED |
| PHYAR errors and post-operation settle | Bounded writes/reads,20us pause | Same operations, waits, first-error and cleanup handling |
| Other profiles and standard page0 | Strict comparison | Unchanged strict API and standard-page recovery |

The private EVL helper retains actual readback, never a software-cached page.
Only a completed zero response for a nonzero vendor page is tolerated;
unexpected nonzero values, MMIO failures and timeouts still refuse. This
matches the reference's ability to proceed without a page echo while making
HFOC's uncertainty visible. It cannot detect an ignored write from a zero
echo alone. The existing negative journal fixture therefore now uses an
unexpected nonzero response2 instead of classifying the newly characterized
zero as a failure; generic zero-mismatch refusal is separately retained.
No negative test or timing/IO gate is removed.

The regression uses rtl8168_cold_boot with actual0/3/5/7/extended/indirect
storage and the independent literal v6.12 write sequence. Before the fix it
fails4checks because it stops before delay cap. Journal/export RED fails
3checks; GREEN covers real FAT12 journal, diag_export_save and CRC reader,
intermediate read/write failures with both echo modes, raw observation
retention, generic strict refusal and other supported profiles.

The Linux evidence is after firmware version0.0.4, whose exact hash matches
the earlier audit. It does not establish cold-ROM page semantics or firmware
necessity, and does not prove HFOC will link. No firmware or Linux code is
imported. PHY initialization is not declared complete. Fresh Toshiba HFOC
linkUP/aneg1/leaseACQUIRED remains the hardware acceptance condition.
