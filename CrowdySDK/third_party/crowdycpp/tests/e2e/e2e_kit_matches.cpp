// Game Kit e2e: matches.
//
// Sessions ARE the match primitive. Player A creates a match (session +
// MatchMeta + per-match notification channel), B joins (session participation
// + channel membership), the creator drives the lifecycle (lobby -> active,
// rounds, finish -> winner recorded), turn order goes through the platform's
// session-turn authority (an is_current_turn-gated probe function shows the
// current player acts while the non-current player is denied), trusted score
// submits go through the server authority, and the notify-to-pull loop is
// exercised natively: B holds a UDP replication connection, receives the
// "match_changed" ping on its channelMessage handler after A invokes a
// lifecycle function, and onMatchChanged re-pulls the fresh state. See
// https://docs.crowdedkingdoms.com/game-api/game-models and
// https://docs.crowdedkingdoms.com/game-api/sessions-and-turns.
#include <atomic>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;
using namespace crowdy::replication;

namespace {

/// A one-function blueprint gated purely on is_current_turn, deployed next
/// to the matches blueprint so the session-turn authority is observable
/// (the matches lifecycle itself is creator-or-host gated).
KitBlueprint turnProbeBlueprint(const std::string& metaType, const std::string& fnName) {
  KitBlueprint bp;
  bp.name = "turn-probe";
  graphql::JVal fn;
  fn["name"] = fnName;
  fn["containerTypeName"] = metaType;
  fn["returnType"] = "int";
  fn["mutations"] = graphql::JVal(graphql::JArray{});
  fn["returnExpression"] = "self.round";
  fn["invokePolicyJson"] = kitPolicyJson(isCurrentTurnPolicy());
  fn["description"] = "e2e probe: succeeds only for the session's current-turn user.";
  bp.functions.push_back(std::move(fn));
  return bp;
}

}  // namespace

int main() {
  try {
    auto cfg = e2e::requireConfig();
    e2e::requireOwner(cfg);
    const std::string prefix = e2e::kitPrefix("Cm");
    const MatchesNames names = matchesNames(prefix);
    const std::string probeFn = toSnakeCase(prefix) + "_turn_probe";

    E2E_SUBTEST("deploy matches blueprint (+ turn probe)");
    auto& admin = e2e::ownerGame(cfg);
    GameKitOptions adminOptions;
    adminOptions.matchesTypePrefix = prefix;
    auto adminKit = makeKit(admin, cfg.appId, nullptr, adminOptions);
    MatchesBlueprintOptions blueprint;
    blueprint.typePrefix = prefix;
    // Server-refereed scores so the trusted submit path is deterministic in
    // this suite (the admin acts as the studio backend).
    blueprint.scoreAuthority = TrustedAuthority::server();
    auto deployed =
        adminKit.deploy({matchesBlueprint(blueprint), turnProbeBlueprint(names.metaType, probeFn)});
    E2E_CHECK(deployed.seed.ok());
    for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

    E2E_SUBTEST("provision players and connect native UDP");
    auto a = e2e::provisionPlayer(cfg, "cm-a");
    auto b = e2e::provisionPlayer(cfg, "cm-b");
    e2e::connectUdp(a, cfg);
    e2e::connectUdp(b, cfg);

    GameKitOptions playerOptions;
    playerOptions.matchesTypePrefix = prefix;
    auto kitA = makeKit(*a.game, cfg.appId, a.conn.get(), playerOptions);
    auto kitB = makeKit(*b.game, cfg.appId, b.conn.get(), playerOptions);

    E2E_SUBTEST("A creates match: session + MatchMeta + per-match channel");
    KitMatch match = kitA.matches().create(a.userId, "e2e", 2);
    E2E_CHECK(!match.sessionId.empty());
    E2E_CHECK(!match.metaId.empty());
    E2E_CHECK(match.state == "lobby");
    E2E_CHECK(match.round == 0);
    E2E_CHECK(match.channelId != "0" && !match.channelId.empty());
    E2E_CHECK(std::to_string(match.creatorUserId) == a.userId);

    // The match is discoverable while in the lobby.
    bool listed = false;
    for (const auto& open : kitB.matches().open()) {
      if (open.metaId == match.metaId) listed = true;
    }
    E2E_CHECK(listed);

    E2E_SUBTEST("both players join (session participation + channel membership)");
    try {
      kitA.matches().join(match);
    } catch (const std::exception& e) {
      // Some deployments auto-enroll the session creator; tolerate a
      // duplicate-participant rejection.
      std::printf("note: creator join refused (likely already a participant): %s\n", e.what());
    }
    graphql::Json participantB = kitB.matches().join(match);
    E2E_CHECK(participantB.ok());
    // Channel membership is what makes the notify-to-pull ping reach B.
    bool bIsMember = false;
    b.game->channels().members(match.channelId).forEach([&](graphql::Json m) {
      if (m["userId"].asString() == b.userId ||
          std::to_string(m["userId"].asBigInt()) == b.userId)
        bIsMember = true;
    });
    E2E_CHECK(bIsMember);

    // B listens for the "match_changed" ping on the native channel handler.
    std::atomic<int> pings{0};
    std::atomic<std::int64_t> pingedChannel{0};
    Handlers hb;
    hb.channelMessage = [&](const ChannelNotification& n) {
      if (asStringView(n.payload) == "match_changed" &&
          std::to_string(n.channelId) == match.channelId) {
        pingedChannel.store(n.channelId);
        ++pings;
      }
    };
    b.conn->setHandlers(std::move(hb));

    // Keep both actors present so channel fan-out reaches live connections.
    const auto uuidA = core::generateActorUuid();
    const auto uuidB = core::generateActorUuid();
    const wire::ChunkCoord chunk{400200, 0, 400000};
    const std::uint8_t pose[] = {1};
    auto keepAlive = [&] {
      (void)a.conn->sendActorUpdate({chunk, uuidA, Bytes(pose, 1), 8});
      (void)b.conn->sendActorUpdate({chunk, uuidB, Bytes(pose, 1), 8});
    };
    keepAlive();
    std::this_thread::sleep_for(std::chrono::seconds(1));  // membership propagation

    E2E_SUBTEST("lifecycle: non-creator denied; creator starts -> active, round 1");
    if (!b.game->host().amIHost(cfg.appId)) {
      auto denied = kitB.matches().start(match);
      E2E_CHECK(!denied.success);
    } else {
      std::puts("note: B is the elected app host; skipping the non-creator start denial");
    }
    auto started = kitA.matches().start(match);
    E2E_CHECK(started.success);
    match = kitA.matches().get(match.metaId);
    E2E_CHECK(match.state == "active");
    E2E_CHECK(match.round == 1);

    E2E_SUBTEST("turn order via the session-turn authority (is_current_turn)");
    bool turnSet = false;
    try {
      graphql::JVal turnInput;
      turnInput["appId"] = cfg.appId;
      turnInput["sessionId"] = match.sessionId;
      turnInput["userId"] = a.userId;
      a.game->gameModel().setSessionTurn(turnInput);
      turnSet = true;
    } catch (const std::exception& e) {
      std::printf("note: initial setSessionTurn by the creator refused (%s); using admin\n",
                  e.what());
    }
    if (!turnSet) {
      graphql::JVal turnInput;
      turnInput["appId"] = cfg.appId;
      turnInput["sessionId"] = match.sessionId;
      turnInput["userId"] = a.userId;
      admin.gameModel().setSessionTurn(turnInput);
    }
    E2E_CHECK(kitA.matches().myTurn(match, a.userId));
    E2E_CHECK(!kitB.matches().myTurn(match, b.userId));

    auto probeA = kitInvoke(a.game->gameModel(), cfg.appId, probeFn, match.metaId,
                            graphql::JVal(), match.sessionId);
    E2E_CHECK(probeA.success);
    auto probeB = kitInvoke(b.game->gameModel(), cfg.appId, probeFn, match.metaId,
                            graphql::JVal(), match.sessionId);
    E2E_CHECK(!probeB.success);

    // Pass the turn to B (current holder authority) and re-check both sides.
    kitA.matches().endTurn(match, b.userId);
    E2E_CHECK(kitB.matches().myTurn(match, b.userId));
    auto probeB2 = kitInvoke(b.game->gameModel(), cfg.appId, probeFn, match.metaId,
                             graphql::JVal(), match.sessionId);
    E2E_CHECK(probeB2.success);
    auto probeA2 = kitInvoke(a.game->gameModel(), cfg.appId, probeFn, match.metaId,
                             graphql::JVal(), match.sessionId);
    E2E_CHECK(!probeA2.success);

    E2E_SUBTEST("scores: trusted (server) submits; plain player submit denied");
    graphql::Json scoreA = kitA.matches().ensureScore(match, a.userId);
    graphql::Json scoreB = kitB.matches().ensureScore(match, b.userId);
    const std::string scoreAId = scoreA["containerId"].asString();
    const std::string scoreBId = scoreB["containerId"].asString();
    E2E_CHECK(!scoreAId.empty() && !scoreBId.empty());

    auto playerSubmit = kitA.matches().score(match, scoreAId, 999);
    E2E_CHECK(!playerSubmit.success);

    auto adminSubmitA = adminKit.matches().score(match, scoreAId, 10);
    E2E_CHECK(adminSubmitA.success);
    E2E_CHECK(adminSubmitA.returnValue.asInt64() == 10);
    auto adminSubmitB = adminKit.matches().score(match, scoreBId, 7);
    E2E_CHECK(adminSubmitB.success);

    auto standings = kitB.matches().standings(match);
    E2E_CHECK(standings.size() == 2);
    E2E_CHECK(standings[0].ownerUserId == a.userId && standings[0].points == 10);
    E2E_CHECK(standings[1].ownerUserId == b.userId && standings[1].points == 7);

    E2E_SUBTEST("notify-to-pull: lifecycle invoke pings B's channelMessage handler");
    pings.store(0);
    bool pinged = e2e::retryUntil(
        [&] {
          keepAlive();
          auto advanced = kitA.matches().advanceRound(match);
          E2E_CHECK(advanced.success);
        },
        [&] {
          b.conn->poll();
          return pings.load() > 0;
        },
        /*attempts=*/20, /*perWaitMs=*/500);
    E2E_CHECK(pinged);

    // onMatchChanged re-pulls the fresh state for the pinged channel.
    KitMatch fresh = match;
    bool matched = kitB.matches().onMatchChanged(match, pingedChannel.load(),
                                                 [&](const KitMatch& m) { fresh = m; });
    E2E_CHECK(matched);
    E2E_CHECK(fresh.state == "active");
    E2E_CHECK(fresh.round >= 2);
    E2E_CHECK(fresh.round == kitA.matches().get(match.metaId).round);

    E2E_SUBTEST("end match: winner recorded; post-finish lifecycle denied");
    const std::int64_t winner = std::strtoll(a.userId.c_str(), nullptr, 10);
    auto finished = kitA.matches().finish(match, winner);
    E2E_CHECK(finished.success);
    match = kitB.matches().get(match.metaId);
    E2E_CHECK(match.state == "finished");
    E2E_CHECK(match.winnerUserId == winner);
    auto afterFinish = kitA.matches().advanceRound(match);
    E2E_CHECK(!afterFinish.success);

    a.conn->disconnect();
    b.conn->disconnect();
    std::puts("e2e_kit_matches OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_kit_matches FAILED: %s\n", e.what());
    return 1;
  }
}
