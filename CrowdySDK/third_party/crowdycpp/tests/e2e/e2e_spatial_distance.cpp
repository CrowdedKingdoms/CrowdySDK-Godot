// Replication API behavior: the per-send `distance` selects the Chebyshev
// ring of chunks (0-8) that receives the fan-out, and values above 8 are
// clamped server-side, so nothing 9+ chunks away can ever be reached.
// See https://docs.crowdedkingdoms.com/replication-api/operations and
// https://docs.crowdedkingdoms.com/replication-api/wire-formats.
//
// Layout: sender at chunk C, receiver1 at Chebyshev distance 1, receiver2 at
// distance 8. A distance=8 send reaches both; a distance=1 send reaches only
// receiver1; a distance=9 send from a chunk 9 away from both receivers
// (clamped to 8) reaches neither. Payload marker bytes distinguish sends;
// negative assertions require zero matching deliveries across an observation
// window during which positive markers keep flowing.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

constexpr std::uint8_t kMarkWide = 0xd8;   // distance=8 from C: reaches r1 + r2
constexpr std::uint8_t kMarkNear = 0xd1;   // distance=1 from C: reaches r1 only
constexpr std::uint8_t kMarkClamp = 0xc9;  // distance=9 from 9 away: reaches nobody

struct MarkCounts {
  std::atomic<int> wide{0}, near{0}, clamp{0};
};

int run() {
  auto cfg = e2e::requireConfig();
  auto s = e2e::provisionPlayer(cfg, "dist-s");
  auto r1 = e2e::provisionPlayer(cfg, "dist-r1");
  auto r2 = e2e::provisionPlayer(cfg, "dist-r2");
  e2e::connectUdp(s, cfg);
  e2e::connectUdp(r1, cfg);
  e2e::connectUdp(r2, cfg);

  const auto uuidS = core::generateActorUuid();
  const auto uuidR1 = core::generateActorUuid();
  const auto uuidR2 = core::generateActorUuid();

  const wire::ChunkCoord chunkC{100300, 0, 100300};       // suite 3 base
  const wire::ChunkCoord chunkR1{100301, 0, 100300};      // Chebyshev 1 from C
  const wire::ChunkCoord chunkR2{100308, 0, 100300};      // Chebyshev 8 from C
  const wire::ChunkCoord chunkFar{100300, 9, 100300};     // Chebyshev 9 from r1 AND r2

  // Load each sender's per-chunk permission window before the assertions so
  // the documented first-chunk denial doesn't skew delivery counts.
  E2E_CHECK(e2e::warmUp(*s.conn, chunkC));
  E2E_CHECK(e2e::warmUp(*s.conn, chunkFar));
  E2E_CHECK(e2e::warmUp(*r1.conn, chunkR1));
  E2E_CHECK(e2e::warmUp(*r2.conn, chunkR2));

  MarkCounts seen1, seen2;
  auto makeHandlers = [&](MarkCounts& seen) {
    Handlers h;
    h.actorUpdate = [&](const SpatialNotification& n) {
      if (std::memcmp(n.uuid, uuidS.data(), wire::kUuidSize) != 0) return;
      if (n.payload.empty()) return;
      switch (n.payload[0]) {
        case kMarkWide: ++seen.wide; break;
        case kMarkNear: ++seen.near; break;
        case kMarkClamp: ++seen.clamp; break;
        default: break;
      }
    };
    return h;
  };
  r1.conn->setHandlers(makeHandlers(seen1));
  r2.conn->setHandlers(makeHandlers(seen2));

  auto sendMarked = [&](const wire::ChunkCoord& from, std::uint8_t marker,
                        std::uint8_t distance) {
    const std::uint8_t payload[4] = {marker, 0xee, 0x77, 0x11};
    E2E_CHECK(s.conn
                  ->sendActorUpdate({.chunk = from,
                                     .uuid = uuidS,
                                     .payload = Bytes(payload, sizeof(payload)),
                                     .distance = distance})
                  .ok());
  };
  const std::uint8_t pose[] = {1};
  auto receiversAlive = [&] {
    E2E_CHECK(r1.conn->sendActorUpdate({chunkR1, uuidR1, Bytes(pose, 1), 8}).ok());
    E2E_CHECK(r2.conn->sendActorUpdate({chunkR2, uuidR2, Bytes(pose, 1), 8}).ok());
  };
  auto pollAll = [&] {
    s.conn->poll();
    r1.conn->poll();
    r2.conn->poll();
  };
  receiversAlive();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  E2E_SUBTEST("distance=8 reaches both receivers (Chebyshev 1 and 8)");
  const bool wideOk = e2e::retryUntil(
      [&] {
        receiversAlive();
        sendMarked(chunkC, kMarkWide, 8);
      },
      [&] {
        pollAll();
        return seen1.wide.load() > 0 && seen2.wide.load() > 0;
      });
  E2E_CHECK(wideOk);

  E2E_SUBTEST("distance=1 reaches only the Chebyshev-1 receiver");
  // Positive (r1 sees near-markers) and negative (r2 never does) run in one
  // window; wide-markers keep flowing to prove r2 stays reachable throughout.
  const int wide2Before = seen2.wide.load();
  const auto nearStart = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - nearStart < std::chrono::seconds(10)) {
    receiversAlive();
    sendMarked(chunkC, kMarkNear, 1);
    sendMarked(chunkC, kMarkWide, 8);
    pollAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // in-flight stragglers
  pollAll();
  std::printf("near window: r1 near=%d wide=%d | r2 near=%d wide=+%d\n", seen1.near.load(),
              seen1.wide.load(), seen2.near.load(), seen2.wide.load() - wide2Before);
  E2E_CHECK(seen1.near.load() >= 3);
  E2E_CHECK(seen2.wide.load() - wide2Before >= 3);  // positives flowed to r2...
  E2E_CHECK(seen2.near.load() == 0);                // ...but no distance-1 leak

  E2E_SUBTEST("distance>8 clamps: distance=9 from 9 chunks away reaches nobody");
  const int wide1Before = seen1.wide.load();
  const int wide2Before2 = seen2.wide.load();
  const auto clampStart = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - clampStart < std::chrono::seconds(10)) {
    receiversAlive();
    sendMarked(chunkFar, kMarkClamp, 9);  // if 9 were honored it would reach both
    sendMarked(chunkC, kMarkWide, 8);
    pollAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  pollAll();
  std::printf("clamp window: r1 clamp=%d wide=+%d | r2 clamp=%d wide=+%d\n", seen1.clamp.load(),
              seen1.wide.load() - wide1Before, seen2.clamp.load(),
              seen2.wide.load() - wide2Before2);
  E2E_CHECK(seen1.wide.load() - wide1Before >= 3);
  E2E_CHECK(seen2.wide.load() - wide2Before2 >= 3);
  E2E_CHECK(seen1.clamp.load() == 0);
  E2E_CHECK(seen2.clamp.load() == 0);

  E2E_CHECK(s.conn->stats().hmacFailures == 0);
  E2E_CHECK(r1.conn->stats().hmacFailures == 0);
  E2E_CHECK(r2.conn->stats().hmacFailures == 0);

  s.conn->disconnect();
  r1.conn->disconnect();
  r2.conn->disconnect();
  std::puts("e2e_spatial_distance OK");
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "unexpected exception: %s\n", e.what());
    return 1;
  }
}
