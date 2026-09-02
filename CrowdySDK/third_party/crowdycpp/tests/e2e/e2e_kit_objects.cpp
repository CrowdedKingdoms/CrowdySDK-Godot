// Game Kit e2e: lockable objects — an admin deploys two lock blueprints (a
// key-gated Door plus an owner-gated Vault reached via objectsFor), issues
// key items, and players operate the objects through the authority-gated
// open/close functions. Denials are results (success:false), never
// exceptions. See docs.crowdedkingdoms.com/game-api/game-models; mirrors the
// CrowdyJS lock kit conventions.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  const std::string doorType = e2e::kitPrefix("Door");
  const std::string vaultType = e2e::kitPrefix("Vault");

  auto admin = e2e::signIn(cfg, cfg.ownerEmail);
  GameKitOptions options;
  options.objectTypeName = doorType;
  auto adminKit = makeKit(*admin.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("deploy key-gated Door + owner-gated Vault blueprints");
  LockBlueprintOptions doorOptions;
  doorOptions.objectTypeName = doorType;
  doorOptions.authority = {LockAuthority::key()};
  LockBlueprintOptions vaultOptions;
  vaultOptions.objectTypeName = vaultType;
  vaultOptions.authority = {LockAuthority::owner()};
  auto deployed = adminKit.deploy({lockBlueprint(doorOptions), lockBlueprint(vaultOptions)});
  E2E_CHECK(deployed.seed.ok());
  for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

  auto alice = e2e::provisionPlayer(cfg, "kitobj-a");
  auto bob = e2e::provisionPlayer(cfg, "kitobj-b");
  auto aliceKit = makeKit(*alice.game, cfg.appId, nullptr, options);
  auto bobKit = makeKit(*bob.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("admin creates a door and issues key items");
  KitLockableCreateInput doorInput;
  doorInput.displayName = "E2E Door";
  doorInput.requiredKeyId = "gold_key";
  graphql::Json doorContainer = adminKit.objects().create(doorInput);
  const std::string doorId = doorContainer["containerId"].asString();
  E2E_CHECK(!doorId.empty());

  graphql::Json aliceKey = adminKit.objects().grantKey("gold_key", alice.userId);
  const std::string aliceKeyId = aliceKey["containerId"].asString();
  E2E_CHECK(!aliceKeyId.empty());
  // Bob holds a key too — but for a different lock (wrong key_id).
  graphql::Json bobKey = adminKit.objects().grantKey("silver_key", bob.userId);
  const std::string bobKeyId = bobKey["containerId"].asString();
  E2E_CHECK(!bobKeyId.empty());
  E2E_CHECK(aliceKit.objects().keysOf(alice.userId).size() >= 1);

  E2E_SUBTEST("the right key opens and closes");
  auto opened = aliceKit.objects().open(doorId, aliceKeyId);
  E2E_CHECK(opened.success);
  E2E_CHECK(opened.returnValue.asBool());
  E2E_CHECK(aliceKit.objects().isOpen(doorId));
  auto closed = aliceKit.objects().close(doorId, aliceKeyId);
  E2E_CHECK(closed.success);
  E2E_CHECK(!aliceKit.objects().isOpen(doorId));

  E2E_SUBTEST("no key / wrong key / someone else's key are denied");
  // No key at all: the key_id param is required by the blueprint; depending
  // on the deployment this surfaces as success:false or as an input error —
  // both are a denial and the door must stay shut.
  bool noKeyDenied = false;
  try {
    noKeyDenied = !bobKit.objects().open(doorId).success;
  } catch (const std::exception&) {
    noKeyDenied = true;
    std::puts("note: open without key_id is rejected as an input error");
  }
  E2E_CHECK(noKeyDenied);

  auto wrongKey = bobKit.objects().open(doorId, bobKeyId);
  E2E_CHECK(!wrongKey.success);
  // Naming Alice's key doesn't help Bob: the key's owner mirror is verified.
  auto stolenKey = bobKit.objects().open(doorId, aliceKeyId);
  E2E_CHECK(!stolenKey.success);
  E2E_CHECK(!aliceKit.objects().isOpen(doorId));

  E2E_SUBTEST("owner-authority lock via objectsFor (second type, same app)");
  ObjectsKit adminVaults = adminKit.objectsFor(vaultType);
  KitLockableCreateInput vaultInput;
  vaultInput.displayName = "E2E Vault";
  vaultInput.ownerUserId = alice.userId;
  graphql::Json vaultContainer = adminVaults.create(vaultInput);
  const std::string vaultId = vaultContainer["containerId"].asString();
  E2E_CHECK(!vaultId.empty());
  E2E_CHECK(adminVaults.list().size() >= 1);

  ObjectsKit aliceVaults = aliceKit.objectsFor(vaultType);
  ObjectsKit bobVaults = bobKit.objectsFor(vaultType);
  auto vaultDeniedOpen = bobVaults.open(vaultId);
  E2E_CHECK(!vaultDeniedOpen.success);
  auto vaultOpened = aliceVaults.open(vaultId);
  E2E_CHECK(vaultOpened.success);
  E2E_CHECK(aliceVaults.isOpen(vaultId));
  auto vaultDeniedClose = bobVaults.close(vaultId);
  E2E_CHECK(!vaultDeniedClose.success);
  E2E_CHECK(aliceVaults.isOpen(vaultId));
  auto vaultClosed = aliceVaults.close(vaultId);
  E2E_CHECK(vaultClosed.success);
  E2E_CHECK(!aliceVaults.isOpen(vaultId));

  std::puts("e2e_kit_objects OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "e2e_kit_objects failed: %s\n", e.what());
  return 1;
}
