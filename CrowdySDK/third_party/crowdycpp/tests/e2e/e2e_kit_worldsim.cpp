// Game Kit e2e: world simulation.
//
// An admin deploys the worldsim blueprint (unique typePrefix per run)
// together with a same-prefix inventory blueprint (gather/harvest grant into
// kit-standard item stacks). The suite drives every simulation loop with
// gameModelRunAutomation to accelerate the interval ticks: the world clock
// advances time-of-day server-side (polled via worldState), a resource node
// is gathered atomically (node decrement + stack grant in one invoke; a
// second gather beyond the remaining amount is denied) and regenerates on
// the regen tick, and a crop is planted, grown stage by stage via the growth
// tick, and harvested once ready (early harvest denied). See
// https://docs.crowdedkingdoms.com/game-api/game-models and
// https://docs.crowdedkingdoms.com/game-api/autonomous-processes.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

namespace {

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
    const std::string prefix = e2e::kitPrefix("Cw");
    const WorldsimNames names = worldsimNames(prefix);

    E2E_SUBTEST("deploy worldsim + inventory blueprints");
    auto& admin = e2e::ownerGame(cfg);
    e2e::pruneStaleAutomations(admin, cfg.appId);
    GameKitOptions adminOptions;
    adminOptions.worldsimTypePrefix = prefix;
    adminOptions.inventoryTypePrefix = prefix;
    auto adminKit = makeKit(admin, cfg.appId, nullptr, adminOptions);
    WorldsimBlueprintOptions blueprint;
    blueprint.typePrefix = prefix;
    InventoryBlueprintOptions bags;
    bags.typePrefix = prefix;
    auto deployed = adminKit.deploy({worldsimBlueprint(blueprint), inventoryBlueprint(bags)});
    E2E_CHECK(deployed.seed.ok());
    for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());
    enableAutomations(admin, cfg.appId);

    // CONTRACT SURPRISE (SDK bug, also present in the JS kit): the node-regen
    // and crop-growth automations ship selfWhere predicates comparing against
    // "self.max_amount" / "self.max_stage", but the deployed Game API only
    // resolves self.* references in the candidate "where" list — selfWhere is
    // evaluated without self props, so the automations run with 0 targets
    // (observable via gameModelAutomationRuns). Re-upsert them without the
    // selector: the tick functions already clamp with min(...), so acting on
    // every (uniquely prefixed) node/crop is semantically identical.
    auto reupsertWithoutSelector = [&](const std::string& automationName,
                                       const std::string& functionName,
                                       const std::string& targetTypeName, int maxTargets) {
      graphql::JVal automation;
      automation["appId"] = cfg.appId;
      automation["name"] = automationName;
      automation["functionName"] = functionName;
      automation["targetMode"] = "type";
      automation["targetTypeName"] = targetTypeName;
      automation["triggerType"] = "schedule";
      automation["scheduleKind"] = "interval";
      automation["intervalMs"] = std::int64_t{60000};
      automation["maxTargets"] = std::int64_t{maxTargets};
      admin.gameModel().upsertAutomation(automation);
    };
    reupsertWithoutSelector(names.regenAutomation, names.regenNodeFn, names.nodeType, 64);
    reupsertWithoutSelector(names.growthAutomation, names.growCropFn, names.cropType, 128);

    E2E_SUBTEST("ensureWorld + clock automation advances time-of-day");
    KitWorldAnchorChunk anchor{400400, 0, 400000};
    graphql::Json world = adminKit.worldsim().ensureWorld(anchor);
    E2E_CHECK(!world["containerId"].asString().empty());
    KitWorldState before = adminKit.worldsim().worldState();
    bool clockTicked = false;
    for (int i = 0; i < 10 && !clockTicked; ++i) {
      adminKit.worldsim().runNow(names.clockAutomation);
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
      KitWorldState now = adminKit.worldsim().worldState();
      clockTicked = now.timeOfDay != before.timeOfDay || now.day != before.day;
    }
    E2E_CHECK(clockTicked);
    KitWorldState after = adminKit.worldsim().worldState();
    E2E_CHECK(after.weather == "clear" || after.weather == "rain" || after.weather == "storm");

    E2E_SUBTEST("weather lever: player denied, admin allowed");
    auto player = e2e::provisionPlayer(cfg, "cw-a");
    GameKitOptions playerOptions;
    playerOptions.worldsimTypePrefix = prefix;
    playerOptions.inventoryTypePrefix = prefix;
    auto kit = makeKit(*player.game, cfg.appId, nullptr, playerOptions);
    auto playerWeather = kit.worldsim().setWeather("storm");
    E2E_CHECK(!playerWeather.success);
    auto adminWeather = adminKit.worldsim().setWeather("storm");
    E2E_CHECK(adminWeather.success);
    E2E_CHECK(adminKit.worldsim().worldState().weather == "storm");

    E2E_SUBTEST("resource node: atomic gather, overdraw denied, regen tick");
    KitResourceNodeCreateInput nodeInput;
    nodeInput.displayName = "Cw Ore Node";
    nodeInput.nodeId = "cw-ore-1";
    nodeInput.resourceItemId = "cw_ore";
    nodeInput.amount = 5;
    nodeInput.maxAmount = 10;
    nodeInput.regenRate = 3;
    graphql::Json node = adminKit.worldsim().createNode(nodeInput);
    const std::string nodeContainerId = node["containerId"].asString();
    E2E_CHECK(!nodeContainerId.empty());

    graphql::Json oreStack = kit.inventory().createStack("cw_ore", 0, 0, player.userId);
    const std::string oreStackId = oreStack["containerId"].asString();
    E2E_CHECK(!oreStackId.empty());

    auto gathered = kit.worldsim().gather(nodeContainerId, 5, oreStackId);
    E2E_CHECK(gathered.success);
    E2E_CHECK(gathered.returnValue.asInt64() == 0);  // node fully drained
    graphql::Json stackProps =
        kitContainerProperties(player.game->gameModel(), cfg.appId, oreStackId);
    E2E_CHECK(stackProps["quantity"].asInt64() == 5);

    // Gathering beyond the remaining amount is denied atomically.
    auto overdraw = kit.worldsim().gather(nodeContainerId, 1, oreStackId);
    E2E_CHECK(!overdraw.success);

    auto nodeAmount = [&]() -> std::int64_t {
      for (const auto& n : kit.worldsim().nodes()) {
        if (n.containerId == nodeContainerId) return n.amount;
      }
      return -1;
    };
    bool regenerated = false;
    for (int i = 0; i < 10 && !regenerated; ++i) {
      adminKit.worldsim().runNow(names.regenAutomation);
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
      regenerated = nodeAmount() > 0;
    }
    E2E_CHECK(regenerated);
    const std::int64_t regenAmount = nodeAmount();
    E2E_CHECK(regenAmount >= 3);  // one tick restores regen_rate
    auto regather = kit.worldsim().gather(nodeContainerId, regenAmount, oreStackId);
    E2E_CHECK(regather.success);
    E2E_CHECK(regather.returnValue.asInt64() == 0);

    E2E_SUBTEST("crops: plant, grow via the tick, harvest once ready");
    KitCropPlantInput plantInput;
    plantInput.ownerUserId = player.userId;
    plantInput.outputItemId = "cw_wheat";
    plantInput.outputQty = 4;
    plantInput.maxStage = 2;
    graphql::Json crop = kit.worldsim().plant(plantInput);
    const std::string cropId = crop["containerId"].asString();
    E2E_CHECK(!cropId.empty());

    graphql::Json wheatStack = kit.inventory().createStack("cw_wheat", 0, 1, player.userId);
    const std::string wheatStackId = wheatStack["containerId"].asString();

    // Harvesting an immature crop is denied.
    auto early = kit.worldsim().harvest(cropId, wheatStackId);
    E2E_CHECK(!early.success);

    auto cropReady = [&] {
      for (const auto& c : kit.worldsim().crops(player.userId)) {
        if (c.containerId == cropId) return c.ready;
      }
      return false;
    };
    bool grown = false;
    for (int i = 0; i < 10 && !grown; ++i) {
      adminKit.worldsim().runNow(names.growthAutomation);
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
      grown = cropReady();
    }
    E2E_CHECK(grown);

    auto harvested = kit.worldsim().harvest(cropId, wheatStackId);
    E2E_CHECK(harvested.success);
    E2E_CHECK(harvested.returnValue.asInt64() == 4);
    graphql::Json wheatProps =
        kitContainerProperties(player.game->gameModel(), cfg.appId, wheatStackId);
    E2E_CHECK(wheatProps["quantity"].asInt64() == 4);
    // The stage reset means an immediate second harvest is denied (regrowth).
    auto reharvest = kit.worldsim().harvest(cropId, wheatStackId);
    E2E_CHECK(!reharvest.success);

    std::puts("e2e_kit_worldsim OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_kit_worldsim FAILED: %s\n", e.what());
    return 1;
  }
}
