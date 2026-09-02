// Data-structure e2e: the WorldSession stores against a live deployment —
// remote-actor lanes classify real fan-out (join/leave callbacks), the
// EventRouter routes live typed events, the local actor tracks real
// self-echo acks and boundary sends, and AndWait correlates against real
// servers (acknowledged and error outcomes).
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::session;
using namespace crowdy::replication;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto a = e2e::provisionPlayer(cfg, "stores-a");
  auto b = e2e::provisionPlayer(cfg, "stores-b");
  e2e::connectUdp(a, cfg);
  e2e::connectUdp(b, cfg);

  const wire::ChunkCoord chunk{500000, 0, 500000};

  // A runs a WorldSession; B is a plain connection we drive by hand.
  WorldSessionConfig sc;
  sc.appId = cfg.appId;
  sc.self.sendHz = 5;
  sc.actors.staleAfterMs = 6000;
  sc.hostHeartbeatIntervalMs = 0;
  WorldSession session(a.conn, a.game.get(), sc);

  E2E_SUBTEST("lanes classify tagged actors from live fan-out");
  int joins = 0, leaves = 0, updates = 0;
  // Marker byte discriminating the "tagged" lane (e.g. mobs vs players).
  constexpr std::uint8_t kMobTag = 0x4d;  // 'M'
  auto& tagLane = session.actors().lane(
      "tagged", {[](const SpatialNotification& n) {
                   return !n.payload.empty() && n.payload[0] == kMobTag;
                 },
                 6000, 2});
  tagLane.onJoin([&](const RemoteActor&) { ++joins; });
  tagLane.onLeave([&](const RemoteActor&) { ++leaves; });
  tagLane.onUpdate([&](const RemoteActor&) { ++updates; });

  const std::uint8_t selfPose[] = {1, 2, 3};
  E2E_CHECK(session.join(chunk, Bytes(selfPose, sizeof(selfPose))).ok());

  const auto bUuid = core::generateActorUuid();
  const std::uint8_t tagged[] = {kMobTag, 9};
  const std::uint8_t untagged[] = {0x00, 9};
  bool laneOk = e2e::retryUntil(
      [&] {
        b.conn->sendActorUpdate({chunk, bUuid, Bytes(tagged, sizeof(tagged)), 8});
        session.tick();
      },
      [&] {
        session.tick();
        return joins >= 1 && tagLane.find(bUuid) != nullptr;
      });
  E2E_CHECK(laneOk);
  E2E_CHECK(updates >= 1);
  // The default lane sees B too; the tagged lane filtered correctly.
  E2E_CHECK(session.actors().find(bUuid) != nullptr);

  // An untagged actor must not enter the tagged lane.
  const auto cUuid = core::generateActorUuid();
  bool defaultSeesC = e2e::retryUntil(
      [&] {
        b.conn->sendActorUpdate({chunk, cUuid, Bytes(untagged, sizeof(untagged)), 8});
        session.tick();
      },
      [&] {
        session.tick();
        return session.actors().find(cUuid) != nullptr;
      });
  E2E_CHECK(defaultSeesC);
  E2E_CHECK(tagLane.find(cUuid) == nullptr);

  E2E_SUBTEST("EventRouter routes live typed events");
  std::atomic<int> doorEvents{0};
  constexpr std::uint16_t kDoorEvent = 4242;
  session.events().on(kDoorEvent, [&](const EventRouter::Event& e) {
    if (e.state.size() == 2 && e.state[0] == 0xca) ++doorEvents;
  });
  const std::uint8_t eventState[] = {0xca, 0xfe};
  bool eventOk = e2e::retryUntil(
      [&] {
        b.conn->sendClientEvent(chunk, bUuid, kDoorEvent, Bytes(eventState, sizeof(eventState)),
                                8);
      },
      [&] {
        session.tick();
        return doorEvents.load() >= 1;
      });
  E2E_CHECK(eventOk);
  E2E_CHECK(session.events().lastEvent(kDoorEvent) != nullptr);
  E2E_CHECK(session.events().lastEvent(kDoorEvent)->senderUuid == bUuid);

  E2E_SUBTEST("local actor tracks real self-echo acks");
  bool ackOk = e2e::retryUntil(
      [&] {
        const std::uint8_t moved[] = {1, 2, 4};
        session.self().setState(Bytes(moved, sizeof(moved)));
        session.tick();
      },
      [&] {
        session.tick();
        return session.self().lastAck().has_value();
      });
  E2E_CHECK(ackOk);
  E2E_CHECK(session.self().lastAck()->serverEpochMs > 0);

  E2E_SUBTEST("moveTo sends immediately on a boundary crossing");
  const wire::ChunkCoord nextChunk{500001, 0, 500000};
  E2E_CHECK(session.self().moveTo(nextChunk).ok());
  E2E_CHECK(session.self().chunk() == nextChunk);

  E2E_SUBTEST("AndWait against real servers: acknowledged and error");
  auto outcome = b.conn->sendActorUpdateAndWait(
      {chunk, bUuid, Bytes(tagged, sizeof(tagged)), 8}, 8000);
  E2E_CHECK(outcome.acknowledged);
  E2E_CHECK(outcome.serverEpochMs > 0);

  // A voxel send into wilderness far from any grid the player holds should
  // draw an error frame (or, if the tier's world grid covers everything,
  // acknowledge) — accept either but require a definitive outcome, proving
  // waitForSequence correlates against real traffic.
  auto voxelOutcome = b.conn->sendVoxelUpdateAndWait({900000, 0, 900000}, bUuid, 1, 1, 1, 5,
                                                     Bytes(), 8, wire::DecayRate::None, 8000);
  E2E_CHECK(voxelOutcome.acknowledged || voxelOutcome.error.has_value());

  E2E_SUBTEST("staleness reaps the lane with onLeave");
  // Stop B's traffic; A keeps ticking until the tagged actor expires.
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(12) &&
         tagLane.find(bUuid) != nullptr) {
    session.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  E2E_CHECK(tagLane.find(bUuid) == nullptr);
  E2E_CHECK(leaves >= 1);

  session.dispose();
  b.conn->disconnect();
  std::puts("e2e_stores_live OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
