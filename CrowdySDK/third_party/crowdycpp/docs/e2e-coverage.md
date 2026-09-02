# CrowdyCPP e2e coverage matrix

This document inventories CrowdyCPP's black-box suite implementations. The
tests drive public APIs as an external integrator would and use native UDP,
but this matrix is not a record of a hosted CI run.

Suites live in [`tests/e2e/`](../tests/e2e/); see that folder's README for
configuration and labels. `slow` and `optional` mark the non-default ctest
labels.

## Evidence levels

- **Default CI:** builds every e2e executable and invokes them through ctest,
  but provides no `CROWDY_E2E_*` deployment credentials. The executables
  therefore exit 77 and are reported as skipped. Default CI supplies compile
  evidence only for live suites.
- **Default offline evidence:** unit tests exercise fake transports, protocol
  replay/cancellation, generated documents, dispatcher safety, and output
  mapping without claiming a live platform round trip.
- **Optional live evidence:** an operator may configure the documented
  environment and run the `e2e`, `e2e_slow`, and `e2e_optional` labels.
  `implemented; optional live` below means the suite exists and compiles; it
  does not mean hosted CI executed its live assertions.
- **Excluded:** no safe public black-box scenario exists, with the reason
  recorded in the row.

## 0.16.0 release validation

The 0.16 release branch adds `e2e_native_studio_integration`, which uses the
real `CrowdyClient::createCrowdyStudioIntegration` factory for a disposable
project edit/save, BUILD attach, native Studio host dispatch, draft submission,
Play lease, and synchronous human-input takeover. A 2026-07-24 dev run passed
that flow; the draft reached the deployment's configured code-admission gate
and correctly remained pending operator approval. Default CI still compiles
the executable and records exit-77 when deployment variables are absent. The
local release run also passed all 24 offline unit tests and compiled all 53 e2e
executables.

Approved restore has deterministic integration coverage in
`studio_integration_test`. A live restore is intentionally not inferred from
ordinary project-save or Agent GraphQL authority: the published deployment does
not advertise a generic synchronization/approval provider. The live suite
prints that subtest as skipped, and fails closed if a capability is asserted
without an injected provider.

## Historical 0.14.x live validation

On 2026-07-24, the dev deployment passed live `e2e_crowdy_studio`,
`e2e_agentic_studio`, `e2e_marketplace_claims`, and
`e2e_gameplay_token_refresh` from the release branch. Agent validation covered
controller create/attach/replay, a provider-backed ASK run, BUILD binding,
Play heartbeat/lease/takeover, and native dispatcher cancellation through the
durable HTTP polling fallback. The builder's libcurl 8.5 lacks a supported
default WebSocket backend, so `e2e_graphql_websocket` exited 77 at its explicit
transport-availability gate. Reconnect/frame/cancellation behavior remains
covered by injected-transport offline tests; this is not represented as a
live WebSocket pass.

## CrowdyJS e2e suites

The TypeScript SDK's end-to-end suites are the primary parity target.

| CrowdyJS suite | CrowdyCPP suite | Status |
|---|---|---|
| `self-echo-actor` | `e2e_self_echo` | implemented; optional live (native UDP) |
| `two-client-actor` | `e2e_two_client_actor` | implemented; optional live (native UDP) |
| `two-client-voxel` | `e2e_two_client_actor` (voxel fan-out) + `e2e_voxels_permissions` | implemented; optional live |
| `two-client-audio` | `e2e_two_client_audio` | implemented; optional live |
| `two-client-messaging` | `e2e_two_client_messaging` | implemented; optional live |
| `two-client-single-actor` | `e2e_two_client_messaging` (direct message) + `e2e_cross_server` | implemented; optional live |
| `two-client-channel` | `e2e_two_client_messaging` (channel) | implemented; optional live |
| `two-client-cross-app` | `e2e_cross_app` | implemented; optional live with a second app |
| `gamer-journey` | `e2e_gamer_journey` | implemented; optional live |
| gameplay-token rotation | `e2e_gameplay_token_refresh` | implemented; optional live (native UDP; preserves the same connection/handlers) |
| marketplace chunk claim/release | `e2e_marketplace_claims` | implemented; optional live with a reserved free chunk and `SELF_CLAIM` |
| Crowdy Studio project CRUD/patch/draft | `e2e_crowdy_studio` | implemented; optional live with an owned Studio grid |
| Agentic Studio ASK/BUILD | `e2e_agentic_studio` | implemented through the controller factory; optional live |
| Native Studio factory/edit/save/BUILD/draft/Play takeover | `studio_integration_test` + `e2e_native_studio_integration` | deterministic factory/host/control coverage in default CI; complete production-factory round trip optional live |
| Approved checkpoint restore | `studio_integration_test` + conditional `e2e_native_studio_integration` subtest | deterministic injected synchronization + exact approval coverage; live excluded until the selected deployment advertises and injects both capabilities |
| Agentic Studio attach/replay/heartbeat | `agent_controller_test` + `e2e_agentic_studio` | fake-transport coverage in default CI; controller-factory round trip optional live |
| Agentic Studio local dispatcher takeover | `player_host_test` + `e2e_agentic_studio` | fake-host coverage in default CI; live-session epoch binding optional live |
| Agentic Studio Play lease/takeover | `e2e_agentic_studio` | implemented; optional live with an engine host capability/entity |
| Agentic Studio operator kill | `e2e_agentic_studio` | implemented behind an explicit destructive-test flag; optional live and released in-test |
| `new-app-default-access` | `e2e_studio_admin` (default-access scenario) | implemented; optional live |
| `new-app-grid-creation` | `e2e_studio_admin` (grid-creation scenario) | implemented; optional live |
| `studio-admin` | `e2e_studio_admin` | implemented; optional live |
| `payments-economy` | `e2e_payments` | partially implemented; optional live for create/list/idempotent replay; capture excluded |
| `operator-control-plane` | `e2e_operator` | partially implemented; optional live read-only queries |
| `malicious-input` | `e2e_malicious_input` | implemented; optional live |

## Game API SDK e2e (public `test/sdk` surface)

| Game API area | CrowdyCPP suite | Status |
|---|---|---|
| actors | `e2e_actors_crud` | implemented; optional live |
| chunks | `e2e_chunks` | implemented; optional live |
| voxels | `e2e_voxels_permissions` | implemented; optional live (the deny verdict is deployment-dependent — see findings) |
| server-status | `e2e_gamer_journey` (`gameClientBootstrap`) + every UDP suite (`serverWithLeastClients`) | implemented; optional live |
| state | `e2e_state_avatars` | implemented; optional live strict base64 round trip |
| teleport | `e2e_teleport` | implemented; optional live |
| host discovery | `e2e_host_election` | implemented; optional live |
| teams / channels | `e2e_teams_channels` | implemented; optional live |
| game model + automations | `e2e_game_model`, plus every `e2e_kit_*` (model composition) | implemented; optional live |
| generic GraphQL-WS + typed container feed | `graphql_subscription_test` + `e2e_graphql_websocket` | protocol behavior in default CI; explicitly gated optional live round trip when a default/injected transport is available |
| compute modules | `e2e_compute` (author → deploy → compile → enable → invoke → observe → delete) | implemented; optional live equivalent of the CrowdyJS scenario |
| player compute + grid ownership | `player_runtime_surface_test` + `e2e_crowdy_studio` (draft submission) | typed surface in default CI; draft round trip optional live on an owned grid |
| player chunk claim/release | `player_runtime_surface_test` + `e2e_marketplace_claims` | exact documents/output mapping in default CI; app-token round trip optional live |
| udp-proxy (`connect`/`send*`/`udpNotifications`) | — | excluded: browser proxy path; CrowdyCPP replicates natively over UDP (see the parity matrix) |

## Management API e2e (public `test/auth` + `test/sdk`)

| Management area | CrowdyCPP suite | Status |
|---|---|---|
| auth / identities | `e2e_auth_identities` | implemented; optional live |
| users | `e2e_auth_identities` (`me`, profile) | implemented; optional live |
| organizations | `e2e_studio_admin` | implemented; optional live |
| apps | `e2e_studio_admin` | implemented; optional live |
| app code admission | `player_runtime_surface_test` (offline routing/variables) | live e2e pending a deployed P1 player-runtime environment |
| Agentic Studio app/operator policy | `agent_api_test` + `e2e_agentic_studio` | typed routing in default CI; operator kill/release optional live |
| app-access (tiers + grants) | `e2e_studio_admin` | implemented; optional live |
| billing | `e2e_billing_quotas` | implemented; optional live |
| quotas | `e2e_billing_quotas` | implemented; optional live |
| usage | `e2e_billing_quotas` (summaries/projections) | implemented; optional live |
| payments | `e2e_payments` | partially implemented; optional live, with capture excluded |
| org-token-auth | `e2e_studio_admin` (create/update/revoke) | implemented; optional live |
| cross-tenant | `e2e_cross_tenant` | implemented; optional live |
| environments | `e2e_operator` (read-only) | partially implemented; optional live, provisioning excluded |
| shared-environment | — | excluded: publishing mutates billing/runtime infra; not safe for a headless suite |

## Replication API behaviors (native UDP)

Reproduced black-box over the native transport, citing the public
[Replication API](https://docs.crowdedkingdoms.com/replication-api/intro)
docs (wire formats, HMAC, operations, troubleshooting).

| Behavior | CrowdyCPP suite | Status |
|---|---|---|
| client echo / self-echo | `e2e_self_echo` | implemented; optional live |
| two-client spatial fan-out | `e2e_two_client_actor` | implemented; optional live |
| spatial distance + decay | `e2e_spatial_distance` | implemented; optional live |
| voxel update replication | `e2e_two_client_actor`, `e2e_chunk_store_live` | implemented; optional live |
| single-actor (directed) message | `e2e_two_client_messaging`, `e2e_cross_server` | implemented; optional live |
| channel messages | `e2e_two_client_messaging`, `e2e_durable_mirror` | implemented; optional live |
| client-event replication | `e2e_stores_live` (EventRouter) | implemented; optional live |
| negative auth (bad HMAC / wrong app / garbage) | `e2e_negative_auth` | implemented; optional live |
| permission refresh (grant/revoke on a live session) | `e2e_permission_refresh` | implemented; optional slow live |
| app-token refresh + reconnect | `client_portable_test` + `e2e_gameplay_token_refresh` | staged failures in default CI; fresh-token HMAC self-echo optional live |
| cross-server replication | `e2e_cross_server` | implemented; optional live with 2+ servers |
| soak / sustained two-client | `e2e_soak_two_clients` | implemented; optional slow live |
| `COMMAND_RECONNECT` load-shed | — | excluded: server-side trigger; client handling is covered by the offline `replication_test` fake-server unit test |
| server-notification inject; runtime-permission schema | — | excluded: internal (server-to-server / operator) surfaces, not part of the public client contract |

## SDK-managed data structures (no JS parity source; new coverage)

CrowdyCPP's WorldSession stores have optional live suites because they are the
layer a shipped game renders from. Default CI covers their deterministic state
transitions only through the named offline tests.

| Structure | CrowdyCPP suite |
|---|---|
| local actor loop + acks, remote-actor lanes, EventRouter, AndWait | `e2e_stores_live` |
| chunk cache: hydrate, optimistic edit + write-back, seed, prune, flush | `e2e_chunk_store_live` |
| SaveState / AvatarState stores, ContainerMirror (notify-to-pull), UUID persistence | `e2e_durable_mirror` |
| revisions, pending write-backs, dirty/save timestamps, error totals, local actor status, private avatar snapshots | `session_test` + `client_portable_test` |

## Game Kit (no JS e2e source; mirrors the `smoke-mmo` / `smoke-plots` samples)

Each optional live suite deploys the blueprint as admin, plays as a player, and
asserts that policy denials resolve as results (`success:false`), never
exceptions. Default CI compiles these targets but does not deploy them.

| Layer | CrowdyCPP suite |
|---|---|
| inventory | `e2e_kit_inventory` |
| lockable objects | `e2e_kit_objects` |
| plots (land + enforced grid grants) | `e2e_kit_plots` |
| NPCs / automations | `e2e_kit_npcs` |
| economy | `e2e_kit_economy` |
| progression | `e2e_kit_progression` |
| loot | `e2e_kit_loot` |
| quests | `e2e_kit_quests` |
| combat | `e2e_kit_combat` |
| matches (+ notify-to-pull) | `e2e_kit_matches` |
| decks (hidden information) | `e2e_kit_decks` |
| world sim | `e2e_kit_worldsim` |
| social (parties/guilds) | `e2e_kit_social` |
| leaderboards | `e2e_kit_leaderboards` |
| feature gates | `e2e_kit_features` |
| composite journey | `e2e_kit_journey` |

## Findings surfaced by writing these suites

Prior opt-in runs against a live deployment exposed the following contract
details. They are retained as observations and suite expectations, not as
claims that hosted CI revalidated them on every commit:

- **Game Kit blueprint selectors** — the combat status-effect tick and the
  worldsim/economy regen automations ship selector shapes the current
  automation resolver does not bind (`bindAs` ref without `pick`; `selfWhere`
  referencing `self.*`). These originate in the shared blueprint definitions
  (the C++ builders reproduce the JS builders exactly, so both SDKs carry
  them); the suites re-upsert corrected automations and the defect is flagged
  for a coordinated blueprint fix in both SDKs.
- **Admin/server-scope callers bypass invoke-policy conditions** — a
  server-scope owner call is not bound by a function's `condition` guard
  (e.g. the loot "no re-roll" guard). This is the documented authority model;
  the loot runtime doc comment is being updated to say so.
- **`gameApps().userPermissions` is admin-only** — players cannot read their
  own grid grants; post-grant verification in the plot suite goes through the
  owner client.
- **`createGrid` reports success via an `error: "NO_ERROR"` sentinel** rather
  than a null error field; the world-grid assignment lands lazily on first
  UDP touch.
- **Open-world default grants make grid-revoke denial deployment-dependent** —
  deployments that auto-grant a world-spanning grid on app access authorize
  entitled players everywhere, so revoking a narrow grid does not produce a
  durable-write denial. The voxel suite asserts the revoke round-trip and
  records the write verdict instead of forcing `FORBIDDEN`.
- **Host election favors the earliest still-fresh actor** — on a shared
  deployment a third party often holds the host, so the suite asserts
  convergence and `amIHost` consistency; the failover subtest runs only when
  one of the suite's own players wins the election.
- **`setSessionTurn` requires an app admin, the elected host, or the current
  turn holder** — a fresh session has no holder, so the admin seeds the first
  turn before players can pass it.
- **Reading a deleted channel throws `Group N not found`** rather than
  returning a soft-deleted record.
- **`gameApps().nearbyPermissions` is admin-only** (like `userPermissions`);
  effective-permission verification goes through the owner client.
- **Revocation semantics on the replication plane** — permission state is
  cached per session and re-pulled only when a send is denied, so revoking a
  player does not interrupt their live authorized session (it flows until the
  session goes stale). Once the session is reinstalled, the revoked user's
  token carries **no app scope at all**, so denials surface as
  `INVALID_APP_ID(18)` — not `UNAUTHORIZED(7)`, which is reserved for a held
  app scope missing a permission bit. A re-grant converges on a live denied
  session without reconnecting (each denial triggers a backoff-limited
  refresh). The management plane cuts over immediately: `mintAppToken` is
  refused as soon as the revoke lands.

These are captured here so the matrix stays honest about what the live
platform does, not only what the docs describe.
