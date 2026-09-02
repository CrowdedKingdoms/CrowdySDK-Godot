// Game Kit e2e: NPCs — an admin deploys an npcBlueprint with one interval
// wander behavior (autonomousInvocable, is_automation-gated), spawns an NPC
// instance, enables the app's automation policy, and kicks the automation
// with runNow; the suite polls the NPC's server-side state until the
// behavior mutated it, checks stats()/runs() recorded the run, verifies
// players cannot puppet the behavior function, and probes the
// disabled-automation runNow contract. See
// docs.crowdedkingdoms.com/game-api/autonomous-processes; mirrors the
// CrowdyJS NPC kit conventions.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  const std::string npcType = e2e::kitPrefix("Goblin");
  const std::string wanderFn = "wander_" + toSnakeCase(npcType);
  const std::string automationName = "wander-" + toSnakeCase(npcType);

  GameKitOptions options;
  options.npcTypeName = npcType;

  auto admin = e2e::signIn(cfg, cfg.ownerEmail);
  e2e::pruneStaleAutomations(*admin.game, cfg.appId);
  auto adminKit = makeKit(*admin.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("deploy the NPC blueprint (one interval wander behavior)");
  NpcBehaviorSpec wander;
  wander.name = automationName;
  wander.functionName = wanderFn;
  {
    graphql::JVal moveX;
    moveX["target"] = "self";
    moveX["property"] = "x";
    moveX["expression"] = "self.x + rand_int(-2, 2)";
    graphql::JVal markState;
    markState["target"] = "self";
    markState["property"] = "behavior_state";
    markState["expression"] = "\"wandering\"";
    wander.mutations = {std::move(moveX), std::move(markState)};
  }
  wander.trigger = NpcBehaviorTrigger::interval(60000);
  NpcBlueprintOptions npcOptions;
  npcOptions.typeName = npcType;
  npcOptions.behaviors = {wander};
  auto deployed = adminKit.deploy({npcBlueprint(npcOptions)});
  E2E_CHECK(deployed.seed.ok());
  E2E_CHECK(deployed.automations.size() == 1);
  for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

  E2E_SUBTEST("enable the app automation policy (owner)");
  graphql::JVal policyInput;
  policyInput["appId"] = cfg.appId;
  policyInput["enabled"] = true;
  graphql::Json policyRes = admin.game->gameModel().setAutomationPolicy(policyInput);
  E2E_CHECK(policyRes.ok());

  E2E_SUBTEST("spawn an NPC instance");
  KitNpcSpawnInput spawn;
  spawn.displayName = "E2E Goblin";
  spawn.role = "wanderer";
  spawn.position = KitNpcPosition{10.0, 0.0, 10.0};
  graphql::Json npcContainer = adminKit.npcs().spawn(spawn);
  const std::string npcId = npcContainer["containerId"].asString();
  E2E_CHECK(!npcId.empty());
  KitNpc before = adminKit.npcs().state(npcId);
  E2E_CHECK(before.behaviorState == "idle");
  E2E_CHECK(adminKit.npcs().list("wanderer").size() >= 1);

  E2E_SUBTEST("players cannot puppet the is_automation-gated behavior");
  auto player = e2e::provisionPlayer(cfg, "kitnpc-a");
  auto denied = kitInvoke(player.game->gameModel(), cfg.appId, wanderFn, npcId);
  E2E_CHECK(!denied.success);

  E2E_SUBTEST("runNow kicks the interval behavior immediately");
  graphql::Json run = adminKit.npcs().runNow(automationName);
  E2E_CHECK(!run["runId"].asString().empty());
  E2E_CHECK(run["success"].asBool());
  bool mutated = false;
  for (int i = 0; i < 40 && !mutated; ++i) {
    KitNpc now = adminKit.npcs().state(npcId);
    mutated = now.behaviorState == "wandering";
    if (!mutated) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  E2E_CHECK(mutated);
  KitNpc after = adminKit.npcs().state(npcId);
  // The wander delta is rand_int(-2, 2) applied to x = 10.
  E2E_CHECK(after.x >= 8.0 && after.x <= 12.0);

  E2E_SUBTEST("stats and run history show the run");
  graphql::Json stats = adminKit.npcs().stats(15);
  E2E_CHECK(stats["totalRuns"].asInt64() >= 1);
  graphql::Json runs = adminKit.npcs().runs(automationName, std::nullopt, 10);
  E2E_CHECK(runs.size() >= 1);
  E2E_CHECK(runs.at(0)["automationName"].asString() == automationName);
  E2E_CHECK(runs.at(0)["success"].asBool());

  E2E_SUBTEST("disabled automations still honor an explicit manual run");
  graphql::Json disabledRes = adminKit.npcs().setEnabled(automationName, false);
  E2E_CHECK(disabledRes.ok());
  // Observed platform contract: enabled=false pauses the SCHEDULER, but an
  // explicit admin gameModelRunAutomation still executes and records a run
  // (success=true). Manual runs are an admin testing tool, not gated by the
  // pause flag.
  graphql::Json disabledRun = adminKit.npcs().runNow(automationName);
  E2E_CHECK(!disabledRun["runId"].asString().empty());
  E2E_CHECK(disabledRun["success"].asBool());
  // Re-enable so the shared app is left tidy (also resets any circuit).
  E2E_CHECK(adminKit.npcs().setEnabled(automationName, true).ok());

  std::puts("e2e_kit_npcs OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "e2e_kit_npcs failed: %s\n", e.what());
  return 1;
}
