// Host election: two heartbeating players converge on one host, amIHost is
// true for exactly one, and when the host stops heartbeating the election
// fails over to the survivor after the staleness window (~15 s). Mirrors
// Game API SDK e2e: host election.
// Reference: https://docs.crowdedkingdoms.com/game-api/host-election
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

int run() {
  auto cfg = e2e::requireConfig();
  auto a = e2e::provisionPlayer(cfg, "host-a");
  auto b = e2e::provisionPlayer(cfg, "host-b");

  // Election picks the user whose earliest still-fresh actor was created
  // first, deterministically across replicas. On a shared deployment other
  // users hold older actors, so the host is often a THIRD party — the
  // black-box guarantees we can assert are (1) both clients converge on the
  // same non-empty host and (2) amIHost is consistent with that host — not
  // that one of our two fresh players wins.
  E2E_SUBTEST("both heartbeat; the election converges on one consistent host");
  std::string hostId;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
    const std::string idA = a.game->host().heartbeat(cfg.appId)["hostUserId"].asString();
    const std::string idB = b.game->host().heartbeat(cfg.appId)["hostUserId"].asString();
    if (!idA.empty() && idA == idB) {
      hostId = idA;
      break;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  E2E_CHECK(!hostId.empty());
  graphql::Json snapshot = a.game->host().get(cfg.appId);
  E2E_CHECK(snapshot["hostUserId"].asString() == hostId);
  E2E_CHECK(snapshot["actorCount"].asInt64() >= 1);  // the elected host's actor, at minimum

  E2E_SUBTEST("amIHost is consistent with the elected host for each client");
  const bool aIsHost = a.game->host().amIHost(cfg.appId);
  const bool bIsHost = b.game->host().amIHost(cfg.appId);
  E2E_CHECK(aIsHost == (hostId == a.userId));
  E2E_CHECK(bIsHost == (hostId == b.userId));
  std::printf("elected host=%s (ours: a=%s b=%s)\n", hostId.c_str(), a.userId.c_str(),
              b.userId.c_str());

  // If one of our players IS the host (isolated deployment), exercise
  // failover: it stops beating and the other overtakes after staleness.
  if (hostId == a.userId || hostId == b.userId) {
    E2E_SUBTEST("elected host stops heartbeating; survivor overtakes after staleness");
    e2e::Player& survivor = (hostId == a.userId) ? b : a;
    const std::string formerHost = hostId;
    std::string newHost = formerHost;
    const auto failoverStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - failoverStart < std::chrono::seconds(60)) {
      newHost = survivor.game->host().heartbeat(cfg.appId)["hostUserId"].asString();
      if (newHost != formerHost) break;
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    E2E_CHECK(newHost != formerHost);  // the stale host was replaced
  } else {
    std::puts("(a third party is host; failover subtest needs an isolated deployment — skipped)");
  }

  std::puts("e2e_host_election OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
