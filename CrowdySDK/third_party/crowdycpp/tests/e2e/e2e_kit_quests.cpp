// Game Kit e2e: quests — catalog, accept, trusted advance, atomic reward
// claim. The owner deploys a questsBlueprint (server advance authority, so
// the OWNER's game client bumps progress) composed with the economy wallet
// and inventory stack blueprints the claim rewards pay into; a player
// accepts a two-step quest, progress advances to completion (clamped at the
// target), and claiming grants the item stack + wallet gold in one
// transaction. Claiming before completion and double-claiming both resolve
// success:false with nothing granted. See
// docs.crowdedkingdoms.com/game-api/game-models; mirrors the CrowdyJS quests
// kit conventions.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  const std::string questPrefix = e2e::kitPrefix("Qst");
  const std::string ecoPrefix = e2e::kitPrefix("QstEco");
  const std::string invPrefix = e2e::kitPrefix("QstBag");

  GameKitOptions options;
  options.questsTypePrefix = questPrefix;
  options.economy.typePrefix = ecoPrefix;
  options.inventoryTypePrefix = invPrefix;

  auto admin = e2e::signIn(cfg, cfg.ownerEmail);
  e2e::pruneStaleAutomations(*admin.game, cfg.appId);
  auto adminKit = makeKit(*admin.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("deploy quests + economy + inventory blueprints");
  QuestsBlueprintOptions questOptions;
  questOptions.typePrefix = questPrefix;
  EconomyBlueprintOptions ecoOptions;
  ecoOptions.typePrefix = ecoPrefix;
  InventoryBlueprintOptions invOptions;
  invOptions.typePrefix = invPrefix;
  auto deployed = adminKit.deploy({questsBlueprint(questOptions), economyBlueprint(ecoOptions),
                                   inventoryBlueprint(invOptions)});
  E2E_CHECK(deployed.seed.ok());
  E2E_CHECK(deployed.automations.size() >= 1);  // the daily-reset automation
  for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

  E2E_SUBTEST("admin defines a quest; the player accepts it");
  KitQuestDefinition questDef;
  questDef.questId = "e2e_gather";
  questDef.targetCount = 2;
  questDef.rewardItemId = "e2e_relic";
  questDef.rewardQty = 3;
  questDef.rewardGold = 25;
  const std::string defId = adminKit.quests().defineQuest(questDef)["containerId"].asString();
  E2E_CHECK(!defId.empty());

  auto player = e2e::provisionPlayer(cfg, "kitqst-a");
  auto playerKit = makeKit(*player.game, cfg.appId, nullptr, options);
  {
    bool found = false;
    for (const auto& d : playerKit.quests().catalog()) {
      if (d.containerId != defId) continue;
      found = true;
      E2E_CHECK(d.questId == "e2e_gather");
      E2E_CHECK(d.targetCount == 2);
      E2E_CHECK(d.rewardGold == 25);
    }
    E2E_CHECK(found);
  }

  const std::string progressId =
      playerKit.quests().accept(player.userId, defId)["containerId"].asString();
  E2E_CHECK(!progressId.empty());
  KitQuestProgress accepted = playerKit.quests().state(progressId);
  E2E_CHECK(accepted.questId == "e2e_gather");
  E2E_CHECK(accepted.count == 0);
  E2E_CHECK(accepted.target == 2);
  E2E_CHECK(!accepted.completed);
  E2E_CHECK(playerKit.quests().mine(player.userId).size() >= 1);

  E2E_SUBTEST("progress advances via the trusted authority only");
  // Players cannot complete their own quests.
  auto selfAdvance = playerKit.quests().advance(progressId, 2);
  E2E_CHECK(!selfAdvance.success);

  const std::string walletId =
      playerKit.economy().ensureWallet(player.userId)["containerId"].asString();
  playerKit.inventory().ensure(player.userId);
  const std::string relicStack =
      playerKit.inventory().createStack("e2e_relic", 0, 0, player.userId)["containerId"].asString();

  // Claiming before completion is denied and nothing is granted.
  auto earlyClaim = playerKit.quests().claim(progressId, defId, relicStack, walletId);
  E2E_CHECK(!earlyClaim.success);
  E2E_CHECK(playerKit.economy().balance(walletId) == 0);

  auto step1 = adminKit.quests().advance(progressId, 1);
  E2E_CHECK(step1.success);
  E2E_CHECK(step1.returnValue.asInt64() == 1);
  E2E_CHECK(!playerKit.quests().state(progressId).completed);
  // Over-advancing clamps to the target; completion flips server-side.
  auto step2 = adminKit.quests().advance(progressId, 5);
  E2E_CHECK(step2.success);
  E2E_CHECK(step2.returnValue.asInt64() == 2);
  KitQuestProgress done = playerKit.quests().state(progressId);
  E2E_CHECK(done.completed);
  E2E_CHECK(!done.claimed);

  E2E_SUBTEST("claim grants stack + wallet atomically, exactly once");
  auto claimed = playerKit.quests().claim(progressId, defId, relicStack, walletId);
  E2E_CHECK(claimed.success);
  E2E_CHECK(claimed.returnValue.asInt64() == 25);  // wallet balance after the grant
  E2E_CHECK(playerKit.economy().balance(walletId) == 25);
  {
    std::int64_t qty = -1;
    for (const auto& s : playerKit.inventory().stacks(player.userId)) {
      if (s.containerId == relicStack) qty = s.quantity;
    }
    E2E_CHECK(qty == 3);
  }
  E2E_CHECK(playerKit.quests().state(progressId).claimed);

  // Double-claim is denied and grants nothing more.
  auto doubleClaim = playerKit.quests().claim(progressId, defId, relicStack, walletId);
  E2E_CHECK(!doubleClaim.success);
  E2E_CHECK(playerKit.economy().balance(walletId) == 25);
  {
    std::int64_t qty = -1;
    for (const auto& s : playerKit.inventory().stacks(player.userId)) {
      if (s.containerId == relicStack) qty = s.quantity;
    }
    E2E_CHECK(qty == 3);
  }

  std::puts("e2e_kit_quests OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "e2e_kit_quests failed: %s\n", e.what());
  return 1;
}
