// Offline replication-client test: a fake in-process UDP "server" on
// 127.0.0.1 receives the client's signed messages, verifies them, and replies
// with signed notifications, bundles, error frames, and a COMMAND_RECONNECT —
// exercising Connection's full lifecycle in manual-pump mode.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "crowdy/replication/connection.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

const std::string kToken(64, 't');

struct FakeServer {
  int fd = -1;
  int port = 0;
  sockaddr_in lastClient{};
  socklen_t lastClientLen = 0;

  void start() {
    fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);
  }

  ~FakeServer() {
    if (fd >= 0) ::close(fd);
  }

  std::vector<std::uint8_t> recvOne(int timeoutMs = 2000) {
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::uint8_t buf[2048];
    lastClientLen = sizeof(lastClient);
    const ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&lastClient), &lastClientLen);
    CHECK(n > 0);
    return std::vector<std::uint8_t>(buf, buf + n);
  }

  void sendToClient(const std::uint8_t* data, std::size_t len) {
    CHECK(lastClientLen > 0);
    CHECK(::sendto(fd, data, len, 0, reinterpret_cast<sockaddr*>(&lastClient), lastClientLen) ==
          static_cast<ssize_t>(len));
  }
};

struct StubProvider final : ISessionProvider {
  int port;
  int assignCalls = 0;
  explicit StubProvider(int p) : port(p) {}
  Result<Assignment> assignServer() override {
    ++assignCalls;
    return Assignment{"127.0.0.1", "", port};
  }
  Result<TokenInfo> refreshToken() override {
    return TokenInfo{kToken, 42, 0};
  }
};

wire::Token64 token64() { return *wire::Token64::fromString(kToken); }

core::ActorUuid uuid(char fill) {
  core::ActorUuid u;
  std::memset(u.data(), fill, 32);
  return u;
}

// Build a signed server->client notification (epoch millis in the tail slot).
std::vector<std::uint8_t> makeNotification(wire::MessageType type, Bytes payload,
                                           std::int64_t epochMs, std::uint8_t seq) {
  wire::LongSpatialParams p;
  p.type = type;
  p.appId = 7;
  p.chunk = {1, 2, 3};
  p.distance = 8;
  p.uuid = uuid('b');
  p.payload = payload;
  p.gameTokenId = epochMs;  // same tail slot
  p.sequence = seq;
  std::vector<std::uint8_t> buf(wire::longSpatialSize(payload.size()));
  auto n = wire::encodeLongSpatial(core::opensslCrypto(), p, token64(),
                                   MutableBytes(buf.data(), buf.size()));
  CHECK(n.ok());
  buf.resize(n.value());
  return buf;
}

void run() {
  FakeServer server;
  server.start();
  auto provider = std::make_shared<StubProvider>(server.port);

  Config cfg;
  cfg.appId = 7;
  cfg.token = TokenInfo{kToken, 42, 0};
  cfg.manualPump = true;
  cfg.sessionReadyWaitMs = 0;

  {
    auto attemptProvider = std::make_shared<StubProvider>(server.port);
    ReplicationClient client(attemptProvider, core::opensslCrypto());
    auto attempt = client.connectWithStatus(cfg);
    CHECK(attempt.ok());
    CHECK(attempt.connection != nullptr);
    attempt.connection->disconnect();
  }

  Connection conn(cfg, provider, core::opensslCrypto());

  int actorUpdates = 0, voxelUpdates = 0, errors = 0, statusChanges = 0;
  std::vector<ConnState> states;
  Handlers handlers;
  handlers.actorUpdate = [&](const SpatialNotification& n) {
    ++actorUpdates;
    CHECK_EQ(n.appId, 7);
    CHECK_EQ(n.chunk.x, 1);
    CHECK_EQ(n.payload.size(), 4u);
    CHECK_EQ(n.epochMillis, 1700000000000LL);
  };
  handlers.voxelUpdate = [&](const SpatialNotification&, const wire::VoxelPayloadView& v) {
    ++voxelUpdates;
    CHECK_EQ(v.voxelType, 9);
  };
  handlers.genericError = [&](const GenericError& e) {
    ++errors;
    CHECK_EQ(static_cast<int>(e.code), 7);  // UNAUTHORIZED
    CHECK_EQ(e.sequence, 200u);
  };
  handlers.status = [&](ConnState s) {
    ++statusChanges;
    states.push_back(s);
  };
  conn.setHandlers(std::move(handlers));

  CHECK(conn.connect().ok());
  CHECK_EQ(provider->assignCalls, 1);

  // --- Client -> server: actor update is well-formed and verifiable.
  const std::uint8_t pose[] = {1, 2, 3, 4};
  SpatialSend send;
  send.chunk = {1, 2, 3};
  send.uuid = uuid('a');
  send.payload = Bytes(pose, sizeof(pose));
  auto seq = conn.sendActorUpdate(send);
  CHECK(seq.ok());

  auto received = server.recvOne();
  CHECK_EQ(received[0], 128u);  // ACTOR_UPDATE_REQUEST
  auto parsed = wire::parseLongSpatial(Bytes(received.data(), received.size()));
  CHECK(parsed.ok());
  CHECK_EQ(parsed->appId, 7);
  CHECK_EQ(parsed->epochMillisOrTokenId, 42);  // gameTokenId in C->S tail
  CHECK(wire::verifyLongSpatial(core::opensslCrypto(), Bytes(received.data(), received.size()),
                                token64())
            .ok());

  // Heartbeat reuses the layout with opcode 26.
  CHECK(conn.sendHeartbeat({1, 2, 3}, uuid('a')).ok());
  CHECK_EQ(server.recvOne()[0], 26u);

  // Channel publish.
  const std::uint8_t hi[] = {'h', 'i'};
  CHECK(conn.sendChannelMessage(55, uuid('a'), Bytes(hi, sizeof(hi))).ok());
  CHECK_EQ(server.recvOne()[0], 17u);

  // Send-side stats: 3 messages so far (actor update, heartbeat, channel),
  // with wire bytes and per-opcode counters tracked.
  {
    auto s = conn.stats();
    CHECK_EQ(s.messagesSent, 3u);
    CHECK_EQ(s.datagramsSent, 3u);
    CHECK(s.bytesSent > 0u);
    CHECK_EQ(s.messagesSentByType[128], 1u);  // ACTOR_UPDATE_REQUEST
    CHECK_EQ(s.messagesSentByType[26], 1u);   // CLIENT_ACTOR_HEARTBEAT
    CHECK_EQ(s.messagesSentByType[17], 1u);   // CHANNEL_MESSAGE_REQUEST
  }

  // --- Server -> client: single notification.
  auto note = makeNotification(wire::MessageType::ActorUpdateNotification,
                               Bytes(pose, sizeof(pose)), 1700000000000LL, 5);
  server.sendToClient(note.data(), note.size());

  // Bundle: voxel notification + error frame.
  std::uint8_t voxelPayload[wire::voxel::kFixedSize];
  wire::encodeVoxelPayload(1, 2, 3, 9, Bytes(), MutableBytes(voxelPayload, sizeof(voxelPayload)));
  auto voxelNote = makeNotification(wire::MessageType::VoxelUpdateNotification,
                                    Bytes(voxelPayload, sizeof(voxelPayload)), 1700000000001LL, 6);
  std::vector<std::uint8_t> bundle;
  bundle.push_back(2);
  bundle.push_back(static_cast<std::uint8_t>(voxelNote.size() & 0xff));
  bundle.push_back(static_cast<std::uint8_t>(voxelNote.size() >> 8));
  bundle.insert(bundle.end(), voxelNote.begin(), voxelNote.end());
  const std::uint8_t errFrame[] = {3, 200, 7};
  bundle.push_back(3);
  bundle.push_back(0);
  bundle.insert(bundle.end(), errFrame, errFrame + 3);
  server.sendToClient(bundle.data(), bundle.size());

  // Tampered notification must be dropped (HMAC mismatch).
  auto tampered = note;
  tampered[wire::offsets::kPayload] ^= 0xff;
  server.sendToClient(tampered.data(), tampered.size());

  // Pump + poll until everything arrives.
  for (int i = 0; i < 100 && (actorUpdates + voxelUpdates + errors) < 3; ++i) {
    conn.pump(20);
    conn.poll();
  }
  CHECK_EQ(actorUpdates, 1);
  CHECK_EQ(voxelUpdates, 1);
  CHECK_EQ(errors, 1);
  CHECK_EQ(conn.stats().hmacFailures, 1u);
  CHECK_EQ(conn.stats().lastServerEpochMs, 1700000000001LL);

  // Receive-side stats: 3 accepted messages (actor + voxel + error; the
  // tampered one dropped), unbundled per-opcode counts, and wire bytes for
  // every datagram including the dropped one.
  {
    auto s = conn.stats();
    CHECK_EQ(s.messagesReceived, 3u);
    CHECK_EQ(s.messagesReceivedByType[130], 1u);  // ACTOR_UPDATE_NOTIFICATION
    CHECK_EQ(s.messagesReceivedByType[133], 1u);  // VOXEL_UPDATE_NOTIFICATION
    CHECK_EQ(s.messagesReceivedByType[3], 1u);    // GENERIC_ERROR
    CHECK_EQ(s.datagramsReceived, 3u);
    CHECK(s.bytesReceived >= s.messagesReceived);
  }
  CHECK(statusChanges >= 1);  // Connecting -> Connected observed
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));

  // --- COMMAND_RECONNECT: verified command triggers reassignment.
  std::uint8_t rc[wire::kCommandReconnectSize];
  rc[0] = 22;
  std::uint8_t msg[1 + wire::kTokenOctets];
  msg[0] = 22;
  std::memcpy(msg + 1, kToken.data(), 64);
  CHECK(core::opensslCrypto().hmacSha256(asBytes(kToken), Bytes(msg, sizeof(msg)), rc + 1));
  server.sendToClient(rc, sizeof(rc));

  for (int i = 0; i < 100 && provider->assignCalls < 2; ++i) {
    conn.pump(20);
    conn.poll();
  }
  CHECK_EQ(provider->assignCalls, 2);
  CHECK_EQ(conn.stats().reconnects, 1u);

  // Sends still work after reassignment.
  CHECK(conn.sendActorUpdate(send).ok());
  CHECK_EQ(server.recvOne()[0], 128u);

  // Forged reconnect (bad HMAC) must NOT trigger reassignment.
  rc[5] ^= 0xff;
  server.sendToClient(rc, sizeof(rc));
  for (int i = 0; i < 5; ++i) {
    conn.pump(20);
    conn.poll();
  }
  CHECK_EQ(provider->assignCalls, 2);

  // --- AndWait: a queued self-echo with the send's sequence resolves the wait.
  conn.setHandlers({});  // the strict handlers above do not apply to this traffic
  auto seqForWait = conn.sendActorUpdate(send);
  CHECK(seqForWait.ok());
  auto sentDatagram = server.recvOne();
  auto parsedSent = wire::parseLongSpatial(Bytes(sentDatagram.data(), sentDatagram.size()));
  CHECK(parsedSent.ok());
  auto echo = makeNotification(wire::MessageType::ActorUpdateNotification,
                               Bytes(pose, sizeof(pose)), 1700000001000LL, parsedSent->sequence);
  std::memcpy(echo.data() + wire::offsets::kUuid, send.uuid.data(), 32);
  // Re-sign after patching the uuid (prefix changed).
  {
    const std::size_t prefixLen = echo.size() - wire::kTailWithHmac;
    CHECK(wire::spatialHmac(core::opensslCrypto(), Bytes(echo.data(), prefixLen), token64(),
                            echo.data() + prefixLen));
  }
  server.sendToClient(echo.data(), echo.size());
  auto outcome = conn.waitForSequence(parsedSent->sequence, send.uuid, 2000);
  CHECK(outcome.acknowledged);
  CHECK(!outcome.error.has_value());
  CHECK_EQ(outcome.serverEpochMs, 1700000001000LL);

  // AndWait: a correlated error resolves with the code instead.
  auto seqForError = conn.sendActorUpdate(send);
  CHECK(seqForError.ok());
  server.recvOne();
  const std::uint8_t errReply[] = {3, seqForError.value(), 7};
  server.sendToClient(errReply, sizeof(errReply));
  auto errOutcome = conn.waitForSequence(seqForError.value(), send.uuid, 2000);
  CHECK(!errOutcome.acknowledged);
  CHECK(errOutcome.error.has_value());
  CHECK_EQ(static_cast<int>(*errOutcome.error), 7);

  conn.disconnect();
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Closed));
}

// ---------------------------------------------------------------------------
// Send path: the send buffer knob, and backpressure told apart from failure.
// ---------------------------------------------------------------------------

// An ephemeral port that nothing is bound to. A connected UDP socket sending
// there gets ICMP port-unreachable back, which the kernel reports as
// ECONNREFUSED on the following send — a real errno for a real fault, with no
// descriptor surgery needed.
int unboundLoopbackPort() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  CHECK(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  socklen_t len = sizeof(addr);
  CHECK(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

// Provoke a genuine send fault on a socket connected to a dead port. Linux
// reports the resulting ICMP port-unreachable on the very next send; other
// kernels take a moment or, on some, never surface it to the application at
// all. Returns the first failing Status, or Ok if the platform never did.
Status provokeSendFault(UdpSocket& sock, Bytes payload) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    const Status st = sock.send(payload);
    if (!st.ok()) return st;
    ::usleep(2000);
  }
  return Errc::Ok;
}

int sockOpt(const UdpSocket& sock, int option) {
  int value = 0;
  socklen_t len = sizeof(value);
  CHECK(::getsockopt(static_cast<int>(sock.nativeHandle()), SOL_SOCKET, option, &value, &len) ==
        0);
  return value;
}

void runSendPath() {
  const std::uint8_t datagram[64] = {};
  const Bytes payload(datagram, sizeof(datagram));

  // --- A socket that was never opened is NotConnected, not a fault.
  {
    UdpSocket sock;
    CHECK_EQ(sock.send(payload).code, Errc::NotConnected);
  }

  // --- Both buffer hints reach the socket.
  //
  // The absolute size a request produces is not assertable: the kernel clamps
  // to net.core.wmem_max before Linux doubles what it granted, so asking for
  // 512 KiB yields 425984 on a stock runner whose ceiling is 212992. An
  // earlier version of this test asserted "at least what was asked for" and
  // passed only because this builder's ceiling happens to be 4 MiB.
  //
  // What holds on any system is that a larger request produces a larger
  // buffer, up to the ceiling — and a hint that never reaches setsockopt
  // cannot do that, since both sockets would come back with the same default.
  {
    FakeServer peer;
    peer.start();
    const int modest = 1 << 15;  // 32 KiB, below every plausible ceiling
    const int larger = 1 << 20;  // 1 MiB, may be clamped but must still win

    UdpSocket small;
    CHECK(small.open("127.0.0.1", peer.port, modest, modest).ok());
    CHECK(sockOpt(small, SO_SNDBUF) >= modest);
    CHECK(sockOpt(small, SO_RCVBUF) >= modest);

    UdpSocket big;
    CHECK(big.open("127.0.0.1", peer.port, larger, larger).ok());
    CHECK(sockOpt(big, SO_SNDBUF) > sockOpt(small, SO_SNDBUF));
    // The receive side too, which catches a send-side argument that displaced
    // it rather than sitting beside it.
    CHECK(sockOpt(big, SO_RCVBUF) > sockOpt(small, SO_RCVBUF));

    CHECK(big.send(payload).ok());
    CHECK_EQ(peer.recvOne().size(), sizeof(datagram));
  }

  // --- Opting out leaves the OS default alone rather than setting zero.
  {
    FakeServer peer;
    peer.start();
    UdpSocket sock;
    CHECK(sock.open("127.0.0.1", peer.port, 0, 0).ok());
    CHECK(sockOpt(sock, SO_SNDBUF) > 0);
    CHECK(sock.send(payload).ok());
  }

  // --- A genuine fault still reports SocketError. This is what keeps the
  // WouldBlock mapping from being over-broad: if it swallowed everything, a
  // dead socket would read as a busy one and no caller could ever give up.
  //
  // A datagram past the hard 65507-byte IPv4 limit is refused by every kernel
  // regardless of buffer sizes or routing, which makes it the one fault that
  // is assertable everywhere.
  {
    FakeServer peer;
    peer.start();
    UdpSocket sock;
    CHECK(sock.open("127.0.0.1", peer.port, 1 << 16, 1 << 16).ok());
    const std::vector<std::uint8_t> oversized(70000, 0);
    CHECK_EQ(sock.send(Bytes(oversized.data(), oversized.size())).code, Errc::SocketError);
    CHECK(sock.send(payload).ok());  // and the socket is still usable after
  }

  // The same conclusion from a real errno rather than a size check. Whether
  // ICMP port-unreachable reaches the application is a platform decision, so
  // this reports rather than fails when a kernel keeps it to itself.
  {
    UdpSocket sock;
    CHECK(sock.open("127.0.0.1", unboundLoopbackPort(), 1 << 16, 1 << 16).ok());
    const Status fault = provokeSendFault(sock, payload);
    if (fault.ok()) {
      std::puts("  icmp fault: not surfaced by this platform (oversize case still covers it)");
    } else {
      CHECK_EQ(fault.code, Errc::SocketError);
    }
  }

  // --- Connection separates the two counters. A real failure moves
  // sendsFailed only, and the datagram is not counted as sent.
  {
    auto provider = std::make_shared<StubProvider>(unboundLoopbackPort());
    Config cfg;
    cfg.appId = 7;
    cfg.token = TokenInfo{kToken, 42, 0};
    cfg.manualPump = true;
    cfg.sessionReadyWaitMs = 0;
    Connection conn(cfg, provider, core::opensslCrypto());
    CHECK(conn.connect().ok());

    SpatialSend p;
    p.chunk = {1, 2, 3};
    p.uuid = uuid('a');
    p.payload = payload;

    Errc observed = Errc::Ok;
    std::uint64_t attempts = 0;
    for (int i = 0; i < 50 && observed == Errc::Ok; ++i) {
      ++attempts;
      auto sent = conn.sendActorUpdate(p);
      if (!sent.ok()) observed = sent.error();
      else ::usleep(2000);
    }

    const auto s = conn.stats();
    if (observed == Errc::Ok) {
      std::puts("  stats split: platform never surfaced a send fault; counters unexercised");
      CHECK_EQ(s.sendsFailed, 0u);
      CHECK_EQ(s.datagramsSent, attempts);
    } else {
      CHECK_EQ(observed, Errc::SocketError);
      CHECK_EQ(s.sendsFailed, 1u);
      CHECK_EQ(s.sendsDeferred, 0u);
      // Every attempt is either counted as sent or as failed, never both and
      // never neither.
      CHECK_EQ(s.datagramsSent + s.sendsFailed, attempts);
      CHECK_EQ(s.messagesSent, s.datagramsSent);
    }
    conn.disconnect();
  }

  // --- Optional live backpressure evidence.
  //
  // Loopback cannot produce it: the sender's buffer is released as the packet
  // is delivered or dropped at the receiver, so a tight loopback send loop
  // never fills it and an assertion there would pass whatever this code does.
  // Real backpressure needs a destination that does not drain — an address on
  // a link-scope subnet with no host to answer ARP will hold the datagrams in
  // the unresolved-neighbour queue, charged to this socket's send buffer.
  // Point CROWDY_TEST_BACKPRESSURE_IP at such an address to exercise it.
  if (const char* host = std::getenv("CROWDY_TEST_BACKPRESSURE_IP")) {
    UdpSocket sock;
    // The smallest send buffer the kernel will grant, so the queue fills fast.
    CHECK(sock.open(host, 9999, 1 << 16, 1).ok());
    std::uint8_t burst[1232] = {};
    int sent = 0, deferred = 0, failed = 0;
    for (int i = 0; i < 2000; ++i) {
      const Status st = sock.send(Bytes(burst, sizeof(burst)));
      if (st.ok()) {
        ++sent;
      } else if (st.code == Errc::WouldBlock) {
        ++deferred;
      } else {
        ++failed;
      }
    }
    std::printf("  backpressure vs %s: sent=%d deferred=%d failed=%d\n", host, sent, deferred,
                failed);
    // The point of the exercise: saturation is never reported as a fault.
    CHECK_EQ(failed, 0);
    CHECK(deferred > 0);
  } else {
    std::puts("  backpressure: not exercised (set CROWDY_TEST_BACKPRESSURE_IP)");
  }

  // --- A descriptor closed behind the socket's back is a fault too. Kept
  // last: the socket still believes it owns the descriptor, so its destructor
  // closes the number again. Nothing opens a descriptor in between, so that
  // second close finds the number free and does nothing.
  {
    FakeServer peer;
    peer.start();
    UdpSocket sock;
    CHECK(sock.open("127.0.0.1", peer.port, 1 << 16, 1 << 16).ok());
    CHECK(::close(static_cast<int>(sock.nativeHandle())) == 0);
    CHECK_EQ(sock.send(payload).code, Errc::SocketError);
  }
}

}  // namespace

int main() {
  run();
  runSendPath();
  std::puts("replication_test OK");
  return 0;
}
