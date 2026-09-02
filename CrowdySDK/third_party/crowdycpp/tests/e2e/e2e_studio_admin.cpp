// Mirrors Management API e2e: studio admin (organizations, app lifecycle,
// access tiers + grants, org roles + tokens). Also mirrors CrowdyJS
// studio-admin e2e: the "new-app-default-access" scenario (a fresh player can
// mint an app token for a brand-new FREE app and receives the open-by-default
// tier auto-grant) and the "new-app-grid-creation" scenario (the owner
// creates a grid on the new app via the app-scoped GAME client and
// nearbyPermissions reflects the granted keys). Black-box against the public
// APIs; see https://docs.crowdedkingdoms.com for the studio-admin surface.
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

/// BigInt scalars cross the wire as decimal strings, but ids may be numbers;
/// normalize to a decimal string either way.
std::string bigIntStr(const graphql::Json& v) {
  if (v.isString()) return v.asString();
  return std::to_string(v.asInt64());
}

bool containsKey(const graphql::Json& arr, std::string_view key) {
  bool found = false;
  arr.forEach([&](graphql::Json k) {
    if (k.asString() == key) found = true;
  });
  return found;
}

graphql::JVal chunkInput(std::int64_t x, std::int64_t y, std::int64_t z) {
  graphql::JVal c;
  c["x"] = std::to_string(x);
  c["y"] = std::to_string(y);
  c["z"] = std::to_string(z);
  return c;
}

int runAll() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto& own = e2e::owner(cfg);

  E2E_SUBTEST("myOrganizations is non-empty; get/getBySlug round-trip");
  graphql::Json myOrgs = own.admin().organizations().mine();
  E2E_CHECK(myOrgs.size() >= 1);
  // Administer the org that owns the configured e2e app.
  graphql::Json e2eApp = own.admin().apps().get(cfg.appId);
  const std::string orgId = bigIntStr(e2eApp["orgId"]);
  const std::string orgSlug = e2eApp["org"]["slug"].asString();
  E2E_CHECK(!orgId.empty() && !orgSlug.empty());
  graphql::Json orgById = own.admin().organizations().get(orgId);
  E2E_CHECK(orgById["slug"].asString() == orgSlug);
  graphql::Json orgBySlug = own.admin().organizations().getBySlug(orgSlug);
  E2E_CHECK(bigIntStr(orgBySlug["orgId"]) == orgId);

  E2E_SUBTEST("createApp with a unique slug");
  const std::string appSlug = "cpp-e2e-" + e2e::runSuffix();
  graphql::JVal createInput;
  createInput["orgId"] = orgId;
  createInput["name"] = "CrowdyCPP E2E " + e2e::runSuffix();
  createInput["slug"] = appSlug;
  createInput["description"] = "CrowdyCPP studio-admin e2e app";
  createInput["visibility"] = "PUBLIC";
  graphql::Json createdApp = own.admin().apps().create(createInput);
  const std::string newAppId = bigIntStr(createdApp["appId"]);
  E2E_CHECK(!newAppId.empty() && newAppId != "0");
  E2E_CHECK(createdApp["slug"].asString() == appSlug);
  E2E_CHECK(createdApp["visibility"].asString() == "PUBLIC");

  E2E_SUBTEST("updateApp + setVisibility read back");
  graphql::JVal updateInput;
  updateInput["description"] = "CrowdyCPP studio-admin e2e app (updated)";
  graphql::Json updatedApp = own.admin().apps().update(newAppId, updateInput);
  E2E_CHECK(updatedApp["description"].asString() ==
            "CrowdyCPP studio-admin e2e app (updated)");
  graphql::Json unlisted = own.admin().apps().setVisibility(newAppId, "UNLISTED");
  E2E_CHECK(unlisted["visibility"].asString() == "UNLISTED");
  E2E_CHECK(own.admin().apps().get(newAppId)["visibility"].asString() == "UNLISTED");
  // Back to PUBLIC for the marketplace + default-access scenarios below.
  (void)own.admin().apps().setVisibility(newAppId, "PUBLIC");

  E2E_SUBTEST("marketplace + marketplaceConnection reads");
  graphql::Json marketplace = own.admin().apps().marketplace();
  E2E_CHECK(marketplace["items"].isArray());
  E2E_CHECK(marketplace["pageInfo"]["totalCount"].asInt64() >= 1);
  graphql::Json mpConn = own.admin().apps().marketplaceConnection();
  E2E_CHECK(mpConn["edges"].isArray());
  E2E_CHECK(mpConn["totalCount"].asInt64() >= 1);

  E2E_SUBTEST("routeFor shape on the new app");
  graphql::Json route = own.admin().apps().routeFor(newAppId);
  E2E_CHECK(bigIntStr(route["appId"]) == newAppId);
  E2E_CHECK(route["status"].asString() == "LIVE");
  // Routing tuple: splitMode is a Boolean flag; deploymentTarget/gameApiUrl
  // are string-or-null (null on a single-endpoint deployment).
  E2E_CHECK(route["splitMode"].isBool() || route["splitMode"].isNull());
  E2E_CHECK(route["deploymentTarget"].isString() || route["deploymentTarget"].isNull());
  E2E_CHECK(route["gameApiUrl"].isString() || route["gameApiUrl"].isNull());

  E2E_SUBTEST("runtimePermissions + access-tier create/update/archive");
  graphql::Json runtimeKeys = own.admin().appAccess().runtimePermissions();
  E2E_CHECK(runtimeKeys.size() >= 1);
  E2E_CHECK(containsKey(runtimeKeys, "access"));
  graphql::JVal tierInput;
  tierInput["appId"] = newAppId;
  tierInput["name"] = "cpp-e2e-tier-" + e2e::runSuffix();
  tierInput["isFree"] = true;
  tierInput["description"] = "CrowdyCPP e2e tier";
  tierInput["permissionKeys"] =
      graphql::JVal::array({graphql::JVal("access"), graphql::JVal("teleport")});
  graphql::Json tier = own.admin().appAccess().createTier(tierInput);
  const std::string tierId = bigIntStr(tier["tierId"]);
  E2E_CHECK(!tierId.empty() && tierId != "0");
  E2E_CHECK(tier["isFree"].asBool());
  E2E_CHECK(containsKey(tier["permissionKeys"], "teleport"));
  graphql::JVal tierUpdate;
  tierUpdate["description"] = "CrowdyCPP e2e tier (updated)";
  tierUpdate["permissionKeys"] = graphql::JVal::array(
      {graphql::JVal("access"), graphql::JVal("update_voxel_data")});
  graphql::Json updatedTier = own.admin().appAccess().updateTier(tierId, tierUpdate);
  E2E_CHECK(updatedTier["description"].asString() == "CrowdyCPP e2e tier (updated)");
  E2E_CHECK(containsKey(updatedTier["permissionKeys"], "update_voxel_data"));
  E2E_CHECK(!containsKey(updatedTier["permissionKeys"], "teleport"));

  E2E_SUBTEST("grantAppAccess shows in usersByApp + connection; revoke removes");
  std::string granteeId;
  auto grantee = e2e::identityClient(cfg, e2e::deriveEmail(cfg, "studio-grantee"),
                                     &granteeId);
  graphql::JVal grantInput;
  grantInput["appId"] = newAppId;
  grantInput["userId"] = granteeId;
  grantInput["tierId"] = tierId;
  grantInput["idempotencyKey"] = "cpp-e2e-grant-" + e2e::runSuffix();
  graphql::Json grant = own.admin().appAccess().grant(grantInput);
  E2E_CHECK(bigIntStr(grant["userId"]) == granteeId);
  E2E_CHECK(bigIntStr(grant["tierId"]) == tierId);
  graphql::JVal listVars;
  listVars["appId"] = newAppId;
  graphql::Json accessRows = own.admin().appAccess().userAccessByApp(listVars);
  bool sawGrant = false;
  accessRows.forEach([&](graphql::Json row) {
    if (bigIntStr(row["userId"]) == granteeId) sawGrant = true;
  });
  E2E_CHECK(sawGrant);
  graphql::JVal connVars;
  connVars["appId"] = newAppId;
  connVars["first"] = std::int64_t{50};
  graphql::Json accessConn = own.admin().appAccess().userAccessConnection(connVars);
  bool sawGrantConn = false;
  accessConn["edges"].forEach([&](graphql::Json edge) {
    if (bigIntStr(edge["node"]["userId"]) == granteeId) sawGrantConn = true;
  });
  E2E_CHECK(sawGrantConn);
  graphql::Json revoked = own.admin().appAccess().revoke(newAppId, granteeId);
  E2E_CHECK(bigIntStr(revoked["userId"]) == granteeId);
  graphql::Json afterRevoke = own.admin().appAccess().userAccessByApp(listVars);
  afterRevoke.forEach([&](graphql::Json row) {
    if (bigIntStr(row["userId"]) == granteeId)
      E2E_CHECK(row["status"].asString() != "active");
  });

  E2E_SUBTEST("archiveTier");
  graphql::Json archivedTier = own.admin().appAccess().archiveTier(tierId);
  E2E_CHECK(bigIntStr(archivedTier["tierId"]) == tierId);
  E2E_CHECK(archivedTier["status"].asString() != "active");

  E2E_SUBTEST("new-app-default-access: fresh player mints + gets auto-grant");
  // Mirrors CrowdyJS new-app-default-access: the player is registered but
  // NEVER granted — minting for the new FREE app must auto-grant the default
  // tier createApp provisioned.
  e2e::Player player;
  player.email = e2e::deriveEmail(cfg, "studio-player");
  player.identity = e2e::identityClient(cfg, player.email, &player.userId);
  player.appToken = player.identity->portal().mintAppToken(newAppId);
  E2E_CHECK(player.appToken.token.size() == 64);
  graphql::Json myAccess = player.identity->admin().appAccess().myAccess(newAppId);
  E2E_CHECK(bigIntStr(myAccess["appId"]) == newAppId);
  E2E_CHECK(myAccess["status"].asString() == "active");
  E2E_CHECK(!bigIntStr(myAccess["tierId"]).empty());

  E2E_SUBTEST("new-app-grid-creation: owner creates a grid; nearbyPermissions");
  // Mirrors CrowdyJS new-app-grid-creation. The world grid + its assignment
  // are provisioned lazily on the first game/UDP touch, so drive that with
  // the fresh player before the owner's createGrid.
  {
    crowdy::ClientConfig gameCfg;
    gameCfg.httpUrl = !cfg.httpUrl.empty() ? cfg.httpUrl
                      : (!player.appToken.gameApiUrl.empty()
                             ? player.appToken.gameApiUrl.valueOrEmpty()
                             : cfg.apiUrl);
    player.game = std::make_unique<CrowdyClient>(std::move(gameCfg));
    player.game->setToken(player.appToken.token);

    // Open-by-default access lands asynchronously; bootstrap succeeding is
    // the access-landed signal.
    bool bootstrapped = false;
    for (int i = 0; i < 50 && !bootstrapped; ++i) {
      try {
        (void)player.game->serverStatus().gameClientBootstrap(newAppId);
        bootstrapped = true;
      } catch (const graphql::CrowdyGraphQLError&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
    E2E_CHECK(bootstrapped);

    // First UDP touch triggers server-side ensureDefaultGrid (world grid +
    // assignment row) for the new app.
    e2e::connectUdp(player, cfg, newAppId);
    const auto uuid = core::generateActorUuid();
    const std::uint8_t pose[] = {0x01};
    const wire::ChunkCoord chunk{0, 0, 0};
    for (int i = 0; i < 3; ++i) {
      (void)player.conn->sendActorUpdate(
          {.chunk = chunk, .uuid = uuid, .payload = Bytes(pose, sizeof(pose)),
           .distance = 8});
      player.conn->poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    // Owner needs an APP-scoped token for grid ops on the new app (the
    // identity session token is rejected).
    auto ownerMint = own.portal().mintAppToken(newAppId);
    crowdy::ClientConfig ownerGameCfg;
    ownerGameCfg.httpUrl = !cfg.httpUrl.empty() ? cfg.httpUrl
                           : (!ownerMint.gameApiUrl.empty()
                                  ? ownerMint.gameApiUrl.valueOrEmpty()
                                  : cfg.apiUrl);
    CrowdyClient ownerGame(std::move(ownerGameCfg));
    ownerGame.setToken(ownerMint.token);

    // Poll createGrid while the world-grid assignment finishes landing.
    graphql::JVal gridInput;
    gridInput["appId"] = newAppId;
    gridInput["corner1"] = chunkInput(100, 0, 100);
    gridInput["corner2"] = chunkInput(110, 0, 110);
    graphql::Json gridResult;
    for (int i = 0; i < 50; ++i) {
      gridResult = ownerGame.gameApps().createGrid(gridInput);
      if (gridResult["error"].asString() != "NO_MATCHING_GRID_ASSIGNMENT") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    E2E_CHECK(gridResult["error"].asString() == "NO_ERROR");
    const std::string gridId = bigIntStr(gridResult["grid"]["grid_id"]);
    E2E_CHECK(!gridId.empty() && gridId != "0");

    // Grant a runtime key to the player on the new grid, then verify
    // nearbyPermissions reflects it.
    graphql::JVal permInput;
    permInput["appId"] = newAppId;
    permInput["gridId"] = gridId;
    permInput["userId"] = player.userId;
    permInput["permissionKeys"] = graphql::JVal::array({graphql::JVal("update_voxel_data")});
    graphql::Json granted = ownerGame.gameApps().grantPermissions(permInput);
    E2E_CHECK(bigIntStr(granted["gridId"]) == gridId);
    E2E_CHECK(containsKey(granted["permissionKeys"], "update_voxel_data"));

    graphql::JVal nearbyInput;
    nearbyInput["appId"] = newAppId;
    nearbyInput["userId"] = player.userId;
    nearbyInput["lowChunk"] = chunkInput(99, 0, 99);
    nearbyInput["highChunk"] = chunkInput(111, 0, 111);
    graphql::Json nearby = ownerGame.gameApps().nearbyPermissions(nearbyInput);
    bool sawGridKeys = false;
    nearby.forEach([&](graphql::Json g) {
      if (bigIntStr(g["gridId"]) == gridId &&
          containsKey(g["permissionKeys"], "update_voxel_data"))
        sawGridKeys = true;
    });
    E2E_CHECK(sawGridKeys);

    player.conn->disconnect();
  }

  E2E_SUBTEST("org roles: create/update/delete");
  graphql::JVal roleInput;
  roleInput["orgId"] = orgId;
  roleInput["roleName"] = "cpp-e2e-role-" + e2e::runSuffix();
  roleInput["description"] = "CrowdyCPP e2e role";
  roleInput["permissions"] = graphql::JVal::array({graphql::JVal("manage_apps")});
  graphql::Json role = own.admin().organizations().createRole(roleInput);
  const std::string roleId = bigIntStr(role["orgRoleId"]);
  E2E_CHECK(!roleId.empty() && roleId != "0");
  E2E_CHECK(containsKey(role["permissions"], "manage_apps"));
  E2E_CHECK(!role["isSystem"].asBool());
  graphql::JVal roleUpdate;
  roleUpdate["description"] = "CrowdyCPP e2e role (updated)";
  graphql::Json updatedRole = own.admin().organizations().updateRole(roleId, roleUpdate);
  E2E_CHECK(updatedRole["description"].asString() == "CrowdyCPP e2e role (updated)");
  E2E_CHECK(own.admin().organizations().deleteRole(roleId).asBool());

  E2E_SUBTEST("org tokens: create/update/revoke");
  graphql::JVal tokenInput;
  tokenInput["orgId"] = orgId;
  tokenInput["label"] = "cpp-e2e-token-" + e2e::runSuffix();
  graphql::Json orgToken = own.admin().organizations().createToken(tokenInput);
  const std::string orgTokenId = bigIntStr(orgToken["orgTokenId"]);
  E2E_CHECK(!orgTokenId.empty() && orgTokenId != "0");
  E2E_CHECK(!orgToken["token"].asString().empty());  // secret only on create
  E2E_CHECK(orgToken["isActive"].asBool());
  graphql::JVal tokenUpdate;
  tokenUpdate["label"] = "cpp-e2e-token-upd-" + e2e::runSuffix();
  graphql::Json updatedToken =
      own.admin().organizations().updateToken(orgTokenId, tokenUpdate);
  E2E_CHECK(updatedToken["label"].asString() == "cpp-e2e-token-upd-" + e2e::runSuffix());
  E2E_CHECK(own.admin().organizations().revokeToken(orgTokenId).asBool());
  graphql::Json tokens = own.admin().organizations().tokens(orgId);
  tokens.forEach([&](graphql::Json t) {
    if (bigIntStr(t["orgTokenId"]) == orgTokenId) E2E_CHECK(!t["isActive"].asBool());
  });

  E2E_SUBTEST("archiveApp");
  graphql::Json archivedApp = own.admin().apps().archive(newAppId);
  E2E_CHECK(archivedApp["status"].asString() == "ARCHIVED");

  std::puts("e2e_studio_admin OK");
  return 0;
}

}  // namespace

int main() {
  try {
    return runAll();
  } catch (const graphql::CrowdyError& e) {
    std::fprintf(stderr, "FATAL [%s]: %s\n", e.code().c_str(), e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    return 1;
  }
}
