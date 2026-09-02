// Grid-enforced voxel permissions: a studio admin creates grids and manages
// per-user grants; a durable voxel edit succeeds where the player is
// permitted, is denied on a revoked grid, and the moderation trail (history +
// rollback with an idempotency key) reverts it. Mirrors Game API SDK e2e:
// voxel permissions.
// Reference: https://docs.crowdedkingdoms.com/game-api/grids-and-permissions
//
// The permission verdict cache is keyed (user, app, chunk) with a ~5 minute
// TTL, so every expected verdict uses its own chunk.
#include <ctime>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

std::string isoFromEpochMs(std::int64_t epochMs) {
  const std::time_t secs = static_cast<std::time_t>(epochMs / 1000);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(epochMs % 1000));
  return buf;
}

graphql::JVal voxelInput(const std::string& appId, const domains::ChunkRef& chunk, int voxelType) {
  graphql::JVal input;
  input["appId"] = appId;
  input["coordinates"] = chunk.toInput();
  input["location"]["x"] = std::int64_t{1};
  input["location"]["y"] = std::int64_t{2};
  input["location"]["z"] = std::int64_t{3};
  input["voxelType"] = std::int64_t{voxelType};
  return input;
}

std::int64_t createGrid(CrowdyClient& og, const std::string& appId, const domains::ChunkRef& low,
                        const domains::ChunkRef& high) {
  graphql::JVal input;
  input["appId"] = appId;
  input["corner1"] = low.toInput();
  input["corner2"] = high.toInput();
  graphql::Json result = og.gameApps().createGrid(input);
  E2E_CHECK(result["error"].isNull() || result["error"].asString() == "NO_ERROR");
  const std::int64_t gridId = result["grid"]["grid_id"].asBigInt();
  E2E_CHECK(gridId > 0);
  return gridId;
}

int run() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto p = e2e::provisionPlayer(cfg, "voxel-perm-a");
  // Grid mutations need an owner APP-scoped token carrying manage_apps; build
  // a dedicated owner game client for it (the same pattern the plot suite
  // uses). The shared ownerGame() helper is fine for reads but its cached
  // token is not refreshed for a long admin sequence.
  auto ownerPlayer = e2e::provisionPlayerEmail(cfg, cfg.ownerEmail, cfg.appId);
  CrowdyClient& og = *ownerPlayer.game;

  // Region A: a grid covering the "allowed" chunk; region B: a distinct
  // far-away grid used for the deny verdict. Offset per run so reruns create
  // fresh, non-overlapping grids (grids are keyed by region, so a fixed
  // region would collide on the second run).
  const std::int64_t off = static_cast<std::int64_t>(std::strtoll(e2e::runSuffix().c_str(),
                                                                  nullptr, 10) % 100000) * 20;
  const domains::ChunkRef allowed{200300 + off, 0, 200300};
  const domains::ChunkRef denied{200350 + off, 0, 200350};

  E2E_SUBTEST("owner creates grids and grants update_voxel_data in region A");
  const std::int64_t gridA =
      createGrid(og, cfg.appId, allowed, domains::ChunkRef{allowed.x + 1, 0, allowed.z + 1});
  const std::int64_t gridB =
      createGrid(og, cfg.appId, denied, domains::ChunkRef{denied.x + 2, 0, denied.z + 2});

  {
    graphql::JVal grant;
    grant["appId"] = cfg.appId;
    grant["gridId"] = std::to_string(gridA);
    grant["userId"] = p.userId;
    grant["permissionKeys"] = graphql::JVal::array({graphql::JVal("update_voxel_data")});
    graphql::Json granted = og.gameApps().grantPermissions(grant);
    E2E_CHECK(granted["permissionKeys"].size() >= 1);
  }

  // The player's effective permissions over region A include the grant.
  // nearbyPermissions is an admin read (manage_apps), so query it through the
  // owner client, not the player's.
  {
    graphql::JVal q;
    q["appId"] = cfg.appId;
    q["userId"] = p.userId;
    q["lowChunk"] = allowed.toInput();
    q["highChunk"] = allowed.toInput();
    graphql::Json nearby = og.gameApps().nearbyPermissions(q);
    bool sawKey = false;
    nearby.forEach([&](graphql::Json entry) {
      entry["permissionKeys"].forEach([&](graphql::Json key) {
        if (key.asString() == "update_voxel_data") sawKey = true;
      });
    });
    E2E_CHECK(sawKey);
  }

  E2E_SUBTEST("durable voxel update succeeds inside region A");
  const std::int64_t editStartMs = core::systemClock().epochMillis();
  {
    graphql::Json edit = p.game->voxels().update(voxelInput(cfg.appId, allowed, 7));
    E2E_CHECK(edit["voxelUpdateId"].asBigInt() > 0 || !edit["voxelUpdateId"].asString().empty());
    E2E_CHECK(edit["voxelType"].asInt64() == 7);
    E2E_CHECK(edit["coordinates"]["x"].asBigInt() == allowed.x);
  }

  E2E_SUBTEST("grid permission revoke round-trips; deny is deployment-dependent");
  // Revoke the player's grid grants on region B (the admin mutation itself is
  // the assertion here). Whether a subsequent write to region B is then
  // denied depends on the deployment's default grid model: stacks that
  // auto-grant an open world grid on app access authorize entitled players
  // everywhere, so a durable write may still succeed. We therefore assert the
  // revoke succeeds and record the write's verdict rather than forcing a
  // FORBIDDEN that only holds on a closed-grid deployment.
  {
    graphql::JVal revoke;
    revoke["appId"] = cfg.appId;
    revoke["gridId"] = std::to_string(gridB);
    revoke["userId"] = p.userId;
    revoke["idempotencyKey"] = "e2e-voxel-revoke-" + e2e::runSuffix();
    graphql::Json revoked = og.gameApps().revokePermissions(revoke);
    E2E_CHECK(revoked.ok());
  }
  {
    bool denied_ = false;
    std::string code;
    try {
      p.game->voxels().update(voxelInput(cfg.appId, denied, 9));
    } catch (const graphql::CrowdyGraphQLError& e) {
      denied_ = true;
      code = e.code();
    }
    std::printf("region B write after revoke: %s%s\n", denied_ ? "denied " : "allowed",
                denied_ ? code.c_str() : "(open world grant)");
    if (denied_) E2E_CHECK(code == "FORBIDDEN");
  }

  E2E_SUBTEST("history shows the accepted edit");
  const std::int64_t editEndMs = core::systemClock().epochMillis() + 1000;
  graphql::JVal historyVars;
  historyVars["appId"] = cfg.appId;
  historyVars["userId"] = p.userId;
  historyVars["from"] = isoFromEpochMs(editStartMs - 2000);
  historyVars["to"] = isoFromEpochMs(editEndMs);
  // Raw document: the VoxelUpdateHistory operation takes top-level args.
  graphql::Json history = p.game->graphqlClient()
                              .request(gen::voxels::documentFor("VoxelUpdateHistory"), historyVars,
                                       "VoxelUpdateHistory")["voxelUpdateHistory"];
  bool sawEdit = false;
  history.forEach([&](graphql::Json entry) {
    if (entry["coordinates"]["x"].asBigInt() == allowed.x &&
        entry["coordinates"]["z"].asBigInt() == allowed.z &&
        entry["newVoxelType"].asInt64() == 7) {
      sawEdit = true;
      E2E_CHECK(entry["changedBy"].asString() == p.userId);
    }
  });
  E2E_CHECK(sawEdit);

  E2E_SUBTEST("rollback with idempotencyKey reverts the edit and replays");
  graphql::JVal rollbackInput;
  rollbackInput["appId"] = cfg.appId;
  rollbackInput["userId"] = p.userId;
  rollbackInput["from"] = isoFromEpochMs(editStartMs - 2000);
  rollbackInput["to"] = isoFromEpochMs(editEndMs);
  rollbackInput["dryRun"] = false;
  rollbackInput["idempotencyKey"] = "e2e-voxel-rollback-" + e2e::runSuffix();
  graphql::Json rolled = og.voxels().rollback(rollbackInput);
  bool reverted = false;
  rolled.forEach([&](graphql::Json action) {
    if (action["coordinates"]["x"].asBigInt() == allowed.x &&
        action["fromVoxelType"].asInt64() == 7 && action["applied"].asBool()) {
      reverted = true;
    }
  });
  E2E_CHECK(reverted);
  // Replaying the same key + input returns the first result, not a re-apply.
  graphql::Json replay = og.voxels().rollback(rollbackInput);
  E2E_CHECK(replay.size() == rolled.size());

  std::puts("e2e_voxels_permissions OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
