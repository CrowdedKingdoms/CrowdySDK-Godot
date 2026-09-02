// Gamer journey: sign-in -> app token -> bootstrap -> profile -> save state
// -> avatars -> host heartbeat -> token refresh. Mirrors CrowdyJS's
// gamer-journey e2e suite over the GraphQL surface.
#include "e2e_util.hpp"

using namespace crowdy;

int main() {
  auto cfg = e2e::requireConfig();
  auto p = e2e::signIn(cfg, cfg.email);

  // Bootstrap: capability + spatial limits for this app.
  graphql::Json bootstrap = p.game->serverStatus().gameClientBootstrap(cfg.appId);
  E2E_CHECK(bootstrap["maxReplicationDistance"].asInt64() >= 1);
  E2E_CHECK(bootstrap["sequenceNumberModulo"].asInt64() == 256);
  E2E_CHECK(!bootstrap["me"]["userId"].asString().empty());

  // Profile on the identity plane.
  graphql::Json me = p.identity->users().me();
  E2E_CHECK(me["userId"].asString() == p.userId);

  // Save state: write + read back the per-user per-app blob. Servers return
  // the stored bytes base64-encoded; accept either identity or one decode.
  const std::string savedBlob = core::base64Encode(asBytes("e2e-save-v1"));
  graphql::JVal stateInput;
  stateInput["appId"] = cfg.appId;
  stateInput["state"] = savedBlob;
  p.game->state().update(stateInput);
  graphql::Json read = p.game->state().getOne(cfg.appId);
  std::string roundTripped = read["state"].asString();
  if (roundTripped != savedBlob) {
    auto decoded = core::base64Decode(roundTripped);
    E2E_CHECK(decoded.has_value());
    roundTripped = std::string(decoded->begin(), decoded->end());
  }
  E2E_CHECK(roundTripped == savedBlob);

  // Avatars: create then list.
  graphql::JVal avatarInput;
  avatarInput["name"] = "E2E Avatar";
  graphql::Json created = p.game->avatars().create(avatarInput);
  const std::string avatarId = created["id"].asString(created["avatarId"].asString());
  E2E_CHECK(!avatarId.empty());
  graphql::Json mine = p.game->avatars().mine();
  E2E_CHECK(mine.size() >= 1);

  // Host election: heartbeat keeps us eligible and returns the current host.
  graphql::Json host = p.game->host().heartbeat(cfg.appId);
  E2E_CHECK(host.ok());
  (void)p.game->host().amIHost(cfg.appId);

  // Token refresh keeps the same app scope.
  auto refreshed = p.game->portal().refresh();
  E2E_CHECK(refreshed.token.size() == 64);
  E2E_CHECK(refreshed.appId == p.appToken.appId);

  std::puts("e2e_gamer_journey OK");
  return 0;
}
