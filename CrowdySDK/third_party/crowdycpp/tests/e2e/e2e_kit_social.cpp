// Game Kit e2e: social (guilds, chat, parties).
//
// The guild itself is a team paired with a chat channel: SocialKit creates
// the guild team FIRST (guildCreate over the platform teams), then the admin
// deploys guildBlueprint with that group id and a unique typePrefix — the
// composite ships a GuildHall lockable gated on guild-team membership
// (group_permission) plus a prefixed guild-bank inventory for shared
// storage. The suite verifies membership add via team join, guild chat
// published over the native UDP replication connection and received on
// another member's channelMessage handler, the hall lock (member opens,
// non-member denied with success=false), guild-bank deposit/withdraw through
// the kit's owner-gated stack conventions (overdraw and foreign withdraw
// denied), and the party helpers. See
// https://docs.crowdedkingdoms.com/game-api/teams-and-channels.
#include <atomic>
#include <cstdlib>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;
using namespace crowdy::replication;

int main() {
  try {
    auto cfg = e2e::requireConfig();
    e2e::requireOwner(cfg);
    const std::string prefix = e2e::kitPrefix("Cs");
    const GuildNames names = guildNames(prefix);

    E2E_SUBTEST("provision members and connect native UDP");
    auto a = e2e::provisionPlayer(cfg, "cs-a");  // guild leader
    auto b = e2e::provisionPlayer(cfg, "cs-b");  // joining member
    auto c = e2e::provisionPlayer(cfg, "cs-c");  // NOT a guild member
    e2e::connectUdp(a, cfg);
    e2e::connectUdp(b, cfg);

    auto kitA = makeKit(*a.game, cfg.appId, a.conn.get());
    auto kitB = makeKit(*b.game, cfg.appId, b.conn.get());
    auto kitC = makeKit(*c.game, cfg.appId, nullptr);

    E2E_SUBTEST("guild team + chat channel created first (guildCreate)");
    const std::string guildName = "cs-" + e2e::runSuffix();
    KitGuildCreateOptions guildOptions;
    guildOptions.membershipPolicy = "open";  // so B can join directly
    KitGroupWithChannel guild = kitA.social().guildCreate(guildName, guildOptions);
    E2E_CHECK(!guild.teamId.empty());
    E2E_CHECK(!guild.channelId.empty());

    // Membership add via team join (B joins the team AND the chat channel).
    b.game->teams().join(guild.teamId);
    b.game->channels().join(guild.channelId);
    bool bOnRoster = false;
    kitA.social().guildRoster(guild).forEach([&](graphql::Json m) {
      if (m["userId"].asString() == b.userId ||
          std::to_string(m["userId"].asBigInt()) == b.userId)
        bOnRoster = true;
    });
    E2E_CHECK(bOnRoster);

    E2E_SUBTEST("deploy guildBlueprint with the guild group id");
    auto& admin = e2e::ownerGame(cfg);
    auto adminKit = makeKit(admin, cfg.appId, nullptr);
    GuildBlueprintOptions blueprint;
    blueprint.typePrefix = prefix;
    blueprint.guildGroupId = guild.teamId;
    auto deployed = adminKit.deploy({guildBlueprint(blueprint)});
    E2E_CHECK(deployed.seed.ok());
    for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

    E2E_SUBTEST("guild chat over native UDP reaches another member");
    const std::string chatText = "guild hello " + e2e::runSuffix();
    std::atomic<int> chats{0};
    Handlers hb;
    const std::int64_t guildChannelId = std::strtoll(guild.channelId.c_str(), nullptr, 10);
    hb.channelMessage = [&](const ChannelNotification& n) {
      if (n.channelId == guildChannelId && asStringView(n.payload) == chatText) ++chats;
    };
    b.conn->setHandlers(std::move(hb));

    const auto uuidA = core::generateActorUuid();
    const auto uuidB = core::generateActorUuid();
    const wire::ChunkCoord chunk{400500, 0, 400000};
    const std::uint8_t pose[] = {1};
    auto keepAlive = [&] {
      (void)a.conn->sendActorUpdate({chunk, uuidA, Bytes(pose, 1), 8});
      (void)b.conn->sendActorUpdate({chunk, uuidB, Bytes(pose, 1), 8});
    };
    keepAlive();
    std::this_thread::sleep_for(std::chrono::seconds(1));  // membership propagation

    bool chatSeen = e2e::retryUntil(
        [&] {
          keepAlive();
          E2E_CHECK(kitA.social().chatSend(guildChannelId, chatText));
        },
        [&] {
          b.conn->poll();
          return chats.load() > 0;
        },
        /*attempts=*/40, /*perWaitMs=*/500);
    E2E_CHECK(chatSeen);

    E2E_SUBTEST("guild hall lock: members open, non-members denied");
    auto adminHall = adminKit.objectsFor(names.hallType);
    KitLockableCreateInput hallInput;
    hallInput.displayName = "Cs Guild Hall";
    graphql::Json hall = adminHall.create(hallInput);
    const std::string hallId = hall["containerId"].asString();
    E2E_CHECK(!hallId.empty());

    auto hallA = kitA.objectsFor(names.hallType);
    auto opened = hallA.open(hallId);
    E2E_CHECK(opened.success);
    E2E_CHECK(hallA.isOpen(hallId));
    auto hallB = kitB.objectsFor(names.hallType);
    auto closed = hallB.close(hallId);  // any member holds the group permission
    E2E_CHECK(closed.success);
    auto hallC = kitC.objectsFor(names.hallType);
    auto deniedOpen = hallC.open(hallId);
    E2E_CHECK(!deniedOpen.success);
    E2E_CHECK(!hallA.isOpen(hallId));

    E2E_SUBTEST("guild bank: deposit/withdraw via the kit stack conventions");
    InventoryKit bankA(cfg.appId, a.game->gameModel(), names.bankTypePrefix);
    InventoryKit bankB(cfg.appId, b.game->gameModel(), names.bankTypePrefix);
    graphql::Json vault = bankA.ensure(a.userId, "Cs Guild Vault");
    E2E_CHECK(!vault["containerId"].asString().empty());
    graphql::Json vaultStack = bankA.createStack("cs_gem", 0, 0, a.userId, "Vault gems");
    const std::string vaultStackId = vaultStack["containerId"].asString();
    bankA.linkStack(vault["containerId"].asString(), vaultStackId);

    auto deposited = bankA.grant(vaultStackId, 20);
    E2E_CHECK(deposited.success);
    E2E_CHECK(deposited.returnValue.asInt64() == 20);
    auto withdrawn = bankA.consume(vaultStackId, 5);
    E2E_CHECK(withdrawn.success);
    E2E_CHECK(withdrawn.returnValue.asInt64() == 15);
    // The bank refuses to overdraw, and a non-owner cannot drain the stack.
    auto overdraw = bankA.consume(vaultStackId, 100);
    E2E_CHECK(!overdraw.success);
    auto foreignWithdraw = bankB.consume(vaultStackId, 1);
    E2E_CHECK(!foreignWithdraw.success);
    // Hand-off between members: an atomic same-item transfer into B's stack.
    graphql::Json stackB = bankB.createStack("cs_gem", 0, 0, b.userId, "B gems");
    const std::string stackBId = stackB["containerId"].asString();
    auto transferred = bankA.transfer(vaultStackId, stackBId, 5);
    E2E_CHECK(transferred.success);
    graphql::Json bProps = kitContainerProperties(b.game->gameModel(), cfg.appId, stackBId);
    E2E_CHECK(bProps["quantity"].asInt64() == 5);
    E2E_CHECK(bankA.contents(vault["containerId"].asString()).size() == 1);

    E2E_SUBTEST("party helpers: create + invite roster");
    const std::string partyName = "cs-party-" + e2e::runSuffix();
    KitGroupWithChannel party = kitA.social().partyCreate(partyName);
    E2E_CHECK(!party.teamId.empty());
    kitA.social().partyInvite(party, b.userId);
    bool bInParty = false;
    kitA.social().partyMembers(party).forEach([&](graphql::Json m) {
      if (m["userId"].asString() == b.userId ||
          std::to_string(m["userId"].asBigInt()) == b.userId)
        bInParty = true;
    });
    E2E_CHECK(bInParty);
    auto found = kitB.social().partyFind(partyName);
    E2E_CHECK(found.has_value());
    E2E_CHECK(found->teamId == party.teamId);

    a.conn->disconnect();
    b.conn->disconnect();
    std::puts("e2e_kit_social OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_kit_social FAILED: %s\n", e.what());
    return 1;
  }
}
