// Two-client messaging over native UDP: proximity text, direct single-actor
// messages, and channel publish/delivery. Mirrors CrowdyJS's
// two-client-messaging / two-client-single-actor / two-client-channel suites.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

int main() {
  auto cfg = e2e::requireConfig(/*needSecondPlayer=*/true);
  auto a = e2e::signIn(cfg, cfg.email);
  auto b = e2e::signIn(cfg, cfg.email2);
  e2e::connectUdp(a, cfg);
  e2e::connectUdp(b, cfg);

  const auto uuidA = core::generateActorUuid();
  const auto uuidB = core::generateActorUuid();
  const wire::ChunkCoord chunk{2000, 0, 2000};

  std::atomic<int> bSawText{0}, bSawDirect{0}, bSawChannel{0};
  std::int64_t channelId = 0;

  Handlers hb;
  hb.text = [&](const SpatialNotification& n) {
    if (asStringView(n.payload) == "hello nearby") ++bSawText;
  };
  hb.singleActorMessage = [&](const SpatialNotification& n) {
    if (asStringView(n.payload) == "dm for b") ++bSawDirect;
  };
  hb.channelMessage = [&](const ChannelNotification& n) {
    if (n.channelId == channelId && asStringView(n.payload) == "channel hi") ++bSawChannel;
  };
  b.conn->setHandlers(std::move(hb));

  // Join presence (first actor update); keep it fresh throughout — a quiet
  // actor goes stale after a few seconds and stops receiving fan-out.
  const std::uint8_t pose[] = {1};
  auto keepAlive = [&] {
    E2E_CHECK(a.conn->sendActorUpdate({chunk, uuidA, Bytes(pose, 1), 8}).ok());
    E2E_CHECK(b.conn->sendActorUpdate({chunk, uuidB, Bytes(pose, 1), 8}).ok());
  };
  keepAlive();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Proximity text.
  bool textSeen = false;
  for (int i = 0; i < 40 && !textSeen; ++i) {
    keepAlive();
    E2E_CHECK(a.conn->sendText({chunk, uuidA, asBytes("hello nearby"), 8}).ok());
    textSeen = e2e::pollUntil(*b.conn, [&] { return bSawText.load() > 0; }, 500);
  }
  E2E_CHECK(textSeen);

  // Direct actor-to-actor message (addressed to B's uuid + chunk).
  bool directSeen = false;
  for (int i = 0; i < 40 && !directSeen; ++i) {
    keepAlive();
    E2E_CHECK(a.conn->sendSingleActorMessage(chunk, uuidB, asBytes("dm for b")).ok());
    directSeen = e2e::pollUntil(*b.conn, [&] { return bSawDirect.load() > 0; }, 500);
  }
  E2E_CHECK(directSeen);

  // Channels: A creates a channel, B joins, A publishes over UDP.
  graphql::JVal channelInput;
  channelInput["appId"] = cfg.appId;
  channelInput["name"] = "e2e-chan-" + std::to_string(core::systemClock().epochMillis());
  graphql::Json channel = a.game->channels().create(channelInput);
  const std::string groupId = channel["groupId"].asString(channel["id"].asString());
  E2E_CHECK(!groupId.empty());
  channelId = channel["channelId"].asBigInt(channel["groupId"].asBigInt());
  b.game->channels().join(groupId);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));  // membership propagation

  bool channelSeen = false;
  for (int i = 0; i < 40 && !channelSeen; ++i) {
    keepAlive();
    E2E_CHECK(a.conn->sendChannelMessage(channelId, uuidA, asBytes("channel hi")).ok());
    channelSeen = e2e::pollUntil(*b.conn, [&] { return bSawChannel.load() > 0; }, 500);
  }
  E2E_CHECK(channelSeen);

  a.conn->disconnect();
  b.conn->disconnect();
  std::puts("e2e_two_client_messaging OK");
  return 0;
}
