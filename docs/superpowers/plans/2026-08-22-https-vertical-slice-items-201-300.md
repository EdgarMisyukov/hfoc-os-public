# HFOC OS v1.0.0 release evidence / Свидетельства для выпуска HFOC OS v1.0.0

The following tagged source excerpts support release verification.
Ниже приведены выдержки из тега для проверки выпуска.

- [x] **207. Virtqueue available publication:** Publish descriptor chains to the available ring with ordering barriers and bounded index arithmetic; prove failed preparation does not advance the visible index.
- [x] **208. Virtqueue used consumption:** Consume used entries once with length, head, generation, and range validation; prove malformed or repeated device completion cannot release unrelated ownership.
- [x] **209. Virtio DMA region contract:** Bind queue and payload regions to explicit physical ranges, direction, lifetime, and zeroization policy; prove overflow, alias, forbidden range, and teardown-with-live-owner rejection.
- [x] **210. Virtio foundation QEMU proof:** Exercise discovery, feature rejection, queue publication/completion, injected reset, exact rollback, interrupt liveness, keyboard responsiveness, and clean shutdown on q35.

- [x] **211. Virtio-net feature profile:** Define the exact accepted net header, MAC, status, checksum, segmentation, multiqueue, and control-queue policy; prove unsupported offload or topology cannot activate.
- [x] **212. Virtio-net queue composition:** Create one RX and one TX queue from generic virtqueues with all-or-nothing publication; prove partial queue initialization tears both down.
- [x] **213. Virtio-net RX buffer posting:** Pre-post fixed receive buffers with device-write ownership and minimum frame capacity; prove shortage or queue-full state does not alias or leak a buffer.
- [x] **214. Virtio-net RX completion:** Validate virtio header, used length, Ethernet bounds, generation, and ownership before publishing a received frame; prove short/oversized/stale completions are dropped and reclaimed once.
- [x] **215. Virtio-net TX submission:** Serialize the minimal virtio header and frame chain with device-read ownership; prove oversized frame, queue exhaustion, and notify failure retain caller-visible failure without double ownership.
- [x] **216. Virtio-net TX reclaim:** Consume TX completions, return descriptors and packet ownership exactly once, and record invalid completion evidence; prove wraparound and reset cannot reclaim a new generation.
- [x] **217. Virtio-net interrupt path:** Add bounded ISR acknowledgement and deferred RX/TX work publication without allocation in interrupt context; prove shared/spurious IRQ does not consume nonexistent work.
- [x] **218. Virtio-net link-state binding:** Publish initializing/up/down/failed only after device status and queue readiness agree; prove link loss closes admission before buffer teardown.
- [x] **219. Virtio-net reset and shutdown:** Quiesce notifications, stop admission, drain or invalidate completions, reset the device, and reclaim every queue/buffer; prove repeated stop is idempotent.
- [x] **220. Virtio-net frame QEMU proof:** Receive and transmit exact Ethernet frames through QEMU, exercise RX/TX pressure and reset, assert balanced descriptors/buffers, timer/keyboard progress, and clean shutdown.

- [x] **227. HFOC netif input contract:** Define one function that transfers a validated RX packet to the future lwIP adapter and returns explicit accepted/backpressured/rejected ownership result; prove every result has one owner.
- [x] **228. HFOC netif output contract:** Define one function that accepts an upper-layer frame for virtio-net TX and returns submitted/backpressured/rejected with exact caller/device ownership; prove retry cannot duplicate a frame.
- [x] **229. Packet pressure metrics and audit:** Publish coherent pool free/in-use/high-water, RX drop, TX backpressure, and invalid-frame counters without addresses or payload bytes; prove snapshot generation consistency.
- [x] **230. Packet-boundary QEMU proof:** Exercise real RX/TX ownership, destination filtering, static profile activation, pressure, link reset, balanced pool counts, unrelated subsystem liveness, and clean shutdown.

- [x] **235. lwIP pbuf/packet ownership bridge:** Convert between HFOC packet ownership and permitted pbuf chains without unbounded copy or double free; prove input acceptance, rejection, chained output, and failure rollback.
- [x] **236. lwIP netif lifecycle:** Stage MAC/MTU/static IPv4/netmask/gateway, activate only after driver/link readiness, and remove atomically on stop; prove partial setup leaves no live netif.
- [x] **237. ARP and ICMP integration:** Prove bounded ARP resolution/cache behavior and ICMP echo through QEMU while rejecting malformed input and rate-limiting diagnostic traffic.
- [x] **238. Raw TCP listener contract:** Add one configured local address/port, fixed backlog, callback context generation, and deny-by-default bind/listen; prove second listener and unauthorized port rejection.
- [x] **239. TCP connection lifecycle adapter:** Normalize accept, receive, sent, error, poll, close, and abort callbacks into one bounded HFOC connection state with stale-callback rejection.
- [x] **240. TCP listener QEMU proof:** Connect through QEMU host forwarding, exchange deterministic plaintext test bytes, exercise reset/timeout/backlog pressure, assert pool balance, and leave production image free of the plaintext proof listener.

- [x] **241. Four-slot connection pool:** Add exactly four generation-tagged connection contexts with reserve/publish/close/reuse lifecycle; prove fifth reservation fails without disturbing live slots.
- [x] **242. Connection deadline scheduler:** Attach monotonic accept, progress, idle, and close deadlines to connection generations; prove late events cannot revive or extend a closed generation.
- [x] **243. Listener backlog admission:** Bound pending accepts and reject overflow before allocating TLS/HTTP state; prove recovery after backlog drains.
- [x] **244. RX backpressure propagation:** Pause or reject TCP receive when packet/connection/application capacity is unavailable and resume only after explicit capacity publication; prove no acknowledged-but-unowned bytes.
- [x] **245. TX backpressure propagation:** Retain bounded response progress across short send capacity and sent callbacks; prove retry neither skips nor duplicates bytes.
- [x] **246. Stale callback quarantine:** Validate connection pointer, slot, generation, expected state, and callback kind before mutation; prove delayed callbacks after abort/reuse are evidence-only.
- [x] **247. Malformed TCP isolation matrix:** Inject reset, duplicate completion, invalid callback order, short receive, and remote abort; prove only the affected connection closes.
- [x] **248. Peer close and local abort policy:** Distinguish graceful FIN, protocol close, timeout abort, link reset, and service shutdown with exact cleanup/audit reason.
- [x] **249. Listener drain and restart:** Stop new admission, allow bounded drain, abort expired connections, remove listener/netif, and restart with exact initial pool counts.
- [x] **250. Network resource QEMU stress:** Exercise four live connections, fifth denial, packet pressure, backlog pressure, timeout, reset, restart, timer/console liveness, and zero ownership leaks.

- [x] **251. Virtio-rng feature profile:** Define supported device identity, feature policy, queue count, and device-write ownership; prove unsupported capability or topology cannot activate.
- [x] **252. Virtio-rng queue composition:** Initialize one bounded request queue with all-or-nothing device publication; prove failed setup resets and reclaims every descriptor/buffer.
- [x] **253. Bounded random request API:** Add explicit destination length, maximum request, caller context, generation, and completion result; prove zero, oversized, overlapping, and interrupt-context requests fail closed.
- [x] **254. Virtio-rng completion path:** Validate completion head, length, generation, and ownership before returning bytes; prove short, oversized, duplicate, and stale completions zeroize and fail.
- [x] **255. Entropy continuous health checks:** Reject repeated blocks, impossible lengths, stalled progress, and device reset generation while avoiding claims of statistical certification.
- [x] **256. Entropy accumulator lifecycle:** Mix bounded accepted device bytes into a fixed accumulator, track credited source state, and zeroize temporary material on every path.
- [x] **257. TLS seed/fill contract:** Expose only bounded DRBG-seed and additional-entropy fill operations with explicit unavailable/degraded/ready states; prove callers cannot read the accumulator.
- [x] **258. Entropy diagnostics privacy:** Publish counts, health state, failure reason, and generation without random bytes, addresses, or seed material; prove serial/audit/support negative fixtures.
- [x] **259. Entropy failure and restart:** Handle missing device, stall, repeated output, reset, and shutdown by closing readiness, zeroizing state, and requiring a new healthy generation.
- [x] **260. Entropy QEMU proof:** Demonstrate healthy seeding, missing-device denial, injected rejection, reset/recovery, secret-negative scans, other subsystem liveness, and clean shutdown.

- [x] **261. mbedTLS source and license lock:** Select one supported upstream release with required server TLS 1.3 features, vendor the exact subset, record URL/version/SHA-256/license inventory, and prove source drift or extra modules fail the build.
- [x] **262. Minimal TLS 1.3 configuration:** Enable server TLS 1.3, the frozen cipher/hash/group/signature/X.509/DRBG set only; disable legacy TLS, client mode, files, sockets, threads, debug secret export, and unused algorithms; prove forbidden symbols/config flags fail.
- [x] **263. mbedTLS allocator boundary:** Route every allowed allocation through fixed per-connection/shared HFOC pools with zeroization classes; prove exhaustion closes one handshake and teardown restores exact counts.
- [x] **264. mbedTLS entropy/DRBG adapter:** Bind seeding and reseed to the Wave Z contract with purpose separation and generation invalidation; prove unavailable/rejected entropy prevents TLS context publication.
- [x] **265. Certificate/key manifest parser:** Validate bounded DER test certificate/key entries, purpose, hostname `hfoc.test`, algorithm compatibility, digest, and key/certificate match before publication.
- [x] **266. Test-secret build separation:** Include certificate/key only in test image, add production/release manifest and ELF/image scans rejecting their digest/marker, and prove negative fixtures.
- [x] **267. TLS BIO transport adapter:** Map nonblocking mbedTLS send/receive outcomes to Wave Y backpressure without POSIX errno or sockets; prove WANT_READ/WANT_WRITE and short progress preserve exact byte ownership.
- [x] **268. TLS handshake state machine:** Compose context reserve, configuration, handshake progress, deadline, success publication, and generation-tagged failure cleanup; prove malformed ClientHello affects one slot only.
- [x] **269. TLS close and error normalization:** Implement close-notify where possible, bounded abort otherwise, stable public failure categories, zeroization, and idempotent context release.
- [x] **270. TLS 1.3 QEMU proof:** Complete a standard client handshake through host forwarding, verify protocol/certificate/hostname policy, exercise malformed handshake and entropy denial, assert pool balance and no secret output.

- [x] **271. HTTP parser state descriptor:** Define EMPTY, REQUEST_LINE, HEADERS, COMPLETE, REJECTED states, fixed offsets/counts, and one-way publication; prove invalid predecessor and stale generation rejection.
- [x] **272. Incremental request-line parser:** Parse method, request target, and HTTP version across arbitrary byte splits with the 1024-byte limit; prove control characters, invalid spacing, invalid target, and overflow rejection.
- [x] **273. Incremental header parser:** Parse bounded field names/values and terminator across byte splits while enforcing the 4096-byte aggregate limit; prove folding, invalid characters, and incomplete termination rejection.
- [x] **274. Framing and body rejection:** Detect Content-Length, Transfer-Encoding, Expect, Upgrade, and conflicting/duplicate framing before dispatch; prove every body-bearing or upgraded request is denied.
- [x] **275. Method/version policy:** Accept only GET and HEAD over HTTP/1.1 with required valid Host; map unsupported method/version/host state to deterministic rejection.
- [x] **276. Fixed route table:** Define exact `/` and `/health` route identities with no wildcard, traversal, query-driven behavior, allocation, or dynamic registration.
- [x] **277. Deterministic response encoder:** Encode bounded status line, Content-Type, Content-Length, Connection: close, security headers, CRLF framing, and fixed body without attacker-controlled echo.
- [x] **278. HEAD response semantics:** Produce the same selected status and Content-Length as GET while emitting zero body bytes; prove repeated/short output cursor behavior.
- [x] **279. HTTP error matrix:** Map malformed syntax to 400, unsupported method to 405, body/framing to 413, request-line overflow to 414, header overflow to 431, and unknown route to 404.
- [x] **280. HTTP parser/encoder host acceptance:** Run split-at-every-byte fixtures, malformed corpus, exact success/error bytes, allocation/ownership counters, and bounded fuzz iterations under sanitizers where supported.

- [x] **281. HTTPS service descriptor:** Add fixed service identity, dependencies on network/entropy/certificate readiness, restart budget, resource envelope, and readiness deadline; prove missing dependency prevents start.
- [x] **282. HTTPS admission transaction**
- [x] **283. HTTPS connection composition:** Reserve one Wave Y slot, TLS context, HTTP parser, and response cursor as one transaction; prove failure at each stage releases earlier resources in reverse order.
- [x] **284. TLS-to-HTTP data flow:** Feed authenticated plaintext incrementally to the parser and response bytes back to TLS with short-progress/backpressure handling; prove no plaintext crosses connection generations.
- [x] **285. Root and health handlers:** Serve deterministic fixed root HTML and coherent bounded `/health` JSON without secret, address, payload, or mutable-owner exposure.
- [x] **286. HTTPS health/audit/metrics:** Publish listener state, connection counts, failure categories, high-water values, and correlation IDs while redacting certificate/key/handshake/plaintext material.
- [x] **287. Fifth-connection denial:** Hold four valid connection contexts, reject the fifth before TLS allocation, then accept a new connection after one exact cleanup.
- [x] **288. Peer-failure throttle:** Add bounded per-peer failure buckets with expiry and no permanent blacklist; prove one abusive peer cannot block unrelated peers or local management.
- [x] **289. Graceful stop and exact restart:** Close admission, drain within deadline, abort remainder, zeroize TLS state, release HTTP/packet contexts, and restart with initial counters.
- [x] **290. HTTPS service QEMU proof:** Serve all fixed routes through TLS, exercise malformed request, fifth denial, peer throttle, shutdown/restart, health/audit evidence, console/timer liveness, and clean shutdown.

- [x] **291. Standard HTTPS client harness:** Add QEMU user-network host forwarding and `curl --tlsv1.3 --cacert`/OpenSSL harness for `hfoc.test:18443`; prove hostname and untrusted-CA failures.
- [x] **292. Exact successful-response matrix:** Prove exact status, headers, Content-Length, body, HEAD zero-body behavior, and health schema for every supported route.
- [x] **293. Encrypted malformed-request matrix:** Send split, control-character, invalid-host, unsupported-version/method, framing, upgrade, and unknown-route requests over TLS; prove exact bounded outcomes.
- [x] **294. Encrypted size-boundary matrix:** Prove 1024-byte request-line and 4096-byte aggregate-header boundaries at below/exact/above cases without parser desynchronization.
- [x] **295. Concurrent exhaustion matrix:** Hold four handshakes/connections, prove fifth denial before TLS allocation, apply packet/TX pressure, release one, and prove subsequent success with balanced counts.
- [x] **296. Entropy fail-closed matrix:** Boot without virtio-rng and with test-only rejection/stall/reset faults; prove listener closed, local operation healthy, evidence bounded, and recovery requires a healthy generation.
- [x] **297. Certificate/key fail-closed matrix:** Exercise absent, malformed, digest-mismatched, wrong-purpose, hostname-mismatched, algorithm-incompatible, and key-mismatch fixtures; prove no listener publication.
- [x] **298. Production artifact secret gate:** Scan kernel ELF, normal ISO, release manifest, serial output, audit snapshot, metrics, and support bundle for test key/certificate digests, TLS secrets, and proof controls; prove negative fixtures trip the gate.
- [x] **299. HTTPS restart/soak rehearsal:** Run repeated connect/request/close, malformed traffic, link reset, service restart, and final success; assert exact initial/final pool counts, no panic, no stale callback, and responsive console/timer.
- [x] **300. HTTPS vertical-slice acceptance gate:** Add one aggregate target and evidence matrix covering items 291–299, attach artifacts to the PR, update architecture/roadmap/readme, and publish explicit accepted/rejected result without claiming public production readiness.
