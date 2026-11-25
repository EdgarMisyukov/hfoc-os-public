# HFOC OS: the declared forward and its access rule (ITEM-449)

The appliance has a stable address (ITEM-446/447) and a declared management
scope (ITEM-399).  What it did not have is a declared rule for the traffic it
carries *on behalf of* the local network: one server the operator names, and
one answer to "who may use it".

This note is the design the implementation follows.  It states what is
declared, where it is stored, what is refused before anything is opened, how
the relay will be built in the stack this tree actually has, and what remains a
hardware question.  The declaration half is implemented first, because the
whole rule has to be readable and refusable before a port is opened.

## 1. What is declared

One forward, and nothing more:

| part | value | why |
| --- | --- | --- |
| mode | `on` or `off` | fail-closed: an absent declaration and an explicit off both forward nothing |
| target | one literal IPv4 address | a name needs a resolver owner this tree does not have yet (see §6) |
| target port | 1..65535 | the port on the declared server |
| listen port | 1..65535, never 443 | the local port the appliance serves the forward on; 443 belongs to the management serving listener (`APPLIANCE_PRODUCTION_SERVING_PORT`) |
| clients | one to four exact IPv4 addresses | **this list is the access rule**: no list, no service |

The grammar in one line, as the installed boot entry carries it:

```
hfoc.forward=on hfoc.forward.target=10.0.2.3 hfoc.forward.target-port=8443 \
  hfoc.forward.listen-port=9443 hfoc.forward.clients=10.0.2.9,10.0.2.8
```

`hfoc.forward=off` states the explicit no-forward.  Values without a mode, a
mode that is not a mode, a duplicate part, an empty client, a client that is a
prefix (`10.0.0.0/24` is **not** a client), a loopback/multicast/unspecified
target and a listen port of 443 are all refused with the status that names
them: `kernel/src/forward_config.c` is the one grammar, validator and admission
decision.

## 2. Where it is declared and where it is stored

* **The install medium.** `scripts/hfoc-install-media-stage.sh` accepts
  `HFOC_INSTALL_MEDIA_FORWARD=on|off` with `_TARGET`, `_TARGET_PORT`,
  `_LISTEN_PORT` and `_CLIENTS`; the declaration lands in
  `boot/hfoc-install.token` and, through `scripts/hfoc-installed-config.py`,
  in the sealed `boot/hfoc-installed.conf` the installer stages into the
  installed ESP.  `make production-installer-forward-media` builds that
  medium; `test-production-installer-qemu` checks the sealed bytes.
* **The installed boot entry.** The installed appliance reads
  `hfoc.forward.*` from its own command line at every boot, exactly as it reads
  `hfoc.management` and `hfoc.network`.  That is the storage: the entry is the
  record, so the declaration survives reboots without a second persistent
  store, and a machine with no declaration publishes
  `[PRODUCTION-FORWARD] state=UNAVAILABLE reason=NO_DECLARATION`.
* **Deliberately not yet: the machine's own menu and the persistent store.**
  A menu-written rule would need its own store in the state layout (the four
  stores are full; `APPLIANCE_STATE_LAYOUT_ENTRY_COUNT_MAX` is 8 and the
  default region is exactly eight stores long), and the relay half has to exist
  before a menu can offer it honestly.  The boot entry is the same place the
  management scope lives, so this is a complete path, not a stopgap.

## 3. What is refused, and before what

1. **On the media build.**  An unknown mode, an enabled declaration without
   its target/ports/clients, an off with values, a non-host target, an
   out-of-range port, the serving port as a listen port, a repeated or
   over-long client list, and a prefix instead of an address: the stage script
   exits 2 before sealing, the generator raises before writing, and the ISO is
   never produced.  Nothing is installed from a medium that could not be
   honoured.
2. **In the installer, before the first write.**  The same declaration is
   parsed from the token by `production_install_media_declaration_parse()` and
   validated by `production_install_media_plan_build()`: an unhonourable
   declaration becomes `FORWARD_INCOMPLETE` or `FORWARD_MALFORMED` and the run
   refuses with zero writes.
3. **At boot.**  `production_install_media_forward_boot()` parses the installed
   entry; a malformed one is published as
   `[PRODUCTION-FORWARD-DECLARATION] state=REFUSED reason=<status>` followed by
   `[PRODUCTION-FORWARD] state=UNAVAILABLE reason=DECLARATION_REFUSED`, and
   nothing is opened.
4. **At admission.**  A connection from a source that is not one of the
   declared clients is refused by `forward_config_admit()` with
   `DENIED_NOT_ALLOWED`; with no declaration it is `DENIED_NO_DECLARATION`,
   with an explicit off it is `DENIED_DISABLED`.  The order matters: the
   admission decision happens **before** any outbound connection exists, so a
   denied client cannot be used to make the appliance dial the server.

Both records are in the boot log's flush-marker table, so the decision is
readable from the disk after the machine has been rebooted
(`kernel/src/appliance_boot_log.c`).

## 4. How the relay is built in *this* stack

The obvious answer - turn on forwarding - is wrong here, and the tree says so:

* `IP_FORWARD` is off (`kernel/include/lwipopts.h` does not set it; lwIP's
  default is 0), and `ip4_forward()` refuses to send a packet back out of the
  netif it arrived on (`third_party/lwip/src/core/ipv4/ip4.c`).  The appliance
  has **one** netif (`hfoc_lwip_proto.c` calls `netif_set_default` once), so
  enabling it would forward nothing.
* There is no NAT module in this tree at all: no `IP_NAT`, no `IP_NAPT`, no
  port translation anywhere under `third_party/lwip/`.  NAT is not an option
  to enable; it would be a new module.

So the relay is a **TCP port forwarder** (`kernel/include/hfoc_lwip_forward.h`),
which is what "traffic to the declared server goes through the machine" means
for one declared target and a bounded client list:

1. the forward owner keeps the declaration when the interface is staged with
   no link. It binds the declared port when the interface has a live IPv4
   address. Its listen PCB shares the bounded HFOC lwIP allocator with
   management HTTPS, ACME HTTP-01 and SSH (`MEMP_MEM_MALLOC=1`);
2. an accepted connection is admitted by `forward_config_admit()` before
   anything else happens; a denied source is aborted and published as
   `[PRODUCTION-FORWARD] state=REFUSED reason=SOURCE_NOT_ALLOWED src=...`;
3. an admitted connection makes the one outbound `tcp_connect()` to the
   declared target and port;
4. bytes are relayed between the two PCBs with backpressure. The relay calls
   `tcp_recved()` after copying a complete pbuf chain into its bounded staging
   buffer and drains those bytes as the peer's send buffer allows. Each staging
   buffer holds one receive window plus one segment so a complete chain can
   fit when the buffer is empty;
5. the session ends on either side's clean close after both directions drain,
   on an idle deadline, or on an error, and every ending is published with its
   counters (`accepted`, `refused`, `bytes`, `verdict`).  A backend that never
   answers exhausts lwIP's `TCP_SYNMAXRTX`; the relay tells that stack-originated
   `ERR_ABRT` from its own `tcp_abort()`, fails the accepted client with
   `TARGET_ABORTED`, and frees the one slot so a later client can connect after
   the backend returns.  The idle deadline belongs to the session and still
   fires when only one leg remains.

One session is the budget. Each relay session uses two connection PCBs,
one for the client and one for the target. A second connection
while a session is live is refused with `reason=BUSY` rather than queued
silently.  No TLS is terminated: the bytes are opaque, and the certificate
belongs to the client and the target.

**No IP_FORWARD, no NAT, no UDP** (`LWIP_UDP` is 0), no shell and no
interpreter: the declaration is a fixed grammar, and the relay is a bounded
state machine.

## 5. Evidence

* **Host, deterministic**: `tests/forward-config-host.c` drives the grammar,
  every refusal and the admission decision; `tests/lwip-forward-host.c` drives
  the real relay against real lwIP, blackholes the target SYN until the stack
  exhausts its retries, requires the accepted client to fail and the next
  client to be admitted, and covers the lone-leg idle deadline;
  `tests/production-install-media-host.c` drives the medium directives, the
  installed-entry parse and the record.  `make test-forward-config-host`,
  `make test-lwip-forward-host` and `make test-production-install-media-host`.
* **Media, build-time**: the installed configuration sealed on the forward
  medium is checked byte for byte by `test-production-installer-qemu`'s
  pre-QEMU checks, and the generator's refusals are negative controls in
  `scripts/check.d/59-forward-declaration.sh`.
* **QEMU, end to end**: `scripts/test-qemu-production-forward.sh` (target
  `make test-production-forward-qemu`) installs a forward test medium with an
  SSH key. It boots the installed disk with its link down, restores the link
  after the declaration is read, and checks the relay, SSH, ACME HTTP-01 and
  panel listeners. An HTTPS exchange verifies the target certificate and the
  request and response bytes, and the test requires
  `[PRODUCTION-FORWARD] state=SESSION verdict=OK` with nonzero counters in both
  directions.  A second medium declares a client list the source is not on: the
  connection is refused before the target is dialled (the host target server
  must never accept), and the console publishes
  `[PRODUCTION-FORWARD] state=REFUSED reason=SOURCE_NOT_ALLOWED src=10.0.2.2`
  with no session record.
* **Only on hardware**: the rtl8168 path, a real switch and gateway, a real
  name for the declared server (today only the probe host is resolved), long
  sessions with loss and retransmission, and behavior under a link flap.  That
  is a hardware-acceptance question (AGENTS.md §3a), not CI.

## 6. Open questions this design answers, and the ones it leaves

Answered here: literal IPv4 target (a name would need the one resolver owner,
which today answers exactly one hard-coded probe host); exact client addresses
(the `appliance_management_bind_admit()` precedent); one session (the PCB and
buffer budgets above); no menu entry in this step (the relay must exist first).

Implemented in this item: the relay itself (`hfoc_lwip_forward`) and the
end-to-end QEMU proof above.

Left for later items, in order:

1. a menu path and a persistent store for the rule, which needs a fifth state
   store or a record in an existing one - a change to the installed layout,
   not a free addition;
2. name targets, which needs the resolver to serve more than its one probe
   host and to answer the boot path, not just a wire read;
3. more than one concurrent relay session, which is a budget decision (PCBs,
   buffers) rather than a new mechanism: the module already refuses a second
   connection with `reason=BUSY` instead of queueing it silently.
