// Game Kit demo: an admin deploys the inventory blueprint ("load the rules"),
// then a player uses the runtime helpers — with the server enforcing the
// owner-gated policies (the overdraw attempt fails server-side).
//
//   kit_seed_and_play <apiUrl> <adminEmail> <adminPassword> <playerEmail> <playerPassword> <appId>
//
// The admin account needs manage_apps on the app; the server must run with
// email + password for the sign-ins.
#include <cstdio>

#include <crowdy/crowdy.hpp>

using namespace crowdy;

namespace {

CrowdyClient makeGameClient(const std::string& apiUrl, const std::string& email,
                            const std::string& password, const std::string& appId,
                            std::string* userId) {
  ClientConfig identityCfg;
  identityCfg.httpUrl = apiUrl;
  CrowdyClient identity(std::move(identityCfg));
  auto login = identity.auth().login(email, password);
  if (userId) *userId = login.userId;
  auto minted = identity.portal().mintAppToken(appId);
  ClientConfig gameCfg;
  gameCfg.httpUrl = minted.gameApiUrl.empty()
                        ? apiUrl
                        : minted.gameApiUrl.valueOrEmpty();
  CrowdyClient game(std::move(gameCfg));
  game.setToken(minted.token);
  return game;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 7) {
    std::fprintf(stderr,
                 "usage: kit_seed_and_play <apiUrl> <adminEmail> <adminPassword>"
                 " <playerEmail> <playerPassword> <appId>\n");
    return 2;
  }
  const std::string apiUrl = argv[1];
  const std::string adminEmail = argv[2];
  const std::string adminPassword = argv[3];
  const std::string playerEmail = argv[4];
  const std::string playerPassword = argv[5];
  const std::string appId = argv[6];

  // Studio phase: deploy the blueprint (idempotent).
  CrowdyClient admin = makeGameClient(apiUrl, adminEmail, adminPassword, appId, nullptr);
  auto adminKit = kit::makeKit(admin, appId);
  kit::InventoryBlueprintOptions options;
  options.typePrefix = "Demo";
  auto deployed = adminKit.deploy({kit::inventoryBlueprint(options)});
  std::printf("deployed: %zu warnings\n", deployed.warnings.size());

  // Player phase: typed runtime helpers against the deployed conventions.
  std::string playerId;
  CrowdyClient player = makeGameClient(apiUrl, playerEmail, playerPassword, appId, &playerId);
  kit::GameKitOptions kitOptions;
  kitOptions.inventoryTypePrefix = "Demo";
  auto playerKit = kit::makeKit(player, appId, nullptr, kitOptions);

  graphql::Json bag = playerKit.inventory().ensure(playerId);
  std::printf("bag: %s\n", bag["containerId"].asString().c_str());

  graphql::Json stack = playerKit.inventory().createStack("gem", 5, 0, playerId);
  const std::string stackId = stack["containerId"].asString();

  auto granted = playerKit.inventory().grant(stackId, 10);
  std::printf("grant +10 -> quantity %lld\n",
              static_cast<long long>(granted.returnValue.asInt64()));

  auto overdraw = playerKit.inventory().consume(stackId, 999);
  std::printf("overdraw consume: success=%d (%s)\n", overdraw.success ? 1 : 0,
              overdraw.errorMessage.c_str());

  auto consumed = playerKit.inventory().consume(stackId, 3);
  std::printf("consume 3 -> quantity %lld\n",
              static_cast<long long>(consumed.returnValue.asInt64()));
  return 0;
}
