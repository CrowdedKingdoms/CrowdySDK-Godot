// Game Kit e2e: features (monetization gates).
//
// Features are string keys defined per app and granted to ACCESS TIERS; any
// model function whose policy carries a tier_feature leaf admits only
// players on a granted tier. The suite defines a feature key
// (kit.features().define), deploys a small lock blueprint whose open/close
// policy ANDs featureGate(key) through the builder's policyExtra composition
// point, and walks the full gate lifecycle: the player's invoke is denied
// (success=false) before the grant, allowed after grantToTier for the e2e
// tier, and denied again after revoke — with polling to allow grant/revoke
// propagation to settle. See
// https://docs.crowdedkingdoms.com/game-api/game-models.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() {
  try {
    auto cfg = e2e::requireConfig();
    e2e::requireOwner(cfg);
    const std::string doorType = e2e::kitPrefix("CfDoor");
    const std::string featureKey = "cf_vip_" + e2e::runSuffix();

    E2E_SUBTEST("define the feature key");
    auto& admin = e2e::ownerGame(cfg);
    auto adminKit = makeKit(admin, cfg.appId, nullptr);
    graphql::Json defined =
        adminKit.features().define(featureKey, "CrowdyCPP e2e feature gate");
    E2E_CHECK(defined.ok());
    bool keyListed = false;
    adminKit.features().list().forEach([&](graphql::Json f) {
      if (f["featureKey"].asString() == featureKey) keyListed = true;
    });
    E2E_CHECK(keyListed);

    E2E_SUBTEST("deploy a lock blueprint whose policy ANDs featureGate(key)");
    LockBlueprintOptions blueprint;
    blueprint.objectTypeName = doorType;
    blueprint.authority = {LockAuthority::owner()};
    blueprint.policyExtra = featureGate(featureKey);  // AND'ed onto the authority
    auto deployed = adminKit.deploy({lockBlueprint(blueprint)});
    E2E_CHECK(deployed.seed.ok());
    for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

    E2E_SUBTEST("player on the e2e tier, door owned by the player");
    auto player = e2e::provisionPlayer(cfg, "cf-a");
    auto kit = makeKit(*player.game, cfg.appId, nullptr);
    auto adminDoors = adminKit.objectsFor(doorType);
    KitLockableCreateInput doorInput;
    doorInput.displayName = "Cf VIP Door";
    doorInput.ownerUserId = player.userId;
    graphql::Json door = adminDoors.create(doorInput);
    const std::string doorId = door["containerId"].asString();
    E2E_CHECK(!doorId.empty());

    auto doors = kit.objectsFor(doorType);

    E2E_SUBTEST("no feature on the tier -> owner's invoke is denied");
    auto before = doors.open(doorId);
    E2E_CHECK(!before.success);

    E2E_SUBTEST("grantToTier -> invoke allowed (poll for settle)");
    // The e2e access tier every provisioned player is entitled on.
    const std::string tierId = e2e::ensureEntitledTier(cfg, cfg.appId);
    graphql::Json granted = adminKit.features().grantToTier(tierId, featureKey);
    E2E_CHECK(granted.ok());
    bool grantVisible = false;
    adminKit.features().tierFeatures(tierId).forEach([&](graphql::Json g) {
      if (g["featureKey"].asString() == featureKey) grantVisible = true;
    });
    E2E_CHECK(grantVisible);

    bool openable = false;
    for (int i = 0; i < 30 && !openable; ++i) {
      openable = doors.open(doorId).success;
      if (!openable) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    E2E_CHECK(openable);
    E2E_CHECK(doors.isOpen(doorId));
    auto closeWithFeature = doors.close(doorId);
    E2E_CHECK(closeWithFeature.success);

    E2E_SUBTEST("revoke -> invoke denied again (poll for settle)");
    E2E_CHECK(adminKit.features().revokeFromTier(tierId, featureKey));
    bool deniedAgain = false;
    for (int i = 0; i < 30 && !deniedAgain; ++i) {
      deniedAgain = !doors.open(doorId).success;
      if (!deniedAgain) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    E2E_CHECK(deniedAgain);

    std::puts("e2e_kit_features OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_kit_features FAILED: %s\n", e.what());
    return 1;
  }
}
