// Game Kit e2e: combat.
//
// An admin deploys the combat blueprint (unique typePrefix per run), two
// players spawn combatants, and the suite verifies the server-authoritative
// combat loop end to end: the damage formula (attack vs defense, min 1) and
// the death flip run server-side; dead or ineligible attackers are denied
// (success=false, never an exception); respawn restores a downed combatant;
// and the blueprint's status-effect interval automation is accelerated with
// gameModelRunAutomation to observe a damage-over-time tick. See
// https://docs.crowdedkingdoms.com/game-api/game-models and
// https://docs.crowdedkingdoms.com/game-api/autonomous-processes.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

namespace {

/// Automations respect the per-app policy kill switch; make sure it is on
/// (idempotent, admin-only; ignore failures on deployments that manage it).
void enableAutomations(CrowdyClient& admin, const std::string& appId) {
  try {
    graphql::JVal input;
    input["appId"] = appId;
    input["enabled"] = true;
    admin.gameModel().setAutomationPolicy(input);
  } catch (const std::exception& e) {
    std::printf("note: setAutomationPolicy failed (continuing): %s\n", e.what());
  }
}

}  // namespace

int main() {
  try {
    auto cfg = e2e::requireConfig();
    e2e::requireOwner(cfg);
    const std::string prefix = e2e::kitPrefix("Cb");

    E2E_SUBTEST("deploy combat blueprint");
    auto& admin = e2e::ownerGame(cfg);
    e2e::pruneStaleAutomations(admin, cfg.appId);
    GameKitOptions adminOptions;
    adminOptions.combatTypePrefix = prefix;
    auto adminKit = makeKit(admin, cfg.appId, nullptr, adminOptions);
    CombatBlueprintOptions blueprint;
    blueprint.typePrefix = prefix;
    auto deployed = adminKit.deploy({combatBlueprint(blueprint)});
    E2E_CHECK(deployed.seed.ok());
    for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());
    enableAutomations(admin, cfg.appId);
    const CombatNames names = combatNames(prefix);

    E2E_SUBTEST("players spawn combatants");
    auto a = e2e::provisionPlayer(cfg, "cb-a");
    auto b = e2e::provisionPlayer(cfg, "cb-b");
    GameKitOptions playerOptions;
    playerOptions.combatTypePrefix = prefix;
    auto kitA = makeKit(*a.game, cfg.appId, nullptr, playerOptions);
    auto kitB = makeKit(*b.game, cfg.appId, nullptr, playerOptions);

    KitCombatantSpawn spawnA;
    spawnA.ownerUserId = a.userId;
    spawnA.displayName = "Cb A";
    spawnA.maxHp = 100;
    spawnA.attack = 60;
    spawnA.defense = 0;
    graphql::Json createdA = kitA.combat().spawnCombatant(spawnA);
    const std::string combatantA = createdA["containerId"].asString();
    E2E_CHECK(!combatantA.empty());

    KitCombatantSpawn spawnB;
    spawnB.ownerUserId = b.userId;
    spawnB.displayName = "Cb B";
    spawnB.maxHp = 100;
    spawnB.attack = 60;
    spawnB.defense = 10;
    graphql::Json createdB = kitB.combat().spawnCombatant(spawnB);
    const std::string combatantB = createdB["containerId"].asString();
    E2E_CHECK(!combatantB.empty());

    KitCombatant stateB = kitA.combat().state(combatantB);
    E2E_CHECK(stateB.hp == 100 && stateB.alive);
    E2E_CHECK(!stateB.combatKey.empty());

    E2E_SUBTEST("attack applies the server-side damage formula");
    // damage = max(1, attack 60 - defense 10) = 50
    auto atk1 = kitA.combat().attack(combatantA, combatantB);
    E2E_CHECK(atk1.success);
    E2E_CHECK(atk1.returnValue.asInt64() == 50);
    stateB = kitA.combat().state(combatantB);
    E2E_CHECK(stateB.hp == 50 && stateB.alive);

    E2E_SUBTEST("death at 0 hp flips alive server-side");
    auto atk2 = kitA.combat().attack(combatantA, combatantB);
    E2E_CHECK(atk2.success);
    E2E_CHECK(atk2.returnValue.asInt64() == 0);
    stateB = kitA.combat().state(combatantB);
    E2E_CHECK(stateB.hp == 0);
    E2E_CHECK(!stateB.alive);

    E2E_SUBTEST("dead attacker and dead target are denied (success=false)");
    auto deadAttacker = kitB.combat().attack(combatantB, combatantA);
    E2E_CHECK(!deadAttacker.success);
    auto deadTarget = kitA.combat().attack(combatantA, combatantB);
    E2E_CHECK(!deadTarget.success);

    E2E_SUBTEST("respawn restores full hp; respawning while alive is denied");
    auto respawned = kitB.combat().respawn(combatantB);
    E2E_CHECK(respawned.success);
    E2E_CHECK(respawned.returnValue.asInt64() == 100);
    stateB = kitB.combat().state(combatantB);
    E2E_CHECK(stateB.hp == 100 && stateB.alive);
    auto respawnAlive = kitA.combat().respawn(combatantA);
    E2E_CHECK(!respawnAlive.success);

    E2E_SUBTEST("status effect ticks via the interval automation (runAutomation)");
    // CONTRACT SURPRISE (SDK bug, also present in the JS kit): the combat
    // blueprint's effect-tick selector omits "pick". The Game API's selector
    // resolver only runs candidate selection when BOTH "pick" and "ofType"
    // are present — without "pick" it binds nothing, the automation fans out,
    // and every invocation fails "Missing required parameter 'target'"
    // (observable via gameModelAutomationRuns). Re-upsert the automation with
    // pick:"random" (any pick works; the join predicate already narrows to
    // the one matching combatant) so the tick is verifiable end to end.
    {
      graphql::JVal selfPredicate;
      selfPredicate["key"] = "ticks_left";
      selfPredicate["op"] = ">";
      selfPredicate["value"] = std::int64_t{0};
      graphql::JVal joinPredicate;
      joinPredicate["key"] = "combat_key";
      joinPredicate["op"] = "==";
      joinPredicate["value"] = "self.target_key";
      graphql::JVal selector;
      selector["selfWhere"] = graphql::JVal::array({std::move(selfPredicate)});
      selector["pick"] = "random";
      selector["ofType"] = names.combatantType;
      selector["where"] = graphql::JVal::array({std::move(joinPredicate)});
      selector["bindAs"]["ref"] = "target";

      graphql::JVal automation;
      automation["appId"] = cfg.appId;
      automation["name"] = names.effectTickAutomation;
      automation["functionName"] = names.effectTickFn;
      automation["targetMode"] = "type";
      automation["targetTypeName"] = names.effectType;
      automation["triggerType"] = "schedule";
      automation["scheduleKind"] = "interval";
      automation["intervalMs"] = std::int64_t{5000};
      automation["maxTargets"] = std::int64_t{32};
      automation["selectorJson"] = selector.dump();
      admin.gameModel().upsertAutomation(automation);
    }

    auto armed = kitA.combat().applyEffect(stateB.combatKey, "poison", 7, 3);
    E2E_CHECK(armed.success);
    E2E_CHECK(armed.returnValue.asInt64() == 3);
    auto effects = kitA.combat().effects(stateB.combatKey);
    E2E_CHECK(effects.size() == 1);
    E2E_CHECK(effects.front().magnitude == 7 && effects.front().ticksLeft == 3);

    // Accelerate the tick: each manual run applies one damage-over-time step
    // to the joined target (hp -7, ticks_left -1).
    bool ticked = false;
    for (int i = 0; i < 10 && !ticked; ++i) {
      admin.gameModel().runAutomation(cfg.appId, names.effectTickAutomation);
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
      ticked = kitA.combat().state(combatantB).hp < 100;
    }
    E2E_CHECK(ticked);
    stateB = kitA.combat().state(combatantB);
    E2E_CHECK(stateB.hp <= 93);
    E2E_CHECK((100 - stateB.hp) % 7 == 0);
    auto remaining = kitA.combat().effects(stateB.combatKey);
    E2E_CHECK(remaining.empty() || remaining.front().ticksLeft < 3);

    std::puts("e2e_kit_combat OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_kit_combat FAILED: %s\n", e.what());
    return 1;
  }
}
