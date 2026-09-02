// Replication API behavior: how runtime permission changes reach sessions.
// The server caches a session's permission verdict and re-pulls it only when
// a send is DENIED (deny -> re-pull with backoff -> converge). So:
//   - a revocation does NOT interrupt a live authorized session; it takes
//     effect at the next session install (reconnect), and
//   - a re-grant converges on a live DENIED session without reconnecting,
//     because each denial triggers a backoff-limited refresh.
// SLOW suite (multi-phase waits) — gated on CROWDY_E2E_SLOW. See
// https://docs.crowdedkingdoms.com/replication-api/operations.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

// Poll a voxel send until it is either accepted (no error / self-echo) or
// rejected with UNAUTHORIZED, up to budgetMs. Returns true when it settles
// into the wanted state (accepted=true wants acceptance).
bool voxelSettles(e2e::Player& p, const wire::ChunkCoord& chunk, const core::ActorUuid& uuid,
                  bool wantAccepted, int budgetMs) {
  const auto start = std::chrono::steady_clock::now();
  const std::uint8_t pose[] = {1};
  int consecutiveSilent = 0;
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(budgetMs)) {
    // Voxel fan-out (including the sender's own echo) only reaches actors
    // with fresh presence in range, so refresh presence each attempt. Its
    // outcome is irrelevant here — after a revoke it is refused too.
    (void)p.conn->sendActorUpdate({chunk, uuid, Bytes(pose, 1), 8});
    auto outcome = p.conn->sendVoxelUpdateAndWait(chunk, uuid, 1, 1, 1, 7, Bytes(), 8,
                                                  wire::DecayRate::None, 1500);
    if (std::getenv("CROWDY_E2E_VERBOSE")) {
      std::fprintf(stderr, "voxel outcome: ack=%d err=%d\n", outcome.acknowledged ? 1 : 0,
                   outcome.error.has_value() ? static_cast<int>(outcome.error.value()) : -1);
    }
    if (wantAccepted && outcome.acknowledged && !outcome.error.has_value()) return true;
    if (!wantAccepted) {
      // Denial surfaces as UNAUTHORIZED(7) when the session holds the app
      // scope but lacks the permission bit, or as INVALID_APP_ID(18) when the
      // access row is gone entirely (a revoked user's token installs with no
      // app scope at all). A server that declines to install the session
      // answers with silence — treat a sustained run of silent sends as the
      // deny verdict too; an authorized session echoes within an attempt.
      if (outcome.error.has_value() &&
          (outcome.error.value() == wire::ErrorCode::Unauthorized ||
           outcome.error.value() == wire::ErrorCode::InvalidAppId))
        return true;
      if (!outcome.acknowledged && !outcome.error.has_value()) {
        if (++consecutiveSilent >= 8) return true;
      } else {
        consecutiveSilent = 0;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  return false;
}

}  // namespace

int main() try {
  e2e::requireFlag("CROWDY_E2E_SLOW");
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto p = e2e::provisionPlayer(cfg, "permref");
  e2e::connectUdp(p, cfg);

  const wire::ChunkCoord chunk{100600, 0, 100600};
  const auto uuid = core::generateActorUuid();

  // Load the chunk's permission window before asserting (first-chunk denial).
  E2E_CHECK(e2e::warmUp(*p.conn, chunk));

  E2E_SUBTEST("granted player: voxel sends are accepted");
  E2E_CHECK(voxelSettles(p, chunk, uuid, /*wantAccepted=*/true, 30000));

  E2E_SUBTEST("revocation spares the live session but denies the next one");
  {
    // RevokeAppAccess takes appId + userId (see the operation).
    e2e::owner(cfg).admin().appAccess().revoke(cfg.appId, p.userId);
  }
  // The server only re-pulls a session's permission state when a send is
  // DENIED, so the authorized live session keeps flowing after the revoke —
  // verify it hasn't been cut off.
  E2E_CHECK(voxelSettles(p, chunk, uuid, /*wantAccepted=*/true, 15000));
  // A revoked user can no longer mint a fresh app token (management plane).
  {
    bool mintDenied = false;
    try {
      p.identity->portal().mintAppToken(cfg.appId);
    } catch (const graphql::CrowdyGraphQLError&) {
      mintDenied = true;
    }
    E2E_CHECK(mintDenied);
  }
  // The replication plane keeps a warm client slot per token and reuses it on
  // immediate reconnects, so the revocation only becomes observable once the
  // slot is reaped by the client-staleness window (~2 min of silence). Go
  // quiet past that window, then reconnect: the fresh install pulls the
  // revoked row and every spatial send draws UNAUTHORIZED.
  p.conn->disconnect();
  std::printf("silent through the client-staleness window (150 s)...\n");
  std::this_thread::sleep_for(std::chrono::seconds(150));
  e2e::connectUdp(p, cfg);
  std::printf("fresh session should now see the revocation (up to 2 min)...\n");
  E2E_CHECK(voxelSettles(p, chunk, uuid, /*wantAccepted=*/false, 120000));

  E2E_SUBTEST("re-grant converges on the live denied session (deny -> refresh)");
  {
    const std::string tierId = e2e::ensureEntitledTier(cfg, cfg.appId);
    graphql::JVal grant;
    grant["appId"] = cfg.appId;
    grant["userId"] = p.userId;
    grant["tierId"] = tierId;
    e2e::owner(cfg).admin().appAccess().grant(grant);
  }
  // No reconnect here — every denial triggers a backoff-limited permission
  // re-pull, so the denied session converges to accepted on its own.
  std::printf("waiting for the live session to converge after re-grant (up to 3 min)...\n");
  E2E_CHECK(voxelSettles(p, chunk, uuid, /*wantAccepted=*/true, 180000));

  p.conn->disconnect();
  std::puts("e2e_permission_refresh OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
