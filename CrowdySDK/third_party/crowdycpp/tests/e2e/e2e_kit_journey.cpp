// Game Kit e2e: composite gamer journey over the kit.
//
// Mirrors the public smoke-test pattern of the Blocks with Friends sample:
// bootstrap -> the studio deploys inventory + economy + progression + loot in
// ONE deploy() call (one shared unique prefix) -> a player joins the world
// over the native UDP replication connection (actor presence) -> earns
// currency through the trusted authority mint -> buys an item from the shop
// (atomic wallet debit + stock decrement + grant) -> gains xp and levels up
// server-side -> rolls and claims loot (single-claim guard) -> saves the
// per-user app state blob -> reads everything back consistent. Trusted
// grants invoked by the plain player resolve success=false throughout. See
// https://docs.crowdedkingdoms.com/game-api/game-models and
// https://docs.crowdedkingdoms.com/game-api/replication.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() {
  try {
    auto cfg = e2e::requireConfig();
    e2e::requireOwner(cfg);
    const std::string prefix = e2e::kitPrefix("Cj");

    E2E_SUBTEST("owner deploys inventory+economy+progression+loot in one call");
    auto& admin = e2e::ownerGame(cfg);
    GameKitOptions options;
    options.inventoryTypePrefix = prefix;
    options.economy.typePrefix = prefix;
    options.progressionTypePrefix = prefix;
    options.lootTypePrefix = prefix;
    auto adminKit = makeKit(admin, cfg.appId, nullptr, options);

    InventoryBlueprintOptions bags;
    bags.typePrefix = prefix;
    EconomyBlueprintOptions economy;
    economy.typePrefix = prefix;
    ProgressionBlueprintOptions progression;
    progression.typePrefix = prefix;
    LootBlueprintOptions loot;
    loot.typePrefix = prefix;
    loot.tables = {{"chest", {{"cj_gem", 1.0, 2, std::nullopt}}}};
    auto deployed = adminKit.deploy({inventoryBlueprint(bags), economyBlueprint(economy),
                                     progressionBlueprint(progression), lootBlueprint(loot)});
    E2E_CHECK(deployed.seed.ok());
    for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

    E2E_SUBTEST("player bootstraps and joins the world over native UDP");
    auto player = e2e::provisionPlayer(cfg, "cj-a");
    graphql::Json bootstrap = player.game->serverStatus().gameClientBootstrap(cfg.appId);
    E2E_CHECK(!bootstrap["me"]["userId"].asString().empty());
    e2e::connectUdp(player, cfg);
    const auto uuid = core::generateActorUuid();
    const wire::ChunkCoord chunk{400800, 0, 400000};
    const std::uint8_t pose[] = {1};
    // UDP is best-effort: retry the join until the self-echo confirms
    // presence (early sends can race the server-side session install).
    bool present = false;
    for (int i = 0; i < 15 && !present; ++i) {
      auto joined =
          player.conn->sendActorUpdateAndWait({chunk, uuid, Bytes(pose, 1), 8}, 2000);
      if (joined.error) {
        std::printf("note: actor update error code %u; retrying\n",
                    static_cast<unsigned>(*joined.error));
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      present = joined.acknowledged;
    }
    E2E_CHECK(present);

    auto kit = makeKit(*player.game, cfg.appId, player.conn.get(), options);

    E2E_SUBTEST("earn currency: trusted mint only (player mint denied)");
    graphql::Json bag = kit.inventory().ensure(player.userId);
    E2E_CHECK(!bag["containerId"].asString().empty());
    graphql::Json wallet = kit.economy().ensureWallet(player.userId);
    const std::string walletId = wallet["containerId"].asString();
    E2E_CHECK(!walletId.empty());

    auto playerMint = kit.economy().earn(walletId, 1000000);
    E2E_CHECK(!playerMint.success);
    auto minted = adminKit.economy().earn(walletId, 100);
    E2E_CHECK(minted.success);
    E2E_CHECK(minted.returnValue.asInt64() == 100);
    E2E_CHECK(kit.economy().balance(walletId) == 100);

    E2E_SUBTEST("buy an item: atomic debit + stock decrement + grant");
    graphql::Json listing = adminKit.economy().shopCreate("Cj Sword", "cj_sword", 60, 2);
    const std::string listingId = listing["containerId"].asString();
    graphql::Json swordStack = kit.inventory().createStack("cj_sword", 0, 0, player.userId);
    const std::string swordStackId = swordStack["containerId"].asString();
    kit.inventory().linkStack(bag["containerId"].asString(), swordStackId);

    auto bought = kit.economy().shopBuy(listingId, walletId, swordStackId);
    E2E_CHECK(bought.success);
    E2E_CHECK(bought.returnValue.asInt64() == 40);  // 100 - 60
    // Second purchase exceeds the remaining balance: denied atomically.
    auto broke = kit.economy().shopBuy(listingId, walletId, swordStackId);
    E2E_CHECK(!broke.success);
    graphql::Json swordProps =
        kitContainerProperties(player.game->gameModel(), cfg.appId, swordStackId);
    E2E_CHECK(swordProps["quantity"].asInt64() == 1);

    E2E_SUBTEST("gain xp and level up server-side (player grant denied)");
    graphql::Json progress = kit.progression().ensure(player.userId);
    const std::string progressId = progress["containerId"].asString();
    auto playerXp = kit.progression().grantXp(progressId, 999999);
    E2E_CHECK(!playerXp.success);
    // Curve: total xp for level 2 is 100 * 2 * 2 = 400; 500 crosses it.
    auto granted = adminKit.progression().grantXp(progressId, 500);
    E2E_CHECK(granted.success);
    E2E_CHECK(granted.returnValue.asInt64() == 2);
    KitProgress state = kit.progression().state(progressId);
    E2E_CHECK(state.xp == 500);
    E2E_CHECK(state.level == 2);
    E2E_CHECK(state.skillPoints == 1);

    E2E_SUBTEST("roll + claim loot (player roll denied; single-claim guard)");
    graphql::Json roll = kit.loot().createRoll(player.userId, "chest");
    const std::string rollId = roll["containerId"].asString();
    auto playerRoll = kit.loot().roll(rollId, "chest");
    E2E_CHECK(!playerRoll.success);
    auto rolled = adminKit.loot().roll(rollId, "chest");
    E2E_CHECK(rolled.success);
    E2E_CHECK(rolled.returnValue.asString() == "cj_gem");

    graphql::Json gemStack = kit.inventory().createStack("cj_gem", 0, 1, player.userId);
    const std::string gemStackId = gemStack["containerId"].asString();
    auto claimed = kit.loot().claim(rollId, gemStackId);
    E2E_CHECK(claimed.success);
    E2E_CHECK(claimed.returnValue.asInt64() == 2);
    auto doubleClaim = kit.loot().claim(rollId, gemStackId);
    E2E_CHECK(!doubleClaim.success);

    E2E_SUBTEST("save per-user app state and read it back");
    const std::string savedBlob = core::base64Encode(asBytes("cj-save-" + e2e::runSuffix()));
    graphql::JVal stateInput;
    stateInput["appId"] = cfg.appId;
    stateInput["state"] = savedBlob;
    player.game->state().update(stateInput);
    graphql::Json readBack = player.game->state().getOne(cfg.appId);
    std::string roundTripped = readBack["state"].asString();
    if (roundTripped != savedBlob) {
      auto decoded = core::base64Decode(roundTripped);
      E2E_CHECK(decoded.has_value());
      roundTripped = std::string(decoded->begin(), decoded->end());
    }
    E2E_CHECK(roundTripped == savedBlob);

    E2E_SUBTEST("read everything back consistent");
    E2E_CHECK(kit.economy().balance(walletId) == 40);
    bool sawSword = false, sawGems = false;
    for (const auto& s : kit.inventory().stacks(player.userId)) {
      if (s.itemId == "cj_sword") sawSword = s.quantity == 1;
      if (s.itemId == "cj_gem") sawGems = s.quantity == 2;
    }
    E2E_CHECK(sawSword && sawGems);
    KitProgress finalState = kit.progression().state(progressId);
    E2E_CHECK(finalState.level == 2 && finalState.xp == 500);
    KitLootRoll finalRoll = kit.loot().state(rollId);
    E2E_CHECK(finalRoll.claimed && finalRoll.rolledItemId == "cj_gem");
    bool listingDecremented = false;
    for (const auto& l : adminKit.economy().shopList()) {
      if (l.containerId == listingId) listingDecremented = l.stock == 1;
    }
    E2E_CHECK(listingDecremented);

    player.conn->disconnect();
    std::puts("e2e_kit_journey OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_kit_journey FAILED: %s\n", e.what());
    return 1;
  }
}
