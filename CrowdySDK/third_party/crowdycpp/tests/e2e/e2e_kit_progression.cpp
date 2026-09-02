// Game Kit e2e: progression — XP/levels, skills, achievements, rating. The
// owner deploys a progressionBlueprint (server-scope XP and rating
// authority, so the OWNER's game client performs the trusted grants); a
// player's level then rises along the fn:xp_for_level curve, a skill with an
// unmet prerequisite is refused until the prerequisite rank is bought, an
// achievement unlock is idempotent per the kit contract, and rating adjusts
// only through the authority. See
// docs.crowdedkingdoms.com/game-api/game-models; mirrors the CrowdyJS
// progression kit conventions.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  const std::string prefix = e2e::kitPrefix("Prog");

  GameKitOptions options;
  options.progressionTypePrefix = prefix;

  auto admin = e2e::signIn(cfg, cfg.ownerEmail);
  auto adminKit = makeKit(*admin.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("deploy the progression blueprint");
  ProgressionBlueprintOptions progOptions;
  progOptions.typePrefix = prefix;
  // Server authority for ratings too, so the owner's client (not an elected
  // match host) can submit them in this suite.
  progOptions.ratingAuthority = TrustedAuthority::server();
  auto deployed = adminKit.deploy({progressionBlueprint(progOptions)});
  E2E_CHECK(deployed.seed.ok());
  for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

  auto player = e2e::provisionPlayer(cfg, "kitprog-a");
  auto playerKit = makeKit(*player.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("XP grants level up along the 100 * level^2 curve");
  const std::string progressId =
      playerKit.progression().ensure(player.userId)["containerId"].asString();
  E2E_CHECK(!progressId.empty());
  KitProgress start = playerKit.progression().state(progressId);
  E2E_CHECK(start.level == 1);
  E2E_CHECK(start.xp == 0);
  E2E_CHECK(start.rating == 1000);

  // Players cannot grant their own XP (trusted-authority convention).
  auto selfGrant = playerKit.progression().grantXp(progressId, 1000);
  E2E_CHECK(!selfGrant.success);

  // Level 2 requires 400 xp on the default curve; one level per grant.
  auto grant1 = adminKit.progression().grantXp(progressId, 400);
  E2E_CHECK(grant1.success);
  E2E_CHECK(grant1.returnValue.asInt64() == 2);
  KitProgress afterLevel2 = playerKit.progression().state(progressId);
  E2E_CHECK(afterLevel2.level == 2);
  E2E_CHECK(afterLevel2.xp == 400);
  E2E_CHECK(afterLevel2.skillPoints == 1);

  // Level 3 requires 900 xp total.
  auto grant2 = adminKit.progression().grantXp(progressId, 500);
  E2E_CHECK(grant2.success);
  E2E_CHECK(grant2.returnValue.asInt64() == 3);
  KitProgress afterLevel3 = playerKit.progression().state(progressId);
  E2E_CHECK(afterLevel3.level == 3);
  E2E_CHECK(afterLevel3.skillPoints == 2);

  E2E_SUBTEST("skills: unmet prerequisite refused, then unlocked in order");
  const std::string basicDefId =
      adminKit.progression().defineSkill("e2e_basic", 1)["containerId"].asString();
  const std::string advancedDefId =
      adminKit.progression()
          .defineSkill("e2e_advanced", 1, "e2e_basic")["containerId"]
          .asString();
  E2E_CHECK(playerKit.progression().skillCatalog().size() >= 2);

  const std::string basicRankId =
      playerKit.progression().ensureSkillRank(player.userId, "e2e_basic")["containerId"].asString();
  const std::string advancedRankId =
      playerKit.progression()
          .ensureSkillRank(player.userId, "e2e_advanced")["containerId"]
          .asString();

  // The prerequisite rank is still 0 — buying the advanced skill fails.
  auto premature =
      playerKit.progression().buySkill(advancedRankId, progressId, advancedDefId, basicRankId);
  E2E_CHECK(!premature.success);

  auto boughtBasic = playerKit.progression().buySkill(basicRankId, progressId, basicDefId);
  E2E_CHECK(boughtBasic.success);
  E2E_CHECK(boughtBasic.returnValue.asInt64() == 1);

  auto boughtAdvanced =
      playerKit.progression().buySkill(advancedRankId, progressId, advancedDefId, basicRankId);
  E2E_CHECK(boughtAdvanced.success);
  E2E_CHECK(boughtAdvanced.returnValue.asInt64() == 1);
  KitProgress afterSkills = playerKit.progression().state(progressId);
  E2E_CHECK(afterSkills.skillPoints == 0);  // both points spent
  // Buying past max_rank (1) with no points left is refused.
  auto overMax = playerKit.progression().buySkill(basicRankId, progressId, basicDefId);
  E2E_CHECK(!overMax.success);

  E2E_SUBTEST("achievements: threshold-gated, idempotent per the kit contract");
  const std::string achDefId =
      adminKit.progression().defineAchievement("e2e_novice", 400)["containerId"].asString();
  const std::string richDefId =
      adminKit.progression().defineAchievement("e2e_legend", 1000000)["containerId"].asString();
  E2E_CHECK(playerKit.progression().achievementCatalog().size() >= 2);

  // Threshold not met -> denied.
  auto tooEarly = playerKit.progression().unlockAchievement(player.userId, progressId,
                                                            richDefId, "e2e_legend");
  E2E_CHECK(!tooEarly.success);

  auto unlocked = playerKit.progression().unlockAchievement(player.userId, progressId, achDefId,
                                                            "e2e_novice");
  E2E_CHECK(unlocked.success);
  E2E_CHECK(unlocked.returnValue.asBool());
  // Second award: the kit contract is idempotent — re-invoking just
  // re-writes unlocked=true, and no duplicate unlock row appears.
  auto again = playerKit.progression().unlockAchievement(player.userId, progressId, achDefId,
                                                         "e2e_novice");
  E2E_CHECK(again.success);
  std::size_t noviceRows = 0;
  for (const auto& a : playerKit.progression().achievements(player.userId)) {
    if (a.achievementId == "e2e_novice") {
      ++noviceRows;
      E2E_CHECK(a.unlocked);
    }
  }
  E2E_CHECK(noviceRows == 1);

  E2E_SUBTEST("rating adjusts only via the authority");
  auto selfRating = playerKit.progression().applyMatchResult(progressId, 999);
  E2E_CHECK(!selfRating.success);
  auto ratingUp = adminKit.progression().applyMatchResult(progressId, 25);
  E2E_CHECK(ratingUp.success);
  E2E_CHECK(ratingUp.returnValue.asInt64() == 1025);
  auto ratingDown = adminKit.progression().applyMatchResult(progressId, -50);
  E2E_CHECK(ratingDown.success);
  E2E_CHECK(ratingDown.returnValue.asInt64() == 975);
  E2E_CHECK(playerKit.progression().state(progressId).rating == 975);

  std::puts("e2e_kit_progression OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "e2e_kit_progression failed: %s\n", e.what());
  return 1;
}
