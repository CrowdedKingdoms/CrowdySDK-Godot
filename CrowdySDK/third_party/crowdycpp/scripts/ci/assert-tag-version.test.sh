#!/usr/bin/env bash
# Proves assert-tag-version.sh refuses, and — just as important — that it accepts.
#
# A check that always fails passes every refusal case, so both directions or
# neither is evidence. The missing-input cases are here because "could not read
# the version" is a third outcome that three gates in this org have quietly
# resolved to "pass"; these pin it to "fail".
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT="$HERE/assert-tag-version.sh"
[ -x "$SCRIPT" ] || { echo "not executable: $SCRIPT" >&2; exit 1; }

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
no() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

printf 'cmake_minimum_required(VERSION 3.22)\nproject(CrowdyCPP VERSION 0.25.0 LANGUAGES C CXX)\n' \
  >"$WORK/CMakeLists.txt"
printf 'cmake_minimum_required(VERSION 3.22)\nproject(Something Else)\n' \
  >"$WORK/no-version.txt"

# expect_refusal <name> <substring the message must contain> [args...]
expect_refusal() {
  local name=$1 want=$2 out rc
  shift 2
  out=$("$SCRIPT" "$@" 2>&1)
  rc=$?
  if [ $rc -eq 0 ]; then
    no "$name: expected refusal, got exit 0 and: $out"
  elif ! grep -qF -- "$want" <<<"$out"; then
    no "$name: refused (good) but message lacks '$want': $out"
  else
    ok "$name"
  fi
}

echo "assert-tag-version.sh"
echo
echo "refusals:"

expect_refusal "tag behind the tree -> refused" \
  "tag says v0.24.0 but the tree builds 0.25.0" v0.24.0 "$WORK/CMakeLists.txt"

expect_refusal "tag ahead of the tree -> refused" \
  "tag says v0.26.0 but the tree builds 0.25.0" v0.26.0 "$WORK/CMakeLists.txt"

expect_refusal "no tag version -> refused" \
  "no tag version given" "" "$WORK/CMakeLists.txt"

expect_refusal "malformed tag version -> refused" \
  "is not vMAJOR.MINOR.PATCH" v0.25 "$WORK/CMakeLists.txt"

# The two missing-input cases. Each one is a plausible "pass quietly" bug.
expect_refusal "unreadable CMakeLists -> refused, not passed" \
  "cannot read" v0.25.0 "$WORK/does-not-exist.txt"

expect_refusal "CMakeLists with no project version -> refused, not passed" \
  "no project(CrowdyCPP VERSION" v0.25.0 "$WORK/no-version.txt"

echo
echo "acceptances:"

out=$("$SCRIPT" v0.25.0 "$WORK/CMakeLists.txt" 2>&1)
if [ $? -ne 0 ]; then
  no "matching tag -> accepted: expected acceptance, got: $out"
elif ! grep -qx "version=0.25.0" <<<"$out"; then
  no "matching tag -> accepted: wrong output: $out"
else
  ok "matching tag -> accepted"
fi

# The real repository, with no explicit path: the argument default is the code path
# CI uses, and a fix to it would otherwise break the only caller with every test
# still green.
out=$(cd "$HERE/../.." && "$SCRIPT" "v$(sed -n 's/^project(CrowdyCPP VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -n1)" 2>&1)
if [ $? -ne 0 ]; then
  no "this repository's own version, default path -> accepted: got: $out"
else
  ok "this repository's own version, default path -> accepted"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
