// End-to-end echo latency: sends actor updates and measures the round trip
// until the self-echo notification returns from the replication server.
// Env-gated like the e2e tests (CROWDY_E2E_*); exits 77 when unconfigured.
#include <algorithm>
#include <cstring>
#include <vector>

#include "../tests/e2e/e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

int main() {
  auto cfg = e2e::requireConfig();
  auto p = e2e::signIn(cfg, cfg.email);
  e2e::connectUdp(p, cfg);

  const auto uuid = core::generateActorUuid();
  const wire::ChunkCoord chunk{4000, 0, 4000};

  std::vector<double> samplesMs;
  std::atomic<int> pendingSeq{-1};
  std::atomic<std::int64_t> sentAtUs{0};

  Handlers h;
  h.actorUpdate = [&](const SpatialNotification& n) {
    if (std::memcmp(n.uuid, uuid.data(), 32) != 0) return;
    if (n.sequence != pendingSeq.load(std::memory_order_acquire)) return;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const std::int64_t nowUs =
        std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    samplesMs.push_back(static_cast<double>(nowUs - sentAtUs.load()) / 1000.0);
    pendingSeq.store(-1, std::memory_order_release);
  };
  p.conn->setHandlers(std::move(h));

  const std::uint8_t pose[88] = {2};
  constexpr int kRounds = 200;
  for (int i = 0; i < kRounds; ++i) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    sentAtUs.store(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    auto seq = p.conn->sendActorUpdate(
        {.chunk = chunk, .uuid = uuid, .payload = Bytes(pose, sizeof(pose)), .distance = 8});
    if (!seq.ok()) continue;
    pendingSeq.store(seq.value(), std::memory_order_release);
    // Spin-poll so the measurement reflects the wire, not a sleep interval.
    const auto waitStart = std::chrono::steady_clock::now();
    while (pendingSeq.load(std::memory_order_acquire) != -1 &&
           std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(1)) {
      p.conn->poll();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if (samplesMs.empty()) {
    std::puts("no echoes received (server may not echo to the sender)");
    return 1;
  }
  std::sort(samplesMs.begin(), samplesMs.end());
  auto pct = [&](double q) { return samplesMs[static_cast<std::size_t>(
                                 q * static_cast<double>(samplesMs.size() - 1))]; };
  std::printf("echo RTT over %zu samples: p50=%.2fms p90=%.2fms p99=%.2fms min=%.2fms\n",
              samplesMs.size(), pct(0.5), pct(0.9), pct(0.99), samplesMs.front());
  p.conn->disconnect();
  return 0;
}
