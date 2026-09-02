// Safe native gameplay-token rotation: quiesce the old-token UDP connection,
// rotate through the portal, reconnect the same Connection with its handlers,
// and verify the fresh session can still exchange native replication traffic.
#include <atomic>

#include "e2e_util.hpp"

using namespace crowdy;

int main() try {
  auto cfg = e2e::requireConfig();
  auto player = e2e::signIn(cfg, cfg.email);
  e2e::connectUdp(player, cfg);

  const auto connection = player.conn;
  const std::string oldToken = player.game->getToken();
  std::atomic<int> statusChanges{0};
  replication::Handlers handlers;
  handlers.status = [&](replication::ConnState) {
    statusChanges.fetch_add(1, std::memory_order_relaxed);
  };
  connection->setHandlers(std::move(handlers));

  GameplayTokenRefreshResult refreshed =
      player.game->refreshGameplayToken();
  E2E_CHECK(refreshed.ok());
  E2E_CHECK(refreshed.hadActiveReplication);
  E2E_CHECK(refreshed.tokenInstalled);
  E2E_CHECK(refreshed.reconnectAttempted);
  E2E_CHECK(refreshed.reconnected);
  E2E_CHECK(refreshed.token.token.size() == 64);
  E2E_CHECK(refreshed.token.token != oldToken);
  E2E_CHECK(player.game->getToken() == refreshed.token.token);
  E2E_CHECK(player.game->replication().activeConnection() == connection);
  E2E_CHECK(
      static_cast<bool>(connection->snapshot().handlers.status));

  for (int i = 0;
       i < 100 &&
       connection->state() != replication::ConnState::Connected;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    connection->poll();
  }
  E2E_CHECK(connection->state() == replication::ConnState::Connected);

  // A self-echo proves the reassigned server accepted the fresh token/HMAC,
  // rather than merely proving that a local UDP socket reopened.
  E2E_CHECK(e2e::warmUp(*connection, {100800, 0, 100800}, 30000));
  connection->poll();
  E2E_CHECK(statusChanges.load(std::memory_order_relaxed) > 0);

  player.appToken = refreshed.token;
  connection->disconnect();
  std::puts("e2e_gameplay_token_refresh OK");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "exception: %s\n", error.what());
  return 1;
}
