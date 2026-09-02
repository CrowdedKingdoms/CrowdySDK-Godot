// Replication API behavior: spatial fan-out and directed messages span
// replication servers transparently — a client only ever talks to its own
// assigned server, and the platform propagates presence/traffic across
// servers. OPTIONAL suite (needs a deployment with 2+ replication servers) —
// gated on CROWDY_E2E_MULTI_SERVER.
#include <atomic>
#include <cstring>
#include <vector>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

int main() try {
  e2e::requireFlag("CROWDY_E2E_MULTI_SERVER");
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  // Provision players until two land on different server assignments.
  std::vector<std::unique_ptr<e2e::Player>> players;
  e2e::Player* pa = nullptr;
  e2e::Player* pb = nullptr;
  for (int i = 0; i < 8 && !(pa && pb); ++i) {
    auto pl = std::make_unique<e2e::Player>(
        e2e::provisionPlayer(cfg, "xserver-" + std::to_string(i)));
    e2e::connectUdp(*pl, cfg);
    const std::string ip = pl->conn->assignment().ip4;
    if (!pa) {
      pa = pl.get();
    } else if (pl->conn->assignment().ip4 != pa->conn->assignment().ip4) {
      pb = pl.get();
    }
    players.push_back(std::move(pl));
  }
  if (!pb) {
    std::puts("could not obtain two distinct server assignments; skipping");
    return 77;
  }
  std::printf("player A on %s, player B on %s\n", pa->conn->assignment().ip4.c_str(),
              pb->conn->assignment().ip4.c_str());

  const wire::ChunkCoord chunk{100700, 0, 100700};
  E2E_CHECK(e2e::warmUp(*pa->conn, chunk));
  E2E_CHECK(e2e::warmUp(*pb->conn, chunk));

  const auto uuidA = core::generateActorUuid();
  const auto uuidB = core::generateActorUuid();
  std::atomic<int> bSawA{0}, aSawB{0}, bDirect{0};
  Handlers ha;
  ha.actorUpdate = [&](const SpatialNotification& n) {
    if (std::memcmp(n.uuid, uuidB.data(), 32) == 0) ++aSawB;
  };
  pa->conn->setHandlers(std::move(ha));
  Handlers hb;
  hb.actorUpdate = [&](const SpatialNotification& n) {
    if (std::memcmp(n.uuid, uuidA.data(), 32) == 0) ++bSawA;
  };
  hb.singleActorMessage = [&](const SpatialNotification& n) {
    if (asStringView(n.payload) == "cross-server dm") ++bDirect;
  };
  pb->conn->setHandlers(std::move(hb));

  E2E_SUBTEST("actor fan-out crosses servers in both directions");
  const std::uint8_t poseA[] = {0xaa};
  const std::uint8_t poseB[] = {0xbb};
  const bool crossed = e2e::retryUntil(
      [&] {
        pa->conn->sendActorUpdate({chunk, uuidA, Bytes(poseA, 1), 8});
        pb->conn->sendActorUpdate({chunk, uuidB, Bytes(poseB, 1), 8});
      },
      [&] {
        pa->conn->poll();
        pb->conn->poll();
        return bSawA.load() > 0 && aSawB.load() > 0;
      },
      /*attempts=*/60, /*perWaitMs=*/500);
  E2E_CHECK(crossed);

  E2E_SUBTEST("single-actor message crosses servers");
  const bool dm = e2e::retryUntil(
      [&] {
        // Keep B's presence fresh, then address B by its chunk + uuid.
        pb->conn->sendActorUpdate({chunk, uuidB, Bytes(poseB, 1), 8});
        pa->conn->sendSingleActorMessage(chunk, uuidB, asBytes("cross-server dm"));
      },
      [&] {
        pb->conn->poll();
        return bDirect.load() > 0;
      },
      /*attempts=*/60, /*perWaitMs=*/500);
  E2E_CHECK(dm);

  for (auto& pl : players) pl->conn->disconnect();
  std::puts("e2e_cross_server OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
