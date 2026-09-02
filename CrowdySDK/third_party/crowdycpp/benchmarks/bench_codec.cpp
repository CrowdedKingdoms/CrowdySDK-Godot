// Microbenchmarks: wire encode/decode and HMAC throughput.
// Run: ./bench_codec  (release build)
#include <chrono>
#include <cstdio>
#include <cstring>

#include "crowdy/core/crypto.hpp"
#include "crowdy/wire/codec.hpp"

using namespace crowdy;
using namespace crowdy::wire;
using Clock = std::chrono::steady_clock;

namespace {

double nsPerOp(Clock::time_point start, Clock::time_point end, long iterations) {
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) /
         static_cast<double>(iterations);
}

}  // namespace

int main() {
  const auto& crypto = core::opensslCrypto();
  auto token = *Token64::fromString(std::string(64, 'k'));

  LongSpatialParams p;
  p.type = MessageType::ActorUpdateRequest;
  p.appId = 1;
  p.chunk = {10, 20, 30};
  p.distance = 8;
  p.decay = DecayRate::Exponential;
  std::memcpy(p.uuid.data(), "0123456789abcdef0123456789abcdef", 32);
  std::uint8_t payload[88] = {2};
  p.payload = Bytes(payload, sizeof(payload));
  p.gameTokenId = 42;

  constexpr long kIters = 200000;
  std::uint8_t buf[512];
  volatile std::size_t sink = 0;

  // Encode + sign (the per-send hot path).
  auto t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    p.sequence = static_cast<std::uint8_t>(i);
    auto n = encodeLongSpatial(crypto, p, token, MutableBytes(buf, sizeof(buf)));
    sink += n.value();
  }
  auto t1 = Clock::now();
  std::printf("encode+sign (88B payload):  %8.1f ns/op\n", nsPerOp(t0, t1, kIters));

  // Parse (zero-copy view).
  const std::size_t msgLen = longSpatialSize(sizeof(payload));
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    auto v = parseLongSpatial(Bytes(buf, msgLen));
    sink += v->payload.size();
  }
  t1 = Clock::now();
  std::printf("parse (zero-copy):          %8.1f ns/op\n", nsPerOp(t0, t1, kIters));

  // Verify (HMAC over the full message).
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    sink += verifyLongSpatial(crypto, Bytes(buf, msgLen), token).ok() ? 1u : 0u;
  }
  t1 = Clock::now();
  std::printf("verify (HMAC-SHA256):       %8.1f ns/op\n", nsPerOp(t0, t1, kIters));

  std::printf("(sink=%zu)\n", static_cast<std::size_t>(sink));
  return 0;
}
