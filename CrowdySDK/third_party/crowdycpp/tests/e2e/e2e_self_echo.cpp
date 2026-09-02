// Mirrors CrowdyJS's single-client echo e2e on the native transport.
// Replication API behavior: an actor update is echoed back to its sender as
// an ACTOR_UPDATE_NOTIFICATION carrying the same uuid, payload and sequence
// (this is what *AndWait correlation is built on), while
// CLIENT_ACTOR_HEARTBEAT refreshes presence WITHOUT any fan-out or echo.
// See https://docs.crowdedkingdoms.com/replication-api/operations and
// https://docs.crowdedkingdoms.com/replication-api/wire-formats.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

int run() {
  auto cfg = e2e::requireConfig();
  auto a = e2e::provisionPlayer(cfg, "echo-a");
  e2e::connectUdp(a, cfg);

  const auto uuid = core::generateActorUuid();
  const wire::ChunkCoord chunk{100100, 0, 100100};  // suite 1 base (see conventions)
  // Load the per-chunk permission window before attaching handlers so the
  // documented first-chunk denial doesn't count against the error assertion.
  E2E_CHECK(e2e::warmUp(*a.conn, chunk));

  const std::uint8_t pose[] = {0x5e, 0x1f, 0xec, 0x40};
  std::atomic<int> payloadEchoes{0}, exactEchoes{0}, anySpatial{0}, errors{0};
  std::atomic<std::uint8_t> lastSeq{0};

  Handlers h;
  h.any = [&](const SpatialNotification&) { ++anySpatial; };
  h.actorUpdate = [&](const SpatialNotification& n) {
    if (std::memcmp(n.uuid, uuid.data(), wire::kUuidSize) != 0) return;
    if (n.payload.size() != sizeof(pose) ||
        std::memcmp(n.payload.data(), pose, sizeof(pose)) != 0)
      return;
    ++payloadEchoes;
    // The echo must carry the sequence number the send returned.
    if (n.sequence == lastSeq.load()) ++exactEchoes;
  };
  h.genericError = [&](const GenericError&) { ++errors; };
  a.conn->setHandlers(std::move(h));

  E2E_SUBTEST("actor update self-echo (uuid + payload + sequence match)");
  const bool echoed = e2e::retryUntil(
      [&] {
        auto seq = a.conn->sendActorUpdate(
            {.chunk = chunk, .uuid = uuid, .payload = Bytes(pose, sizeof(pose)), .distance = 8});
        E2E_CHECK(seq.ok());
        lastSeq.store(seq.value());
      },
      [&] {
        a.conn->poll();
        return exactEchoes.load() > 0;
      });
  E2E_CHECK(echoed);
  E2E_CHECK(payloadEchoes.load() > 0);
  E2E_CHECK(errors.load() == 0);

  E2E_SUBTEST("sendActorUpdateAndWait acknowledges with server time");
  Connection::WaitOutcome outcome;
  const bool acked = e2e::retryUntil(
      [&] {
        outcome = a.conn->sendActorUpdateAndWait(
            {.chunk = chunk, .uuid = uuid, .payload = Bytes(pose, sizeof(pose)), .distance = 8},
            2000);
      },
      [&] { return outcome.acknowledged; }, /*attempts=*/8, /*perWaitMs=*/10);
  E2E_CHECK(acked);
  E2E_CHECK(!outcome.error.has_value());
  E2E_CHECK(outcome.serverEpochMs > 0);

  E2E_SUBTEST("heartbeat produces no echo/fan-out");
  // Drain in-flight echoes until the connection is quiet for a full second
  // (bounded at 10 s) so late echoes can't be misattributed to heartbeats.
  {
    const auto drainStart = std::chrono::steady_clock::now();
    auto quietSince = drainStart;
    int seen = anySpatial.load();
    while (std::chrono::steady_clock::now() - quietSince < std::chrono::seconds(1) &&
           std::chrono::steady_clock::now() - drainStart < std::chrono::seconds(10)) {
      a.conn->poll();
      if (anySpatial.load() != seen) {
        seen = anySpatial.load();
        quietSince = std::chrono::steady_clock::now();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  const int baselineSpatial = anySpatial.load();
  const int baselineErrors = errors.load();
  const auto hbStart = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - hbStart < std::chrono::seconds(3)) {
    E2E_CHECK(a.conn->sendHeartbeat(chunk, uuid).ok());
    a.conn->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  std::this_thread::sleep_for(std::chrono::seconds(1));  // let stragglers land
  a.conn->poll();
  std::printf("heartbeat window: spatial %d -> %d, errors %d -> %d\n", baselineSpatial,
              anySpatial.load(), baselineErrors, errors.load());
  E2E_CHECK(anySpatial.load() == baselineSpatial);
  E2E_CHECK(errors.load() == baselineErrors);

  E2E_CHECK(a.conn->stats().hmacFailures == 0);
  a.conn->disconnect();
  std::puts("e2e_self_echo OK");
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
