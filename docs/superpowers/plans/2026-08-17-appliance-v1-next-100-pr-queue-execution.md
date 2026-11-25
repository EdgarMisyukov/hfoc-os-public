# HFOC OS v1.0.0 release evidence / Свидетельства для выпуска HFOC OS v1.0.0

The following tagged source excerpts support release verification.
Ниже приведены выдержки из тега для проверки выпуска.

- [x] **106. Backup manifest inventory:** Export deterministic metadata inventory for configuration, audit checkpoint, schema, and generation; prove repeated export of equal state is byte-identical.
- [x] **107. Bounded backup producer:** Add back-pressure-safe bounded export iterator; prove exhaustion, short output buffer, and close leave source state unchanged.
- [x] **108. Restore dry-run planner:** Parse and validate a backup stream into an unpublished restore plan; prove no device, journal, or live service mutation during dry run.
- [x] **109. Staged restore activation:** Apply a validated restore plan to inactive staged state and publish atomically; prove injected write fault preserves active state.
- [x] **110. Backup/restore QEMU acceptance:** Add QEMU proof for export, destructive state change, import, restored values/audit continuity, and clean reboot.

- [x] **143. Signed metadata envelope:** Add bounded signed-envelope framing around image/update/backup metadata; prove altered payload, length, version, and signer identity fail verification.
- [x] **144. Key-use separation:** Enforce distinct accepted-purpose fields for image, update, backup, and integration metadata; prove cross-purpose replay is rejected.
- [x] **145. Trust-anchor rotation staging:** Add inactive anchor staging, validation, and activation; prove interrupted rotation preserves active verification behavior.
- [x] **146. Trust-anchor revocation:** Add explicit revoked state with audit record and no rollback to revoked anchor; prove revoked signer cannot validate new input.
- [x] **147. Image-policy binding:** Bind verified image manifest to active policy generation; prove a valid manifest under an inactive policy cannot boot/activate.
- [x] **148. Measured boot evidence record:** Persist bounded measurement/policy/image-generation evidence into audit state; prove reboot continuity and digest mismatch evidence.
- [x] **149. Recovery trust mode:** Restrict recovery mode to recovery-purpose verified metadata and read-only evidence; prove normal control/update actions are denied.
- [x] **150. Trust-policy QEMU proof:** Prove valid verification, tamper rejection, wrong-purpose rejection, rotation, revocation, recovery restrictions, audit continuity, and clean reboot.

- [x] **151. Update compatibility manifest:** Define bounded source/target schema, ABI, service, and policy compatibility fields; prove incompatible candidate is rejected before staging.
- [x] **152. Candidate-slot inventory:** Add immutable candidate slot/generation/digest inventory; prove duplicate or stale generation cannot replace active inventory.
- [x] **153. Candidate preflight:** Validate bundle, trust, compatibility, size, and required inactive slot; prove every preflight failure leaves slots unchanged.
- [x] **154. Staged write verification:** Write candidate only to inactive slot and verify exact readback digest; prove write/readback fault never marks candidate ready.
- [x] **155. Activation intent record:** Atomically record pending activation target and rollback source; prove torn intent resolves to the previous known-good slot.
- [x] **156. First-boot health contract:** Define bounded post-activation health milestones from boot, storage, service, and management subsystems; prove absent milestone prevents acceptance.
- [x] **157. Automatic rollback selection:** Select last-known-good slot after failed candidate health; prove candidate remains non-active and evidence records reason.
- [x] **158. Update acceptance commit:** Mark candidate known-good only after complete health contract; prove restart after acceptance selects it deterministically.
- [x] **159. Update retention policy:** Reclaim only unreferenced inactive candidates under bounded policy; prove active/rollback generations are never reclaimed.
- [x] **160. Update lifecycle QEMU proof:** Prove compatible update, activation, healthy acceptance, incompatible rejection, failed candidate rollback, audit continuity, and clean reboot.

- [x] **161. Health-check descriptor:** Define bounded health check IDs, severity, cadence, and result states; prove invalid descriptors publish nothing.
- [x] **162. Storage health check:** Add journal/checkpoint/slot consistency health result; prove injected integrity fault is visible without a panic.
- [x] **163. Service health aggregation:** Aggregate service readiness, restart exhaustion, and dependency state into deterministic appliance health; prove severity ordering.
- [x] **164. Network health aggregation:** Add link/configuration/transport admission health state; prove network failure does not invalidate local control.
- [x] **165. Audit-ring pressure health:** Expose bounded audit/event pressure and overwrite/loss policy evidence; prove no silent event loss.
- [x] **166. Versioned metrics snapshot:** Publish fixed-schema counters and gauges for health/state/service/update paths; prove readers receive a coherent generation.
- [x] **167. Support-bundle manifest:** Define deterministic bundle inventory containing permitted snapshots, audit slice, configuration metadata, and versions; prove secret-bearing fields are excluded.
- [x] **168. Support-bundle exporter:** Add bounded streaming exporter with digest and back pressure; prove partial export has no side effect on appliance state.
- [x] **169. Diagnostic privacy policy:** Enforce redaction of credentials, verifier material, raw capability values, and private address fields where required; prove negative fixtures.
- [x] **170. Supportability QEMU proof:** Prove degraded health, status exposure, support-bundle export, redaction, update/service evidence, and normal recovery to healthy state.

- [x] **171. Integration boundary document:** Publish exact ownership split: appliance state, accounting state, audit hand-off, transport, versioning, and unsupported operations.
- [x] **172. Integration principal role:** Add dedicated external-integration principal/capability scope; prove it cannot access local-admin-only control.
- [x] **173. Integration session handshake:** Define bounded version negotiation, peer identity, nonce, and explicit rejection codes; prove mismatch and replay rejection.
- [x] **174. Accounting hand-off envelope v1:** Define deterministic envelope ID, schema version, correlation ID, payload length, digest, and purpose; prove malformed envelope rejection.
- [x] **175. Idempotency ledger:** Add bounded accepted-envelope identity ledger with atomic commit; prove replay returns same result without duplicate side effect.
- [x] **176. Outbound event cursor:** Expose ordered, acknowledged appliance audit/event export cursor; prove bounded retention and no mixed-generation cursor snapshot.
- [x] **177. Inbound configuration contract:** Define narrow externally requested appliance configuration intents; prove every intent is locally authorized, validated, staged, and audited.
- [x] **178. Integration failure isolation:** Ensure a malformed, unavailable, or slow external peer cannot block local management, service supervision, or state recovery.
- [x] **179. Integration compatibility policy:** Add explicit supported protocol/schema version range and deprecation state; prove unsupported peer never obtains a live session.
- [x] **180. Accounting-boundary QEMU proof:** Prove handshake, version rejection, idempotent envelope, outbound cursor, denied overreach, audit correlation, peer failure isolation, and clean shutdown.

- [x] **181. Fault-injection catalogue:** Define named, bounded fault points and permitted test-only activation contract; prove no test hooks leak into normal production image.
- [x] **182. Storage interruption drill:** Prove interrupted journal compaction, backup export, and staged restore converge to valid prior or committed state.
- [x] **183. Service cascade drill:** Prove dependency failure, restart exhaustion, controlled stop, and later recovery leave no orphan process or endpoint.
- [x] **184. Local-control outage drill:** Prove session expiry, authentication throttle, and malformed command do not prevent future authorized local recovery.
- [x] **185. Network partition drill:** Prove link loss/peer silence preserves local operation and records bounded health/audit evidence.
- [x] **186. Trust failure drill:** Prove revoked signer, tampered metadata, and recovery-purpose mismatch lead to denial with preserved known-good state.
- [x] **187. Update failure drill:** Prove staged-write interruption and first-boot health failure select known-good image deterministically.
- [x] **188. Integration replay/outage drill:** Prove duplicate, malformed, incompatible, and unavailable accounting peer states are idempotent and isolated.
- [x] **189. Power-cycle continuity drill:** Prove audit, state generation, health, slot choice, service state, and recovery reason are coherent over QEMU reset.
- [x] **190. Resilience matrix QEMU proof:** Run a deterministic multi-fault matrix with artifact capture and assert no kernel panic, ownership leak, or invalid publication.

- [x] **191. Release profile descriptor:** Define immutable release ID, supported architecture, image generation, required policy, schema, and integration contract versions.
- [x] **192. Reproducible build input manifest:** Record exact source revision, toolchain identifiers, build flags, and generated-input digests; prove missing/mismatched input fails release build.
- [x] **193. Reproducible image comparison:** Build the appliance image twice from identical inputs and compare approved deterministic artifacts/digests.
- [x] **194. Release artifact manifest:** Generate deterministic manifest for image, recovery image, update bundle, support schema, and protocol versions; prove digest verification.
- [x] **195. Release upgrade matrix:** Define and test allowed predecessor-to-target update/rollback combinations; prove unsupported path is rejected.
- [x] **196. Release backup/restore matrix:** Define and test backup compatibility across supported release schema ranges; prove no-change rollback for incompatible restore.
- [x] **197. Release security baseline:** Aggregate W^X, syscall/user-pointer, local-control, trust, network-admission, and integration-denial checks into one non-bypassable target.
- [x] **198. Release operational baseline:** Aggregate boot, state, services, health, support bundle, update, recovery, and QEMU liveness checks into one target.
- [x] **199. Appliance v1 disaster-recovery rehearsal:** Run documented clean-image → restore → service recovery → integration handshake → support-bundle evidence scenario in QEMU.
- [x] **200. Appliance v1 acceptance gate:** Add final release target and concise evidence matrix; prove all required matrices pass and publish explicit acceptance/rejection result.
