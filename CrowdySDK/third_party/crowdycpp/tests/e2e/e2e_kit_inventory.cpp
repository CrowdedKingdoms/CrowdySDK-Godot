// Game Kit end-to-end: an admin deploys an inventory blueprint (idempotent
// seed), then a player uses the runtime kit — ensure bag, create stack,
// grant/consume with the server refusing to overdraw. Requires
// CROWDY_E2E_OWNER_EMAIL (an account with manage_apps on the app) in
// addition to the standard CROWDY_E2E_* variables.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() {
  auto cfg = e2e::requireConfig();
  const std::string ownerEmail = e2e::envOr("CROWDY_E2E_OWNER_EMAIL");
  if (ownerEmail.empty()) {
    std::puts("CROWDY_E2E_OWNER_EMAIL not configured; skipping");
    return 77;
  }

  // Admin: deploy the blueprint with a unique prefix so reruns are isolated
  // from other tests' definitions but the deploy itself stays idempotent.
  auto admin = e2e::signIn(cfg, ownerEmail);
  auto adminKit = makeKit(*admin.game, cfg.appId);
  InventoryBlueprintOptions options;
  options.typePrefix = "E2ECpp";
  auto deployed = adminKit.deploy({inventoryBlueprint(options)});
  E2E_CHECK(deployed.seed.ok());
  // Idempotency: deploying the same blueprint again succeeds.
  auto redeployed = adminKit.deploy({inventoryBlueprint(options)});
  E2E_CHECK(redeployed.seed.ok());

  // Player: runtime helpers against the deployed conventions.
  auto player = e2e::signIn(cfg, cfg.email);
  GameKitOptions kitOptions;
  kitOptions.inventoryTypePrefix = "E2ECpp";
  auto kit = GameKitClient(cfg.appId, player.game->gameModel(), player.game->gameApps(),
                           &player.game->teams(), &player.game->channels(), nullptr, kitOptions);

  graphql::Json bag = kit.inventory().ensure(player.userId);
  E2E_CHECK(!bag["containerId"].asString().empty());

  graphql::Json stack = kit.inventory().createStack("e2e_gem", 5, 0, player.userId);
  const std::string stackId = stack["containerId"].asString();
  E2E_CHECK(!stackId.empty());

  auto granted = kit.inventory().grant(stackId, 10);
  E2E_CHECK(granted.success);
  E2E_CHECK(granted.returnValue.asInt64() == 15);

  auto consumed = kit.inventory().consume(stackId, 6);
  E2E_CHECK(consumed.success);
  E2E_CHECK(consumed.returnValue.asInt64() == 9);

  // The policy guard refuses to overdraw — success=false, nothing written.
  auto overdraw = kit.inventory().consume(stackId, 100);
  E2E_CHECK(!overdraw.success);
  auto after = kit.inventory().consume(stackId, 9);
  E2E_CHECK(after.success);
  E2E_CHECK(after.returnValue.asInt64() == 0);

  std::puts("e2e_kit_inventory OK");
  return 0;
}
