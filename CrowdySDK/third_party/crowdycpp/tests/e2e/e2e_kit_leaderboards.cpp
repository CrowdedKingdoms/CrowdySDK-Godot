// Game Kit e2e: leaderboards.
//
// An admin deploys the leaderboards blueprint (unique typePrefix per run)
// with the server submit authority: only the trusted authority (the studio
// backend / app admin) may write scores — a plain player-invoked submit
// resolves success=false. With keepBest (the default) a lower re-submit
// leaves the score unchanged while a higher one updates it. Ranking is
// client-side by convention (no server-side ORDER BY on container lists):
// board()/top()/around() sort best-first with 1-based positions, verified
// over three players. See
// https://docs.crowdedkingdoms.com/game-api/game-models.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() {
  try {
    auto cfg = e2e::requireConfig();
    e2e::requireOwner(cfg);
    const std::string prefix = e2e::kitPrefix("Cl");
    const std::string boardId = "clb-" + e2e::runSuffix();

    E2E_SUBTEST("deploy leaderboards blueprint (server submit authority)");
    auto& admin = e2e::ownerGame(cfg);
    GameKitOptions adminOptions;
    adminOptions.leaderboardsTypePrefix = prefix;
    auto adminKit = makeKit(admin, cfg.appId, nullptr, adminOptions);
    LeaderboardsBlueprintOptions blueprint;
    blueprint.typePrefix = prefix;
    blueprint.submitAuthority = TrustedAuthority::server();
    auto deployed = adminKit.deploy({leaderboardsBlueprint(blueprint)});
    E2E_CHECK(deployed.seed.ok());
    for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

    E2E_SUBTEST("three players ensure their board entries");
    auto p1 = e2e::provisionPlayer(cfg, "cl-1");
    auto p2 = e2e::provisionPlayer(cfg, "cl-2");
    auto p3 = e2e::provisionPlayer(cfg, "cl-3");
    GameKitOptions playerOptions;
    playerOptions.leaderboardsTypePrefix = prefix;
    auto kit1 = makeKit(*p1.game, cfg.appId, nullptr, playerOptions);
    auto kit2 = makeKit(*p2.game, cfg.appId, nullptr, playerOptions);
    auto kit3 = makeKit(*p3.game, cfg.appId, nullptr, playerOptions);

    const std::string entry1 =
        kit1.leaderboards().ensureEntry(p1.userId, boardId)["containerId"].asString();
    const std::string entry2 =
        kit2.leaderboards().ensureEntry(p2.userId, boardId)["containerId"].asString();
    const std::string entry3 =
        kit3.leaderboards().ensureEntry(p3.userId, boardId)["containerId"].asString();
    E2E_CHECK(!entry1.empty() && !entry2.empty() && !entry3.empty());
    // ensureEntry is find-or-create: a second call returns the same row.
    E2E_CHECK(kit1.leaderboards().ensureEntry(p1.userId, boardId)["containerId"].asString() ==
              entry1);

    E2E_SUBTEST("plain player-invoked submit is denied");
    auto playerSubmit = kit1.leaderboards().submit(entry1, 12345);
    E2E_CHECK(!playerSubmit.success);
    E2E_CHECK(kit1.leaderboards().board(boardId).front().score == 0);

    E2E_SUBTEST("trusted submits keep the BEST score");
    auto s1 = adminKit.leaderboards().submit(entry1, 100);
    E2E_CHECK(s1.success && s1.returnValue.asInt64() == 100);
    auto s2 = adminKit.leaderboards().submit(entry2, 50);
    E2E_CHECK(s2.success && s2.returnValue.asInt64() == 50);
    auto s3 = adminKit.leaderboards().submit(entry3, 75);
    E2E_CHECK(s3.success && s3.returnValue.asInt64() == 75);

    // Lower re-submit -> unchanged (keepBest).
    auto lower = adminKit.leaderboards().submit(entry1, 40);
    E2E_CHECK(lower.success);
    E2E_CHECK(lower.returnValue.asInt64() == 100);
    // Higher re-submit -> updated.
    auto higher = adminKit.leaderboards().submit(entry2, 120);
    E2E_CHECK(higher.success);
    E2E_CHECK(higher.returnValue.asInt64() == 120);

    E2E_SUBTEST("board()/top()/around() rank best-first");
    std::vector<KitLeaderboardEntry> board = kit3.leaderboards().board(boardId);
    E2E_CHECK(board.size() == 3);
    E2E_CHECK(board[0].ownerUserId == p2.userId && board[0].score == 120 &&
              board[0].position == 1);
    E2E_CHECK(board[1].ownerUserId == p1.userId && board[1].score == 100 &&
              board[1].position == 2);
    E2E_CHECK(board[2].ownerUserId == p3.userId && board[2].score == 75 &&
              board[2].position == 3);

    std::vector<KitLeaderboardEntry> top2 = kit3.leaderboards().top(boardId, 2);
    E2E_CHECK(top2.size() == 2);
    E2E_CHECK(top2[0].ownerUserId == p2.userId);
    E2E_CHECK(top2[1].ownerUserId == p1.userId);

    // Neighborhood around the middle player covers all three entries.
    std::vector<KitLeaderboardEntry> near = kit3.leaderboards().around(boardId, p1.userId, 1);
    E2E_CHECK(near.size() == 3);
    E2E_CHECK(near[0].ownerUserId == p2.userId);
    E2E_CHECK(near[1].ownerUserId == p1.userId);
    E2E_CHECK(near[2].ownerUserId == p3.userId);
    // Around the top player there is no one above (radius clipped).
    std::vector<KitLeaderboardEntry> aroundTop =
        kit3.leaderboards().around(boardId, p2.userId, 1);
    E2E_CHECK(aroundTop.size() == 2);
    E2E_CHECK(aroundTop[0].ownerUserId == p2.userId);

    std::puts("e2e_kit_leaderboards OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_kit_leaderboards FAILED: %s\n", e.what());
    return 1;
  }
}
