#!/usr/bin/env bash
# Public content policy check: CrowdyCPP is a public repository. Nothing in it
# may reference private repositories, internal infrastructure, or internal
# server-to-server protocol details. CI fails if any denylisted term appears
# in tracked files.
set -euo pipefail

cd "$(dirname "$0")/.."

DENYLIST=(
  'cks-udp-api'
  'cks-michael-root'
  'cks-project-root'
  'MessageType\.hpp'
  'wire-protocol-reference'
  'P2P_SECRET'
  'P2P_TOKEN'
  'CHANNEL_MUTATION'
  'peer port'
  'port 9081'
  ':9081'
  'buddydev'
  'BUDDY_BUILDER'
  'dev-run-buddy'
)

# Schema snapshots are exempt: they are verbatim copies of the PUBLISHED SDLs
# (docs.crowdedkingdoms.com/schema/). Description fixes belong server-side,
# not in these deterministic validation inputs.
fail=0
for term in "${DENYLIST[@]}"; do
  # Search tracked files only; exclude this script and the published SDL snapshot.
  if hits=$(git grep -nIE --untracked "$term" -- \
      ':!scripts/check-content-policy.sh' \
      ':!schema.gql' \
      2>/dev/null); then
    echo "DENYLISTED TERM '$term' found:" >&2
    echo "$hits" >&2
    fail=1
  fi
done

if [[ $fail -ne 0 ]]; then
  echo "Content policy check FAILED — remove internal references before publishing." >&2
  exit 1
fi
echo "Content policy check passed."
