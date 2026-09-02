// Durable actor records CRUD: create / get / update / updateState / list /
// listConnection / batchLookup / remove (idempotent replay) against a live
// deployment. Mirrors Game API SDK e2e: actors CRUD.
// Reference: https://docs.crowdedkingdoms.com/game-api/actors
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

// Servers return stored state blobs base64-encoded; we store base64 text, so
// accept either the identical string or one decode of it.
bool blobMatches(const std::string& returned, const std::string& storedB64) {
  if (returned == storedB64) return true;
  auto decoded = core::base64Decode(returned);
  return decoded && std::string(decoded->begin(), decoded->end()) == storedB64;
}

int run() {
  auto cfg = e2e::requireConfig();
  auto p = e2e::provisionPlayer(cfg, "actors-a");

  const domains::ChunkRef home{200100, 0, 200100};
  const domains::ChunkRef moved{200101, 0, 200100};
  const std::string uuid = core::toString(core::generateActorUuid());
  const std::string pubV1 = core::base64Encode(asBytes("actor-pub-v1"));
  const std::string privV1 = core::base64Encode(asBytes("actor-priv-v1"));
  const std::string pubV2 = core::base64Encode(asBytes("actor-pub-v2"));

  // A dedicated avatar scopes the list/listConnection filters to exactly this
  // run's actor (the avatarId filter is the run-scoped filter that works for
  // 32-char wire actor ids).
  graphql::JVal avatarInput;
  avatarInput["name"] = "E2E Actors " + e2e::runSuffix();
  const std::string avatarId = p.game->avatars().create(avatarInput)["avatarId"].asString();
  E2E_CHECK(!avatarId.empty());

  E2E_SUBTEST("create");
  graphql::JVal createInput;
  createInput["appId"] = cfg.appId;
  createInput["uuid"] = uuid;
  createInput["avatarId"] = avatarId;
  createInput["chunk"] = home.toInput();
  createInput["publicState"] = pubV1;
  createInput["privateState"] = privV1;
  graphql::Json created = p.game->actors().create(createInput);
  E2E_CHECK(created["uuid"].asString() == uuid);
  E2E_CHECK(created["userId"].asString() == p.userId);
  E2E_CHECK(created["chunk"]["x"].asBigInt() == home.x);

  E2E_SUBTEST("get");
  graphql::Json got = p.game->actors().get(uuid);
  E2E_CHECK(got["uuid"].asString() == uuid);
  E2E_CHECK(got["appId"].asBigInt() == std::stoll(cfg.appId));
  E2E_CHECK(blobMatches(got["publicState"].asString(), pubV1));
  E2E_CHECK(blobMatches(got["privateState"].asString(), privV1));

  E2E_SUBTEST("update (move chunk)");
  graphql::JVal updateInput;
  updateInput["chunk"] = moved.toInput();
  graphql::Json updated = p.game->actors().update(uuid, updateInput);
  E2E_CHECK(updated["chunk"]["x"].asBigInt() == moved.x);
  E2E_CHECK(updated["chunk"]["z"].asBigInt() == moved.z);

  E2E_SUBTEST("updateState round-trip");
  graphql::JVal stateInput;
  stateInput["publicState"] = pubV2;
  graphql::Json stated = p.game->actors().updateState(uuid, stateInput);
  E2E_CHECK(blobMatches(stated["publicState"].asString(), pubV2));
  // Private state untouched by a public-only update.
  E2E_CHECK(blobMatches(p.game->actors().get(uuid)["privateState"].asString(), privV1));

  E2E_SUBTEST("list with filter");
  graphql::JVal filter;
  filter["appId"] = cfg.appId;
  filter["avatarId"] = avatarId;
  graphql::Json listed = p.game->actors().list(filter);
  E2E_CHECK(listed.size() == 1);
  E2E_CHECK(listed.at(0)["uuid"].asString() == uuid);

  E2E_SUBTEST("listConnection pagination shape");
  graphql::Json page = p.game->actors().listConnection(10, {}, filter);
  E2E_CHECK(page["totalCount"].asInt64() >= 1);
  E2E_CHECK(page["edges"].size() >= 1);
  bool inPage = false;
  page["edges"].forEach([&](graphql::Json edge) {
    if (edge["node"]["uuid"].asString() == uuid) inPage = true;
    E2E_CHECK(!edge["cursor"].asString().empty());
  });
  E2E_CHECK(inPage);

  E2E_SUBTEST("batchLookup");
  graphql::Json batch = p.game->actors().batchLookup({uuid});
  E2E_CHECK(batch.size() == 1);
  E2E_CHECK(batch.at(0)["uuid"].asString() == uuid);

  E2E_SUBTEST("remove with idempotencyKey replays identically");
  const std::string key = "e2e-actors-del-" + e2e::runSuffix();
  graphql::Json removed1 = p.game->actors().remove(uuid, key);
  E2E_CHECK(removed1["uuid"].asString() == uuid);
  // Replay: same key, same input -> the first result, not a NOT_FOUND.
  graphql::Json removed2 = p.game->actors().remove(uuid, key);
  E2E_CHECK(removed2["uuid"].asString() == removed1["uuid"].asString());
  E2E_CHECK(removed2["userId"].asString() == removed1["userId"].asString());

  // Gone from reads afterwards: actor(uuid) throws a coded error for a
  // deleted record (observed live behavior).
  bool goneThrew = false;
  try {
    p.game->actors().get(uuid);
  } catch (const graphql::CrowdyGraphQLError& e) {
    goneThrew = true;
    E2E_CHECK(!e.code().empty());
  }
  E2E_CHECK(goneThrew);

  p.game->avatars().remove(avatarId, "e2e-actors-avatar-del-" + e2e::runSuffix());

  std::puts("e2e_actors_crud OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
