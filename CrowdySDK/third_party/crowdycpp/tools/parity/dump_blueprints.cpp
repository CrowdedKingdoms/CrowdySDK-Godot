// Dump every CrowdyCPP blueprint builder's output as JSON for the blueprint
// equivalence diff (tools/parity/blueprints-diff.mjs). The variant matrix
// MUST stay in lockstep with tools/parity/dump-blueprints.mjs.
//
// Build (from the repo root):
//   cmake -S . -B build-parity -DCROWDY_BUILD_PARITY_TOOLS=ON
//   cmake --build build-parity --target crowdy_blueprint_dump
#include <cstdio>
#include <functional>
#include <map>

#include "crowdy/kit/combat.hpp"
#include "crowdy/kit/decks.hpp"
#include "crowdy/kit/economy.hpp"
#include "crowdy/kit/inventory.hpp"
#include "crowdy/kit/leaderboards.hpp"
#include "crowdy/kit/loot.hpp"
#include "crowdy/kit/matches.hpp"
#include "crowdy/kit/npcs.hpp"
#include "crowdy/kit/objects.hpp"
#include "crowdy/kit/plots.hpp"
#include "crowdy/kit/progression.hpp"
#include "crowdy/kit/quests.hpp"
#include "crowdy/kit/social.hpp"
#include "crowdy/kit/worldsim.hpp"

using namespace crowdy::kit;
using crowdy::graphql::JArray;
using crowdy::graphql::JVal;

namespace {

JVal blueprintToJson(const KitBlueprint& bp) {
  JVal v;
  v["name"] = bp.name;
  if (!bp.containerTypes.empty()) v["containerTypes"] = JVal(bp.containerTypes);
  if (!bp.propertyDefinitions.empty())
    v["propertyDefinitions"] = JVal(bp.propertyDefinitions);
  if (!bp.functions.empty()) v["functions"] = JVal(bp.functions);
  if (!bp.containers.empty()) v["containers"] = JVal(bp.containers);
  if (!bp.edges.empty()) v["edges"] = JVal(bp.edges);
  if (!bp.automations.empty()) v["automations"] = JVal(bp.automations);
  if (!bp.automationTriggers.empty()) v["automationTriggers"] = JVal(bp.automationTriggers);
  return v;
}

}  // namespace

int main() {
  std::map<std::string, std::function<KitBlueprint()>> variants;

  variants["inventory_default"] = [] { return inventoryBlueprint(); };
  variants["inventory_bank"] = [] {
    InventoryBlueprintOptions options;
    options.typePrefix = "Bank";
    options.maxSlots = 10;
    options.slotCount = 8;
    return inventoryBlueprint(options);
  };
  variants["lock_key"] = [] {
    LockBlueprintOptions o;
    o.authority = {LockAuthority::key()};
    return lockBlueprint(o);
  };
  variants["lock_chest"] = [] {
    LockBlueprintOptions o;
    o.objectTypeName = "Chest";
    o.authority = {LockAuthority::owner(), LockAuthority::chunkPermission("access", "smallest")};
    return lockBlueprint(o);
  };
  variants["npc_wander"] = [] {
    NpcBehaviorSpec behavior;
    behavior.name = "npc-wander";
    behavior.trigger = NpcBehaviorTrigger::interval(60000);
    JVal mutation;
    mutation["target"] = "self";
    mutation["property"] = "x";
    mutation["expression"] = "self.x + rand_int(-2, 2)";
    behavior.mutations = {mutation};
    NpcBlueprintOptions o;
    o.behaviors = {behavior};
    return npcBlueprint(o);
  };
  variants["plots_default"] = [] { return plotBlueprint({}); };
  variants["plots_rentable"] = [] {
    PlotBlueprintOptions o;
    o.rentable = true;
    return plotBlueprint(o);
  };
  variants["economy_default"] = [] { return economyBlueprint({}); };
  variants["progression_default"] = [] { return progressionBlueprint({}); };
  variants["loot_chest"] = [] {
    LootTableSpec table;
    table.tableId = "chest";
    LootEntrySpec sword;
    sword.itemId = "sword";
    sword.weight = 2;
    LootEntrySpec gold;
    gold.itemId = "gold";
    gold.weight = 6;
    gold.minQty = 5;
    gold.maxQty = 20;
    LootEntrySpec gem;
    gem.itemId = "gem";
    gem.weight = 1;
    table.entries = {sword, gold, gem};
    LootBlueprintOptions o;
    o.tables = {table};
    return lootBlueprint(o);
  };
  variants["quests_default"] = [] { return questsBlueprint({}); };
  variants["combat_default"] = [] { return combatBlueprint({}); };
  variants["matches_default"] = [] { return matchesBlueprint({}); };
  variants["matches_turn_timer"] = [] {
    MatchesBlueprintOptions o;
    o.turnTimer = MatchTurnTimer{30000};
    return matchesBlueprint(o);
  };
  variants["matches_turn_tick"] = [] {
    MatchesBlueprintOptions o;
    o.turnTick = MatchTurnTick{15000};
    return matchesBlueprint(o);
  };
  variants["decks_default"] = [] { return decksBlueprint({}); };
  variants["worldsim_default"] = [] { return worldsimBlueprint({}); };
  variants["guild_default"] = [] {
    GuildBlueprintOptions o;
    o.guildGroupId = "77";
    return guildBlueprint(o);
  };
  variants["leaderboards_default"] = [] { return leaderboardsBlueprint({}); };

  JVal out;
  for (auto& [name, build] : variants) {
    try {
      out[name] = blueprintToJson(build());
    } catch (const std::exception& e) {
      JVal err;
      err["__error"] = std::string(e.what());
      out[name] = std::move(err);
    }
  }
  std::puts(out.dump().c_str());
  return 0;
}
