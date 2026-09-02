# CrowdyCPP release checklist

Run every required gate from a clean CrowdyCPP checkout. CrowdyJS must be the
exact target declared by `crowdyjsParityTarget` in `package.json`, built with
`npm ci && npm run build`, and exposed through `CROWDYJS_PATH`.

If the release moves the pin, do that first with
`npm run parity:repin -- --crowdyjs "$CROWDYJS_PATH"` (see
[`AGENTS.md`](../AGENTS.md)) — the CrowdyJS commit must already be fetchable
from the remote (CrowdyJS default branch is `prod`; the SHA must exist on
GitHub — typically after landing on `dev` and promoting).

Bumping the minor also means updating `project(CrowdyCPP VERSION ...)` in
`CMakeLists.txt`, the `find_package(CrowdyCPP <minor> ...)` request in
`tests/consumer/CMakeLists.txt`, `version` in `package.json`, the target
line in `README.md`, both `version` fields in `package-lock.json`, and the
version `docs/compatibility.md` attaches its parity claim to. The consumer test
is what proves the package config rejects a different `0.x` minor, so a stale
request there passes for the wrong reason.

`npm test` refuses when those six disagree
(`tests/parity/release-version-agreement.test.mjs`). That gate exists because
this item was on this list and was still missed at 0.15, 0.18 and 0.20 — every
time in the consumer fixture, and every time found by CI two minutes into a job
that first builds and installs the whole SDK. A stale request cannot fail
locally until a package of the new minor exists to reject it, which is why
reading the checklist was never enough. The last two sites were added at 0.25.0,
when the lock file was found six releases behind: a gate reading four of six
sites reports the other two as fine.

## Cut the tag — this is the step that reaches a consumer

**A release nobody can find is not a release.** CrowdyCPP is consumed as source,
so the tag IS the artifact. Everything above this line proves the tree is good;
none of it makes the tree reachable. 0.20.0, 0.21.0, 0.23.0 and 0.24.0 all
passed this checklist and none was ever tagged on the remote, so a consumer
looking for the newest release found `v0.19.0` and a developer re-reported a
defect that had been fixed five days earlier.

Tag the tier branch after the merge lands on it, one tag per tier:

```bash
git checkout dev && git pull
git tag -a "dev/v$(node -p "require('./package.json').version")" \
  -m "CrowdyCPP dev v$(node -p "require('./package.json').version")"
git push origin "dev/v$(node -p "require('./package.json').version")"
```

Annotated (`-a`), not lightweight: a lightweight tag carries no author, date or
message, and `dev/v0.22.0` — the one tier-prefixed tag that predates this
section — is lightweight and cannot say who cut it or when.

The push triggers [`release.yml`](../.github/workflows/release.yml), which
refuses a tag whose commit is not contained in the branch it names
(`scripts/ci/resolve-release-tier.sh`) and a tag whose version is not the
version the tree builds (`scripts/ci/assert-tag-version.sh`), then installs the
package and links `tests/consumer` against it before creating the release. Both
gates have self-tests that watch them refuse, and CI runs those on every branch
push, not only when a release is being cut.

## Content, codegen, and parity

```bash
npm ci
bash scripts/check-content-policy.sh
npm run check:codegen
npm run check:operations
node tools/parity/parity.mjs \
  --crowdyjs "$CROWDYJS_PATH" \
  --check docs/parity-matrix.md \
  --strict
node tools/parity/agent-fixtures.mjs --crowdyjs "$CROWDYJS_PATH"
node tools/parity/control-gate-fixtures.mjs --crowdyjs "$CROWDYJS_PATH"
node tools/parity/studio-host-fixtures.mjs --crowdyjs "$CROWDYJS_PATH"
node tools/parity/layout-fixtures.mjs --crowdyjs "$CROWDYJS_PATH"
node tools/parity/studio-state-fixtures.mjs --crowdyjs "$CROWDYJS_PATH"
npm test
```

The parity command above is the command used by CI. The tools reject tracked
CrowdyJS checkout changes, a same-version checkout at any commit other than
the package pin, or mismatched fixture target metadata.

Build and compare the exact pinned blueprints:

```bash
cmake -S . -B build-release-parity \
  -DCROWDY_BUILD_PARITY_TOOLS=ON \
  -DCROWDY_BUILD_TESTS=OFF \
  -DCROWDY_BUILD_E2E=OFF \
  -DCROWDY_BUILD_BENCHMARKS=OFF \
  -DCROWDY_BUILD_EXAMPLES=OFF
cmake --build build-release-parity --target crowdy_blueprint_dump -j
node tools/parity/dump-blueprints.mjs "$CROWDYJS_PATH" > /tmp/crowdyjs-blueprints.json
./build-release-parity/crowdy_blueprint_dump > /tmp/crowdycpp-blueprints.json
node tools/parity/blueprints-diff.mjs \
  /tmp/crowdyjs-blueprints.json /tmp/crowdycpp-blueprints.json
```

## Default build and tests

```bash
cmake -S . -B build-release-default -DCMAKE_BUILD_TYPE=Release
cmake --build build-release-default -j
ctest --test-dir build-release-default --output-on-failure
./build-release-default/examples/native_studio_shell
```

Unconfigured live e2e executables must report skipped through exit 77; they are
not default live release evidence. The native Studio shell is intentionally
offline and must run without credentials.

## Curl-free build and tests

```bash
cmake -S . -B build-release-no-curl \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDY_WITH_CURL=OFF \
  -DCROWDY_WITH_CURL_WEBSOCKETS=OFF \
  -DCROWDY_BUILD_E2E=OFF
cmake --build build-release-no-curl -j
ctest --test-dir build-release-no-curl --output-on-failure
```

## Non-throwing compatibility build and tests

```bash
cmake -S . -B build-release-no-exceptions \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDY_NO_EXCEPTIONS=ON \
  -DCROWDY_BUILD_E2E=OFF \
  -DCROWDY_BUILD_BENCHMARKS=OFF
cmake --build build-release-no-exceptions -j
ctest --test-dir build-release-no-exceptions --output-on-failure
```

Install this profile and verify the reduced package omits Compute authoring,
Crowdy Studio project API/models/controller, Agent/controller, player-host,
Game Kit, and `ContainerMirror` headers while the independent
`crowdy/studio/layout.hpp` surface remains installed and the umbrella consumer
still links. `no_exceptions_install_test` generates one translation unit for
every installed header, verifies `CROWDY_NO_EXCEPTIONS` is exported to each,
links the complete consumer, and runs it.

## OpenSSL-off library build

```bash
cmake -S . -B build-release-no-openssl \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDY_WITH_OPENSSL=OFF \
  -DCROWDY_BUILD_TESTS=OFF \
  -DCROWDY_BUILD_E2E=OFF \
  -DCROWDY_BUILD_BENCHMARKS=OFF \
  -DCROWDY_BUILD_EXAMPLES=OFF
cmake --build build-release-no-openssl -j
```

This verifies that consumers may supply `ICrypto`; tests that intentionally
link the packaged OpenSSL provider are disabled for this configuration.

## Public headers compile standalone

```bash
git ls-files -z 'include/crowdy/*.hpp' 'include/crowdy/**/*.hpp' |
while IFS= read -r -d '' header; do
  include="${header#include/}"
  printf '#include <%s>\nint main() { return 0; }\n' "$include" |
    c++ -std=c++20 -Iinclude -x c++ -fsyntax-only -
done
```

## Address and undefined-behavior sanitizers

```bash
cmake -S . -B build-release-sanitizers \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCROWDY_BUILD_E2E=OFF \
  -DCROWDY_BUILD_BENCHMARKS=OFF \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-release-sanitizers -j
ctest --test-dir build-release-sanitizers --output-on-failure
```

## Install and external consumer

```bash
cmake -S . -B build-release-install \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDY_BUILD_TESTS=OFF \
  -DCROWDY_BUILD_E2E=OFF \
  -DCROWDY_BUILD_BENCHMARKS=OFF \
  -DCROWDY_BUILD_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/build-release-prefix"
cmake --build build-release-install -j
cmake --install build-release-install
cmake -S tests/consumer -B build-release-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build-release-prefix"
cmake --build build-release-consumer -j
./build-release-consumer/crowdycpp_consumer
```

The package version file accepts compatible patches within the requested
minor, but must reject a different `0.x` minor.

## Optional live e2e evidence

Only run these against an explicitly selected test deployment. Configure the
variables in [`tests/e2e/README.md`](../tests/e2e/README.md); WebSocket evidence
also requires `CROWDY_E2E_WEBSOCKET=1` and a default or injected transport.

```bash
ctest --test-dir build-release-default -L e2e --output-on-failure
ctest --test-dir build-release-default -L e2e_slow --output-on-failure
ctest --test-dir build-release-default -L e2e_optional --output-on-failure
ctest --test-dir build-release-default \
  -R e2e_native_studio_integration --output-on-failure
```

Record passes, failures, and exit-77 skips separately. A skipped live suite is
not evidence that its platform round trip passed. Approved restore is live
evidence only when an independently authorized synchronization provider and
exact approval gate are injected; never treat ordinary project-save access as
that capability.
