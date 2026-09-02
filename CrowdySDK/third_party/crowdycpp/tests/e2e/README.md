# CrowdyCPP end-to-end suites

Black-box e2e tests against a live Crowded Kingdoms deployment. Everything is
unprivileged: provisioning happens through the public API the way a
real integrator does it (owner sign-in -> access tier -> `grantAppAccess`) —
no database access anywhere. Replication traffic runs over **native UDP**
directly to the replication servers, so these suites exercise the real
shipping client path.

Coverage accounting lives in [docs/e2e-coverage.md](../../docs/e2e-coverage.md):
every scenario from the platform's other e2e suites is mapped to a CrowdyCPP
suite or carries an explicit exclusion reason.

## Configuration

| Variable | Required | Purpose |
|---|---|---|
| `CROWDY_E2E_API_URL` | yes | API base URL (shared entry origin). `CROWDY_E2E_MANAGEMENT_URL` is still read as a fallback — same origin now |
| `CROWDY_E2E_HTTP_URL` | no | per-game API base URL (falls back to the minted `gameApiUrl`) |
| `CROWDY_E2E_EMAIL` | yes | base email; suites derive fresh accounts by plus-addressing |
| `CROWDY_E2E_APP_ID` | yes | the app under test |
| `CROWDY_E2E_OWNER_EMAIL` | yes* | account with `manage_apps` + `manage_access_tiers` on the app (entitles players, deploys kit blueprints) |
| `CROWDY_E2E_APP_ID_2` | no | second app on the same deployment (cross-app isolation) |
| `CROWDY_E2E_OPERATOR_EMAIL` | no | `is_operator` account (operator read-only suite) |
| `CROWDY_E2E_MULTI_SERVER=1` | no | deployment runs 2+ replication servers (cross-server suite) |
| `CROWDY_E2E_CLAIM_CHUNK_X/Y/Z` | no | free decimal-string chunk coordinate for the marketplace claim suite; the app must use `SELF_CLAIM` |
| `CROWDY_E2E_STUDIO_GRID_ID` | no | owner-controlled grid used for Studio CRUD/patch/draft submission |
| `CROWDY_E2E_AGENT=1` | no | enable Agentic Studio ASK/BUILD session coverage |
| `CROWDY_E2E_AGENT_PROJECT_ID` | no | saved owner project used by the BUILD session |
| `CROWDY_E2E_AGENT_RUN=1` | no | send one ASK provider turn (the deployment owns provider credentials) |
| `CROWDY_E2E_AGENT_PLAY=1` | no | enable Play lease grant/takeover; also set controlled-entity and host-capability vars |
| `CROWDY_E2E_STUDIO_APPROVED_RESTORE_CAPABILITY=1` | no | assert that the host has wired an independently authorized synchronization + approval provider; the stock black-box executable fails rather than infer restore authority |
| `CROWDY_E2E_AGENT_POLICY_KILL=1` | no | explicitly allow a temporary operator app kill/release; requires operator email |
| `CROWDY_E2E_WEBSOCKET=1` | no | enable generic GraphQL-WS + typed container-feed live evidence; the client must have an injected or default transport |
| `CROWDY_E2E_SLOW=1` | no | enable long-running suites (soak, cache-TTL waits) |

\* a few legacy suites can run with pre-entitled fixed accounts
(`CROWDY_E2E_EMAIL`/`_EMAIL_2`) instead, but the owner unlocks everything.

The management server must run with `DEV_AUTH_BYPASS` (the suites sign in with
`devLogin`; the auth suite also exercises the magic-link dev-token flow).
Every test exits **77** (ctest `SKIP_RETURN_CODE`) when its required
variables are unset, so an unconfigured checkout reports skips, not failures.
Hosted CI intentionally has no live deployment credentials: it compiles these
targets and records exit-77 skips, not live platform passes.

## Running

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# default suites (fast; skip when unconfigured)
ctest --test-dir build -L e2e --output-on-failure

# long-running suites (soak, permission-cache TTL) — also needs CROWDY_E2E_SLOW=1
ctest --test-dir build -L e2e_slow --output-on-failure

# optional suites needing extra deployment features (operator account,
# multiple replication servers) — gated on their own env vars
ctest --test-dir build -L e2e_optional --output-on-failure
```

Run a single suite directly for its per-subtest output:

```bash
./build/tests/e2e/e2e_two_client_actor
```

## Labels

| Label | Suites | Gate |
|---|---|---|
| `e2e` | everything not below | env config |
| `e2e_slow` | `e2e_permission_refresh`, `e2e_soak_two_clients` | `CROWDY_E2E_SLOW=1` |
| `e2e_optional` | `e2e_agentic_studio`, `e2e_crowdy_studio`, `e2e_native_studio_integration`, `e2e_cross_server`, `e2e_graphql_websocket`, `e2e_marketplace_claims`, `e2e_operator` | explicit feature flag / project+grid+Play host / multi-server / WebSocket transport / claim coordinate / operator |

## Notes for reruns

- Suites derive fresh accounts (plus-addressed with a per-run suffix) and
  use unique kit blueprint prefixes, so back-to-back runs never collide on a
  shared app.
- Each suite owns a disjoint chunk-coordinate band (base
  `{100000..500000 + suite*100, 0, ...}`) so parallel suites don't cross
  spatial fan-out.
- `e2e_marketplace_claims` is opt-in because it temporarily owns a real chunk.
  It releases the grid before passing; choose a coordinate reserved for the
  test deployment and an app configured with `SELF_CLAIM`.
- `e2e_crowdy_studio` archives its unique project after submitting the exact
  saved revision as a draft player-compute version.
- `e2e_agentic_studio` never reads a provider key. `CROWDY_E2E_AGENT_RUN=1`
  asks the configured server-side provider to run. The suite uses the
  production controller factory for create/attach/replay/heartbeat and binds a
  fake native host to the live session epoch for takeover cancellation.
  Policy-kill coverage is a separate explicit opt-in and releases the kill
  before asserting.
- `e2e_native_studio_integration` uses the production
  `CrowdyClient::createCrowdyStudioIntegration` factory end to end. It creates
  and archives a disposable project, edits and saves through the in-memory
  editor plus explicit maintenance lane, attaches BUILD, dispatches a native
  Studio status tool, submits the exact saved draft, grants a Play lease, and
  verifies synchronous control-gate takeover. The published APIs do not advertise a
  generic approved-restore capability, so that live subtest is skipped unless
  an independent synchronization and approval provider is injected. Setting
  the capability assertion without such a provider fails closed.
- `e2e_graphql_websocket` exits 77 when the client has neither an injected nor
  compatible default WebSocket transport. With its explicit flag set,
  endpoint/protocol failures are failures and structured GraphQL details are
  printed.
- `assignServer failed: No available servers found` during connect is
  transient on small deployments (server-status heartbeats briefly lapse) and
  is absorbed by the harness's assignment retry — not a failure.
- On a busy shared deployment, `Connection::stats().hmacFailures` may be
  nonzero: a concurrent player's foreign-signed frame fanning into a shared
  chunk is correctly dropped by verification. Suites treat this as a
  diagnostic, asserting on their own traffic's integrity instead.