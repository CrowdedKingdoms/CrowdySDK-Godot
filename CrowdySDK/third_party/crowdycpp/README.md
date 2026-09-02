# CrowdyCPP

The official portable C++ SDK for **Crowded Kingdoms**. CrowdyCPP gives native
games typed clients for auth, GraphQL HTTP and WebSocket APIs, and — unlike
the browser-first [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS) SDK —
a **native UDP replication client** that speaks the
[Replication API wire protocol](https://docs.crowdedkingdoms.com/replication-api/intro)
directly to the replication servers, with zero-copy binary framing and no
GraphQL proxy in the hot path.

CrowdyCPP is designed from first principles to be:

- **Portable.** Standard C++20, CMake, Linux/Windows/macOS. No engine types,
  no framework assumptions. Every platform dependency (HTTP, crypto, clock,
  logging, allocation) sits behind a small interface you can replace.
- **High performance.** Steady-state zero heap allocation on the replication
  path, zero-copy datagram parsing, lock-free queues between the network and
  game threads, batched socket I/O, and no exceptions on hot paths.
- **Embeddable.** Usable directly by a native game, and equally designed to be
  wrapped by engine-specific SDKs — see
  [Wrapping CrowdyCPP in engines](#wrapping-crowdycpp-in-engines) for the
  Unreal Engine plan.

CrowdyCPP mirrors the [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS)
API surface (same domains, same two-token model, same error codes) so the
[platform docs](https://docs.crowdedkingdoms.com) and examples translate
directly. Where CrowdyJS routes realtime traffic through the Game API's
GraphQL UDP proxy (a browser constraint), CrowdyCPP opens a raw UDP socket and
implements the
[wire formats](https://docs.crowdedkingdoms.com/replication-api/wire-formats)
and [HMAC scheme](https://docs.crowdedkingdoms.com/replication-api/hmac)
natively.

**v0.26.0 drops the dev auth bypass:** `devLogin` and `devLoginAsync` are gone,
along with `requestLoginLink`'s `devToken` selection, because ck-api deleted all
three — the mirror cannot offer a field the server does not have. Sign in with
`login(email, password)` or create an account with `registerUser`, which is
`register` renamed because C++ cannot name a method after a keyword. Parity is
pinned to CrowdyJS 15.0.0. Breaking for any caller that used the bypass; there
was no other way to remove it.

**v0.24.0 signs with a pre-keyed MAC:** the replication path signed every
datagram with a one-shot HMAC that re-imported the 64-octet token each time,
which was essentially the entire per-datagram CPU cost — encoding is 4 ns,
signing was 1225. `ICrypto::makeHmacSha256()` now supplies a reusable keyed
`IMac`, cutting sign to 319 ns and notification verify to 337 ns, about 1.5x
less CPU for a 200-entity frame. No wire change. Implementing the new method is
optional: providers that do not fall back to the old path unchanged. Numbers
and method in [benchmarks/README.md](benchmarks/README.md).

**v0.23.0 tells backpressure apart from a broken socket:** a full kernel send
buffer now returns the new transient `Errc::WouldBlock` instead of
`Errc::SocketError`, so a client outrunning its send buffer under burst load is
no longer indistinguishable from a genuine socket fault. The POSIX send is
non-blocking to match Winsock and the receive path, `SO_SNDBUF` is configurable
through `ReplicationConfig::socketSendBufferBytes`, and `Connection::Stats`
separates `sendsDeferred` from `sendsFailed`. See [MIGRATION.md](MIGRATION.md) —
callers that switch exhaustively over `Errc` need a new case.

**v0.22.0 builds on Windows again:** line endings are normalised through
`.gitattributes` so a Windows checkout is byte-reproducible, the generated
headers are regenerated from an LF checkout, and the CMake build compiles and
tests under MSVC. Nothing about the API changed — this is the 0.21.0 surface,
buildable on the platform where 0.21.0 was not.

Released from the `dev` branch as `dev/v0.22.0`. Since 2026-08-12 this repo
carries `dev`, `test` and `prod`, and a release tag names the branch it was cut
from; a bare `vX.Y.Z` tag is the retired convention. Note that **v0.20.0 and
v0.21.0 were never tagged on the remote** despite a commit saying they were —
the highest published tag is `v0.19.0`.

**v0.21.0 invoke fault attribution:** a failed `gameModelInvoke` now reports
whose fault it was and whether repeating it can work, on both channels the
server uses: `fault { code blame retryable }` in band on a rejection, and
`extensions.blame` on the thrown overload refusal. The parity pin is unmoved at
CrowdyJS 14.1.0 `90f4b7bb2562d007aa62d01d4b21abdb76923e9b`, and the release gate
reported zero portable gaps, unclassified differences, and stale
classifications against it.

The schema sync this needed advanced the published SDL by several server
releases, which added ten root fields. They are wrapped here rather than
waived, so the SDK still reaches every field its schema declares. This release
also builds and tests with MSVC again: the embedded agent fixture had outgrown
the 65535-byte limit on a single string literal, and is now emitted as adjacent
literals that concatenate to the identical constant.

**v0.20.0 one origin, movable endpoints:** the release gate reported zero
portable gaps, unclassified differences, and stale classifications against
CrowdyJS 14.1.0 at
`90f4b7bb2562d007aa62d01d4b21abdb76923e9b`. This is not a claim that the
implementations are identical: the generated matrix retains reviewed native
equivalents and browser-only exclusions. Production Agentic Studio uses typed
GraphQL-WS durable events, reconnect gap-fill, and lifetime-safe controller
construction; the native player-host dispatcher has an exact
`IAgentBrowserToolDispatcher` bridge. Native Studio adds the complete
editor/layout/host/control assembly through
`createCrowdyStudioIntegration()`, with nonblocking `poll()` and an explicit
potentially-blocking maintenance lane. See the
[compatibility matrix](docs/compatibility.md) and
[0.16 migration notes](MIGRATION.md).

Older minors (0.7–0.19) are in [MIGRATION.md](MIGRATION.md). The live data
plane is **PostgreSQL + Citus** via `cks-game-api`; `cks-management-api` is
retired. There is one GraphQL origin since 0.20.0.

## Layout

```text
include/crowdy/          public headers
  core/                  bytes, endian, result, clock, logging, allocator interfaces
  wire/                  zero-copy wire codec + HMAC framing (header-only)
  graphql/               GraphQL HTTP + WebSocket transports, JSON, errors
  replication/           native UDP replication client
  session/               world session layer (actors, chunks, inboxes, host)
  kit/                   Game Kit (blueprints + runtime helpers)
  player_host/           typed Play capabilities, observations, leases, adapters
  agent/                 native local-tool dispatcher (no model/server fallback)
src/                     implementation
include/crowdy/generated/  committed codegen output (operations + enums)
operations/              GraphQL operation documents (codegen input)
schema.gql               the published API SDL snapshot (codegen input)
scripts/                 schema sync + codegen (Node, maintainers only)
tests/                   unit tests (ctest) + env-gated e2e tests
benchmarks/              micro + end-to-end benchmarks
```

## Build

```bash
sudo apt-get install build-essential cmake libcurl4-openssl-dev libssl-dev   # Ubuntu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build        # unit tests: no network required
```

A clean external clone builds offline: the schema snapshot (`schema.gql`) and
the generated code (`include/crowdy/generated/`) are committed. Only maintainers run the
schema sync / codegen scripts (see [Schema refresh](#schema-refresh-and-codegen)).

Dependencies (all replaceable through interfaces):

| Dependency | Used by | Replaceable via |
|---|---|---|
| libcurl | default HTTP transport | `crowdy::graphql::IHttpTransport` |
| libcurl 8.13+ WebSocket APIs | optional default GraphQL subscriptions | `crowdy::graphql::IWebSocketTransport` |
| OpenSSL (libcrypto) | HMAC-SHA256 | `crowdy::core::ICrypto` |
| yyjson (vendored) | JSON parse/serialize | internal only, not on the UDP path |

The wire and replication layers depend only on BSD/Winsock sockets and the
`ICrypto` interface — no libcurl, no JSON.

The default HTTP transport works with older supported libcurl releases. The
optional default WebSocket backend requires libcurl 8.13+ because older
releases do not provide the fragmented-message semantics it needs. CMake
feature-detects that backend; if support is absent (or
`CROWDY_WITH_CURL_WEBSOCKETS=OFF`), the SDK builds with a clear no-default
fallback and `makeCurlWebSocketTransport()` returns null; injected engine
transports continue to work on Linux, macOS, and Windows. The factory also
checks that the linked libcurl is 8.13+ and actually advertises both `ws` and
`wss`, since some distributions expose the APIs while compiling those
protocols out.

`CROWDY_NO_EXCEPTIONS=ON` creates a reduced strict `-fno-exceptions` package:
core GraphQL outcomes, auth/portal, replication, non-authoring domains, and
session stores remain available. The independent Crowdy Studio pane-layout
header remains available. Compute authoring, Crowdy Studio project
models/API/controller, Agent/controller, player-host, Game Kit, and
`ContainerMirror` headers are not installed because their validation
contracts throw. Blocking GraphQL failures return an invalid `Json`; use
`*Async` callbacks for typed details. Injected transports must not throw
across the SDK boundary.

## Quick start

```cpp
#include <crowdy/crowdy.hpp>

int main() {
  const std::string appId = "42";  // GraphQL BigInt stays a decimal string.

  // 1) Identity client on the shared entry origin — passwordless sign-in.
  crowdy::CrowdyClient identity(crowdy::ClientConfig{
      .httpUrl = "https://ck.example.com",
  });
  auto login = identity.auth().devLogin("player@example.com");  // dev/test only

  // 2) Mint an app-scoped token and build the per-game client.
  auto minted = identity.portal().mintAppToken(appId);
  // httpUrl is the app's OWN datacenter (that is where its shards are);
  // discoveryUrl is the shared origin to fall back to if it stops answering.
  crowdy::CrowdyClient game(crowdy::ClientConfig{
      .httpUrl = minted.gameApiUrl,
      .discoveryUrl = minted.discoveryUrl,
  });
  game.setToken(minted.token);

  // 3) Connect the native replication client (assigns a server, installs the
  //    UDP session, waits for session-ready).
  const auto appIdInt = crowdy::graphql::parseBigInt(minted.appId);
  const auto tokenIdInt = minted.gameTokenIdInt64();
  if (!appIdInt || !tokenIdInt || !minted.gameApiUrl.has_value()) return 2;
  crowdy::replication::Config repl{
      .appId = *appIdInt,
      .token = {.token = minted.token,
                .gameTokenId = *tokenIdInt,
                .expiresAtEpochMs = 0},  // Parse minted.expiresAt in production.
  };
  // 4) Install receive handlers and join the world.
  crowdy::replication::Handlers handlers;
  handlers.actorUpdate = [](const crowdy::replication::SpatialNotification& u) {
    // u.uuid, u.chunk, u.payload (span over the datagram — copy if you keep it)
  };
  auto connected = game.replication().connectWithStatus(repl, handlers);
  if (!connected.ok()) return 3;
  auto conn = connected.connection;
  conn->sendActorUpdate({.chunk = {0, 0, 0},
                         .uuid = myActorUuid,
                         .payload = poseBytes,
                         .distance = 8,
                         .decay = crowdy::wire::DecayRate::Exponential});

  // 5) Pump notifications from your game loop (or use the owned-thread mode).
  while (running) {
    conn->poll();
    if (conn->state() == crowdy::replication::ConnState::Failed ||
        conn->state() == crowdy::replication::ConnState::Closed) {
      return 4;
    }
  }
}
```

The full lifecycle (tokens, server assignment, reconnect commands, token
refresh) is documented in
[Authenticate and assign](https://docs.crowdedkingdoms.com/replication-api/authenticate-and-assign)
and handled by `crowdy::replication` automatically.

### Generic GraphQL subscriptions

`GraphQLSubscriptionClient` implements `graphql-transport-ws` for portable
push APIs. It normalizes an HTTP(S) API URL to one WS(S) `/graphql` endpoint,
authenticates with the shared token in `connection_init`, bounds UTF-8 JSON
messages, and replays active operations after capped jittered reconnects.

```cpp
crowdy::graphql::GraphQLSubscriptionCallbacks callbacks;
callbacks.onNext = [](crowdy::graphql::GraphQLSubscriptionOutcome next) {
  if (next.ok()) {
    // Read next.data on the game thread.
  }
};
callbacks.onError = [](crowdy::graphql::GraphQLSubscriptionError error) {
  // Branch on error.kind / error.code; terminal auth, app-scope, and stale
  // client-epoch failures are not reconnected.
};
callbacks.onReconnect = [](crowdy::graphql::GraphQLReconnectInfo replay) {
  // Optionally start a durable gap-fill query before replayed events arrive.
};

auto subscription = game.subscriptions().subscribe(
    "subscription Changes($appId: BigInt!) {"
    " gameModelContainerChanged(appId: $appId) { containerId changedKeys }"
    "}",
    crowdy::graphql::JVal::object({{"appId", appId}}), "Changes",
    std::move(callbacks));

while (running) game.poll();  // all callbacks are delivered here
// subscription.cancel() is explicit; destruction also cancels.
```

Use the typed wrappers where available:
`game.gameModel().containerChanged(...)` maps container metadata pushes, and
`game.gameModel().activePlayerCount(appId)` returns the app-scoped session
gauge with its completeness status and decimal revision.
`activePlayerCountChanged(...)` is a best-effort transition feed with no
bootstrap event: establish the feed, query the snapshot, deduplicate by
revision, and requery after a reconnect or revision gap. Both calls require an
app-scoped token for the same app. The gauge counts active gameplay sessions,
not distinct users or actors; an abandoned session can remain visible for
roughly 120 seconds while inactivity is recognized.
`client.createCrowdyStudioAgentController(...)` owns the durable Agentic Studio
event adapter and replay/gap-fill lifecycle. The generic client remains for
application-specific subscriptions. `crowdy::session::ContainerMirror` does
not subscribe automatically: it remains a pull cache. Call `refresh()` from a
typed container-change callback, or continue feeding channel notifications to
`notifyChannelPing()`, when that is the application's notify-to-pull contract.
See
[GraphQL WebSocket examples](docs/graphql-websocket.md).

## Sub-clients at a glance

CrowdyCPP mirrors CrowdyJS's domain layout. Game-client surface (end-user,
app-scoped token):

| Sub-client | What it does |
|---|---|
| `client.auth()` | Passwordless sign-in (magic link, social/OIDC, dev bypass), log out, linked identities. |
| `client.users()` | `me`, `updateGamertag`, profile reads. |
| `client.session()` | Token store, restore, set/get token. |
| `client.portal()` | `mintAppToken`, `refresh`, PKCE portal entry for cross-origin handoffs. |
| `client.serverStatus()` | `gameClientBootstrap(appId)` — version info, spatial limits. |
| `client.chunks()`, `client.voxels()`, `client.actors()`, `client.avatars()`, `client.state()` | World data reads + writes. |
| `client.host()` | Host election reads + actor liveness heartbeat. |
| `client.teleport()` | Teleport requests. |
| `client.channels()`, `client.teams()` | Messaging channels and app-scoped teams. |
| `client.gameModel()` | Abstract game model: containers, properties, functions, sessions, automations, one-shot timers. |
| `client.compute()` | **Compute Modules** — server-side Rust/WASM logic: author + deploy source (`upsertModule`, `deploySource`), compile polling (`moduleVersions`), triggers + policy, synchronous `invoke`, monitoring (`moduleRuns`, `moduleStats`, `moduleLogs`, `appDiagnostics`). Server-only execution; see the [Compute Modules docs](https://docs.crowdedkingdoms.com/game-api/compute-modules). |
| `client.playerCompute()` | Player-authored SERVER/CLIENT Rust/WASM bound to player-owned grids: deploy, activate/deactivate, list modules/versions, and remove self-authored modules. |
| `client.marketplace()` | Player-code store/install/consent plus player-authorized one-chunk claim/release (`claimGridChunk`, `releaseClaimedGrid`) on the app-token Game API. |
| `client.crowdyStudio()` | Caller-owned Crowdy Studio projects and reusable files: list/get/create, revision-fenced atomic saves, metadata/file updates, archives, personal library, curated common files, copy-by-value imports, and authored-module recovery. |
| `client.gameApps()` | App grids, first-class ownership (`ownership` / `assignOwnership` / `transferOwnership`), and grid runtime-permission administration. |
| `client.subscriptions()` | Generic `graphql-transport-ws` operations with RAII cancellation, reconnect/replay notification, and game-thread delivery from `poll()`. |
| `client.crowdyStudioAgent()` | Exact Agentic Studio sessions/history/descriptors/budgets/control operations on the unified API. Pair with `crowdy::agent::CrowdyStudioAgentController`; see [native agent integration](docs/native-agent-api.md). |
| `client.replication()` | **Native UDP** replication: connect/assign, spatial sends, notifications, channel publish, single-actor messages, heartbeats. |
| `crowdy::session::WorldSession` | SDK-managed game state: your actor with a fixed-Hz send loop, remote-actor registry with staleness + interpolation history, chunk/voxel cache, inboxes, host tracking — see [the session layer](#the-session-layer-data-structures-that-do-the-bookkeeping). |
| `crowdy::kit::makeKit(client, appId)` | Game Kit: ready-made mappings of game concepts onto the game model across 15 genre layers, plus the engine-aware helpers (`mobs()` refereed attacks, `pets()`, `engines()` capability detection, the `crowdy/kit/wire.hpp` engine pose codec + event parsers), blueprint builders, and `deploy()` for the admin "load the rules" step — see [Game Kit](#game-kit-genre-building-blocks-over-the-game-model). |

Studio-admin surface (privileged; drive with an org/admin token from a trusted
context): `client.admin().organizations() / apps()` (including player-code
admission policy) `/ appAccess() / billing() /
payments() / quotas() / usage() / sharedEnvironment()`.
Operator surface (platform operations, requires operator rights):
`client.operator_()`. The SDK never relaxes server-side authorization — these
are typed wrappers; the caller still needs the right token and permission.

## Headless Crowdy Studio

`client.crowdyStudio()` targets the Game API with the app-scoped player token.
All ids and revisions remain decimal strings, project source is filtered by
the server's `(app, owner, project)` tuple, and nullable metadata patches use
`CrowdyStudioPatchField<T>` so omit, explicit null, and value stay distinct.
The API exposes no raw operation executor.

For an engine-owned editor, construct the controller from injected interfaces:

```cpp
crowdy::studio::CrowdyStudioPlayerComputeRuntime runtime(
    game.playerCompute(), &engineArtifactRuntime);
crowdy::studio::CrowdyStudioController studio(
    {.appId = appId, .gridId = gridId},
    game.crowdyStudio(), runtime, engineCrypto, engineClock,
    &durableSynchronization, &agentApprovalGate);

studio.initialize();
studio.updateFile(crowdy::studio::CrowdyStudioTarget::Server,
                  "src/lib.rs", source);
studio.tick();  // engine-loop autosave/retry/monitor pump
```

`CrowdyStudioState::authoritativeDiagnostics` and `localDiagnostics` are typed
`CrowdyStudioDiagnostic` values with target-relative ranges, severity, source,
message, and optional rustc code. `parseRustcDiagnostics()` accepts bounded
human/JSON rustc output; the string overload of `setLocalDiagnostics()` and
the `*DiagnosticTexts()` helpers remain for older engine views.

Wallet balance is optional observation, not authoring authority. Inject
`CrowdyStudioPlayerWalletProvider` (the read-only adapter over
`PlayerWalletAPI::balance()`) as the controller's final constructor argument
to populate `state.wallet` whenever the visible Usage surface refreshes.
Wallet read failures clear that optional snapshot and do not block editing,
saving, compilation, or deployment.

Runtime actions are revision-bound:

- draft/live plans must name the exact complete project target set;
- live plans additionally bind pairing preference and the canonical
  full-project content hash, then pass through the injected agent approval
  gate before compilation;
- full-stack publication compiles CLIENT then SERVER, binds `setRequires`,
  enables SERVER, then starts the exact CLIENT artifact through the engine
  runtime;
- checkpoint restore likewise requires the external agent layer's opaque,
  exact approval grant;
- `state.runtimeSync` explicitly distinguishes never-run, running-saved,
  running-stale, and stopped state.

The published GraphQL schemas have durable agent checkpoint events, but no
generic checkpoint-list, atomic-patch, or approved-restore root.
`ICrowdyStudioSynchronizationProvider` is therefore an explicit
host/orchestrator bridge, not a generated GraphQL adapter. Missing bridge
operations throw `CrowdyStudioCapabilityUnavailableError`. Integrations can
convert a scope-fenced `AgentCheckpoint` with
`crowdyStudioCheckpointEventFromAgentV1()` and feed the metadata to
`ingestCheckpointEvent()`; that observation never creates restore authority.
Approved restore still requires both the injected durable bridge and the exact
external approval gate.

The synchronization and runtime interfaces are intentionally server-free in
unit tests. They do not grant grid permissions or source visibility: Game API
ownership, target write/run permissions, and admission checks still execute on
every playerCompute call. The installed Studio parity fixtures pin the common
CrowdyJS runtime projection while retaining native content-hash, module, and
pairing bindings. See [MIGRATION.md](MIGRATION.md) for source-behavior and
runtime-ownership notes.

## Native player-host and local tool integration

Native games can expose agent-addressable Play controls without browser APIs
through the installed headers under `crowdy/player_host/` and
`crowdy/agent/native_tool_dispatcher.hpp`.

- `PlayerHostAdapterV1` is the only gameplay execution boundary. Implement it
  over the same movement, inventory, interaction, crafting, mount, combat,
  chat, and travel intent services that human controls use. Do not hand an
  adapter a generic `CrowdyClient`, transport, input-injection, or raw network
  escape hatch.
- `AgentControlLeaseManager` binds Play authority to the exact client epoch,
  game context, controlled entity, capability revision, lease scopes, fresh
  observation, heartbeat, and command rate. Call `tick()` from the game thread.
  Human input, Escape, Stop, death, disconnect, and permission/admission/context
  changes call the corresponding synchronous preemption method.
- `NativePlayerControlGate` is the no-DOM/no-OS CrowdyJS 12.1 control-gate
  equivalent. Engines report keyboard, pointer, movement, background, death,
  permission, context, and controlled-entity transitions through imperative
  hooks. Local intent clears before best-effort remote revoke/Pause/Stop,
  Stop works offline, and snapshots retain the 150 ms human-input-active
  window without a timer thread. Construction requires the fallback local
  intent-clear callback.
- `NativeToolDispatcherV1` is an execute-once callback router for the 14
  mandatory `game.*` tools and the 11 native Studio/runtime tools. It validates
  canonical v12 descriptor digests, typed input/output bounds, mode, deadline,
  epoch, context, lease, and approval metadata; late or ambiguous effects are
  never blindly retried. Server, model, and provider tools have no local
  fallback.
- `CrowdyStudioHostAdapter` connects local Studio/runtime tools to the same
  headless Studio controller used by human actions. Engine adapters own thread
  scheduling; callbacks may complete inline or later, and cancellation tokens
  provide a cooperative stop signal while the dispatcher fences late results.
- `CrowdyStudioControllerHostAdapter` is the concrete 11-tool controller
  mapping. `ICrowdyStudioEditorAdapter` receives only selected/open files and
  synchronized in-memory buffers, while `CrowdyStudioIntegration` owns the
  controller/runtime/dispatcher/editor/optional-agent assembly plus the
  concrete layout, lease-manager, and human-control gate. Engines inject
  layout storage, the typed player host, and input/lifecycle events.
- Integration `poll()` is a nonblocking platform + Agent callback/deadline
  pump. Autosave, monitoring HTTP, compile polling/sleep, and scheduled
  effectful Studio tools run only from the explicit serialized
  `runStudioMaintenance()` lane (or an injected `studioHost.schedule` lane).

World coordinates, distances, health values, fuel, revisions, and other
contract values that may exceed a native or JSON number remain decimal
strings. The typed schemas reject non-canonical forms before an adapter runs.

`NativeBrowserToolDispatcherAdapter` is the narrow Agent Controller bridge. It
validates canonical JSON, converts into closed native variants, forwards
`NativeToolResultV1` output/error/timing/context, maps cancellation reasons,
and pumps deadlines from the controller loop. Clear execute-once records only
after the attached session is closed. See the
[native player-host example](docs/native-player-host.md) and
[native Studio integration guide](docs/native-studio-integration.md).

## The native replication client

`client.replication()` implements the public
[Replication API](https://docs.crowdedkingdoms.com/replication-api/intro):

- **Connect** = mint/hold an app token → `serverWithLeastClients` on the Game
  API (which installs your UDP session server-side) → wait for session-ready →
  signed UDP traffic to the returned host and client port.
- **Sends**: actor updates, voxel updates, audio, text, client events, generic
  spatial, single-actor messages, channel publishes, and idle heartbeats — all
  HMAC-SHA256 signed per the
  [HMAC guide](https://docs.crowdedkingdoms.com/replication-api/hmac).
- **Receives**: bundle unpacking, per-notification HMAC verification
  (constant-time), typed dispatch, and error frames correlated by sequence
  number.
- **Lifecycle**: automatic app-token refresh before expiry, verified
  `COMMAND_RECONNECT` handling (reassign within the grace period), and a
  silent-drop watchdog (traffic going out with nothing coming back triggers
  reassignment — see
  [Troubleshooting](https://docs.crowdedkingdoms.com/replication-api/troubleshooting)).

Two integration modes:

- **Owned net thread** (default): the SDK runs a receive/send thread and hands
  you notifications through a lock-free SPSC ring; you drain it with `poll()`
  from your game thread.
- **Manual pump**: no SDK threads at all. You call `pump()` from your own
  network thread (or tick) and `poll()` from the game thread. This is the mode
  engine wrappers use.

Performance characteristics: after connect, the steady-state path performs no
heap allocation (pooled datagram buffers), no copies on parse (payloads are
spans into the receive buffer until you copy them), and no exceptions.
Notification callbacks run on the thread that calls `poll()` — never on the
network thread.

Sends never block. When a burst outruns the kernel send buffer, `send` returns
`Errc::WouldBlock`: the datagram was not transmitted, the socket is healthy,
and the caller should requeue and retry shortly. That is a different outcome
from `Errc::SocketError`, which means a real fault — branch on the two rather
than treating any non-`Ok` as fatal, or a saturated client silently loses
outbound updates at exactly the moment it can least afford to. `Connection`
counts them separately (`stats().sendsDeferred` vs `stats().sendsFailed`), and
`ReplicationConfig::socketSendBufferBytes` (default 1 MiB) sizes the buffer
that absorbs the burst — raise it for clients replicating many entities per
frame.

## The session layer: data structures that do the bookkeeping

The replication client moves datagrams; `crowdy::session::WorldSession` turns
them into game state. Every store below is a structure that multiplayer games
otherwise hand-write (and debug) themselves — using them means your first
playable build is a render loop over ready-made state instead of weeks of
netcode bookkeeping. One connection feeds all of them; you call
`session.tick()` once per frame and read plain snapshots (single-threaded by
design, so reads never lock).

| Structure | What it replaces | How it makes you faster |
|---|---|---|
| `LocalActorStore` (`session.self()`) | your presence loop | Joins the world, re-sends state at a fixed Hz with send-on-change dedup, periodic keyframes, and cheap idle heartbeats so presence never lapses; tracks `lastAck()` from self-echoes. You just `setState(bytes)` from the game loop — or `moveTo(chunk)` on boundary crossings for an immediate send. |
| `RemoteActorStore` (`session.actors()`) | everyone-else tracking | Self-filtered registry keyed by actor uuid with staleness reaping, `onJoin`/`onLeave`/`onUpdate` callbacks, and a per-actor sample history (state + server timestamp pairs) ready for interpolation/extrapolation. Render directly from `list()`. |
| `RemoteActorLane` (`actors().lane("mobs", ...)`) | per-kind actor lists | Filtered sub-registries (players vs mobs vs vehicles) so each kind is classified once at ingest — no per-frame re-scanning or re-decoding of the full registry. |
| `ChunkStore` (`session.chunks()`) | terrain sync | Chunk/voxel cache: bulk `ensureAround()` hydration from the durable store, realtime merge of incoming voxel edits, optimistic `setVoxel()` (applies locally, replicates, queues persistence), worldgen write-back via `seed()`, `pruneBeyond()`/`flush()` for streaming worlds, and `voxelTypeAt()`/`voxelStateAt()` reads for meshing/collision. |
| `EventRouter` (`session.events()`) | RPC dispatch switch | Routes typed client/server events (`[u16 eventType][state]`) to per-type handlers and retains `lastEvent(type)` — your gameplay events become `events().on(kDoorOpened, ...)` instead of a hand-rolled switch over payload bytes. |
| `Inbox` (`channelInbox()` / `directInbox()`) | chat/message queues | Bounded queues for channel and direct messages with `drain()`, non-consuming `messages()`, `onMessage` callbacks, and channel discovery — plus `send()` helpers back through the connection. |
| `ErrorStore` (`session.errors()`) | "why was that send rejected?" | Correlates server error frames (sequence-numbered, uint8 wrap) with the *kind* of send that used that sequence, so a permission denial points at "your voxel edit", not a bare error code. |
| Host tracking (`amIHost()` / `onHostChanged`) | election polling | Heartbeats host eligibility on a cadence and caches the elected host with a change callback; gate host-only simulation without writing the polling loop. |
| `SaveStateStore` / `AvatarStateStore` | persistence plumbing | Byte-level caches over the durable save/avatar surfaces with explicit `load()`/`save()`; base64 stays at the wire boundary, your code sees bytes. |
| `ContainerMirror` | game-model polling | A pull cache, not a subscription owner: `watch()` containers, re-pull on demand or when a bound channel pings, and read versioned snapshots via `get()`/`onChange`. Applications may also call `refresh()` from the separate typed `containerChanged` metadata feed. |
| `PodCodec<T>` / `UnrealPose` | wire layout code | Your replicated state as a packed struct: the struct layout *is* the little-endian wire layout (static-asserted), with the 88-byte Unreal-compatible pose included. No serializer to write, nothing to keep in sync. |
| `IUuidStore` (memory/file) | identity persistence | Persist your actor uuid across restarts so remote registries treat you as the same actor. |

Under the hood these sit on the same primitives the hot path uses — the
lock-free SPSC ring between network and game thread, pooled fixed-size
buffers, and zero-copy parsed views — so the convenience layer does not trade
away the performance story.

## Game Kit: genre building blocks over the game model

The platform's [game model](https://docs.crowdedkingdoms.com/game-api/game-models)
gives you server-authoritative rules without running a server: typed
containers, properties, and transactional functions gated by **invoke
policies** (`owner_of_self`, `condition` expressions, `is_host`,
`is_current_turn`, ...), plus
[automations](https://docs.crowdedkingdoms.com/game-api/autonomous-processes)
that run functions server-side on schedules or events. The **Game Kit** maps
traditional game concepts onto that machinery so you don't design the schema
yourself. Two phases, matching the platform's model:

1. **Studio loads the rules** — blueprint builders emit declarative bundles
   (container types, property schemas, policy-gated functions, automations);
   `deploy()` seeds them in one idempotent pass. Admin context
   (`manage_apps`) only — never the shipped client.
2. **The game client plays** — runtime kits wrap the conventions with typed
   helpers. Authority is enforced server-side on every call: `KitInvokeResult`
   carries the verdict (`success == false` with `errorMessage` on a policy
   denial — never an exception), so an untrusted client can try anything and
   change nothing it shouldn't.

```cpp
// Studio (admin token): one-time "load the rules".
auto adminKit = crowdy::kit::makeKit(admin, appId);
adminKit.deploy({crowdy::kit::inventoryBlueprint(),
                 crowdy::kit::lockBlueprint({.objectTypeName = "Door",
                                             .authority = {crowdy::kit::LockAuthority::key()}})});

// Game client (player token): typed runtime helpers.
auto kit = crowdy::kit::makeKit(game, appId);
auto bag = kit.inventory().ensure(myUserId);
auto result = kit.objects().open(doorId, keyId);
if (!result.success) showLockedMessage(result.errorMessage);
```

### Genre and capability map

Every layer is a blueprint builder plus a runtime kit. Compose the layers
your genre needs — they share the model, so they interoperate (a quest can
pay into a wallet, a plot purchase can grant enforced build permissions):

| Genre / concept | Builder → runtime | Capabilities |
|---|---|---|
| Items & bags (RPG, survival, sandbox) | `inventoryBlueprint` → `kit.inventory()` | Per-player bags and item stacks (`item_id`/`quantity`/`slot`); owner-gated grant/consume/move and atomic two-stack transfer; the consume guard refuses overdraw server-side. |
| Doors, chests, gates | `lockBlueprint` → `kit.objects()` | Lockable world objects with pluggable authority: key item, owner, group/team permission, grid permission, enforced chunk permission, or custom policy trees; several lock types per app via `objectsFor()`. |
| NPCs & world ticks | `npcBlueprint` → `kit.npcs()` | Server-driven behaviors (interval/cron/event triggers) with selector targeting — wander, restock, guards reacting only to intruders via permission predicates; runs with no client online. |
| Land ownership (MMO, sandbox) | `plotBlueprint` → `kit.plots()` | Buy/rent/evict plots where payment and **replication-enforced grid permissions** commit atomically — buying land grants real build rights, not just a database row. |
| Economy (any genre) | `economyBlueprint` → `kit.economy()` | Multi-currency wallets, atomic shop purchases, escrow player trades, a player market with escrowed listings, restock automation; every mutation guard is server-side. |
| Progression (RPG, arcade) | `progressionBlueprint` → `kit.progression()` | XP/levels on a configurable curve, skill trees with prerequisite chains, achievements, host-gated rating. |
| Loot (RPG, roguelike) | `lootBlueprint` → `kit.loot()` | Weighted tables compiled into seed-driven server expressions (clients can't reroll), atomic single-claim drops, event-triggered drops. |
| Quests (RPG, live-ops) | `questsBlueprint` → `kit.quests()` | Event-driven progress via automations, atomic reward turn-in (items + currency in one transaction), cron daily resets. |
| Combat (action, MMO) | `combatBlueprint` → `kit.combat()` | Server-authoritative damage/death/respawn plus turn-based and host-synced modes. |
| Matches & lobbies (arena, board, card) | `matchesBlueprint` → `kit.matches()` | Session lobbies, rounds, turn order via the platform's session-turn authority, scores, per-match notification channel (notify-to-pull re-pulls on ping). |
| Hidden information (card games) | `decksBlueprint` → `kit.decks()` | Hidden hands via owner-visibility properties, server-dealt shuffles by position — opponents' cards never reach your client. |
| Living world (farming, survival) | `worldsimBlueprint` → `kit.worldsim()` | Day/night clock with spatial notifications, atomic gather/crop functions, and wave counters. |
| Social (MMO, co-op) | `guildBlueprint` → `kit.social()` | Parties and guilds over teams + channels, guild chat, territory grants, guild hall (a group-permission lock) + guild bank (a shared inventory) composites. |
| Leaderboards (arcade, competitive) | `leaderboardsBlueprint` → `kit.leaderboards()` | Trusted keep-best submits (server/host/automation authority — anti-cheat by construction), ranking reads, cron season resets. |
| Monetization | `featureGate` → `kit.features()` | Feature keys granted per access tier; AND a gate into any builder's policy (`andPolicies(..., featureGate("vip"))`) to tier-gate a capability. |

Trusted mutations (XP grants, loot rolls, currency mints, score submits) take
a `TrustedAuthority` — server, host, automation, owner, or a custom policy —
so reward-granting functions are never plain player calls. The C++ builders
emit **the same model definitions as CrowdyJS's** (verified structurally in
CI-adjacent tooling), so a world deployed from either SDK is playable from
both, and studios can seed from TypeScript tooling while the game ships C++.
The pinned CrowdyJS blueprint currently emits selector JSON that the deployed
Game API cannot execute for combat status ticks and automatic node/crop
regeneration. CrowdyCPP preserves that structural parity but does not claim
those automations as operational; use explicit scheduling until the
coordinated CrowdyJS/Game API blueprint contract is corrected.

## Wrapping CrowdyCPP in engines

CrowdyCPP is the intended foundation for engine-specific SDKs, including the
official [Crowdy Unreal SDK](https://github.com/CrowdedKingdoms/CrowdySDK-Unreal)
(docs: [Unreal SDK guide](https://docs.crowdedkingdoms.com/unreal-sdk/intro)).
The design rules that make it wrappable:

1. **No engine types, ever.** The public API uses `std::span`, `std::string_view`,
   and POD structs. Nothing in CrowdyCPP allocates with `new` on hot paths or
   leaks platform handles, so an engine can marshal at the boundary it chooses.
2. **Pluggable platform services.** Engines inject their own implementations:
   - `IHttpTransport` — Unreal wraps `FHttpModule` so all GraphQL traffic uses
     the engine's HTTP stack, proxies, and certificate handling. (Alternatively
     link the default libcurl transport; Unreal ships libcurl + OpenSSL in its
     ThirdParty tree.)
   - `IWebSocketTransport` / `IWebSocketConnection` — create a dormant socket,
     install its event callback in `start()`, and provide thread-safe,
     non-blocking `send()` / `close()`. The engine may complete on any thread;
     CrowdyCPP fences stale connections and posts user callbacks to `poll()`.
   - `ICrypto` — Unreal binds its bundled OpenSSL for HMAC-SHA256. Implement
     `makeHmacSha256()` as well: it returns a keyed `IMac` reused across
     datagrams, and it is the single largest CPU win on the replication path.
     Omitting it is supported and falls back to the one-shot call.
   - `ILogger` / `IAllocator` / `IClock` — adapters onto `UE_LOG`, `FMemory`,
     and engine time so SDK activity shows up in engine tooling.
3. **Threading stays with the engine.** Use manual-pump mode: the plugin runs
   `pump()` on an `FRunnable` network thread (or the task graph) and `poll()`
   on the game thread from a ticker. Callbacks therefore fire on the game
   thread, where `UObject`s are safe to touch. Nothing in CrowdyCPP spawns
   threads in this mode.
4. **Binary state stays binary.** Actor-state payloads are opaque bytes on the
   wire. An Unreal wrapper maps its entity replication snapshots directly into
   the payload span — no base64, no JSON, no intermediate copies. The
   open-source [cks-loadtest](https://github.com/CrowdedKingdoms/cks-loadtest)
   tool includes an Unreal-compatible 88-byte actor-state layout that
   interoperates with the current Unreal SDK's pose format.
5. **Session layer maps to entity systems.** `WorldSession`'s remote-actor
   registry (staleness, sample history) is exactly the input an engine wrapper
   needs to drive owner/proxy entity components and interpolation; the chunk
   cache backs voxel/terrain streaming; inboxes back chat and direct messages.

The expected Unreal integration shape: CrowdyCPP builds as a static library in
a `ThirdParty` module of the plugin; the plugin's subsystems (connection,
entities, voice, teams, persistence) become thin adapters over
`crowdy::CrowdyClient`, `crowdy::replication`, and `crowdy::session::WorldSession`,
replacing the plugin's bespoke networking while keeping its Blueprint-facing
API stable. Other engines (custom C++ engines, Godot via GDExtension, Unity
via a C shim) follow the same recipe.

## Two tokens, two clients

There is one API origin since 0.20.0, but still two tokens. CrowdyCPP follows the
platform's
[portal / app-scoped token model](https://docs.crowdedkingdoms.com/management-api/portals-and-app-tokens):

1. Passwordless sign-in yields an **identity session token** — account, studio
   admin and minting. Not accepted for gameplay.
2. Gameplay requires a short-lived **app-scoped token** per app
   (`portal().mintAppToken(appId)`), which is also the 64-octet HMAC key for
   native UDP. With an active native connection, rotate it through
   `refreshGameplayToken()` so the old socket is quiesced before the bearer
   changes and the same handlers reconnect under the fresh token. Use
   `portal().refresh()` directly only when no replication lifecycle needs to
   be preserved.
3. Build one identity client and one client per game. All world/UDP calls run
   on the game client. The identity client points at the shared entry origin;
   the game client points at the app's own datacenter (`gameApiUrl`) with
   `discoveryUrl` set so it can recover if that instance stops answering.

Persisting them keeps the same split. `FileTokenStore::sessionPath(dir, origin)`
names the session file, `FileTokenStore::appPath(dir, appId)` the gameplay one:

```cpp
crowdy::ClientConfig identityCfg;
identityCfg.httpUrl = apiOrigin;
identityCfg.tokenStore = std::make_shared<crowdy::graphql::FileTokenStore>(
    crowdy::graphql::FileTokenStore::sessionPath(stateDir, apiOrigin));
```

A session is **one per origin** and an app token is **one per app**, and the
naming is not a formality. Browser games keyed their credential by the game's own
path, so two games on one origin could not see each other's login: a player who
signed in for one was anonymous to the next and got bounced back to the portal.
What they stored was an app token anyway, which is per-game by definition, so
there was nothing to share even had the keys matched. Do not key a session by
anything per-game — that is the bug, not the fix.

## Versioning and binary compatibility

CrowdyCPP remains pre-1.0. Within a minor line, patch releases preserve public
source compatibility and ABI compatibility for the installed libraries.
Each new minor release may make source or ABI changes, even though the major
version remains `0`; consumers must review the migration notes and rebuild.

The installed CMake package follows that policy with `SameMinorVersion`.
For example, `find_package(CrowdyCPP 0.16 CONFIG REQUIRED)` can select a newer
`0.16.x` package, but it will not accept `0.17.x`. No compatibility is promised
between arbitrary `0.x` minors.

## Server compatibility

CrowdyCPP targets the current platform APIs and degrades gracefully on older
deployments:

- **Game-model invoke denials:** current servers report invoke-policy denials
  as `FORBIDDEN` GraphQL errors; newer builds resolve them as
  `success: false` invoke results (with a failure event). The kit's
  `kitInvoke` maps both onto `KitInvokeResult{success:false, errorMessage}`,
  so kit code behaves identically on either generation. A `BAD_REQUEST` whose
  message begins with the stable `Invoke params violate` contract prefix is
  also a typed unsuccessful verdict; unrelated BAD_REQUESTs still throw.
- **`userAppState` round-trip:** older game-api builds stored the base64
  `state` input verbatim and re-encoded on read (reads returned
  base64(base64(bytes))); newer builds round-trip symmetrically. Decode
  defensively if you must read rows written through an old server.
- **Compute Modules (`client.compute()`):** requires a `cks-game-api` build
  that serves the `compute*` root fields (v0.13.13+ dev line). Older servers
  reject compute operations with a GraphQL validation error; every other
  sub-client is unaffected.
- **Realtime + live-ops surfaces (v0.6.0):** `abilities()` (server-validated
  casts), `movement()` (warden violation books, observe/flag), `territory()`
  (control points + factions), `racing()` (server-timed laps, ghosts, the
  possession ball), `liveops()` (event windows, seasons, battle-pass
  composition), `moderation()` + `telemetry()` (model-first), the loot
  engine path (`enginePull` pity rolls), `compute().templates()` /
  `deployTemplate()` (the platform engine registry), and the type-94..98
  wire parsers. Capability-detected as always.
- **Session-genre engine surfaces (v0.5.0):** `kit.instances()` /
  `director()` / `matchmaking()` / `minigames()`, the engine paths on
  `matches()` (`engineReady`/`engineSubmitMove`/`findByProposal`),
  `decks()` (hidden hands via the deck engine), `leaderboards()`
  (server-ranked pages), `economy().orderBook()` (escrowed bid/ask), the
  quests tutorial sequencing, and the type-91/92/93 wire parsers talk to
  the Wave 2 engine templates. Capability-detected; model-only deployments
  keep today's behavior.
- **Engine kit surfaces (v0.4.0):** `kit.mobs()` / `kit.pets()` /
  `combat().attackRouted()` / `worldsim().forecast()` and the
  `crowdy/kit/wire.hpp` pose/lane registry talk to compute-module game
  engines built on the Wave 0/1 `cks-game-api` dev line (`crowdy-game-kit`
  crates). Capability detection (`kit.engines()`) makes them degrade
  gracefully — model-only deployments keep today's behavior.

## Errors

GraphQL-layer failures throw structured exceptions mirroring CrowdyJS:
`CrowdyHttpError`, `CrowdyGraphQLError` (preserves `extensions.code`,
`remediation`), `CrowdyNetworkError`, `CrowdyTimeoutError`,
`CrowdyProtocolError`. Branch on `error.code()` rather than parsing messages.
Subscriptions are non-throwing: `onNext` receives
`GraphQLSubscriptionOutcome`, while `onError` receives a typed terminal
`GraphQLSubscriptionError`. Destroying its move-only handle suppresses queued
callbacks and sends protocol `complete` when connected.

The replication layer never throws on the hot path: sends return
`crowdy::Result` codes, server-reported failures arrive as
`GenericError` notifications correlated by sequence number, and connection
state changes surface through a status callback. Note the protocol's
documented semantics: UDP is best-effort, sequence numbers are correlation
(not idempotency), and **auth failures are often silent drops** — see
[Operations](https://docs.crowdedkingdoms.com/replication-api/operations).

## Schema refresh and codegen

The GraphQL surface is generated from committed artifacts so external builds
never need network access or sibling repos:

```bash
# Maintainer-only Node dependencies (not part of a CMake consumer build).
npm ci

# Maintainers: refresh the unified published SDL
node scripts/schema-sync.mjs            # writes schema.gql from game-api.graphql
node scripts/codegen.mjs                # regenerates include/crowdy/generated/
# commit schema.gql and include/crowdy/generated/ together
```

`scripts/schema-sync.mjs` downloads the published unified SDL
(`https://docs.crowdedkingdoms.com/schema/game-api.graphql`); `--game <path|url>`
overrides the source. There is one schema because there is one origin: the
published management SDL is a **derived** subset for the docs tab, not a second
endpoint. `--check` compares the committed `schema.gql` with the source without
writing. Operation documents live in `operations/<domain>/*.graphql` and follow
the same shapes as CrowdyJS. Codegen isolates each named operation with only
its transitive fragments and embeds schema and operation-input digests in
generated headers; `node scripts/codegen.mjs --check` verifies them without
modifying files.

### Parity maintenance gates

CrowdyCPP tracks CrowdyJS **14.1.0** at
`90f4b7bb2562d007aa62d01d4b21abdb76923e9b`. The source of truth is
`crowdyjsParityTarget` in `package.json`; CI reads that commit before checkout,
and the parity/fixture tools reject a checkout whose package version or HEAD
does not match. After either SDK changes its public surface:

```bash
# Optional for nonstandard layouts. Otherwise tools resolve ../CrowdyJS,
# ./CrowdyJS (CI), then the sibling of this worktree's primary git checkout.
export CROWDYJS_PATH=/path/to/CrowdyJS

# Compare both schemas in both directions, audit roots/methods, and refresh docs.
node tools/parity/parity.mjs --crowdyjs "$CROWDYJS_PATH" \
  --write docs/parity-matrix.md --strict
npm run check:operations
npm run check:parity

# CrowdyJS must be built first. Every fixture tool rejects tracked checkout
# changes, a wrong package version, or a wrong commit.
npm run check:agent-fixtures
npm run check:control-gate-fixtures
npm run check:studio-host-fixtures
npm run check:layout-fixtures
npm run check:studio-state-fixtures

# Parser/gate behavior.
npm test
```

An explicitly configured `CROWDYJS_PATH` is authoritative and fails clearly
when invalid. Automatic resolution likewise fails if no deterministic
sibling/CI/worktree checkout exists; it never skips or weakens parity.

The reviewed baseline accepts only named classifications. A **portable gap** is
shown as missing work and is not presented as parity; native equivalents and
inherently browser-only surfaces are the only waivers. New differences and
stale classifications fail. `--strict` additionally fails on every remaining
portable gap and is the strict portable-parity release gate used by CI.

Blueprint builders are a compiled structural gate:

```bash
cmake -S . -B build-parity -DCROWDY_BUILD_PARITY_TOOLS=ON
cmake --build build-parity --target crowdy_blueprint_dump
node tools/parity/dump-blueprints.mjs /path/to/built/CrowdyJS > /tmp/js.json
./build-parity/crowdy_blueprint_dump > /tmp/cpp.json
node tools/parity/blueprints-diff.mjs /tmp/js.json /tmp/cpp.json
```

When intentionally changing the target, update the pinned CrowdyJS SHA, sync
the descriptor/preemption, control-gate, 11-tool Studio host, and layout
fixtures with their `tools/parity/*-fixtures.mjs --write` commands, validate
the shared Studio state fixtures, regenerate `docs/parity-matrix.md`, and
commit the pin plus changed fixtures and generated evidence together. Include
schema snapshots and generated headers whenever the target also changes SDL.
None of these maintainer gates run during a normal external CMake build.

## Tests

- `ctest` — offline unit tests (wire codec golden vectors, HMAC vectors,
  GraphQL-WebSocket handshake/reconnect/frame/cancellation behavior, bundle
  parsing, malformed-input fuzz, codec round-trips).
- A build configured with `CROWDY_NO_EXCEPTIONS=ON` compiles with
  `-fno-exceptions` and runs the applicable reduced-surface matrix. The
  package omits exception-contract layers listed in [Build](#build), and its
  install test verifies those unsupported headers are not shipped.
- `npm test` — offline Node tests for schema/parity parser behavior.
- `tests/e2e/` — end-to-end suites (two-client fan-out, gamer journey, token
  refresh/reconnect, opt-in marketplace chunk claim/release, and the complete
  native Studio factory/edit/BUILD/draft/Play takeover lifecycle) that run
  against a deployment you configure via
  environment variables (`CROWDY_E2E_API_URL`, `CROWDY_E2E_HTTP_URL`,
  `CROWDY_E2E_EMAIL`, `CROWDY_E2E_APP_ID`, …). Skipped when unset.
- `tests/prodsmoke/` — a read-only smoke test of the discovery and endpoint-move
  path against a live tier. Not part of any build and not run by CI; it needs the
  network. It exists because the estate rule is the one piece that cannot be
  proven by a fixture: a guard that refused
  `ck.<tier>.v7.cks-env.com -> ck-<dc>.<tier>.v7.cks-env.com` would pass every
  test in this repo and then decline every redirect in production. Build it
  against an installed package and run:

  ```bash
  cmake -S tests/prodsmoke -B build-prodsmoke -DCMAKE_PREFIX_PATH=<install-prefix>
  cmake --build build-prodsmoke
  ./build-prodsmoke/prod_smoke https://ck.prod.v7.cks-env.com <appId>
  ```
- `benchmarks/` — codec ns/op, the send-path cost breakdown (`bench_send`:
  encode vs MAC vs socket write, with the alternatives for each priced side by
  side), and end-to-end echo latency against an env-configured deployment. See
  [benchmarks/README.md](benchmarks/README.md) for how to run them and the
  recorded results.

## Docs

- [Replication API (native UDP)](https://docs.crowdedkingdoms.com/replication-api/intro)
- [Wire formats](https://docs.crowdedkingdoms.com/replication-api/wire-formats) · [HMAC](https://docs.crowdedkingdoms.com/replication-api/hmac)
- [Management API](https://docs.crowdedkingdoms.com/management-api/intro) · [Game API](https://docs.crowdedkingdoms.com/game-api/intro)
- [Native Agentic Studio](docs/native-agent-api.md)
- [Native Studio integration](docs/native-studio-integration.md) · [Native player host](docs/native-player-host.md) · [GraphQL WebSockets](docs/graphql-websocket.md)
- [CrowdyJS / CrowdyCPP / Game API compatibility](docs/compatibility.md)
- [Release verification checklist](docs/release-checklist.md)
- [Game Models](https://docs.crowdedkingdoms.com/game-api/game-models) · [Grids & permissions](https://docs.crowdedkingdoms.com/game-api/grids-and-permissions)
- [CrowdyJS](https://github.com/CrowdedKingdoms/CrowdyJS) — the TypeScript SDK this API surface mirrors
- Agent index: [llms.txt](https://docs.crowdedkingdoms.com/llms.txt)

## License

MIT
