// Player-authorized marketplace claim flow: an app-scoped game client claims
// one free chunk under SELF_CLAIM and releases the exact owner-created grid.
// The coordinate is deployment-specific and opt-in so shared environments are
// never mutated accidentally.
#include "e2e_util.hpp"

using namespace crowdy;

int main() try {
  auto cfg = e2e::requireConfig();
  const std::string x = e2e::envOr("CROWDY_E2E_CLAIM_CHUNK_X");
  const std::string y = e2e::envOr("CROWDY_E2E_CLAIM_CHUNK_Y");
  const std::string z = e2e::envOr("CROWDY_E2E_CLAIM_CHUNK_Z");
  if (x.empty() || y.empty() || z.empty()) {
    std::puts("CROWDY_E2E_CLAIM_CHUNK_{X,Y,Z} not configured; skipping");
    return 77;
  }

  auto player = e2e::signIn(cfg, cfg.email);
  graphql::JVal claimVariables;
  claimVariables["appId"] = cfg.appId;
  claimVariables["chunk"]["x"] = x;
  claimVariables["chunk"]["y"] = y;
  claimVariables["chunk"]["z"] = z;
  graphql::Json claimed =
      player.game->marketplace().claimGridChunk(claimVariables);

  const std::string gridId = claimed["gridId"].asString();
  const bool claimShape =
      !gridId.empty() && claimed["lowChunk"]["x"].asString() == x &&
      claimed["lowChunk"]["y"].asString() == y &&
      claimed["lowChunk"]["z"].asString() == z &&
      claimed["highChunk"]["x"].asString() == x &&
      claimed["highChunk"]["y"].asString() == y &&
      claimed["highChunk"]["z"].asString() == z &&
      claimed["policy"].asString() == "SELF_CLAIM" &&
      claimed["ownership"]["ownerRef"].asString() == player.userId;

  graphql::Json released =
      player.game->marketplace().releaseClaimedGrid(cfg.appId, gridId);
  E2E_CHECK(claimShape);
  E2E_CHECK(released["gridId"].asString() == gridId);
  E2E_CHECK(released["lowChunk"]["x"].asString() == x);
  E2E_CHECK(released["highChunk"]["z"].asString() == z);
  E2E_CHECK(released["policy"].asString() == "SELF_CLAIM");
  E2E_CHECK(released["released"].asBool());

  std::puts("e2e_marketplace_claims OK");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "exception: %s\n", error.what());
  return 1;
}
