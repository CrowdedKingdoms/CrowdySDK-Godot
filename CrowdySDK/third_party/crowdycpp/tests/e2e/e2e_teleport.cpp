// Teleport requests: an entitled player (teleport permission on their access
// tier) requests a teleport for their live actor to a destination chunk and
// gets a success result. Mirrors Game API SDK e2e: teleport.
// Reference: https://docs.crowdedkingdoms.com/game-api/teleport
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

int run() {
  auto cfg = e2e::requireConfig();
  auto p = e2e::provisionPlayer(cfg, "teleport-a");

  // Teleport addresses a live actor, so join presence over native UDP first.
  // warmUp loads the per-chunk permission window (the documented first-chunk
  // denial) and confirms presence is accepted before we teleport.
  e2e::connectUdp(p, cfg);
  const auto uuid = core::generateActorUuid();
  const wire::ChunkCoord origin{200500, 0, 200500};
  const std::uint8_t pose[] = {1};
  E2E_CHECK(e2e::warmUp(*p.conn, origin));
  auto joined = p.conn->sendActorUpdateAndWait({origin, uuid, Bytes(pose, 1), 8});
  E2E_CHECK(joined.acknowledged);

  E2E_SUBTEST("teleportRequest to a destination chunk succeeds");
  const domains::ChunkRef destination{200501, 0, 200500};
  graphql::JVal input;
  input["appId"] = cfg.appId;
  input["chunkAddress"] = destination.toInput();
  input["voxelAddress"]["x"] = std::int64_t{4};
  input["voxelAddress"]["y"] = std::int64_t{5};
  input["voxelAddress"]["z"] = std::int64_t{6};
  input["uuid"] = core::toString(uuid);
  graphql::Json result = p.game->teleport().request(input);
  E2E_CHECK(result["success"].asBool());
  E2E_CHECK(result["errorCode"].isNull() || result["errorCode"].asString() == "NO_ERROR");

  p.conn->disconnect();
  std::puts("e2e_teleport OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
