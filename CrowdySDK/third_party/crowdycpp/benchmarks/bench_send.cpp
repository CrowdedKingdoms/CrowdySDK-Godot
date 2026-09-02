// Send-path cost breakdown: where the per-datagram microseconds actually go.
//
// bench_codec reports encode+sign as one number. This one takes it apart, so an
// optimisation can be aimed at the part that costs something rather than the
// part that is easy to change. It also measures the alternatives for the MAC
// and for the syscall side by side, which is what decides whether the work is
// worth doing at all.
//
// Run: ./bench_send   (release build)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "crowdy/replication/connection.hpp"
#include "crowdy/replication/udp_socket.hpp"
#include "crowdy/wire/codec.hpp"

using namespace crowdy;
using namespace crowdy::wire;
using Clock = std::chrono::steady_clock;

namespace {

constexpr long kIters = 200000;
constexpr int kFrameEntities = 200;  // the reported 200-entity one-frame flush

double nsPerOp(Clock::time_point a, Clock::time_point b, long n) {
  return double(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()) / double(n);
}

void row(const char* label, double ns, double baseline = 0.0) {
  if (baseline > 0.0)
    std::printf("  %-42s %8.1f ns   %5.2fx\n", label, ns, baseline / ns);
  else
    std::printf("  %-42s %8.1f ns\n", label, ns);
}

// A crypto provider whose MAC does nothing, to price the encode work alone.
class NoMacCrypto final : public core::ICrypto {
 public:
  bool hmacSha256(Bytes, Bytes, std::uint8_t* out) const override {
    std::memset(out, 0, kHmacTagSize);
    return true;
  }
  bool sha256(Bytes, std::uint8_t*) const override { return false; }
  bool constantTimeEquals(const std::uint8_t*, const std::uint8_t*, std::size_t) const override {
    return true;
  }
  bool randomBytes(std::uint8_t*, std::size_t) const override { return false; }
};

int loopbackPeer(int* port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int big = 8 << 20;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
  ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  socklen_t l = sizeof(a);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
  *port = ntohs(a.sin_port);
  return fd;
}

// ---------------------------------------------------------------------------
// 1. Encode with and without the MAC.
// ---------------------------------------------------------------------------
void encodeBreakdown(const LongSpatialParams& base, const Token64& token) {
  std::printf("\n1. Encode (88B payload, %ld iterations)\n", kIters);
  std::uint8_t buf[512];
  volatile std::size_t sink = 0;
  LongSpatialParams p = base;

  const NoMacCrypto noMac;
  auto t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    p.sequence = std::uint8_t(i);
    sink += encodeLongSpatial(noMac, p, token, MutableBytes(buf, sizeof(buf))).value();
  }
  auto t1 = Clock::now();
  const double encodeOnly = nsPerOp(t0, t1, kIters);
  row("encode only (MAC stubbed out)", encodeOnly);

  const auto& real = core::opensslCrypto();
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    p.sequence = std::uint8_t(i);
    sink += encodeLongSpatial(real, p, token, MutableBytes(buf, sizeof(buf))).value();
  }
  t1 = Clock::now();
  const double full = nsPerOp(t0, t1, kIters);
  row("encode + sign, one-shot MAC (fallback)", full);
  std::printf("  %-42s %8.1f ns   %.0f%% of the total\n", "-> attributable to the MAC",
              full - encodeOnly, 100.0 * (full - encodeOnly) / full);

  auto mac = real.makeHmacSha256(token.bytes());
  if (mac == nullptr) {
    std::printf("  (provider offers no pre-keyed MAC)\n");
    (void)sink;
    return;
  }
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    p.sequence = std::uint8_t(i);
    sink += encodeLongSpatial(real, p, token, MutableBytes(buf, sizeof(buf)), mac.get()).value();
  }
  t1 = Clock::now();
  row("encode + sign, pre-keyed MAC", nsPerOp(t0, t1, kIters), full);

  // The receive side signs nothing but verifies every notification with the
  // same key, so it gets the same saving.
  const std::size_t msgLen = longSpatialSize(p.payload.size());
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) sink += verifyLongSpatial(real, Bytes(buf, msgLen), token).ok();
  t1 = Clock::now();
  const double verifyOneShot = nsPerOp(t0, t1, kIters);
  row("verify, one-shot MAC (fallback)", verifyOneShot);
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i)
    sink += verifyLongSpatial(real, Bytes(buf, msgLen), token, mac.get()).ok();
  t1 = Clock::now();
  row("verify, pre-keyed MAC", nsPerOp(t0, t1, kIters), verifyOneShot);
  (void)sink;
}

// ---------------------------------------------------------------------------
// 2. The MAC itself: one-shot versus a pre-keyed context.
//
// The SDK hashes prefix||token64 with the token as the key. The key changes
// only on token refresh, so everything the one-shot call redoes per datagram
// (context allocation, ipad/opad schedule) is avoidable in principle. These
// variants price each way of avoiding it.
// ---------------------------------------------------------------------------
void macVariants(const std::uint8_t* prefix, std::size_t prefixLen, const Token64& token) {
  std::printf("\n2. HMAC-SHA256 over %zu bytes, 64-byte key (%ld iterations)\n",
              prefixLen + kTokenOctets, kIters);

  // The concatenated message the current code builds on the stack every call.
  std::vector<std::uint8_t> joined(prefixLen + kTokenOctets);
  std::memcpy(joined.data(), prefix, prefixLen);
  std::memcpy(joined.data() + prefixLen, token.octets, kTokenOctets);

  std::uint8_t out[32];
  volatile unsigned sink = 0;

  // Before timing anything: every variant must produce the same tag. A faster
  // MAC that disagrees with the one on the wire is not a faster MAC.
  {
    std::uint8_t reference[32], candidate[32];
    unsigned int rlen = 0;
    HMAC(EVP_sha256(), token.octets, int(kTokenOctets), joined.data(), joined.size(), reference,
         &rlen);

    HMAC_CTX* c = HMAC_CTX_new();
    HMAC_Init_ex(c, token.octets, int(kTokenOctets), EVP_sha256(), nullptr);
    for (int round = 0; round < 3; ++round) {  // reuse must be repeatable
      unsigned int clen = 0;
      HMAC_Init_ex(c, nullptr, 0, nullptr, nullptr);
      HMAC_Update(c, joined.data(), joined.size());
      HMAC_Final(c, candidate, &clen);
      if (std::memcmp(reference, candidate, 32) != 0) {
        std::printf("  FATAL: HMAC_CTX reset disagrees on round %d\n", round);
        return;
      }
    }
    HMAC_CTX_free(c);

    EVP_MAC* a = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    EVP_MAC_CTX* k = EVP_MAC_CTX_new(a);
    char digest[] = "SHA256";
    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string("digest", digest, 0),
                           OSSL_PARAM_construct_end()};
    EVP_MAC_init(k, token.octets, kTokenOctets, params);
    for (int round = 0; round < 3; ++round) {
      std::size_t clen = 0;
      EVP_MAC_init(k, nullptr, 0, nullptr);  // reset, key retained
      EVP_MAC_update(k, prefix, prefixLen);  // two-part
      EVP_MAC_update(k, token.octets, kTokenOctets);
      EVP_MAC_final(k, candidate, &clen, sizeof(candidate));
      if (clen != 32 || std::memcmp(reference, candidate, 32) != 0) {
        std::printf("  FATAL: EVP_MAC reset/two-part disagrees on round %d\n", round);
        return;
      }
    }
    EVP_MAC_CTX_free(k);
    EVP_MAC_free(a);
    std::printf("  all variants agree with one-shot HMAC (3 reuse rounds each)\n");
  }

  // (a) What the SDK does now.
  auto t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    unsigned int len = 0;
    HMAC(EVP_sha256(), token.octets, int(kTokenOctets), joined.data(), joined.size(), out, &len);
    sink += out[0];
  }
  auto t1 = Clock::now();
  const double oneShot = nsPerOp(t0, t1, kIters);
  row("(a) one-shot HMAC() [current]", oneShot);

  // (b) Legacy HMAC_CTX reset: key schedule kept, context reused.
  HMAC_CTX* legacy = HMAC_CTX_new();
  HMAC_Init_ex(legacy, token.octets, int(kTokenOctets), EVP_sha256(), nullptr);
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    unsigned int len = 0;
    HMAC_Init_ex(legacy, nullptr, 0, nullptr, nullptr);  // reset, keep the key
    HMAC_Update(legacy, joined.data(), joined.size());
    HMAC_Final(legacy, out, &len);
    sink += out[0];
  }
  t1 = Clock::now();
  row("(b) HMAC_CTX reset, single context", nsPerOp(t0, t1, kIters), oneShot);
  HMAC_CTX_free(legacy);

  // (c) EVP_MAC pre-keyed, duplicated per call (thread-safe shape).
  EVP_MAC* algo = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  EVP_MAC_CTX* keyed = EVP_MAC_CTX_new(algo);
  {
    char digest[] = "SHA256";
    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string("digest", digest, 0),
                           OSSL_PARAM_construct_end()};
    EVP_MAC_init(keyed, token.octets, kTokenOctets, params);
  }
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    EVP_MAC_CTX* ctx = EVP_MAC_CTX_dup(keyed);
    std::size_t len = 0;
    EVP_MAC_update(ctx, joined.data(), joined.size());
    EVP_MAC_final(ctx, out, &len, sizeof(out));
    EVP_MAC_CTX_free(ctx);
    sink += out[0];
  }
  t1 = Clock::now();
  row("(c) EVP_MAC pre-keyed + dup per call", nsPerOp(t0, t1, kIters), oneShot);

  // (d) Same, but fed in two parts so the caller need not concatenate.
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    EVP_MAC_CTX* ctx = EVP_MAC_CTX_dup(keyed);
    std::size_t len = 0;
    EVP_MAC_update(ctx, prefix, prefixLen);
    EVP_MAC_update(ctx, token.octets, kTokenOctets);
    EVP_MAC_final(ctx, out, &len, sizeof(out));
    EVP_MAC_CTX_free(ctx);
    sink += out[0];
  }
  t1 = Clock::now();
  row("(d) (c) + two-part, no concatenation", nsPerOp(t0, t1, kIters), oneShot);

  // (e) Pre-keyed context reused with no dup. Fastest possible, but a single
  // context cannot be shared by concurrent senders -- priced to show the
  // ceiling, and what a thread-local cache would be worth over (d).
  t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) {
    std::size_t len = 0;
    EVP_MAC_init(keyed, nullptr, 0, nullptr);  // reset, key retained
    EVP_MAC_update(keyed, prefix, prefixLen);
    EVP_MAC_update(keyed, token.octets, kTokenOctets);
    EVP_MAC_final(keyed, out, &len, sizeof(out));
    sink += out[0];
  }
  t1 = Clock::now();
  row("(e) pre-keyed reset, no dup (not MT-safe)", nsPerOp(t0, t1, kIters), oneShot);

  EVP_MAC_CTX_free(keyed);
  EVP_MAC_free(algo);
  (void)sink;
}

// ---------------------------------------------------------------------------
// 3. The syscall: one send per datagram versus sendmmsg.
// ---------------------------------------------------------------------------
void syscallVariants(const std::uint8_t* datagram, std::size_t len) {
  std::printf("\n3. Socket write, %zu-byte datagrams to a draining loopback peer\n", len);
  int port = 0;
  const int peer = loopbackPeer(&port);

  replication::UdpSocket sock;
  if (!sock.open("127.0.0.1", port, 1 << 20, 1 << 20).ok()) {
    std::printf("  open failed\n");
    ::close(peer);
    return;
  }
  const int fd = int(sock.nativeHandle());

  auto t0 = Clock::now();
  for (long i = 0; i < kIters; ++i) (void)sock.send(Bytes(datagram, len));
  auto t1 = Clock::now();
  const double single = nsPerOp(t0, t1, kIters);
  row("send() per datagram [current]", single);

  for (int batch : {8, 16, 32, 64}) {
    std::vector<mmsghdr> msgs(batch);
    std::vector<iovec> iov(batch);
    for (int i = 0; i < batch; ++i) {
      iov[i].iov_base = const_cast<std::uint8_t*>(datagram);
      iov[i].iov_len = len;
      std::memset(&msgs[i], 0, sizeof(mmsghdr));
      msgs[i].msg_hdr.msg_iov = &iov[i];
      msgs[i].msg_hdr.msg_iovlen = 1;
    }
    const long rounds = kIters / batch;
    t0 = Clock::now();
    for (long r = 0; r < rounds; ++r)
      (void)!::sendmmsg(fd, msgs.data(), unsigned(batch), MSG_DONTWAIT);
    t1 = Clock::now();
    char label[64];
    std::snprintf(label, sizeof(label), "sendmmsg, batch of %d", batch);
    row(label, nsPerOp(t0, t1, rounds * batch), single);
  }
  ::close(peer);
}

// ---------------------------------------------------------------------------
// 4. The reported case: flushing a whole population in one frame.
// ---------------------------------------------------------------------------
void frameFlush(const LongSpatialParams& base, const Token64& token) {
  std::printf("\n4. One-frame flush of %d entities (encode + sign + write)\n", kFrameEntities);
  int port = 0;
  const int peer = loopbackPeer(&port);
  replication::UdpSocket sock;
  if (!sock.open("127.0.0.1", port, 1 << 20, 1 << 20).ok()) {
    ::close(peer);
    return;
  }
  const auto& crypto = core::opensslCrypto();
  const int fd = int(sock.nativeHandle());
  constexpr long kFrames = 2000;

  std::vector<std::uint8_t> slab(std::size_t(kFrameEntities) * 512);
  LongSpatialParams p = base;

  auto t0 = Clock::now();
  for (long f = 0; f < kFrames; ++f) {
    for (int e = 0; e < kFrameEntities; ++e) {
      p.sequence = std::uint8_t(e);
      std::uint8_t* slot = slab.data() + std::size_t(e) * 512;
      auto n = encodeLongSpatial(crypto, p, token, MutableBytes(slot, 512));
      (void)sock.send(Bytes(slot, n.value()));
    }
  }
  auto t1 = Clock::now();
  const double perFrame = nsPerOp(t0, t1, kFrames);
  std::printf("  %-42s %8.1f us/frame  (%.1f ns/entity)\n", "one-shot MAC + send() each",
              perFrame / 1000.0, perFrame / kFrameEntities);

  auto mac = crypto.makeHmacSha256(token.bytes());
  if (mac != nullptr) {
    t0 = Clock::now();
    for (long f = 0; f < kFrames; ++f) {
      for (int e = 0; e < kFrameEntities; ++e) {
        p.sequence = std::uint8_t(e);
        std::uint8_t* slot = slab.data() + std::size_t(e) * 512;
        auto n = encodeLongSpatial(crypto, p, token, MutableBytes(slot, 512), mac.get());
        (void)sock.send(Bytes(slot, n.value()));
      }
    }
    t1 = Clock::now();
    const double keyed = nsPerOp(t0, t1, kFrames);
    std::printf("  %-42s %8.1f us/frame  (%.1f ns/entity)  %.2fx\n",
                "pre-keyed MAC + send() each", keyed / 1000.0, keyed / kFrameEntities,
                perFrame / keyed);
  }

  std::vector<mmsghdr> msgs(kFrameEntities);
  std::vector<iovec> iov(kFrameEntities);
  t0 = Clock::now();
  for (long f = 0; f < kFrames; ++f) {
    for (int e = 0; e < kFrameEntities; ++e) {
      p.sequence = std::uint8_t(e);
      std::uint8_t* slot = slab.data() + std::size_t(e) * 512;
      auto n = encodeLongSpatial(crypto, p, token, MutableBytes(slot, 512));
      iov[e].iov_base = slot;
      iov[e].iov_len = n.value();
      std::memset(&msgs[e], 0, sizeof(mmsghdr));
      msgs[e].msg_hdr.msg_iov = &iov[e];
      msgs[e].msg_hdr.msg_iovlen = 1;
    }
    for (int off = 0; off < kFrameEntities;) {
      const int chunk = std::min(32, kFrameEntities - off);
      const int sent = ::sendmmsg(fd, msgs.data() + off, unsigned(chunk), MSG_DONTWAIT);
      if (sent <= 0) break;
      off += sent;
    }
  }
  t1 = Clock::now();
  const double batched = nsPerOp(t0, t1, kFrames);
  std::printf("  %-42s %8.1f us/frame  (%.1f ns/entity)  %.2fx\n",
              "encode+sign then sendmmsg in 32s", batched / 1000.0, batched / kFrameEntities,
              perFrame / batched);
  ::close(peer);
}

// ---------------------------------------------------------------------------
// 5. The number a game actually pays: through Connection's public API, which
//    adds the token/stats locking and the per-send bookkeeping on top of the
//    encode and the write.
// ---------------------------------------------------------------------------
class StubProvider final : public replication::ISessionProvider {
 public:
  explicit StubProvider(int port) : port_(port) {}
  Result<replication::Assignment> assignServer() override {
    return replication::Assignment{"127.0.0.1", "", port_};
  }
  Result<replication::TokenInfo> refreshToken() override {
    return replication::TokenInfo{std::string(64, 'k'), 42, 0};
  }

 private:
  int port_;
};

void throughConnection() {
  std::printf("\n5. Connection::sendActorUpdate, the public path (%d per frame)\n", kFrameEntities);
  int port = 0;
  const int peer = loopbackPeer(&port);

  replication::Config cfg;
  cfg.appId = 7;
  cfg.token = replication::TokenInfo{std::string(64, 'k'), 42, 0};
  cfg.manualPump = true;
  cfg.sessionReadyWaitMs = 0;
  replication::Connection conn(cfg, std::make_shared<StubProvider>(port), core::opensslCrypto());
  if (!conn.connect().ok()) {
    std::printf("  connect failed\n");
    ::close(peer);
    return;
  }

  std::uint8_t pose[88] = {2};
  replication::SpatialSend s;
  s.chunk = {10, 20, 30};
  std::memcpy(s.uuid.data(), "0123456789abcdef0123456789abcdef", 32);
  s.payload = Bytes(pose, sizeof(pose));
  s.distance = 8;
  s.decay = DecayRate::Exponential;

  constexpr long kFrames = 2000;
  auto t0 = Clock::now();
  for (long f = 0; f < kFrames; ++f)
    for (int e = 0; e < kFrameEntities; ++e) (void)conn.sendActorUpdate(s);
  auto t1 = Clock::now();
  const double perFrame = nsPerOp(t0, t1, kFrames);
  std::printf("  %-42s %8.1f us/frame  (%.1f ns/entity)\n", "sendActorUpdate x200",
              perFrame / 1000.0, perFrame / kFrameEntities);
  const auto st = conn.stats();
  std::printf("  %-42s sent=%llu deferred=%llu failed=%llu\n", "",
              (unsigned long long)st.datagramsSent, (unsigned long long)st.sendsDeferred,
              (unsigned long long)st.sendsFailed);
  conn.disconnect();
  ::close(peer);
}

}  // namespace

int main() {
  std::printf("OpenSSL: %s\n", OpenSSL_version(OPENSSL_VERSION));
  // Hardware SHA changes the absolute numbers by several times. Report it, so a
  // slow result from an engine's bundled OpenSSL is diagnosable rather than
  // mysterious.
  (void)!system(
      "grep -qw sha_ni /proc/cpuinfo && echo 'CPU SHA extensions: present' "
      "|| echo 'CPU SHA extensions: ABSENT (hashing will be several times slower)'");

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
  p.sequence = 0;

  std::uint8_t datagram[512];
  const std::size_t total =
      encodeLongSpatial(core::opensslCrypto(), p, token, MutableBytes(datagram, sizeof(datagram)))
          .value();
  const std::size_t prefixLen = kLongSpatialHeaderSize + sizeof(payload);

  encodeBreakdown(p, token);
  macVariants(datagram, prefixLen, token);
  syscallVariants(datagram, total);
  frameFlush(p, token);
  throughConnection();
  return 0;
}
