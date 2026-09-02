// Offline session-layer test over the fake-server pattern from
// replication_test: two "players" through one fake server would need real
// fan-out, so instead the fake server replays notifications and we assert
// the stores (self send loop, remote registry, chunk merge, inboxes, error
// attribution) behave per the World Stores contract.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "crowdy/session/world_session.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::session;
using namespace crowdy::replication;

namespace {

const std::string kToken(64, 't');
wire::Token64 token64() { return *wire::Token64::fromString(kToken); }

struct FakeServer {
  int fd = -1;
  int port = 0;
  sockaddr_in client{};
  socklen_t clientLen = 0;

  void start() {
    fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
  }
  ~FakeServer() {
    if (fd >= 0) ::close(fd);
  }

  std::vector<std::uint8_t> recvOne(int timeoutMs = 2000) {
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::uint8_t buf[2048];
    clientLen = sizeof(client);
    const ssize_t n =
        ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&client), &clientLen);
    CHECK(n > 0);
    return std::vector<std::uint8_t>(buf, buf + n);
  }
  void reply(const std::uint8_t* data, std::size_t len) {
    CHECK(::sendto(fd, data, len, 0, reinterpret_cast<sockaddr*>(&client), clientLen) ==
          static_cast<ssize_t>(len));
  }
};

struct StubProvider final : ISessionProvider {
  int port;
  explicit StubProvider(int p) : port(p) {}
  Result<Assignment> assignServer() override { return Assignment{"127.0.0.1", "", port}; }
  Result<TokenInfo> refreshToken() override { return TokenInfo{kToken, 42, 0}; }
};

core::ActorUuid uuidOf(char fill) {
  core::ActorUuid u;
  std::memset(u.data(), fill, 32);
  return u;
}

std::vector<std::uint8_t> makeNotification(wire::MessageType type, const core::ActorUuid& uuid,
                                           const wire::ChunkCoord& chunk, Bytes payload,
                                           std::int64_t epochMs, std::uint8_t seq) {
  wire::LongSpatialParams p;
  p.type = type;
  p.appId = 7;
  p.chunk = chunk;
  p.distance = 8;
  p.uuid = uuid;
  p.payload = payload;
  p.gameTokenId = epochMs;
  p.sequence = seq;
  std::vector<std::uint8_t> buf(wire::longSpatialSize(payload.size()));
  auto n = wire::encodeLongSpatial(core::opensslCrypto(), p, token64(),
                                   MutableBytes(buf.data(), buf.size()));
  CHECK(n.ok());
  return buf;
}

void run() {
  FakeServer server;
  server.start();

  Config cfg;
  cfg.appId = 7;
  cfg.token = TokenInfo{kToken, 42, 0};
  cfg.manualPump = true;
  cfg.sessionReadyWaitMs = 0;
  auto conn = std::make_shared<Connection>(
      cfg, std::make_shared<StubProvider>(server.port),
      core::defaultCrypto());
  CHECK(conn->connect().ok());

  WorldSessionConfig sess;
  sess.appId = "7";
  sess.self.sendHz = 1000;  // effectively every tick
  sess.self.heartbeatIntervalMs = 0;
  sess.actors.staleAfterMs = 200;
  sess.actors.historySize = 2;
  sess.hostHeartbeatIntervalMs = 0;  // no CrowdyClient in this test
  sess.reapIntervalMs = 10;
  WorldSession session(conn, nullptr, sess);

  // --- Join sends the first actor update.
  const std::uint8_t pose[] = {9, 9, 9, 9};
  CHECK(session.join({0, 0, 0}, Bytes(pose, sizeof(pose))).ok());
  auto joinMsg = server.recvOne();
  CHECK_EQ(joinMsg[0], 128u);
  auto parsedJoin = wire::parseLongSpatial(Bytes(joinMsg.data(), joinMsg.size()));
  CHECK(parsedJoin.ok());
  CHECK(std::memcmp(parsedJoin->uuid, session.actorUuid().data(), 32) == 0);
  CHECK(session.self().lastSent().has_value());
  CHECK_EQ(session.self().state().len, sizeof(pose));
  CHECK_EQ(session.self().status(), LocalActorStatus::Pending);

  // --- Send-on-change: unchanged state does not resend before the keyframe.
  session.tick();
  session.self().setState(Bytes(pose, sizeof(pose)));  // identical -> no dirty
  // Changed state resends.
  const std::uint8_t pose2[] = {1, 2, 3, 4};
  session.self().setState(Bytes(pose2, sizeof(pose2)));
  for (int i = 0; i < 10; ++i) session.tick();
  auto changed = server.recvOne();
  CHECK_EQ(changed[0], 128u);

  // --- Remote actor ingest: another player's update lands in the registry.
  const auto other = uuidOf('z');
  const std::uint8_t otherPose[] = {5, 5};
  auto note = makeNotification(wire::MessageType::ActorUpdateNotification, other, {1, 0, 0},
                               Bytes(otherPose, sizeof(otherPose)), 1700000000000LL, 1);
  server.reply(note.data(), note.size());
  auto note2 = makeNotification(wire::MessageType::ActorUpdateNotification, other, {1, 0, 0},
                                Bytes(otherPose, sizeof(otherPose)), 1700000000100LL, 2);
  server.reply(note2.data(), note2.size());

  // Self-echo must be filtered out of the remote registry.
  auto selfNote = makeNotification(wire::MessageType::ActorUpdateNotification,
                                   session.actorUuid(), {0, 0, 0},
                                   Bytes(pose, sizeof(pose)), 1700000000200LL, 3);
  server.reply(selfNote.data(), selfNote.size());

  for (int i = 0; i < 100 && session.actors().size() < 1; ++i) {
    conn->pump(20);
    session.tick();
  }
  CHECK_EQ(session.actors().size(), 1u);
  const RemoteActor* remote = session.actors().find(other);
  CHECK(remote != nullptr);
  CHECK_EQ(remote->chunk.x, 1);
  CHECK_EQ(remote->samples.size(), 2u);                          // history kept
  CHECK_EQ(remote->lastServerEpochMs, 1700000000100LL);          // newest first
  CHECK_EQ(remote->state().size(), 2u);
  CHECK(session.actors().revision() >= 2);
  CHECK_EQ(session.self().status(), LocalActorStatus::Acked);

  // --- Voxel merge into the chunk cache.
  std::uint8_t voxelPayload[wire::voxel::kFixedSize + 2];
  const std::uint8_t meta[] = {0xca, 0xfe};
  wire::encodeVoxelPayload(3, 4, 5, 7, Bytes(meta, sizeof(meta)),
                           MutableBytes(voxelPayload, sizeof(voxelPayload)));
  auto voxelNote = makeNotification(wire::MessageType::VoxelUpdateNotification, other, {2, 0, 0},
                                    Bytes(voxelPayload, sizeof(voxelPayload)), 1700000000300LL, 4);
  server.reply(voxelNote.data(), voxelNote.size());

  // --- Channel + direct messages into inboxes.
  {
    wire::ChannelMessageParams cm;
    cm.channelId = 99;
    cm.uuid = other;
    const std::uint8_t hi[] = {'h', 'i'};
    cm.payload = Bytes(hi, sizeof(hi));
    cm.gameTokenId = 42;
    cm.sequence = 5;
    // Server->client channel notification layout differs from the request;
    // build it directly.
    std::uint8_t frame[128];
    frame[0] = 18;
    le::writeI64(frame + wire::channel::kChannelIdOffset, 99);
    std::memcpy(frame + wire::channel::kUuidOffset, other.data(), 32);
    le::writeU16(frame + wire::channel::kPayloadLenOffset, 2);
    frame[wire::channel::kPayloadOffset] = 'h';
    frame[wire::channel::kPayloadOffset + 1] = 'i';
    le::writeI64(frame + wire::channel::kPayloadOffset + 2, 1700000000400LL);
    frame[wire::channel::kPayloadOffset + 10] = 5;
    server.reply(frame, wire::channel::kPayloadOffset + 11);
  }
  auto direct = makeNotification(wire::MessageType::SingleActorMessage, session.actorUuid(),
                                 {0, 0, 0}, Bytes(reinterpret_cast<const std::uint8_t*>("dm"), 2),
                                 1700000000500LL, 6);
  server.reply(direct.data(), direct.size());

  // --- Error attribution.
  const std::uint8_t seqUsed = *session.self().lastSequence();
  const std::uint8_t errFrame[] = {3, seqUsed, 7};  // UNAUTHORIZED for our actor send
  server.reply(errFrame, sizeof(errFrame));

  for (int i = 0;
       i < 200 && (session.chunks().size() < 1 || session.channelInbox().size() < 1 ||
                   session.directInbox().size() < 1 || session.errors().recent().empty());
       ++i) {
    conn->pump(20);
    session.tick();
  }

  const ChunkData* chunk = session.chunks().find({2, 0, 0});
  CHECK(chunk != nullptr);
  CHECK_EQ(chunk->voxels[static_cast<std::size_t>(voxelIndex(3, 4, 5))], 7u);
  auto vs = chunk->voxelStates.find(voxelIndex(3, 4, 5));
  CHECK(vs != chunk->voxelStates.end());
  CHECK_EQ(vs->second.state.size(), 2u);

  auto channelMsgs = session.channelInbox().drain();
  CHECK_EQ(channelMsgs.size(), 1u);
  CHECK_EQ(channelMsgs[0].channelId, 99);
  CHECK_EQ(channelMsgs[0].payload.size(), 2u);

  auto dms = session.directInbox().drain();
  CHECK_EQ(dms.size(), 1u);
  CHECK_EQ(dms[0].payload.size(), 2u);

  CHECK(!session.errors().recent().empty());
  CHECK_EQ(session.errors().total(), std::uint64_t{1});
  const AttributedError& err = session.errors().recent().front();
  CHECK_EQ(static_cast<int>(err.code), 7);
  CHECK_EQ(static_cast<int>(err.kind), static_cast<int>(SendKind::ActorUpdate));
  CHECK(err.actorUuid.has_value());
  CHECK(*err.actorUuid == session.actorUuid());
  CHECK(session.errors().last() == &err);
  CHECK(session.errors().lastFor(session.actorUuid()) == &err);
  CHECK_EQ(session.errors().recent(1).size(), 1u);

  session.errors().recordSend(250, SendKind::VoxelUpdate, other);
  session.errors().ingest(
      {250, static_cast<wire::ErrorCode>(8)}, 1700000000600LL);
  CHECK_EQ(session.errors().total(), std::uint64_t{2});
  CHECK_EQ(session.errors().recent().front().sequence, 250);
  CHECK_EQ(session.errors().recent().back().sequence, seqUsed);
  CHECK(session.errors().last() == &session.errors().recent().front());
  CHECK(session.errors().lastFor(other) ==
        &session.errors().recent().front());

  // --- Optimistic local voxel edit sends a VOXEL_UPDATE_REQUEST.
  const std::uint8_t stateBytes[] = {1};
  auto seq = session.chunks().setVoxel({2, 0, 0}, 1, 1, 1, 3, Bytes(stateBytes, 1),
                                       session.actorUuid());
  CHECK(seq.ok());
  const ChunkData* edited = session.chunks().find({2, 0, 0});
  CHECK_EQ(edited->voxels[static_cast<std::size_t>(voxelIndex(1, 1, 1))], 3u);
  CHECK(session.chunks().revision() >= 2);
  CHECK_EQ(session.chunks().pendingWriteBacks(), std::size_t{1});
  // Drain until we see the voxel request (actor keyframes may interleave).
  bool sawVoxel = false;
  for (int i = 0; i < 5 && !sawVoxel; ++i) sawVoxel = server.recvOne()[0] == 131;
  CHECK(sawVoxel);

  // --- Lanes, callbacks, and the event router (phase-2 store surface).
  int laneJoins = 0, laneLeaves = 0;
  auto& mobLane = session.actors().lane("mobs", {[](const replication::SpatialNotification& sn) {
                                                   return sn.payload.size() == 3;  // mob tag
                                                 },
                                                 200, 2});
  mobLane.onJoin([&](const RemoteActor&) { ++laneJoins; });
  mobLane.onLeave([&](const RemoteActor&) { ++laneLeaves; });

  int routedEvents = 0;
  session.events().on(42, [&](const EventRouter::Event& e) {
    ++routedEvents;
    CHECK_EQ(e.eventType, 42u);
    CHECK_EQ(e.state.size(), 1u);
  });

  const auto mob = uuidOf('m');
  const std::uint8_t mobPose[] = {7, 7, 7};
  auto mobNote = makeNotification(wire::MessageType::ActorUpdateNotification, mob, {1, 0, 0},
                                  Bytes(mobPose, sizeof(mobPose)), 1700000000600LL, 8);
  server.reply(mobNote.data(), mobNote.size());

  std::uint8_t eventPayload[wire::kEventTypeSize + 1];
  const std::uint8_t eventState[] = {9};
  wire::encodeEventPayload(42, Bytes(eventState, 1),
                           MutableBytes(eventPayload, sizeof(eventPayload)));
  auto eventNote = makeNotification(wire::MessageType::ClientEventNotification, other, {1, 0, 0},
                                    Bytes(eventPayload, sizeof(eventPayload)), 1700000000700LL, 9);
  server.reply(eventNote.data(), eventNote.size());

  for (int i = 0; i < 100 && (laneJoins < 1 || routedEvents < 1); ++i) {
    conn->pump(20);
    session.tick();
  }
  CHECK_EQ(laneJoins, 1);
  CHECK(mobLane.find(mob) != nullptr);
  CHECK(mobLane.revision() >= 1);
  CHECK_EQ(routedEvents, 1);
  CHECK(session.events().lastEvent(42) != nullptr);
  CHECK(session.events().lastEvent(43) == nullptr);

  // ChunkStore ergonomics.
  CHECK_EQ(session.chunks().voxelTypeAt({2, 0, 0}, 3, 4, 5), 7u);
  CHECK(session.chunks().voxelStateAt({2, 0, 0}, 3, 4, 5) != nullptr);
  CHECK(session.chunks().list().size() >= 1);
  CHECK_EQ(session.chunks().pruneBeyond({2, 0, 0}, 0), 0u);  // only chunk 2,0,0 cached

  // --- Staleness reaping: with no more traffic the remote actors expire
  // (default lane and named lanes; onLeave fires).
  for (int i = 0; i < 100 && (session.actors().size() > 0 || mobLane.size() > 0); ++i) {
    conn->pump(5);
    session.tick();
  }
  CHECK_EQ(session.actors().size(), 0u);
  CHECK_EQ(mobLane.size(), 0u);
  CHECK_EQ(laneLeaves, 1);
  CHECK(mobLane.revision() >= 2);
  session.errors().clear();
  CHECK(session.errors().recent().empty());
  CHECK_EQ(session.errors().total(), std::uint64_t{2});

  session.dispose();
}

// An ephemeral port nothing is bound to: a connected UDP socket sending there
// takes ICMP port-unreachable and reports ECONNREFUSED on the next send.
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

// A heartbeat that never reached the wire must not be recorded as sent.
// Banking the interval for a send that did not happen lets presence lapse on a
// saturated or briefly broken socket while every counter still reads healthy.
//
// The failing socket here alternates strictly: a send to a port with no
// listener succeeds, the ICMP port-unreachable arms the socket error, the next
// send reports ECONNREFUSED and clears it, and because that send transmitted
// nothing there is no new ICMP — so the pattern is ok, fail, ok, fail. The
// join below is the first "ok", which leaves the next send due to fail.
void runHeartbeatNotSent() {
  Config cfg;
  cfg.appId = 7;
  cfg.token = TokenInfo{kToken, 42, 0};
  cfg.manualPump = true;
  cfg.sessionReadyWaitMs = 0;
  auto conn = std::make_shared<Connection>(
      cfg, std::make_shared<StubProvider>(unboundLoopbackPort()), core::defaultCrypto());
  CHECK(conn->connect().ok());

  LocalActorStore::Options options;
  options.sendHz = 1000;                 // a tick slot every millisecond
  options.keyframeIntervalMs = 1000000;  // never due, so the heartbeat branch runs
  options.heartbeatIntervalMs = 100;
  LocalActorStore self(*conn, uuidOf('a'), options);

  const std::uint8_t pose[] = {1, 2, 3, 4};
  CHECK(self.join({0, 0, 0}, Bytes(pose, sizeof(pose))).ok());
  const auto afterJoin = conn->stats();
  CHECK_EQ(afterJoin.datagramsSent, 1u);
  CHECK_EQ(afterJoin.sendsFailed, 0u);

  // Whether ICMP port-unreachable is surfaced to the application is a platform
  // decision. Where it is not, there is no portable way to make this
  // connection's socket fail, so say so rather than assert something that
  // cannot happen here.
  if (conn->sendHeartbeat({0, 0, 0}, uuidOf('a')).ok()) {
    std::puts("  heartbeat retry: platform does not surface the fault; scenario skipped");
    conn->disconnect();
    return;
  }
  CHECK_EQ(conn->stats().sendsFailed, 1u);
  CHECK_EQ(conn->stats().datagramsSent, 1u);

  // The probe above consumed the pending error, so the next send succeeds and
  // the one after that fails: line the parity up so the tick at t=100 lands on
  // a failing send.
  CHECK(conn->sendHeartbeat({0, 0, 0}, uuidOf('a')).ok());

  // t=100: the heartbeat is due and the socket is in its failing phase.
  const auto before = conn->stats();
  self.tick(100);
  const auto afterFailed = conn->stats();
  CHECK_EQ(afterFailed.sendsFailed - before.sendsFailed, 1u);
  CHECK_EQ(afterFailed.datagramsSent, before.datagramsSent);  // it did not go out
  CHECK_EQ(afterFailed.sendsDeferred, 0u);                    // a fault, not backpressure

  // t=101: because nothing was sent, the heartbeat is still owed and the very
  // next tick must try again. A store that recorded the failed attempt as sent
  // goes quiet here until another full interval has passed.
  self.tick(101);
  CHECK_EQ(conn->stats().datagramsSent, before.datagramsSent + 1);

  // A genuine fault is still reported to the application. The WouldBlock
  // exemption in sendNow must not have turned into "never record an error".
  const std::uint8_t moved[] = {9, 9, 9, 9};
  self.setState(Bytes(moved, sizeof(moved)));
  const auto beforeDirty = conn->stats();
  self.tick(200);  // dirty send, and the socket is failing again
  CHECK_EQ(conn->stats().sendsFailed - beforeDirty.sendsFailed, 1u);
  CHECK(self.lastError().has_value());
  CHECK_EQ(self.lastError()->status.code, Errc::SocketError);
  CHECK_EQ(static_cast<int>(self.status()), static_cast<int>(LocalActorStatus::Error));

  // ...and the update it could not send is still owed, so the next tick sends
  // it rather than dropping the state change.
  self.tick(201);
  CHECK_EQ(conn->stats().datagramsSent, beforeDirty.datagramsSent + 1);

  conn->disconnect();
}

}  // namespace

int main() {
  run();
  runHeartbeatNotSent();
  std::puts("session_test OK");
  return 0;
}
