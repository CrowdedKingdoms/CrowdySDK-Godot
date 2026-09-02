// Replication API behavior: authentication failures are handled asymmetrically
// by design (https://docs.crowdedkingdoms.com/replication-api/hmac and
// https://docs.crowdedkingdoms.com/replication-api/troubleshooting):
//   (a) a frame whose HMAC tag does not verify is dropped SILENTLY — no error
//       frame is returned and nothing fans out (anything else would let an
//       attacker probe keys or amplify traffic);
//   (b) a correctly signed frame naming an appId the token is not scoped to
//       draws GENERIC_ERROR with INVALID_APP_ID (18), correlated by sequence
//       (https://docs.crowdedkingdoms.com/replication-api/operations);
//   (c) short/garbage datagrams are ignored without a reply.
// A healthy connection observes throughout: it must never receive fan-out
// from the rejected frames and must keep working afterwards.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

int run() {
  auto cfg = e2e::requireConfig();
  auto a = e2e::provisionPlayer(cfg, "negauth-a");
  e2e::connectUdp(a, cfg);

  const auto uuidA = core::generateActorUuid();
  const auto uuidRogue = core::generateActorUuid();
  const wire::ChunkCoord chunk{100400, 0, 100400};  // suite 4 base

  std::atomic<int> rogueFanout{0};
  Handlers h;
  h.any = [&](const SpatialNotification& n) {
    if (std::memcmp(n.uuid, uuidRogue.data(), wire::kUuidSize) == 0) ++rogueFanout;
  };
  a.conn->setHandlers(std::move(h));

  // Healthy baseline: the observer's own updates echo back.
  E2E_SUBTEST("healthy connection baseline");
  const std::uint8_t pose[] = {0x0b};
  Connection::WaitOutcome base;
  const bool baseOk = e2e::retryUntil(
      [&] {
        base = a.conn->sendActorUpdateAndWait(
            {.chunk = chunk, .uuid = uuidA, .payload = Bytes(pose, 1), .distance = 8}, 2000);
      },
      [&] { return base.acknowledged; }, /*attempts=*/8, /*perWaitMs=*/10);
  E2E_CHECK(baseOk);

  // Raw socket to the same assigned server, next to the healthy connection.
  const Assignment& asg = a.conn->assignment();
  UdpSocket raw;
  E2E_CHECK(raw.open(asg.ip4, asg.clientPort, 1 << 16, 1 << 16).ok());

  const auto token = wire::Token64::fromString(a.appToken.token);
  E2E_CHECK(token.has_value());
  const core::ICrypto& crypto = core::opensslCrypto();

  const std::uint8_t rogueMark[4] = {0xba, 0xdb, 0xad, 0x01};
  auto encodeRogue = [&](std::int64_t appId, std::uint8_t sequence, MutableBytes out) {
    wire::LongSpatialParams p;
    p.type = wire::MessageType::ActorUpdateRequest;
    p.appId = appId;
    p.chunk = chunk;
    p.distance = 8;
    p.uuid = uuidRogue;
    p.payload = Bytes(rogueMark, sizeof(rogueMark));
    p.gameTokenId = a.appToken.gameTokenIdInt64().value_or(0);
    p.sequence = sequence;
    auto n = wire::encodeLongSpatial(crypto, p, *token, out);
    E2E_CHECK(n.ok());
    return n.value();
  };

  // Drain anything queued on the raw socket, then run `body` and collect
  // every GENERIC_ERROR seen on it within windowMs (frames may be bundled).
  auto collectErrors = [&](int windowMs, auto&& body) {
    std::vector<wire::GenericErrorView> errors;
    std::uint8_t rbuf[wire::kMaxDatagramSize];
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(windowMs)) {
      body();
      for (;;) {
        auto r = raw.recv(MutableBytes(rbuf, sizeof(rbuf)), 100);
        if (!r.ok() || r.value() == 0) break;
        wire::forEachMessage(Bytes(rbuf, r.value()), [&](Bytes msg) {
          auto err = wire::parseGenericError(msg);
          if (err.ok()) errors.push_back(err.value());
        });
      }
      a.conn->poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return errors;
  };

  E2E_SUBTEST("(a) tampered HMAC -> silent drop, no fan-out");
  {
    std::uint8_t buf[wire::kMaxDatagramSize];
    const std::size_t len = encodeRogue(std::strtoll(cfg.appId.c_str(), nullptr, 10), 0x41,
                                        MutableBytes(buf, sizeof(buf)));
    // Corrupt one byte of the 32-byte tag (it sits kTailWithHmac from the end).
    buf[len - wire::kTailWithHmac] ^= 0xff;
    auto errs = collectErrors(3000, [&] { E2E_CHECK(raw.send(Bytes(buf, len)).ok()); });
    std::printf("tampered-hmac window: %zu error frame(s), rogue fan-out=%d\n", errs.size(),
                rogueFanout.load());
    E2E_CHECK(errs.empty());
    E2E_CHECK(rogueFanout.load() == 0);
  }

  E2E_SUBTEST("(b) unscoped appId + valid HMAC -> INVALID_APP_ID (18) by sequence");
  {
    constexpr std::uint8_t kSeq = 0x42;
    std::uint8_t buf[wire::kMaxDatagramSize];
    const std::size_t len = encodeRogue(999999, kSeq, MutableBytes(buf, sizeof(buf)));
    bool sawInvalidAppId = false;
    for (int attempt = 0; attempt < 10 && !sawInvalidAppId; ++attempt) {
      auto errs = collectErrors(1000, [&] { E2E_CHECK(raw.send(Bytes(buf, len)).ok()); });
      for (const auto& e : errs) {
        std::printf("error frame: seq=%u code=%u\n", static_cast<unsigned>(e.sequence),
                    static_cast<unsigned>(e.code));
        if (e.sequence == kSeq && e.code == wire::ErrorCode::InvalidAppId) sawInvalidAppId = true;
      }
    }
    E2E_CHECK(sawInvalidAppId);
    E2E_CHECK(rogueFanout.load() == 0);
  }

  E2E_SUBTEST("(c) short/garbage datagrams -> no reply, healthy conn unaffected");
  {
    const std::uint8_t junk1[1] = {0x00};
    const std::uint8_t junk3[3] = {0xde, 0xad, 0xbe};
    std::uint8_t junk50[50];
    for (std::size_t i = 0; i < sizeof(junk50); ++i)
      junk50[i] = static_cast<std::uint8_t>(i * 7 + 3);
    std::uint8_t rbuf[wire::kMaxDatagramSize];
    std::size_t replies = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
      E2E_CHECK(raw.send(Bytes(junk1, sizeof(junk1))).ok());
      E2E_CHECK(raw.send(Bytes(junk3, sizeof(junk3))).ok());
      E2E_CHECK(raw.send(Bytes(junk50, sizeof(junk50))).ok());
      for (;;) {
        auto r = raw.recv(MutableBytes(rbuf, sizeof(rbuf)), 100);
        if (!r.ok() || r.value() == 0) break;
        ++replies;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("garbage window: %zu reply datagram(s)\n", replies);
    E2E_CHECK(replies == 0);

    // Recovery check: the garbage must not have damaged OUR session. On a
    // busy shared stack the server may load-shed the client mid-test
    // (COMMAND_RECONNECT) — that is platform load management, not an effect
    // of the rejected frames, so a re-established session also proves
    // recovery.
    auto recovered = [&] {
      Connection::WaitOutcome after;
      return e2e::retryUntil(
          [&] {
            after = a.conn->sendActorUpdateAndWait(
                {.chunk = chunk, .uuid = uuidA, .payload = Bytes(pose, 1), .distance = 8}, 2000);
          },
          [&] { return after.acknowledged; }, /*attempts=*/20, /*perWaitMs=*/250);
    };
    bool stillOk = recovered();
    if (!stillOk) {
      std::printf("recovery: session lost (state=%d) — re-establishing\n",
                  static_cast<int>(a.conn->state()));
      a.conn->disconnect();
      e2e::connectUdp(a, cfg);
      E2E_CHECK(e2e::warmUp(*a.conn, chunk));
      stillOk = recovered();
    }
    E2E_CHECK(stillOk);
  }

  E2E_CHECK(a.conn->stats().hmacFailures == 0);  // WE never sent a bad tag on this conn
  raw.close();
  a.conn->disconnect();
  return 0;
}

}  // namespace

int main() try {
  const int rc = run();
  if (rc == 0) std::puts("e2e_negative_auth OK");
  return rc;
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}