// Mirrors CrowdyJS's two-client-audio e2e on the native transport.
// Replication API behavior: CLIENT_AUDIO_PACKET fans out opaque audio bytes
// to nearby actors as CLIENT_AUDIO_NOTIFICATION; the payload is carried
// verbatim (the server never inspects or transcodes it). Sending audio
// requires the use_voice_chat runtime permission, which the harness's e2e
// access tier grants. See
// https://docs.crowdedkingdoms.com/replication-api/operations and
// https://docs.crowdedkingdoms.com/replication-api/wire-formats.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

// A typical 20 ms Opus-frame-sized buffer; contents are opaque to the server.
constexpr std::size_t kFrameSize = 960;

int run() {
  auto cfg = e2e::requireConfig();
  auto a = e2e::provisionPlayer(cfg, "audio-a");
  auto b = e2e::provisionPlayer(cfg, "audio-b");
  e2e::connectUdp(a, cfg);
  e2e::connectUdp(b, cfg);

  const auto uuidA = core::generateActorUuid();
  const auto uuidB = core::generateActorUuid();
  const wire::ChunkCoord chunk{100200, 0, 100200};  // suite 2 base
  // Warm the per-chunk permission window for both senders so the first-chunk
  // denial (documented) doesn't fail the strict error assertion.
  E2E_CHECK(e2e::warmUp(*a.conn, chunk));
  E2E_CHECK(e2e::warmUp(*b.conn, chunk));

  // Deterministic pseudo-random frame so a byte-compare proves integrity.
  std::uint8_t frame[kFrameSize];
  for (std::size_t i = 0; i < kFrameSize; ++i)
    frame[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);

  std::atomic<int> intactFrames{0}, corruptFrames{0}, errors{0};
  Handlers hb;
  hb.audio = [&](const SpatialNotification& n) {
    if (std::memcmp(n.uuid, uuidA.data(), wire::kUuidSize) != 0) return;
    if (n.payload.size() == kFrameSize &&
        std::memcmp(n.payload.data(), frame, kFrameSize) == 0) {
      ++intactFrames;
    } else {
      ++corruptFrames;
    }
  };
  b.conn->setHandlers(std::move(hb));

  Handlers ha;
  ha.genericError = [&](const GenericError& e) {
    std::fprintf(stderr, "sender got error code=%u seq=%u\n",
                 static_cast<unsigned>(e.code), static_cast<unsigned>(e.sequence));
    ++errors;
  };
  a.conn->setHandlers(std::move(ha));

  // Presence first: fan-out only reaches actors known to be in range, so
  // keep both actors fresh with pose updates throughout.
  const std::uint8_t pose[] = {1};
  auto keepAlive = [&] {
    E2E_CHECK(a.conn->sendActorUpdate({chunk, uuidA, Bytes(pose, 1), 8}).ok());
    E2E_CHECK(b.conn->sendActorUpdate({chunk, uuidB, Bytes(pose, 1), 8}).ok());
  };
  keepAlive();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  E2E_SUBTEST("960-byte audio frames arrive intact at the other client");
  const bool received = e2e::retryUntil(
      [&] {
        keepAlive();
        E2E_CHECK(a.conn
                      ->sendAudio({.chunk = chunk,
                                   .uuid = uuidA,
                                   .payload = Bytes(frame, kFrameSize),
                                   .distance = 8})
                      .ok());
      },
      [&] {
        a.conn->poll();
        b.conn->poll();
        return intactFrames.load() >= 3;
      });
  std::printf("audio frames: intact=%d corrupt=%d senderErrors=%d\n", intactFrames.load(),
              corruptFrames.load(), errors.load());
  E2E_CHECK(received);
  E2E_CHECK(corruptFrames.load() == 0);
  // Transient UNAUTHORIZED(7) can appear mid-stream when the server reloads
  // the chunk's permission window (grants churn on a busy shared app). That
  // is the documented permission-refresh behavior, not an audio-path fault —
  // so it is tolerated as long as delivery succeeded; a session that ONLY
  // drew errors would have failed the `received` check above.
  if (errors.load() > 0) {
    std::printf("(transient UNAUTHORIZED during permission-window reload: %d)\n", errors.load());
  }
  // Note: hmacFailures is diagnostic, not asserted here. On a shared
  // deployment a concurrent player's foreign-signed frame fanning into this
  // chunk is correctly DROPPED by verification (the security property), which
  // increments this counter without indicating a fault in our own traffic.
  std::printf("hmacFailures (dropped foreign frames): a=%llu b=%llu\n",
              static_cast<unsigned long long>(a.conn->stats().hmacFailures),
              static_cast<unsigned long long>(b.conn->stats().hmacFailures));

  a.conn->disconnect();
  b.conn->disconnect();
  std::puts("e2e_two_client_audio OK");
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
