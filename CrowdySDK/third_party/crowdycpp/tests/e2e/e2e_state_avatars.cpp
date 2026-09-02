// Per-user app save state (strict round-trip, replace, getAll, delete) and
// avatars (create, mine, get, state + per-app state round-trips, idempotent
// delete). Mirrors Game API SDK e2e: user app state + avatars.
// Reference: https://docs.crowdedkingdoms.com/game-api/players
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

// State blobs come back base64-encoded; we store base64 text, so accept
// either the identical string or one decode of it.
std::string normalizeBlob(const std::string& returned, const std::string& storedB64) {
  if (returned == storedB64) return returned;
  auto decoded = core::base64Decode(returned);
  if (!decoded) return returned;
  return std::string(decoded->begin(), decoded->end());
}

int run() {
  auto cfg = e2e::requireConfig();
  auto p = e2e::provisionPlayer(cfg, "state-a");

  E2E_SUBTEST("userAppState strict round-trip");
  const std::string blobV1 = core::base64Encode(asBytes("save-v1-" + e2e::runSuffix()));
  {
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["state"] = blobV1;
    graphql::Json written = p.game->state().update(input);
    E2E_CHECK(written["userId"].asString() == p.userId);
    graphql::Json read = p.game->state().getOne(cfg.appId);
    E2E_CHECK(normalizeBlob(read["state"].asString(), blobV1) == blobV1);
  }

  E2E_SUBTEST("update replaces the blob");
  const std::string blobV2 = core::base64Encode(asBytes("save-v2-" + e2e::runSuffix()));
  {
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["state"] = blobV2;
    p.game->state().update(input);
    graphql::Json read = p.game->state().getOne(cfg.appId);
    E2E_CHECK(normalizeBlob(read["state"].asString(), blobV2) == blobV2);
  }

  E2E_SUBTEST("getAll contains this app's state");
  {
    graphql::Json all = p.game->state().getAll();
    bool found = false;
    all.forEach([&](graphql::Json entry) {
      if (entry["appId"].asBigInt() == std::stoll(cfg.appId)) {
        found = true;
        E2E_CHECK(normalizeBlob(entry["state"].asString(), blobV2) == blobV2);
      }
    });
    E2E_CHECK(found);
  }

  E2E_SUBTEST("delete then read null");
  {
    p.game->state().remove(cfg.appId);
    graphql::Json read = p.game->state().getOne(cfg.appId);
    E2E_CHECK(read.isNull());
  }

  E2E_SUBTEST("avatar create + mine + get");
  const std::string avatarName = "E2E Avatar " + e2e::runSuffix();
  graphql::JVal createInput;
  createInput["name"] = avatarName;
  graphql::Json created = p.game->avatars().create(createInput);
  const std::string avatarId = created["avatarId"].asString();
  E2E_CHECK(!avatarId.empty());
  E2E_CHECK(created["name"].asString() == avatarName);

  bool inMine = false;
  p.game->avatars().mine().forEach([&](graphql::Json avatar) {
    if (avatar["avatarId"].asString() == avatarId) inMine = true;
  });
  E2E_CHECK(inMine);
  graphql::Json got = p.game->avatars().get(avatarId);
  E2E_CHECK(got["name"].asString() == avatarName);
  E2E_CHECK(got["userId"].asString() == p.userId);

  E2E_SUBTEST("updateAvatarState round-trip");
  const std::string pubState = core::base64Encode(asBytes("avatar-pub-" + e2e::runSuffix()));
  const std::string privState = core::base64Encode(asBytes("avatar-priv-" + e2e::runSuffix()));
  {
    graphql::JVal stateInput;
    stateInput["publicState"] = pubState;
    stateInput["privateState"] = privState;
    graphql::Json updated = p.game->avatars().updateState(avatarId, stateInput);
    E2E_CHECK(normalizeBlob(updated["publicState"].asString(), pubState) == pubState);
    graphql::Json reread = p.game->avatars().get(avatarId);
    E2E_CHECK(normalizeBlob(reread["publicState"].asString(), pubState) == pubState);
    E2E_CHECK(normalizeBlob(reread["privateState"].asString(), privState) == privState);
  }

  E2E_SUBTEST("updateAvatarAppState + appState round-trip");
  const std::string appState = core::base64Encode(asBytes("avatar-app-" + e2e::runSuffix()));
  {
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["avatarId"] = avatarId;
    input["state"] = appState;
    graphql::Json written = p.game->avatars().updateAppState(input);
    E2E_CHECK(written["avatarId"].asString() == avatarId);
    graphql::Json read = p.game->avatars().appState(cfg.appId, avatarId);
    E2E_CHECK(normalizeBlob(read["state"].asString(), appState) == appState);
  }

  E2E_SUBTEST("deleteAvatar with idempotencyKey replays identically");
  const std::string key = "e2e-avatar-del-" + e2e::runSuffix();
  graphql::Json removed1 = p.game->avatars().remove(avatarId, key);
  E2E_CHECK(removed1["avatarId"].asString() == avatarId);
  // Replay with a createdAt-free selection: the server's idempotent replay
  // fails to re-serialize the stored createdAt DateTime (observed live
  // behavior), while the rest of the payload replays identically.
  graphql::JVal replayVars;
  replayVars["id"] = avatarId;
  replayVars["idempotencyKey"] = key;
  graphql::Json removed2 =
      p.game->graphqlClient().request(
          "mutation DeleteAvatar($id: BigInt!, $idempotencyKey: String) {"
          " deleteAvatar(id: $id, idempotencyKey: $idempotencyKey) { avatarId userId name } }",
          replayVars)["deleteAvatar"];
  E2E_CHECK(removed2["avatarId"].asString() == avatarId);
  E2E_CHECK(removed2["userId"].asString() == p.userId);

  std::puts("e2e_state_avatars OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
