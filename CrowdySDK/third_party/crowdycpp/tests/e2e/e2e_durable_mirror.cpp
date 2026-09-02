// Data-structure e2e: durable stores and the notify-to-pull mirror against a
// live deployment — SaveStateStore/AvatarStateStore round-trips,
// ContainerMirror watch/refresh/onChange revisions over real game-model
// writes plus a channel-ping-driven refreshAll, and FileUuidStore actor
// identity surviving a reconnect (the remote side sees the same actor).
#include <atomic>
#include <cstdio>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::session;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto p = e2e::provisionPlayer(cfg, "durable");

  E2E_SUBTEST("SaveStateStore patch stays local until explicit save");
  SaveStateStore save(*p.game, cfg.appId);
  const std::uint8_t blob[] = {1, 2, 3, 4, 5};
  save.save(Bytes(blob, sizeof(blob)));
  {
    SaveStateStore fresh(*p.game, cfg.appId);
    const auto& loaded = fresh.load();
    E2E_CHECK(loaded.size() == sizeof(blob));
    E2E_CHECK(std::memcmp(loaded.data(), blob, sizeof(blob)) == 0);
    const std::uint8_t patchBytes[] = {9, 9};
    fresh.patch(1, Bytes(patchBytes, sizeof(patchBytes)));
    E2E_CHECK(fresh.dirty());
    const auto local = fresh.snapshot();
    E2E_CHECK(local[0] == 1 && local[1] == 9 && local[2] == 9 &&
              local[3] == 4);

    SaveStateStore beforeSave(*p.game, cfg.appId);
    const auto& persisted = beforeSave.load();
    E2E_CHECK(persisted.size() == sizeof(blob));
    E2E_CHECK(std::memcmp(persisted.data(), blob, sizeof(blob)) == 0);

    fresh.save();
    E2E_CHECK(!fresh.dirty());
    E2E_CHECK(fresh.lastSavedAt().has_value());
  }
  {
    SaveStateStore afterSave(*p.game, cfg.appId);
    const auto& loaded = afterSave.load();
    E2E_CHECK(loaded.size() == sizeof(blob));
    E2E_CHECK(loaded[0] == 1 && loaded[1] == 9 && loaded[2] == 9 &&
              loaded[3] == 4);
  }

  E2E_SUBTEST("AvatarStateStore identity + per-app state round-trips");
  graphql::JVal avatarInput;
  avatarInput["name"] = "durable-e2e-" + e2e::runSuffix();
  graphql::Json avatar = p.game->avatars().create(avatarInput);
  const std::string avatarId = avatar["id"].asString(avatar["avatarId"].asString());
  E2E_CHECK(!avatarId.empty());
  AvatarStateStore avatarStore(*p.game, cfg.appId, avatarId);
  const std::uint8_t identityBytes[] = {0xaa, 0xbb};
  const std::uint8_t appBytes[] = {0xcc};
  avatarStore.setIdentityState(Bytes(identityBytes, sizeof(identityBytes)));
  avatarStore.setAppState(Bytes(appBytes, sizeof(appBytes)));
  {
    AvatarStateStore fresh(*p.game, cfg.appId, avatarId);
    fresh.load();
    E2E_CHECK(fresh.identityState().size() == 2 && fresh.identityState()[0] == 0xaa);
    E2E_CHECK(fresh.appState().size() == 1 && fresh.appState()[0] == 0xcc);
  }

  E2E_SUBTEST("ContainerMirror: watch/refresh/onChange revisions");
  // Owner seeds a tiny model type + an allow-policy setter, then the mirror
  // watches a container while the value changes underneath it.
  auto& admin = e2e::ownerGame(cfg);
  const std::string typeName = "MirrorBox" + e2e::runSuffix();
  {
    graphql::JVal seed;
    seed["appId"] = cfg.appId;
    graphql::JVal type;
    type["typeName"] = typeName;
    type["displayName"] = typeName;
    type["instantiableBy"] = "member";
    seed["containerTypes"] = graphql::JVal::array({type});
    graphql::JVal prop;
    prop["containerTypeName"] = typeName;
    prop["key"] = "value";
    prop["valueType"] = "int";
    prop["defaultValueJson"] = "0";
    seed["propertyDefinitions"] = graphql::JVal::array({prop});
    graphql::JVal fn;
    fn["name"] = "mirror_set_" + e2e::runSuffix();
    fn["containerTypeName"] = typeName;
    fn["returnType"] = "int";
    graphql::JVal param;
    param["name"] = "v";
    param["valueType"] = "int";
    param["required"] = true;
    fn["parameters"] = graphql::JVal::array({param});
    graphql::JVal mutation;
    mutation["target"] = "self";
    mutation["property"] = "value";
    mutation["expression"] = "$v";
    fn["mutations"] = graphql::JVal::array({mutation});
    fn["returnExpression"] = "self.value";
    fn["invokePolicyJson"] = kit::kitPolicyJson(kit::allowPolicy());
    seed["functions"] = graphql::JVal::array({fn});
    admin.gameModel().seed(seed);
  }
  graphql::JVal containerInput;
  containerInput["appId"] = cfg.appId;
  containerInput["typeName"] = typeName;
  containerInput["displayName"] = "mirror-target";
  graphql::Json container = p.game->gameModel().createContainer(containerInput);
  const std::string containerId = container["containerId"].asString();
  E2E_CHECK(!containerId.empty());

  ContainerMirror mirror(*p.game, cfg.appId);
  int changes = 0;
  mirror.onChange([&](const MirroredContainer& c) {
    (void)c;
    ++changes;
  });
  const MirroredContainer& snapshot = mirror.watch(containerId);
  E2E_CHECK(snapshot.revision == 1);  // initial pull counts as a change
  E2E_CHECK(snapshot.value["value"].asInt64() == 0);

  auto invoke = kit::kitInvoke(p.game->gameModel(), cfg.appId, "mirror_set_" + e2e::runSuffix(),
                               containerId, [] {
                                 graphql::JVal params;
                                 params["v"] = std::int64_t{42};
                                 return params;
                               }());
  E2E_CHECK(invoke.success);
  E2E_CHECK(mirror.refresh(containerId));  // changed -> true
  E2E_CHECK(mirror.get(containerId)->value["value"].asInt64() == 42);
  E2E_CHECK(mirror.get(containerId)->revision == 2);
  E2E_CHECK(!mirror.refresh(containerId));  // unchanged -> false
  E2E_CHECK(changes == 2);

  E2E_SUBTEST("ContainerMirror: channel ping drives refreshAll");
  // The SENDER creates the channel (creators hold send rights); the watcher
  // joins as a member BEFORE its replication session is installed, then a
  // real channel notification feeds notifyChannelPing.
  auto sender = e2e::provisionPlayer(cfg, "durable-ping");
  graphql::JVal channelInput;
  channelInput["appId"] = cfg.appId;
  channelInput["name"] = "mirror-ping-" + e2e::runSuffix();
  graphql::Json channel = sender.game->channels().create(channelInput);
  const std::int64_t channelId = channel["groupId"].asBigInt();
  const std::string groupId = channel["groupId"].asString();
  mirror.bindToChannel(channelId);
  p.game->channels().join(groupId);

  e2e::connectUdp(p, cfg);
  e2e::connectUdp(sender, cfg);
  std::this_thread::sleep_for(std::chrono::seconds(1));  // membership propagation

  // Change the value behind the mirror's back, then ping the channel.
  invoke = kit::kitInvoke(p.game->gameModel(), cfg.appId, "mirror_set_" + e2e::runSuffix(),
                          containerId, [] {
                            graphql::JVal params;
                            params["v"] = std::int64_t{77};
                            return params;
                          }());
  E2E_CHECK(invoke.success);

  std::atomic<int> refreshed{0};
  replication::Handlers h;
  h.channelMessage = [&](const replication::ChannelNotification& n) {
    refreshed += static_cast<int>(mirror.notifyChannelPing(n.channelId));
  };
  p.conn->setHandlers(std::move(h));
  const auto uuid = core::generateActorUuid();
  bool pingOk = e2e::retryUntil(
      [&] { sender.conn->sendChannelMessage(channelId, uuid, asBytes("state changed")); },
      [&] {
        p.conn->poll();
        return refreshed.load() >= 1;
      });
  E2E_CHECK(pingOk);
  E2E_CHECK(mirror.get(containerId)->value["value"].asInt64() == 77);

  E2E_SUBTEST("FileUuidStore identity survives reconnect");
  const std::string uuidPath = "/tmp/crowdycpp-e2e-uuid-" + e2e::runSuffix();
  FileUuidStore uuidStore(uuidPath);
  const core::ActorUuid stable = ensureActorUuid(uuidStore);
  E2E_CHECK(ensureActorUuid(uuidStore) == stable);  // persisted, not re-minted

  const wire::ChunkCoord chunk{520000, 0, 520000};
  std::atomic<int> seenStable{0};
  replication::Handlers hs;
  hs.actorUpdate = [&](const replication::SpatialNotification& n) {
    if (std::memcmp(n.uuid, stable.data(), 32) == 0) ++seenStable;
  };
  p.conn->setHandlers(std::move(hs));

  // The observer needs spatial presence near the chunk to receive fan-out.
  const auto observerUuid = core::generateActorUuid();
  const std::uint8_t pose[] = {5};
  bool seen1 = e2e::retryUntil(
      [&] {
        p.conn->sendActorUpdate({chunk, observerUuid, Bytes(pose, 1), 8});
        sender.conn->sendActorUpdate({chunk, stable, Bytes(pose, 1), 8});
      },
      [&] {
        p.conn->poll();
        return seenStable.load() >= 1;
      });
  E2E_CHECK(seen1);

  // Reconnect the sender; the persisted uuid keeps the same identity.
  sender.conn->disconnect();
  e2e::connectUdp(sender, cfg);
  FileUuidStore reload(uuidPath);
  const core::ActorUuid after = ensureActorUuid(reload);
  E2E_CHECK(after == stable);
  const int baseline = seenStable.load();
  bool seen2 = e2e::retryUntil(
      [&] {
        p.conn->sendActorUpdate({chunk, observerUuid, Bytes(pose, 1), 8});
        sender.conn->sendActorUpdate({chunk, after, Bytes(pose, 1), 8});
      },
      [&] {
        p.conn->poll();
        return seenStable.load() > baseline;
      });
  E2E_CHECK(seen2);

  std::remove(uuidPath.c_str());
  p.conn->disconnect();
  sender.conn->disconnect();
  std::puts("e2e_durable_mirror OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
