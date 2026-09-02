#!/usr/bin/env bash
# Refuse a release tag whose version is not the version the tree builds.
#
# resolve-release-tier.sh answers "does this tag's COMMIT belong to the branch the
# tag names". It says nothing about whether the tag's NUMBER is the number that
# commit compiles into, and those come apart in the obvious way: bump the version,
# forget to tag, then tag the next release from a tree that still says the old one.
# CrowdyCPP has the receipts — v0.20.0, v0.21.0, v0.23.0 and v0.24.0 were all built
# and none was ever tagged on the remote, so the newest tag claimed a release four
# versions behind the branch.
#
# The four in-tree sites are already held together by
# tests/parity/release-version-agreement.test.mjs, so comparing the tag against
# CMakeLists.txt compares it against all of them.
#
# WHEN THE INPUT IS MISSING THIS FAILS. Not being able to read the version is a
# third outcome, and the tempting one to treat as a pass is the one that lets a
# release out unchecked.
#
# Usage: scripts/ci/assert-tag-version.sh v0.25.0 [path/to/CMakeLists.txt]
# Run scripts/ci/assert-tag-version.test.sh to see it refuse.
set -euo pipefail

TAG_VERSION=${1-}
CMAKELISTS=${2-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/CMakeLists.txt}

die() {
  echo "::error::$*" >&2
  exit 1
}

[ -n "$TAG_VERSION" ] || die "no tag version given (argument 1, e.g. v0.25.0)"
[ -r "$CMAKELISTS" ] || die "cannot read $CMAKELISTS, so the built version is unknown; refusing rather than assuming the tag is right"

case "$TAG_VERSION" in
  v[0-9]*.[0-9]*.[0-9]*) ;;
  *) die "tag version '$TAG_VERSION' is not vMAJOR.MINOR.PATCH" ;;
esac

project_version=$(sed -n 's/^project(CrowdyCPP VERSION \([0-9][0-9.]*\).*/\1/p' "$CMAKELISTS" | head -n1)
[ -n "$project_version" ] || die "no project(CrowdyCPP VERSION ...) line in $CMAKELISTS; refusing rather than passing on an unreadable version"

if [ "${TAG_VERSION#v}" != "$project_version" ]; then
  die "tag says ${TAG_VERSION} but the tree builds ${project_version}. Bump the version and merge it to the tier branch before tagging, or tag v${project_version}."
fi

echo "version=$project_version"
