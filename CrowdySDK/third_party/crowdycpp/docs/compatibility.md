# SDK and Game API compatibility

CrowdyCPP 0.26.0 (this `dev` line) passes the strict portable-parity gate
against CrowdyJS **15.0.0**. The gate pins CrowdyJS commit
`2b0a5a5aa269c3822a5db8fdde689519bee5fe86` (`crowdyjsParityTarget` in
`package.json`); see [`parity-matrix.md`](parity-matrix.md) for the generated
method-by-method evidence. Native equivalents and browser exclusions remain
intentional, so this does not claim identical transports or browser behavior.

| Surface | CrowdyCPP 0.26.0 | CrowdyJS 15.0.0 | Required public API generation |
|---|---|---|---|
| Core Management and Game GraphQL | Supported | Supported | Current published Management + Game SDL |
| Native UDP replication | Direct native transport | Browser GraphQL UDP proxy | Current Replication API |
| Generic GraphQL WebSocket | `GraphQLSubscriptionClient` | `graphql-ws` | `graphql-transport-ws` endpoint |
| Game-model container feed | Typed `gameModel().containerChanged` | Typed `containerChanged` | Game API 2026-07+ |
| App-scoped player counts | Typed snapshot + change stream | Typed snapshot + change stream | Game API 2026-07-24+ |
| Keyed container ensure/filter | `ensureContainer`, `bindingKey` filter | `ensureContainer`, `bindingKey` filter | Game API 2026-07-24+ |
| App listing-version administration | `marketplace().appListingVersions` | Typed listing-version methods | Management API 2026-07-24+ |
| Crowdy Studio projects/runtime | Headless native controller, typed diagnostics/wallet observation | Browser/headless controller | Game API project/runtime roots; durable checkpoint mutations require an injected bridge |
| Crowdy Studio pane layout | Headless controller with injected storage | Headless controller with browser-local default storage | None |
| Native Studio integration | Owned editor/layout/runtime/host/Agent/control assembly with explicit maintenance scheduling | Browser Studio composition | Project/runtime roots plus `crowdy.studio-agent/1` when Agent support is enabled |
| Agentic Studio HTTP + event stream | Typed controller, GraphQL-WS replay/gap-fill | Typed controller and transport | `crowdy.studio-agent/1` |
| Local Play/Studio tools | Native player-host + closed typed dispatcher | Browser dispatcher + player host | Agent descriptor contract v1 |

`schema.gql` is the committed snapshot of the published API SDL. Codegen
isolates every operation with only its transitive fragments and validates it
independently, so an unrelated root field in the same file cannot make a
request valid.

Since 0.20.0 there is one schema because there is one origin. The per-plane
snapshots are gone: the published management SDL is now derived from the unified
schema by filtering it to a root-field allowlist, so validating against it would
have answered "is this in the management docs tab" rather than "will the server
accept it".

Older servers reject only operations they do not know. A client can continue
using older surfaces by not calling the newer methods. There is no provider
key or provider client in CrowdyCPP: Agentic Studio provider selection and
credentials remain server-side.

The only intentional parity waivers are generated in the parity matrix:
native equivalents for browser UDP/runtime behavior and browser exclusions
for inherently browser-owned PKCE persistence, DOM/Monaco/VFS worker chrome,
splitters, embed panel/dock/HUD/styles/focus handling, and worker-entry
packaging. CLIENT
artifact-byte decoding is portable and is available through
`playerCompute().artifactBytes(...)` and
`marketplace().clientArtifactBytes(...)`.
Portable gaps, unclassified differences, and stale classifications are zero.

Known coordinated limitation: the pinned CrowdyJS/CrowdyCPP Game Kit
blueprints retain combat status-tick and worldsim node/crop selector forms that
the deployed Game API does not currently execute. Runtime helpers remain
available, but those automatic schedules are not claimed as supported until
the shared blueprint/server contract changes in both SDKs.

## CrowdyCPP 0.x source and ABI policy

Until 1.0, each minor release may contain source-incompatible or ABI-incompatible
changes. Patch releases within one minor line preserve the public source and
installed-library ABI. Consumers should review `MIGRATION.md` and rebuild when
moving between minors.

The installed `CrowdyCPPConfigVersion.cmake` uses CMake's
`SameMinorVersion` policy. A request for `0.16` may select a compatible newer
`0.16.x` package, but no `0.17.x` package is accepted as compatible merely
because both versions have major version `0`.
