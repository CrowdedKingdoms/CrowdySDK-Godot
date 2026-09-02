// Two-client actor fan-out over native UDP: player A joins a chunk and sends
// actor updates; player B (in the same chunk) receives them, and vice versa.
// Also checks self-echo and voxel fan-out. Mirrors CrowdyJS's
// two-client-actor / two-client-voxel e2e suites on the native transport.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

int main() {
  auto cfg = e2e::requireConfig(/*needSecondPlayer=*/true);
  auto a = e2e::signIn(cfg, cfg.email);
  auto b = e2e::signIn(cfg, cfg.email2);
  e2e::connectUdp(a, cfg);
  e2e::connectUdp(b, cfg);

  const auto uuidA = core::generateActorUuid();
  const auto uuidB = core::generateActorUuid();
  const wire::ChunkCoord chunk{1000, 0, 1000};  // away from other traffic

  std::atomic<int> bSawA{0}, aSawB{0}, aSelfEcho{0}, bSawVoxel{0};

  Handlers ha;
  ha.actorUpdate = [&](const SpatialNotification& n) {
    if (std::memcmp(n.uuid, uuidB.data(), 32) == 0) ++aSawB;
    if (std::memcmp(n.uuid, uuidA.data(), 32) == 0) ++aSelfEcho;
  };
  a.conn->setHandlers(std::move(ha));

  Handlers hb;
  hb.actorUpdate = [&](const SpatialNotification& n) {
    if (std::memcmp(n.uuid, uuidA.data(), 32) == 0) ++bSawA;
  };
  hb.voxelUpdate = [&](const SpatialNotification&, const wire::VoxelPayloadView& v) {
    if (v.voxelType == 77) ++bSawVoxel;
  };
  b.conn->setHandlers(std::move(hb));

  // Both join the same chunk, then keep sending pose updates.
  const std::uint8_t poseA[] = {0xa1, 0xa2, 0xa3};
  const std::uint8_t poseB[] = {0xb1, 0xb2};
  auto sendBoth = [&] {
    E2E_CHECK(a.conn
                  ->sendActorUpdate({.chunk = chunk,
                                     .uuid = uuidA,
                                     .payload = Bytes(poseA, sizeof(poseA)),
                                     .distance = 8})
                  .ok());
    E2E_CHECK(b.conn
                  ->sendActorUpdate({.chunk = chunk,
                                     .uuid = uuidB,
                                     .payload = Bytes(poseB, sizeof(poseB)),
                                     .distance = 8})
                  .ok());
  };

  bool crossSeen = false;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(15)) {
    sendBoth();
    a.conn->poll();
    b.conn->poll();
    if (bSawA > 0 && aSawB > 0) {
      crossSeen = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  E2E_CHECK(crossSeen);

  // Voxel fan-out from A to B.
  const std::uint8_t voxelMeta[] = {1};
  E2E_CHECK(a.conn
                ->sendVoxelUpdate(chunk, uuidA, 1, 2, 3, 77, Bytes(voxelMeta, sizeof(voxelMeta)),
                                  8)
                .ok());
  bool voxelSeen = e2e::pollUntil(*b.conn, [&] { return bSawVoxel.load() > 0; });
  E2E_CHECK(voxelSeen);

  std::printf("fan-out ok (bSawA=%d aSawB=%d selfEcho=%d voxel=%d)\n", bSawA.load(), aSawB.load(),
              aSelfEcho.load(), bSawVoxel.load());
  E2E_CHECK(a.conn->stats().hmacFailures == 0);
  E2E_CHECK(b.conn->stats().hmacFailures == 0);

  a.conn->disconnect();
  b.conn->disconnect();
  std::puts("e2e_two_client_actor OK");
  return 0;
}
