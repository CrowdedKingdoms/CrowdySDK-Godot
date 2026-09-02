# CrowdyCPP migration notes

## 0.25.0 rate-limit refusals carry how long to wait

The server sends `extensions.retryAfterMs` on a `RATE_LIMITED` refusal.
`GraphQLErrorDetail` parsed a fixed set of extension keys and kept no raw
`extensions` handle, so that one was unreachable: a caller could tell that it had
been refused for asking too often and could not tell for how long. CrowdyJS
callers read `error.extensions` directly and never had this gap.

### Added

- **`GraphQLErrorDetail::retryAfterMs`** — `std::optional<std::int64_t>`,
  populated from `extensions.retryAfterMs` when it is a JSON number.

It is optional deliberately, and the `retryable` field beside it is the wrong
model to copy. `retryable` defaults to `true` because the server contract says an
absent value means "trying again is reasonable". There is no such default for a
duration: **`0` means retry now and absent means the server named no wait**, and
collapsing the two would turn silence into a busy loop. A value that is present
but not a number reads as absent.

```cpp
if (const auto& wait = error.retryAfterMs) {
  scheduleRetryIn(std::chrono::milliseconds(*wait));
} else {
  scheduleRetryWithLocalBackoff();
}
```

**Read it as a deadline, not as an interval to reuse.** On the invoke rate limit
the server computes what REMAINS of a fixed window rather than a fixed backoff,
so a second refusal inside the same window carries a smaller number, and a value
cached from an earlier refusal will be too long.

### Fixed

Subscriptions and HTTP requests were parsing the GraphQL `errors` array through
two independent copies of the same code, and they had drifted: the subscription
copy never read `blame`, so the same refusal arriving over the websocket lost its
attribution. Both call one internal `readGraphQLError` now.

**If you branched on `blame` and treated its absence on a subscription error as
"unattributed", that branch will now see the real value** — which is the
documented contract, but it is a behaviour change on the websocket path.

No wire change, and no other public type moved.

## 0.24.0 sign and verify with a pre-keyed MAC

Every datagram was signed with OpenSSL's one-shot `HMAC()`, which builds a
context and re-imports the 64-octet token for each call. The token changes only
on refresh, so that setup was repeated needlessly on every send and on every
inbound notification, and it was not a small part of the cost: it was
essentially all of it. Encoding a datagram takes 4 ns; signing it took 1225.

Measured on the builder, with the numbers and the method in
[benchmarks/README.md](benchmarks/README.md):

- encode + sign: 1225 ns -> **319 ns**
- verify a notification: 1276 ns -> **337 ns**
- a 200-entity frame through `Connection::sendActorUpdate`: about
  **1.5x** less CPU

No wire change. Same key, same message, same tag — the golden vectors are
unmoved and a test signs against them through the new path, including from
several threads at once.

### Added

- **`crowdy::core::IMac`** — a MAC bound to one key, reusable across messages,
  computing over parts without concatenating them.
- **`ICrypto::makeHmacSha256(key)`** — returns a pre-keyed `IMac`, or nullptr.

**Implementing it is optional.** The base class returns nullptr and every
caller falls back to `hmacSha256`, so an engine-injected `ICrypto` written
before this release keeps compiling and behaving identically; it simply does
not get the speedup. Engines that bind their own crypto should implement
`makeHmacSha256` to pick it up — it is the single largest CPU win available on
the replication path.

- **`spatialHmac`, `encodeLongSpatial`, `encodeChannelMessage` and
  `verifyLongSpatial`** take an optional trailing `const IMac*`. Existing calls
  compile and behave exactly as before.

### Changed

- `Connection` builds the pre-keyed MAC once per token, rebuilds it on
  rotation, and reads the token id, key and MAC under a single lock where it
  previously took the same mutex twice per send.

### Considered and rejected

Batching writes with `sendmmsg` measured between 1.00x and 1.04x, including
with loopback delivery removed from the measurement, because the kernel does
the same per-datagram work either way and only the syscall boundary is
amortised. No batch API was added. The evidence is in
[benchmarks/README.md](benchmarks/README.md) so the decision can be revisited
on hardware where syscalls cost more.

## 0.23.0 send backpressure is not a socket failure

The UDP send path treated every short write as `Errc::SocketError` and threw
the OS error code away. On a non-blocking socket a full kernel send buffer is
`WSAEWOULDBLOCK`/`EAGAIN` — ordinary backpressure — so a client emitting a large
population in one frame silently lost outbound updates and logged them as
socket faults. It was reported from a 200-entity, 10 Hz Unreal harness: several
hundred sends "failing" inside three milliseconds, which no broken socket does.

Nothing about the wire changed. No datagram, framing, or HMAC behavior is
affected.

### Added

- **`Errc::WouldBlock`** — transient: nothing was sent, the socket is healthy,
  retry shortly. Appended to the enum, so every existing value is unmoved.
- **`ReplicationConfig::socketSendBufferBytes`** (default `1 << 20`) — the
  `SO_SNDBUF` hint, matching the `socketRecvBufferBytes` that already existed.
  The send side previously ran on whatever the OS default happened to be.
- **`Connection::Stats::sendsDeferred`** and **`sendsFailed`** — saturation and
  faults counted apart. `sendsDeferred` rising under load is expected;
  `sendsFailed` rising is not.
- **`UdpSocket::nativeHandle()`** — the underlying descriptor, for diagnostics
  and reading socket options back. The socket still owns it.

### Changed

- **`UdpSocket::send` returns `Errc::WouldBlock` for a full send buffer.**
  Callers treating any non-`Ok` as fatal will now see it. Requeue and retry
  instead of dropping; a permanently full socket needs a bounded queue.
- **An exhaustive `switch` over `Errc` needs a `WouldBlock` case.** `errcName`
  handles it, and unmatched values still fall through to `"Unknown"`.
- **The POSIX send is now non-blocking** (`MSG_DONTWAIT`), matching Winsock and
  the receive path in the same file. Previously a saturated buffer blocked the
  calling thread on POSIX — a frame hitch in a game loop — where it now returns
  `WouldBlock` promptly. Portable callers must handle that return.
- **`UdpSocket::open` takes a fourth argument**, `sendBufferBytes`. Direct
  callers of the socket (rather than `Connection`) need updating; `<= 0` keeps
  the OS default.
- **`LocalActorStore` no longer records a deferred send as an error.**
  `status()` stays out of `Error` for a merely busy socket, and the update
  remains dirty so the next tick retries it.
- **A heartbeat that failed to send is no longer recorded as sent.** It
  previously advanced the heartbeat timestamp regardless of the result, so a
  heartbeat that never left the box banked the whole interval and presence
  could lapse while every counter still read healthy. This applies to any
  failure, not only backpressure.

## 0.20.0 one origin, and endpoints that can move (breaking)

Tracks CrowdyJS 14.1.0. 0.17.0 unified the two APIs behind one server but kept
the two-endpoint shape in the client; this release removes it, and adds the
machinery a client needs when the one endpoint it holds stops answering.

### Removed

- **`ClientConfig::managementUrl`** and **`ClientConfig::managementGraphqlEndpoint`**.
  Pass `httpUrl` (and `wsUrl`). For a per-game client that is the app's OWN
  datacenter endpoint from `mintAppToken`, because that is where its shards live.
- **`CrowdyClient::managementClient()`** and **`CrowdyClient::managementSubscriptions()`**.
  Use `graphqlClient()` and `subscriptions()`.
- **`MarketplaceAPI`** and **`CrowdyStudioAgentAPI`** take one GraphQL client.
  Studio moderation, policy, usage and operator roots are separated from player
  operations by the permissions they require, not by endpoint.
- **`gen::GraphQLEndpoint`** and the per-domain **`endpointFor()`**, along with
  the `schema.management.gql` / `schema.game.gql` snapshots that produced them.

CrowdyJS throws a `TypeError` when a caller passes a removed option. C++ gets
the stronger version for free: a removed struct field is a compile error, so
there is no build in which the old spelling is silently ignored.

### Added

- **`ClientConfig::discoveryUrl`** — the shared origin (multivalue DNS over
  every datacenter's balancer), returned as `discoveryUrl` by `mintAppToken`,
  `refreshAppToken`, `exchangePortalCode` and `gameClientBootstrap`. Set it and
  a client whose endpoint dies can ask where to go next.
- **`ClientConfig::rediscover`** / **`rediscoverAfterFailures`** (default 3) —
  the re-discovery hook, coalesced and never fatal.
- **`discovery()`** — `appDiscovery`, answering where an app is served BEFORE
  login, so a client can start on the shared origin and move before it
  authenticates.
- **Datacenter redirect** — `WRONG_DATACENTER` moves the client (HTTP, WS and
  UDP together) and retries once; `APP_UNAVAILABLE` raises
  `CrowdyAppUnavailableError` and carries no endpoint, on purpose.
- **`SERVER_DRAINING`** — a control-only `udpNotifications` subscription, so a
  native client gets the same advance warning a browser client already had.

### Environment variables

`CROWDY_E2E_API_URL` replaces `CROWDY_E2E_MANAGEMENT_URL` in the e2e harness.
The old name is still read: it names the same origin now, and a harness still
setting only the old name would have made every suite exit 77 — a skip, which
reads as a pass rather than as a failure.

## 0.17.0 unified API (breaking)

Tracks CrowdyJS 13.0.0: the platform merged the Management and Game APIs into
ONE server on the shared database (galaxy then; PostgreSQL + Citus since
2026-08-04). The committed schema snapshots were resynced from the unified SDL,
and the surfaces the platform retired are removed. (The two snapshots this
release kept, `schema.management.gql` and `schema.game.gql`, were themselves
retired in 0.20.0 — see below.)

- **`client.admin().environments()` removed.** Dedicated customer
  environments no longer exist; every app runs on the shared platform, and
  infrastructure provisioning moved to the separate infra-control-plane
  service. The `EnvironmentsAPI` class, its accessor, and the
  `operations/environments/` documents are gone.
- **`client.operator_()` reduced to the platform compute ceilings**
  (`computePlatformCeilings` / `setComputePlatformCeilings`). Environments,
  change orders, secrets, releases, audit, and operator-user listing moved to
  the infra-control-plane service.
- **`client.admin().usage()`**: the per-environment rollups
  (`environmentSummary` / `environmentByApp` / `orgByEnvironment`) are gone;
  `appSummary`, `appGraphqlOperations`, `playerPulse`, and the org/app
  projections stay.
- **`client.admin().billing()`**: the per-environment capacity tier catalogs
  (`buddyBillingTiers` / `graphqlBillingTiers` / `postgresBillingTiers`) are
  gone; wallets, budgets, and transactions stay.
- **Endpoints**: `managementUrl` and `httpUrl` may be the same origin;
  configuring both remains supported here, and the two-token model is
  unchanged. (0.20.0 removed `managementUrl` outright.)

Everything game-client, replication, kit, session, Crowdy Studio, agent, and
player-host is unchanged — the merged schema is a superset for those surfaces.

## 0.16.0 native Crowdy Studio integration

0.16.0 changes the public source and installed-library ABI. CrowdyCPP remains
pre-1.0: rebuild consumers and engine wrappers, and do not load 0.16 libraries
behind binaries compiled against 0.15 headers.

- Prefer `CrowdyClient::createCrowdyStudioIntegration(options)` for an
  engine-owned Studio. The returned non-copyable `CrowdyStudioIntegration`
  owns the project/runtime adapters, controller, editor bridge, concrete Studio
  host, native dispatcher, optional Agent runtime, layout controller, lease
  manager, control gate, and wallet adapter in destruction-safe order.
- Implement `ICrowdyStudioEditorAdapter` over in-memory buffers. Its callbacks
  intentionally provide no `CrowdyClient`, filesystem, shell, DOM, transport,
  or raw GraphQL authority. Existing direct-controller integrations can remain,
  but must own every borrowed provider for the controller's full lifetime.
- Persist pane state through `ICrowdyStudioLayoutStorage`, or retain
  `InMemoryCrowdyStudioLayoutStorage` for session-only state. CrowdyCPP does not
  choose a filesystem, registry, browser local storage, or process-global
  settings service. `crowdy/studio/layout.hpp` remains available in the reduced
  no-exception install.
- Replace opaque diagnostic views with `CrowdyStudioDiagnostic`. Diagnostics
  now carry target-relative path/range, severity, source, message, and optional
  rustc code. The string setter and `*DiagnosticTexts()` accessors remain
  compatibility helpers.
- Wallet state is optional read-only observation. The client integration
  installs `CrowdyStudioPlayerWalletProvider` by default; set
  `observePlayerWallet = false` or inject `walletProvider` to override it.
  Wallet failures clear the snapshot and never block editing, saving, testing,
  or deployment.
- `CrowdyStudioControllerHostAdapter` is the closed 11-tool Studio host.
  Potentially effectful calls validate session, epoch, context, lease,
  cancellation, and approval at the final boundary. Do not replace it with a
  generic host call, raw operation executor, or transport callback.
- Supply `playerHost` to let the integration own one exact
  `AgentControlLeaseManager` shared by native dispatch and
  `NativePlayerControlGate`. Forward existing keyboard, pointer, movement,
  background, death, permission, admission, context, and controlled-entity
  events through the gate. It observes takeover intent; it does not synthesize
  or consume engine input.
- `CrowdyStudioIntegration::poll()` (and compatibility spelling `tick()`) is
  nonblocking. It drains platform/Agent callbacks and native deadlines only.
  Autosave, monitoring HTTP, compile polling/sleep, and scheduled Studio host
  effects run from `runStudioMaintenance()`. Serialize that potentially
  blocking lane with all controller/editor access; never call it concurrently.
- DOM, Monaco, CSS, splitters, browser workers/VFS, renderer chrome, and
  browser input plumbing remain intentional browser exclusions. Engines own
  equivalent presentation and sandbox implementations; 0.16 adds no hidden
  DOM/filesystem authority.

Approved checkpoint restore is not implied by project-save access. It remains
available only when an independently authorized
`ICrowdyStudioSynchronizationProvider` and exact
`ICrowdyStudioApprovalGate` are both injected. The published GraphQL schema
does not expose a generic synchronization or restore root.

## 0.15.1 installed-package compatibility patch

0.15.1 changes no runtime API. It updates external-consumer verification to
request the current `0.15` CMake compatibility line.

## 0.15.0 app-scoped player counts

0.15.0 is additive. Native clients can query
`gameModel().activePlayerCount(appId)` and subscribe with
`activePlayerCountChanged(appId, callbacks)`. Only `FRESH` snapshots are
complete; deduplicate transition events by their decimal-string revision and
requery after reconnects or gaps. These methods require the matching
2026-07-24 Game API generation.

## 0.14.1 Windows replication fix

0.14.1 is a drop-in patch for 0.14.0. Winsock UDP connections are now
configured nonblocking, so `Connection::pump(0)` and manual-pump clients return
immediately when no datagram is available instead of blocking the caller.

## 0.14.0 strict portable-parity release

0.14.0 is not purely additive. It closes every portable gap against the pinned
CrowdyJS 12.0.0 target and adds typed wrappers for current platform SDL
extensions, but it also changes `SaveStateStore::patch` persistence timing and
introduces non-copyable owning runtime types.

- Prefer `client.createCrowdyStudioAgentController(options)` for production
  agent construction. It owns `CrowdyStudioAgentGraphQLTransport`, the
  GraphQL-WS event adapter, and the controller in a safe lifetime order.
- Replace custom local-tool glue with
  `NativeBrowserToolDispatcherAdapter`. It maps controller invocations to
  `NativeToolDispatcherV1`, pumps native deadlines from `controller.poll()`,
  and propagates cancellation, context, timing, output, and typed errors.
- `AgentControlLeaseManager::revoke(reason)` is an exact alias of the
  synchronous intent-first `preempt(reason)` path.
- Use `gameModel().containerChanged(...)` instead of a raw subscription for
  the typed metadata feed. The returned `SubscriptionHandle` cancels on
  destruction.
- `gameModel().ensureContainer(input)` and the `bindingKey` list filter require
  Game API 2026-07-24 or newer.
  `marketplace().appListingVersions(vars)` requires Management API 2026-07-24
  or newer. Both operations are validated against their exact published
  endpoint SDL, not the merged schema snapshot.
- Native CLIENT runtimes can use `playerCompute().artifactBytes(...)` and
  `marketplace().clientArtifactBytes(...)` for validated base64 decoding plus
  artifact hash, decimal-string fuel, nullable contract, and version metadata.
- `SaveStateStore::patch` no longer performs a network write. It updates the
  local cache and marks it dirty; code that relied on the old immediate
  persistence must call `save()` explicitly. The new `set` method has the same
  local-only behavior. `dirty()` and `lastSavedAt()` expose the save lifecycle,
  and persistence failures now occur at `save()`, not at `patch()`.
- Store revision, queue, error, local-actor, and private-avatar observability
  are real bounded snapshots/counters; lifetime totals are not reset by
  clearing retained rings.
- `AppTokenResponse::gameTokenId` and `AuthResponse::gameTokenId` are decimal
  strings. Use the checked `gameTokenIdInt64()` helper only at the native UDP
  wire boundary. Token route URLs and nullable profile fields now use
  `NullableString`, preserving GraphQL null separately from `""`.
- Native replication startup requires complete `Config::token` metadata.
  Prefer `ReplicationClient::connectWithStatus()` so assignment/socket
  failures are not discarded; the old `connect()` convenience remains.
- `ClientConfig::crypto` is the injection point for non-OpenSSL builds.
  Direct `Connection` construction still requires an `ICrypto`; with OpenSSL
  disabled, omitted crypto fails with `CryptoUnavailable` rather than leaving
  an unresolved symbol.
- `CROWDY_NO_EXCEPTIONS=ON` is a reduced package profile. It does not install
  Compute authoring, Crowdy Studio project API/models/controller,
  Agent/controller, player-host, Game Kit, or `ContainerMirror` headers; use
  the normal package for those layers.

### Runtime ownership and copyability

The new runtime objects own callbacks, protocol state, or host authority and
must not be copied:

- `graphql::SubscriptionHandle`, `graphql::GraphQLSubscriptionClient`,
  `player_host::AgentControlLeaseManager`, and
  `agent::NativeToolDispatcherV1` are move-only.
- `agent::CrowdyStudioAgentController` and
  `agent::CrowdyStudioAgentControllerRuntime` are non-copyable and
  non-movable. Keep the factory result in its returned `std::unique_ptr`
  rather than storing either object in a container that relocates values.

No provider API key or provider client was added. Agent providers remain a
server-side platform concern. See
[`docs/compatibility.md`](docs/compatibility.md) for server requirements.

## Crowdy Studio portable parity

The Crowdy Studio API surface adds
`client.crowdyStudio()` for the current CrowdyJS v12 project, personal-library,
and common-file GraphQL surface, and
`crowdy::studio::CrowdyStudioController` for the portable headless state
machine.

Key integration points:

- Keep GraphQL `BigInt` ids, revisions, sizes, and fuel values as decimal
  strings.
- Use `CrowdyStudioPatchField<T>::omitted()`, `::null()`, or `::value(...)`
  when updating nullable metadata. `std::optional` alone cannot preserve this
  wire distinction.
- Call `CrowdyClient::poll()` to deliver `*Async` API callbacks on the
  configured dispatcher/game thread.
- Call `CrowdyStudioController::tick()` from the engine loop for autosave,
  offline retry, and visible monitoring refreshes.
- Consume `CrowdyStudioDiagnostic` from the authoritative/local state vectors.
  Existing string producers can keep using the
  `setLocalDiagnostics(std::vector<std::string>)` overload; existing text-only
  views can use
  `localDiagnosticTexts()` or `authoritativeDiagnosticTexts()`.
- Inject `ICrowdyStudioSynchronizationProvider` only when the host has an
  independently authorized durable atomic-patch/checkpoint service,
  `ICrowdyStudioRuntime` (or
  `CrowdyStudioPlayerComputeRuntime`) for playerCompute execution, and
  `ICrowdyStudioApprovalGate` for exact live/restore approval.
- Optionally inject `ICrowdyStudioWalletProvider` as the last controller
  argument. `CrowdyStudioPlayerWalletProvider` adapts only the viewer-scoped
  `PlayerWalletAPI::balance()` read; failures leave authoring operational.
- Live deployment no longer has an unapproved convenience overload. Pass the
  exact plan from `makeDeploymentPlan()` plus an opaque grant issued and
  checked by the agent layer.

There are no generic checkpoint-list, atomic-patch, or approved-restore roots
in the published GraphQL SDL. Without an injected bridge those calls now fail
with `CrowdyStudioCapabilityUnavailableError` instead of suggesting that
ordinary project-save GraphQL authority is sufficient. Durable
`AgentCheckpoint` event metadata can be mapped with
`crowdyStudioCheckpointEventFromAgentV1()` and scope-checked by
`ingestCheckpointEvent()`; restore still requires a separate exact approval
grant and bridge.

The native phase intentionally excludes DOM, Monaco, CSS, browser Rust workers,
and engine rendering. It also does not add a generic GraphQL executor or any
owner/grid/source authority override. Servers must expose the current
`crowdyStudio*` Game API roots; older deployments reject only these new
operations.
