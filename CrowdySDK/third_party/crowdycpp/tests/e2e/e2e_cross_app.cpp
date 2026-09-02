// Mirrors CrowdyJS two-client-cross-app e2e on the native transport.
// Replication traffic is scoped to its app: two players in the SAME chunk
// coordinates but different apps must never see each other's updates, while
// same-app fan-out still works. Requires a second app on the deployment
// (CROWDY_E2E_APP_ID_2); skips otherwise.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  if (cfg.appId2.empty()) {
    std::puts("CROWDY_E2E_APP_ID_2 not configured; skipping");
    return 77;
  }

  // Two players on app 1, one on app 2, all at identical coordinates.
  auto a1 = e2e::provisionPlayerEmail(cfg, e2e::deriveEmail(cfg, "xapp-a1"), cfg.appId);
  auto a1b = e2e::provisionPlayerEmail(cfg, e2e::deriveEmail(cfg, "xapp-a1b"), cfg.appId);
  auto a2 = e2e::provisionPlayerEmail(cfg, e2e::deriveEmail(cfg, "xapp-a2"), cfg.appId2);
  e2e::connectUdp(a1, cfg, cfg.appId);
  e2e::connectUdp(a1b, cfg, cfg.appId);
  e2e::connectUdp(a2, cfg, cfg.appId2);

  const wire::ChunkCoord chunk{100500, 0, 100500};
  E2E_CHECK(e2e::warmUp(*a1.conn, chunk));
  E2E_CHECK(e2e::warmUp(*a1b.conn, chunk));
  E2E_CHECK(e2e::warmUp(*a2.conn, chunk));

  const auto uuidA1 = core::generateActorUuid();
  const auto uuidA1b = core::generateActorUuid();
  const auto uuidA2 = core::generateActorUuid();
  constexpr std::uint8_t kApp1Mark = 0x11;
  constexpr std::uint8_t kApp2Mark = 0x22;

  std::atomic<int> a1bSawApp1{0}, a1bSawApp2{0}, a2SawApp1{0};
  Handlers h1b;
  h1b.actorUpdate = [&](const SpatialNotification& n) {
    if (n.payload.empty()) return;
    if (n.payload[0] == kApp1Mark && std::memcmp(n.uuid, uuidA1.data(), 32) == 0) ++a1bSawApp1;
    if (n.payload[0] == kApp2Mark) ++a1bSawApp2;
  };
  a1b.conn->setHandlers(std::move(h1b));
  Handlers h2;
  h2.actorUpdate = [&](const SpatialNotification& n) {
    if (!n.payload.empty() && n.payload[0] == kApp1Mark) ++a2SawApp1;
  };
  a2.conn->setHandlers(std::move(h2));

  E2E_SUBTEST("same-app fan-out reaches the same-app peer");
  const std::uint8_t app1Payload[] = {kApp1Mark, 1, 2, 3};
  const std::uint8_t app2Payload[] = {kApp2Mark, 4, 5, 6};
  const std::uint8_t pose[] = {9};
  const bool sameApp = e2e::retryUntil(
      [&] {
        a1b.conn->sendActorUpdate({chunk, uuidA1b, Bytes(pose, 1), 8});
        a2.conn->sendActorUpdate({chunk, uuidA2, Bytes(pose, 1), 8});
        a1.conn->sendActorUpdate({chunk, uuidA1, Bytes(app1Payload, sizeof(app1Payload)), 8});
        a2.conn->sendActorUpdate({chunk, uuidA2, Bytes(app2Payload, sizeof(app2Payload)), 8});
      },
      [&] {
        a1b.conn->poll();
        a2.conn->poll();
        return a1bSawApp1.load() > 0;
      });
  E2E_CHECK(sameApp);

  E2E_SUBTEST("cross-app isolation: no leakage in either direction");
  // Keep both apps' traffic flowing for a full window; app-2 marks must never
  // reach the app-1 peer, and app-1 marks must never reach the app-2 player.
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8)) {
    a1b.conn->sendActorUpdate({chunk, uuidA1b, Bytes(pose, 1), 8});
    a2.conn->sendActorUpdate({chunk, uuidA2, Bytes(pose, 1), 8});
    a1.conn->sendActorUpdate({chunk, uuidA1, Bytes(app1Payload, sizeof(app1Payload)), 8});
    a2.conn->sendActorUpdate({chunk, uuidA2, Bytes(app2Payload, sizeof(app2Payload)), 8});
    a1b.conn->poll();
    a2.conn->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::printf("app1 peer saw: app1=%d app2=%d | app2 player saw app1=%d\n", a1bSawApp1.load(),
              a1bSawApp2.load(), a2SawApp1.load());
  E2E_CHECK(a1bSawApp1.load() > 0);   // same-app still flowing
  E2E_CHECK(a1bSawApp2.load() == 0);  // no app-2 -> app-1 leakage
  E2E_CHECK(a2SawApp1.load() == 0);   // no app-1 -> app-2 leakage

  a1.conn->disconnect();
  a1b.conn->disconnect();
  a2.conn->disconnect();
  std::puts("e2e_cross_app OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
