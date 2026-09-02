// Teams and channels: team CRUD + custom roles + join policy + membership,
// with idempotent leave/delete; channels with membership-gated realtime
// publish over native UDP (member delivery succeeds, non-member publish is
// refused or silently dropped). Mirrors Game API SDK e2e: teams + channels.
// Reference: https://docs.crowdedkingdoms.com/game-api/groups
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

int run() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto a = e2e::provisionPlayer(cfg, "teams-a");
  auto b = e2e::provisionPlayer(cfg, "teams-b");
  auto c = e2e::provisionPlayer(cfg, "teams-c");  // never joins the channel

  E2E_SUBTEST("owner sets a permissive team policy");
  {
    graphql::JVal policy;
    policy["appId"] = cfg.appId;
    policy["creationPolicy"] = "member";
    policy["defaultMembershipPolicy"] = "open";
    graphql::Json set = e2e::ownerGame(cfg).teams().setPolicy(policy);
    E2E_CHECK(set["creationPolicy"].asString() == "member");
    graphql::Json read = a.game->teams().policy(cfg.appId);
    E2E_CHECK(read["defaultMembershipPolicy"].asString() == "open");
  }

  E2E_SUBTEST("create / get / mine / list");
  const std::string teamName = "E2E Team " + e2e::runSuffix();
  graphql::JVal teamInput;
  teamInput["appId"] = cfg.appId;
  teamInput["name"] = teamName;
  teamInput["membershipPolicy"] = "open";
  graphql::Json team = a.game->teams().create(teamInput);
  const std::string groupId = team["groupId"].asString();
  E2E_CHECK(!groupId.empty());
  E2E_CHECK(team["ownerUserId"].asString() == a.userId);

  E2E_CHECK(a.game->teams().get(groupId)["name"].asString() == teamName);
  bool inMine = false;
  a.game->teams().mine(cfg.appId).forEach([&](graphql::Json membership) {
    if (membership["group"]["groupId"].asString() == groupId) inMine = true;
  });
  E2E_CHECK(inMine);
  bool inList = false;
  a.game->teams().list(cfg.appId).forEach([&](graphql::Json entry) {
    if (entry["groupId"].asString() == groupId) inList = true;
  });
  E2E_CHECK(inList);

  E2E_SUBTEST("custom role + member add + setMemberRoles");
  graphql::JVal roleInput;
  roleInput["groupId"] = groupId;
  roleInput["roleName"] = "e2e-officer-" + e2e::runSuffix();
  roleInput["permissions"] = graphql::JVal::array({graphql::JVal("manage_members")});
  roleInput["rank"] = std::int64_t{5};
  graphql::Json role = a.game->teams().createRole(roleInput);
  const std::string roleId = role["groupRoleId"].asString();
  E2E_CHECK(!roleId.empty());

  graphql::Json added = a.game->teams().addMember(groupId, b.userId);
  E2E_CHECK(added["userId"].asString() == b.userId);

  graphql::JVal rolesInput;
  rolesInput["groupId"] = groupId;
  rolesInput["userId"] = b.userId;
  rolesInput["roleIds"] = graphql::JVal::array({graphql::JVal(roleId)});
  graphql::Json rolesSet = a.game->teams().setMemberRoles(rolesInput);
  bool hasRole = false;
  rolesSet["roles"].forEach([&](graphql::Json r) {
    if (r["groupRoleId"].asString() == roleId) hasRole = true;
  });
  E2E_CHECK(hasRole);
  E2E_CHECK(a.game->teams().members(groupId).size() >= 2);

  E2E_SUBTEST("remove member, open join, idempotent leave");
  E2E_CHECK(a.game->teams().removeMember(groupId, b.userId).asBool());
  graphql::Json joined = b.game->teams().join(groupId);  // open policy
  E2E_CHECK(joined["userId"].asString() == b.userId);
  const std::string leaveKey = "e2e-team-leave-" + e2e::runSuffix();
  E2E_CHECK(b.game->teams().leave(groupId, leaveKey).asBool());
  // Replay with the same key succeeds identically instead of NOT_A_MEMBER.
  E2E_CHECK(b.game->teams().leave(groupId, leaveKey).asBool());

  E2E_SUBTEST("idempotent team delete");
  const std::string deleteKey = "e2e-team-del-" + e2e::runSuffix();
  E2E_CHECK(a.game->teams().remove(groupId, deleteKey).asBool());
  E2E_CHECK(a.game->teams().remove(groupId, deleteKey).asBool());

  // ----- Channels ------------------------------------------------------------

  E2E_SUBTEST("channel create + member join");
  graphql::JVal channelInput;
  channelInput["appId"] = cfg.appId;
  channelInput["name"] = "e2e-chan-" + e2e::runSuffix();
  graphql::Json channel = a.game->channels().create(channelInput);
  const std::string channelGroupId = channel["groupId"].asString();
  E2E_CHECK(!channelGroupId.empty());
  const std::int64_t channelId = channel["groupId"].asBigInt();
  b.game->channels().join(channelGroupId);
  E2E_CHECK(b.game->channels().members(channelGroupId).size() >= 2);

  E2E_SUBTEST("member publish delivers over native UDP");
  e2e::connectUdp(a, cfg);
  e2e::connectUdp(b, cfg);
  e2e::connectUdp(c, cfg);

  const auto uuidA = core::generateActorUuid();
  const auto uuidB = core::generateActorUuid();
  const auto uuidC = core::generateActorUuid();
  const wire::ChunkCoord chunk{200700, 0, 200700};

  std::atomic<int> bSawMember{0}, bSawIntruder{0}, cSawError{0};
  Handlers hb;
  hb.channelMessage = [&](const ChannelNotification& n) {
    if (n.channelId != channelId) return;
    if (asStringView(n.payload) == "member hi") ++bSawMember;
    if (asStringView(n.payload) == "intruder") ++bSawIntruder;
  };
  b.conn->setHandlers(std::move(hb));
  Handlers hc;
  hc.genericError = [&](const GenericError& err) {
    if (err.code == wire::ErrorCode::Unauthorized) ++cSawError;
  };
  c.conn->setHandlers(std::move(hc));

  const std::uint8_t pose[] = {1};
  auto keepAlive = [&] {
    E2E_CHECK(a.conn->sendActorUpdate({chunk, uuidA, Bytes(pose, 1), 8}).ok());
    E2E_CHECK(b.conn->sendActorUpdate({chunk, uuidB, Bytes(pose, 1), 8}).ok());
    E2E_CHECK(c.conn->sendActorUpdate({chunk, uuidC, Bytes(pose, 1), 8}).ok());
  };
  keepAlive();
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));  // membership propagation

  bool memberDelivered = false;
  for (int i = 0; i < 40 && !memberDelivered; ++i) {
    keepAlive();
    E2E_CHECK(a.conn->sendChannelMessage(channelId, uuidA, asBytes("member hi")).ok());
    memberDelivered = e2e::pollUntil(*b.conn, [&] { return bSawMember.load() > 0; }, 500);
  }
  E2E_CHECK(memberDelivered);

  E2E_SUBTEST("non-member publish draws UNAUTHORIZED or silence");
  for (int i = 0; i < 10; ++i) {
    keepAlive();
    E2E_CHECK(c.conn->sendChannelMessage(channelId, uuidC, asBytes("intruder")).ok());
    e2e::pollUntil(*c.conn, [&] { return cSawError.load() > 0; }, 300);
    b.conn->poll();
  }
  // Give any straggling fan-out a moment, then require: either an explicit
  // UNAUTHORIZED(7) error frame at the sender, or no delivery to the member.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  b.conn->poll();
  c.conn->poll();
  E2E_CHECK(cSawError.load() > 0 || bSawIntruder.load() == 0);

  E2E_SUBTEST("deleteChannel");
  E2E_CHECK(a.game->channels().remove(channelGroupId).asBool());
  // Reading a deleted channel either throws "Group N not found" or returns a
  // non-active record, depending on the deployment's soft-delete behavior.
  bool goneOrInactive = false;
  try {
    graphql::Json afterDelete = a.game->channels().get(channelGroupId);
    goneOrInactive = afterDelete.isNull() || afterDelete["groupId"].asString().empty() ||
                     afterDelete["status"].asString() != "active";
  } catch (const graphql::CrowdyGraphQLError&) {
    goneOrInactive = true;
  }
  E2E_CHECK(goneOrInactive);

  a.conn->disconnect();
  b.conn->disconnect();
  c.conn->disconnect();
  std::puts("e2e_teams_channels OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
