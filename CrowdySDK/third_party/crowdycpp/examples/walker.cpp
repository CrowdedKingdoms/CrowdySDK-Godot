// Minimal native game client: sign in (dev bypass), mint an app token,
// connect the native UDP replication client, and walk an actor around while
// printing whoever else is nearby.
//
//   walker <apiUrl> <email> <password> <appId> [seconds]
//
// apiUrl is the shared entry origin. Sign-in is email + password, which works
// against every environment; magic link and social are the other two paths.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <crowdy/crowdy.hpp>

using namespace crowdy;

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: walker <apiUrl> <email> <password> <appId> [seconds]\n");
    return 2;
  }
  const std::string apiUrl = argv[1];
  const std::string email = argv[2];
  const std::string password = argv[3];
  const std::string appId = argv[4];
  const int seconds = argc > 5 ? std::atoi(argv[5]) : 30;

  // 1) Identity client + email/password sign-in.
  ClientConfig identityCfg;
  identityCfg.httpUrl = apiUrl;
  CrowdyClient identity(std::move(identityCfg));
  auto login = identity.auth().login(email, password);
  std::printf("signed in as user %s\n", login.userId.c_str());

  // 2) App-scoped token -> per-game client.
  auto minted = identity.portal().mintAppToken(appId);
  ClientConfig gameCfg;
  gameCfg.httpUrl = minted.gameApiUrl.empty()
                        ? apiUrl
                        : minted.gameApiUrl.valueOrEmpty();
  // Where to look when that one instance stops answering.
  gameCfg.discoveryUrl =
      minted.discoveryUrl.empty() ? apiUrl : minted.discoveryUrl.valueOrEmpty();
  CrowdyClient game(std::move(gameCfg));
  game.setToken(minted.token);

  // 3) Native replication connection (owned net thread).
  replication::Config rc;
  rc.appId = std::strtoll(appId.c_str(), nullptr, 10);
  rc.token.token = minted.token;
  rc.token.gameTokenId = minted.gameTokenIdInt64().value_or(0);
  rc.token.expiresAtEpochMs =
      core::parseIso8601Millis(minted.expiresAt.data(), minted.expiresAt.size());
  auto conn = game.replication().connect(rc);
  while (conn->state() != replication::ConnState::Connected) {
    conn->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::printf("connected to %s:%d\n", conn->assignment().ip4.c_str(),
              conn->assignment().clientPort);

  // 4) World session: our pose at 5 Hz, everyone else in the registry.
  session::WorldSessionConfig sc;
  sc.appId = appId;
  session::WorldSession world(conn, &game, sc);
  world.actors().onJoin([](const session::RemoteActor& actor) {
    std::printf("+ actor %.8s joined\n", actor.uuid.data());
  });
  world.actors().onLeave([](const session::RemoteActor& actor) {
    std::printf("- actor %.8s left\n", actor.uuid.data());
  });

  session::UnrealPose pose;
  world.join({0, 0, 0}, Bytes(reinterpret_cast<const std::uint8_t*>(&pose), sizeof(pose)));

  const auto start = std::chrono::steady_clock::now();
  double t = 0;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
    t += 0.05;
    pose.positionX = 100.0 * std::cos(t);
    pose.positionY = 100.0 * std::sin(t);
    world.self().setState(Bytes(reinterpret_cast<const std::uint8_t*>(&pose), sizeof(pose)));
    world.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  std::printf("done: %zu actors visible, %llu datagrams received\n", world.actors().size(),
              static_cast<unsigned long long>(conn->stats().datagramsReceived));
  world.dispose();
  return 0;
}
