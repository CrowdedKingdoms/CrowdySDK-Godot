# CrowdyCPP agent guidance

CrowdyCPP is a standalone public C++20 SDK. A normal configure, build, install,
or unit-test run must not require network access, Node, CrowdyJS, or private
platform repositories. Schema and generated artifacts are committed.

GitHub default is **`prod`**. Work lands on `dev`. Parity pin is CrowdyJS
**14.1.0**. One GraphQL origin since 0.20.0 (`managementUrl` removed). Gameplay
is PostgreSQL + Citus, not galaxy. `cks-management-api` is not a running
service (GitHub repo still exists, unarchived).

## Public-surface maintenance

When the unified GraphQL surface changes:

1. Run `npm ci` for maintainer-only schema tooling.
2. Refresh `schema.gql` with `scripts/schema-sync.mjs` from the published
   unified SDL (`/schema/game-api.graphql`). The management docs SDL is
   derived from that schema; do not sync a second endpoint.
3. Run `node scripts/codegen.mjs`; commit `schema.gql`,
   `include/crowdy/generated/enums.hpp`, and
   `include/crowdy/generated/operations.hpp` together.
4. Run `node scripts/codegen.mjs --check`.
5. Compare the reviewed CrowdyJS checkout with
   `node tools/parity/parity.mjs --crowdyjs <path> --write
   docs/parity-matrix.md`, then run the same command with `--check`.
6. If agent contracts changed, build CrowdyJS and run
   `node tools/parity/agent-fixtures.mjs --crowdyjs <path>`. Use `--write`
   only for an intentional coordinated fixture update.
7. If portable Studio layout changed, build CrowdyJS and run
   `node tools/parity/layout-fixtures.mjs --crowdyjs <path>`. Use `--write`
   only for an intentional coordinated fixture update.
8. If player-control or native Studio host behavior changed, run
   `control-gate-fixtures.mjs` and `studio-host-fixtures.mjs` the same way.
   Both exact fixtures must be replayed by their focused C++ tests.
9. Run the blueprint structural gate documented in `README.md`.

## Releasing

**Cut the tag.** This SDK has no registry — consumers clone a ref and
`cmake --install` it — so the tag IS the artifact, and a release nobody can pin
is not a release. From 0.20.0 to 0.24.0 this repo had no release path at all,
four versions shipped untagged, and an SDK author re-reported a defect that had
been fixed five days earlier because the newest tag was `v0.19.0`.

Work through [`docs/release-checklist.md`](docs/release-checklist.md), then tag
the tier branch after the merge lands on it:

```bash
git checkout dev && git pull
V="v$(node -p "require('./package.json').version")"
git tag -a "dev/$V" -m "CrowdyCPP dev $V" && git push origin "dev/$V"
```

Annotated, and tier-prefixed. The push runs
[`.github/workflows/release.yml`](.github/workflows/release.yml), whose guard
refuses a tag outside the branch it names and a tag that is not the version the
tree builds, before installing the package and linking `tests/consumer` against
it. Both gates have self-tests and CI runs them on every branch push.

The version lives in **six** places and `npm test` refuses when they disagree.
Two of them — `package-lock.json` and `docs/compatibility.md` — were added to
that gate at 0.25.0, after the lock file was found six releases stale.

## Moving the CrowdyJS pin

The CrowdyJS commit in `package.json` is deliberately pinned and consumed by
CI, which checks out that exact commit. Update the target, the generated
matrix, and every fixture in one reviewed change:

```bash
npm run parity:repin -- --crowdyjs /path/to/CrowdyJS   # built, and clean
rg -l '<old-commit-prefix>' tests docs                 # literal assertions
npm run check:release
```

`parity:repin` rewrites the pin and reruns all five fixture generators plus the
matrix; it prints the steps it cannot do for you. Four things reliably bite
when this is done by hand:

- **The two repos have a merge order.** A CrowdyCPP change that mirrors new
  CrowdyJS behavior cannot go green until that CrowdyJS commit is fetchable
  from GitHub (CI checks out the pinned SHA). CrowdyJS's default branch is
  **`prod`**; land the commit on `dev` and promote so the SHA exists remotely,
  then re-pin here. A pin pointing at an unpushed commit fails at checkout.
- **`parity.mjs` embeds its own gate mode**, so a matrix written without
  `--strict` never satisfies the `--strict` check CI runs. Write and check with
  the same flags. `parity:repin` always writes the strict form.
- **Reconfigure existing CMake build dirs after touching a fixture.** The agent
  and Studio-layout fixtures are embedded into headers by `configure_file`, and
  the JSONs are registered in `CMAKE_CONFIGURE_DEPENDS` so a rebuild picks them
  up — but a build tree configured before that was added will keep serving a
  stale header and fail the replay tests against a file you just fixed.
- **Some fixtures assert the pin literally** in C++ tests and
  `docs/parity-matrix.md`. The `rg` above is how you find them.

Parity classifications are strict:

- `portable gap` means missing implementation that remains visible and must be
  removed as work lands;
- `native equivalent` is allowed only when CrowdyCPP provides the same contract
  through its native architecture;
- `browser exclusion` is allowed only for browser/UI/worker-specific behavior.

Do not label planned native WebSockets, Crowdy Studio, agent control, leases, or
player-host work as browser-only. New differences and stale classifications
must fail the baseline gate. `parity.mjs --strict` must pass before declaring
strict portable parity complete.

## Game Kit blueprints

Kit blueprint builders must emit byte-identical JSON to CrowdyJS's, which the
structural gate in `README.md` enforces by diffing dumps of a variant matrix.
Both matrices (`tools/parity/dump-blueprints.mjs` and
`tools/parity/dump_blueprints.cpp`) must list the same variants, so adding a
builder option means adding a variant to both or the gate never covers it.

Two things about blueprint content, both learned from live deploys rather than
from the gate, which only compares the two SDKs to each other:

- **Declare a function before anything references it.** `gameModelSeed`
  processes functions in array order, so a `timers` effect naming a function
  defined later in the array warns about an unresolved target.
- **Expressions have no conditional and no `now()`.** Guards have to be
  arithmetic — `matchesBlueprint`'s turn deadline keeps a monotonic sequence
  and compares it rather than branching, which is what makes a timer that
  fires after its turn ended harmless.

## Writing tests against `graphql::Json`

Index arrays with `at(i)`. `json[0]` is now a compile error: `operator[]` takes
a `string_view`, and the literal `0` used to bind to it as a null pointer and
segfault in `strlen` at runtime.
