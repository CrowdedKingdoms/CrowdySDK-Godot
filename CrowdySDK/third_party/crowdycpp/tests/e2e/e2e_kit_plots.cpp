// Game Kit e2e: plots — sellable/rentable land. The owner deploys a
// plotBlueprint (rentable) plus the economy blueprint whose Wallet the plot
// functions debit, creates a grid over a chunk region and a priced plot on
// it; a funded buyer purchases the plot (wallet debit + owner_user_id +
// runtime grid-permission grant in one transaction, verified through
// gameApps().userPermissions), an underfunded re-buy is denied, a renter
// takes a TTL grant, and the plot owner evicts them. See
// docs.crowdedkingdoms.com/game-api/grids-and-permissions; mirrors the
// CrowdyJS plot kit conventions.
#include "e2e_util.hpp"

#include <algorithm>

using namespace crowdy;
using namespace crowdy::kit;

namespace {

/// Poll a permission read until it matches `want` (grants/revokes apply at
/// commit but reads may lag a moment on a busy deployment).
bool pollPermission(PlotsKit& plots, const std::string& userId, const std::string& gridId,
                    const char* key, bool want) {
  for (int i = 0; i < 40; ++i) {
    const auto keys = plots.accessOf(userId, gridId);
    const bool has = std::find(keys.begin(), keys.end(), key) != keys.end();
    if (has == want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  return false;
}

}  // namespace

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  const std::string plotType = e2e::kitPrefix("Plot");
  const std::string ecoPrefix = e2e::kitPrefix("PlotEco");

  GameKitOptions options;
  options.plotTypeName = plotType;
  options.economy.typePrefix = ecoPrefix;

  auto admin = e2e::signIn(cfg, cfg.ownerEmail);
  auto adminKit = makeKit(*admin.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("deploy plot + economy blueprints");
  PlotBlueprintOptions plotOptions;
  plotOptions.typeName = plotType;
  plotOptions.rentable = true;
  EconomyBlueprintOptions ecoOptions;
  ecoOptions.typePrefix = ecoPrefix;
  auto deployed = adminKit.deploy({plotBlueprint(plotOptions), economyBlueprint(ecoOptions)});
  E2E_CHECK(deployed.seed.ok());
  for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

  E2E_SUBTEST("owner creates a grid and a priced plot over it");
  // Suite-index-2 chunk base, nudged by the run suffix. Grids persist across
  // runs, so a slot can already be occupied by an earlier run's grid — on a
  // non-NO_ERROR verdict (e.g. overlap), nudge to a fresh region and retry.
  const std::int64_t baseX = 300200;
  std::int64_t baseZ = 300000 + std::stoll(e2e::runSuffix()) % 100000;
  std::string gridId;
  for (int attempt = 0; attempt < 10 && gridId.empty(); ++attempt, baseZ += 7) {
    graphql::JVal gridInput;
    gridInput["appId"] = cfg.appId;
    gridInput["corner1"]["x"] = std::to_string(baseX);
    gridInput["corner1"]["y"] = "0";
    gridInput["corner1"]["z"] = std::to_string(baseZ);
    gridInput["corner2"]["x"] = std::to_string(baseX + 3);
    gridInput["corner2"]["y"] = "0";
    gridInput["corner2"]["z"] = std::to_string(baseZ + 3);
    graphql::Json gridRes = admin.game->gameApps().createGrid(gridInput);
    // The payload's error field is an enum with a "NO_ERROR" sentinel.
    const std::string gridError = gridRes["error"].asString();
    if (!gridError.empty() && gridError != "NO_ERROR") continue;
    const std::int64_t gridIdNum = gridRes["grid"]["grid_id"].asBigInt();
    if (gridIdNum > 0) gridId = std::to_string(gridIdNum);
  }
  E2E_CHECK(!gridId.empty());

  KitPlotCreateInput plotInput;
  plotInput.displayName = "E2E Plot";
  plotInput.gridId = gridId;
  plotInput.price = 100;
  plotInput.rentPrice = 30;
  plotInput.rentTtlSeconds = 3600;
  graphql::Json plotContainer = adminKit.plots().create(plotInput);
  const std::string plotId = plotContainer["containerId"].asString();
  E2E_CHECK(!plotId.empty());

  auto buyer = e2e::provisionPlayer(cfg, "kitplot-buy");
  auto renter = e2e::provisionPlayer(cfg, "kitplot-rent");
  auto buyerKit = makeKit(*buyer.game, cfg.appId, nullptr, options);
  auto renterKit = makeKit(*renter.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("fund wallets via the economy trusted mint");
  graphql::Json buyerWallet = buyerKit.economy().ensureWallet(buyer.userId);
  const std::string buyerWalletId = buyerWallet["containerId"].asString();
  graphql::Json renterWallet = renterKit.economy().ensureWallet(renter.userId);
  const std::string renterWalletId = renterWallet["containerId"].asString();
  E2E_CHECK(adminKit.economy().earn(buyerWalletId, 120).success);
  E2E_CHECK(adminKit.economy().earn(renterWalletId, 50).success);

  E2E_SUBTEST("underfunded buy is denied before anything changes");
  auto poorBuy = renterKit.plots().buy(plotId, renterWalletId);  // 50 < 100
  E2E_CHECK(!poorBuy.success);
  E2E_CHECK(renterKit.economy().balance(renterWalletId) == 50);

  E2E_SUBTEST("funded buy debits the wallet, sets the owner, grants access");
  auto bought = buyerKit.plots().buy(plotId, buyerWalletId);
  E2E_CHECK(bought.success);
  E2E_CHECK(bought.returnValue.asInt64() == 20);  // 120 - 100
  E2E_CHECK(buyerKit.economy().balance(buyerWalletId) == 20);
  bool found = false;
  for (const auto& p : buyerKit.plots().list()) {
    if (p.containerId != plotId) continue;
    found = true;
    E2E_CHECK(p.ownerUserId == std::stoll(buyer.userId));
    E2E_CHECK(p.gridId == std::stoll(gridId));
    E2E_CHECK(p.price == 100);
  }
  E2E_CHECK(found);
  // Note: gameApps().userPermissions is an admin read on this deployment
  // (requires manage_apps), so the verification goes through the owner.
  E2E_CHECK(pollPermission(adminKit.plots(), buyer.userId, gridId, "access", true));
  E2E_CHECK(pollPermission(adminKit.plots(), buyer.userId, gridId, "update_voxel_data", true));

  E2E_SUBTEST("a second buy from the now-drained wallet is denied");
  auto rebuy = buyerKit.plots().buy(plotId, buyerWalletId);  // 20 < 100
  E2E_CHECK(!rebuy.success);
  E2E_CHECK(buyerKit.economy().balance(buyerWalletId) == 20);

  E2E_SUBTEST("renting grants an expiring permission for the rent price");
  auto rented = renterKit.plots().rent(plotId, renterWalletId);
  E2E_CHECK(rented.success);
  E2E_CHECK(rented.returnValue.asInt64() == 20);  // 50 - 30
  E2E_CHECK(pollPermission(adminKit.plots(), renter.userId, gridId, "access", true));

  E2E_SUBTEST("only the plot owner can evict");
  auto evictDenied = renterKit.plots().evict(plotId, std::stoll(buyer.userId));
  E2E_CHECK(!evictDenied.success);
  auto evicted = buyerKit.plots().evict(plotId, std::stoll(renter.userId));
  E2E_CHECK(evicted.success);
  E2E_CHECK(pollPermission(adminKit.plots(), renter.userId, gridId, "access", false));
  // The owner's own grant is untouched by the eviction.
  E2E_CHECK(pollPermission(adminKit.plots(), buyer.userId, gridId, "access", true));

  std::puts("e2e_kit_plots OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "e2e_kit_plots failed: %s\n", e.what());
  return 1;
}
