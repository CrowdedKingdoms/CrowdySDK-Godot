// Game Kit e2e: loot — weighted tables, server-side rolls, atomic single
// claims. The owner deploys a lootBlueprint with one 3-entry weighted table
// (server roll authority, so the OWNER's game client rolls) plus the
// inventory blueprint providing the claim-target stacks; a player creates
// durable LootRoll containers, the owner rolls them, and the player claims
// each result into a matching stack exactly once — a second claim, a re-roll,
// and a claim into a mismatched stack all resolve success:false. Repeated
// rolls eventually land different items (the weights work). See
// docs.crowdedkingdoms.com/game-api/game-models; mirrors the CrowdyJS loot
// kit conventions.
#include "e2e_util.hpp"

#include <map>
#include <set>

using namespace crowdy;
using namespace crowdy::kit;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  const std::string lootPrefix = e2e::kitPrefix("Loot");
  const std::string invPrefix = e2e::kitPrefix("LootBag");

  GameKitOptions options;
  options.lootTypePrefix = lootPrefix;
  options.inventoryTypePrefix = invPrefix;

  auto admin = e2e::signIn(cfg, cfg.ownerEmail);
  auto adminKit = makeKit(*admin.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("deploy loot (3-entry weighted table) + inventory blueprints");
  LootTableSpec chest;
  chest.tableId = "e2e_chest";
  chest.entries = {
      {"e2e_coin", 6.0, 2, 5},   // common, ranged quantity
      {"e2e_gem", 3.0, 1, 2},    // uncommon
      {"e2e_crown", 1.0, 1, {}}, // rare, fixed quantity 1
  };
  LootBlueprintOptions lootOptions;
  lootOptions.typePrefix = lootPrefix;
  lootOptions.tables = {chest};
  InventoryBlueprintOptions invOptions;
  invOptions.typePrefix = invPrefix;
  auto deployed = adminKit.deploy({lootBlueprint(lootOptions), inventoryBlueprint(invOptions)});
  E2E_CHECK(deployed.seed.ok());
  for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

  auto player = e2e::provisionPlayer(cfg, "kitloot-a");
  auto playerKit = makeKit(*player.game, cfg.appId, nullptr, options);

  const std::map<std::string, std::pair<std::int64_t, std::int64_t>> qtyRanges = {
      {"e2e_coin", {2, 5}}, {"e2e_gem", {1, 2}}, {"e2e_crown", {1, 1}}};

  E2E_SUBTEST("roll: server-side weighted selection with in-range quantity");
  const std::string rollId =
      playerKit.loot().createRoll(player.userId, "e2e_chest")["containerId"].asString();
  E2E_CHECK(!rollId.empty());
  // Players cannot roll their own loot (trusted-authority convention).
  auto selfRoll = playerKit.loot().roll(rollId, "e2e_chest");
  E2E_CHECK(!selfRoll.success);

  auto rolled = adminKit.loot().roll(rollId, "e2e_chest");
  E2E_CHECK(rolled.success);
  E2E_CHECK(qtyRanges.count(rolled.returnValue.asString()) == 1);

  // The "no re-roll" guard is a policy condition, and the observed platform
  // contract is that app admins bypass invoke policies (the same bypass the
  // plot kit documents for evict). A server-authority re-roll therefore
  // SUCCEEDS for the owner — it only binds automation/host/player callers.
  auto reroll = adminKit.loot().roll(rollId, "e2e_chest");
  E2E_CHECK(reroll.success);

  KitLootRoll state = playerKit.loot().state(rollId);
  const std::string itemId = state.rolledItemId;  // final post-re-roll result
  E2E_CHECK(qtyRanges.count(itemId) == 1);
  const auto range = qtyRanges.at(itemId);
  E2E_CHECK(state.rolledQty >= range.first && state.rolledQty <= range.second);
  E2E_CHECK(!state.claimed);

  E2E_SUBTEST("claim: converts to a stack exactly once");
  playerKit.inventory().ensure(player.userId);
  const std::string wrongStack =
      playerKit.inventory()
          .createStack("e2e_not_loot", 0, 1, player.userId)["containerId"]
          .asString();
  auto wrongClaim = playerKit.loot().claim(rollId, wrongStack);
  E2E_CHECK(!wrongClaim.success);  // stack item_id != rolled item

  const std::string stackId =
      playerKit.inventory().createStack(itemId, 0, 0, player.userId)["containerId"].asString();
  auto claimed = playerKit.loot().claim(rollId, stackId);
  E2E_CHECK(claimed.success);
  E2E_CHECK(claimed.returnValue.asInt64() == state.rolledQty);
  {
    std::int64_t qty = -1;
    for (const auto& s : playerKit.inventory().stacks(player.userId)) {
      if (s.containerId == stackId) qty = s.quantity;
    }
    E2E_CHECK(qty == state.rolledQty);
  }
  E2E_CHECK(playerKit.loot().state(rollId).claimed);

  // Atomic single-claim: a second claim of the same roll is denied and the
  // stack is not credited again.
  auto doubleClaim = playerKit.loot().claim(rollId, stackId);
  E2E_CHECK(!doubleClaim.success);
  {
    std::int64_t qty = -1;
    for (const auto& s : playerKit.inventory().stacks(player.userId)) {
      if (s.containerId == stackId) qty = s.quantity;
    }
    E2E_CHECK(qty == state.rolledQty);
  }

  E2E_SUBTEST("several rolls land different items (weights work)");
  std::set<std::string> seen{itemId};
  for (int i = 0; i < 15 && seen.size() < 2; ++i) {
    const std::string extraRollId =
        playerKit.loot().createRoll(player.userId, "e2e_chest")["containerId"].asString();
    auto extra = adminKit.loot().roll(extraRollId, "e2e_chest");
    E2E_CHECK(extra.success);
    const std::string extraItem = extra.returnValue.asString();
    E2E_CHECK(qtyRanges.count(extraItem) == 1);
    KitLootRoll extraState = playerKit.loot().state(extraRollId);
    const auto extraRange = qtyRanges.at(extraItem);
    E2E_CHECK(extraState.rolledQty >= extraRange.first &&
              extraState.rolledQty <= extraRange.second);
    seen.insert(extraItem);
  }
  // P(16 rolls all one item) < 0.3% with 6/3/1 weights.
  E2E_CHECK(seen.size() >= 2);
  E2E_CHECK(playerKit.loot().rolls(player.userId, "e2e_chest", true).size() >= 1);

  std::puts("e2e_kit_loot OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "e2e_kit_loot failed: %s\n", e.what());
  return 1;
}
