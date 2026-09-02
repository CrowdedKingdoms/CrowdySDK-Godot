#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/session/durable.hpp"
#include "test_util.hpp"

using namespace crowdy;

namespace {

const std::string kOldToken(64, 'o');
const std::string kFreshToken(64, 'f');

std::string bearer(const graphql::HttpRequest& request) {
  for (const auto& [name, value] : request.headers) {
    if (name == "Authorization") return value;
  }
  return {};
}

class PortableTransport final : public graphql::IHttpTransport {
 public:
  std::vector<graphql::HttpRequest> requests;
  int assignmentCalls = 0;
  int refreshCalls = 0;
  bool failRefresh = false;
  bool failReconnect = false;
  std::string refreshGameTokenId = "202";
  /// When set, the next AppDiscovery is answered with WRONG_DATACENTER pointing
  /// at this origin, so the client's own redirect handler has to run.
  std::string redirectAppDiscoveryTo;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    requests.push_back(request);
    if (request.body.find("AppDiscovery") != std::string::npos) {
      if (!redirectAppDiscoveryTo.empty()) {
        const std::string target = redirectAppDiscoveryTo;
        redirectAppDiscoveryTo.clear();
        return {200,
                std::string(
                    R"({"errors":[{"message":"wrong dc","extensions":{"code":"WRONG_DATACENTER","appId":"42","appDatacenter":"or","gameApiUrl":")") +
                    target + R"("}}]})"};
      }
      return {200,
              R"({"data":{"appDiscovery":[{"appId":"42","datacenterCode":"or","gameApiUrl":"https://ck-or.game.invalid","gameApiWsUrl":"wss://ck-or.game.invalid"}]}})"};
    }
    if (request.body.find("RefreshAppToken") != std::string::npos) {
      ++refreshCalls;
      if (failRefresh) {
        return {
            200,
            R"({"errors":[{"message":"refresh refused","extensions":{"code":"UNAUTHENTICATED"}}]})"};
      }
      return {
          200,
          std::string(
              R"({"data":{"refreshAppToken":{"token":")") +
              kFreshToken +
              R"(","gameTokenId":")" + refreshGameTokenId +
              R"(","appId":"42","expiresAt":"2030-01-01T00:00:00.000Z","gameApiUrl":"https://game.invalid","gameApiWsUrl":"wss://game.invalid","launchUrl":null}}})"};
    }
    if (request.body.find("ServerWithLeastClients") != std::string::npos) {
      ++assignmentCalls;
      if (failReconnect && assignmentCalls > 1) {
        return {
            200,
            R"({"errors":[{"message":"no replication server","extensions":{"code":"UNAVAILABLE"}}]})"};
      }
      return {
          200,
          R"({"data":{"serverWithLeastClients":{"serverId":"server-1","ip4":"127.0.0.1","ip6":"","clientPort":39001,"status":"READY","peers":[],"clients":"0","cpuPeakPct":0,"updatedAt":"","createdAt":""}}})"};
    }
    return {200, R"({"data":{"ok":true}})"};
  }
};

class DurableTransport final : public graphql::IHttpTransport {
 public:
  int updates = 0;
  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    if (request.body.find("UserAppState") != std::string::npos &&
        request.body.find("UpdateUserAppState") == std::string::npos) {
      return {200,
              R"({"data":{"userAppState":{"appId":"42","userId":"7","state":"AQI=","createdAt":"","updatedAt":""}}})"};
    }
    if (request.body.find("UpdateUserAppState") != std::string::npos) {
      ++updates;
      return {200,
              R"({"data":{"updateUserAppState":{"appId":"42","userId":"7","state":"AwQ=","createdAt":"","updatedAt":""}}})"};
    }
    if (request.body.find(R"("operationName":"AvatarById")") !=
        std::string::npos) {
      return {200,
              R"({"data":{"avatar":{"avatarId":"9","userId":"7","name":"Hero","publicState":"AQ==","privateState":"AgM=","createdAt":""}}})"};
    }
    if (request.body.find(R"("operationName":"AvatarAppState")") !=
        std::string::npos) {
      return {200,
              R"({"data":{"avatarAppState":{"appId":"42","avatarId":"9","state":"BA==","createdAt":"","updatedAt":""}}})"};
    }
    if (request.body.find(R"("operationName":"UpdateAvatarState")") !=
        std::string::npos) {
      ++updates;
      return {200,
              R"({"data":{"updateAvatarState":{"avatarId":"9","userId":"7","name":"Hero","publicState":"AQ==","privateState":"BQY=","createdAt":""}}})"};
    }
    return {200, R"({"data":{"ok":true}})"};
  }
};

ClientConfig portableConfig(
    const std::shared_ptr<PortableTransport>& transport) {
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.wsUrl = "wss://game.invalid";
  config.transport = transport;
  return config;
}

std::shared_ptr<replication::Connection> connectNative(
    CrowdyClient& client, int* statusCalls = nullptr) {
  replication::Config config;
  config.appId = 42;
  config.token = {kOldToken, 101, 0};
  config.manualPump = true;
  config.sessionReadyWaitMs = 0;
  replication::Handlers handlers;
  if (statusCalls) {
    handlers.status = [statusCalls](replication::ConnState) {
      ++*statusCalls;
    };
  }
  return client.replication().connect(config, std::move(handlers));
}

void testEndpointNormalization() {
  struct Case {
    std::string http;
    std::string ws;
    std::string expectedHttp;
    std::string expectedWs;
  };
  const std::vector<Case> cases = {
      {"http://game.invalid", "ws://game.invalid",
       "http://game.invalid/graphql", "ws://game.invalid/graphql"},
      {"https://game.invalid/", "wss://game.invalid/",
       "https://game.invalid/graphql", "wss://game.invalid/graphql"},
      {"https://game.invalid/tenant/7///",
       "wss://game.invalid/tenant/7///",
       "https://game.invalid/tenant/7/graphql",
       "wss://game.invalid/tenant/7/graphql"},
      {"https://game.invalid/graphql/",
       "wss://game.invalid/graphql/",
       "https://game.invalid/graphql", "wss://game.invalid/graphql"},
      {" https://game.invalid/graphql/graphql/ ",
       " wss://game.invalid/graphql/graphql/ ",
       "https://game.invalid/graphql", "wss://game.invalid/graphql"},
  };

  for (const auto& testCase : cases) {
    auto transport = std::make_shared<PortableTransport>();
    ClientConfig config;
    config.httpUrl = testCase.http;
    config.wsUrl = testCase.ws;
    config.transport = transport;
    CrowdyClient client(std::move(config));
    CHECK_EQ(client.graphqlClient().endpoint(), testCase.expectedHttp);
    CHECK_EQ(client.websocketEndpoint(), testCase.expectedWs);
    CHECK_EQ(client.subscriptions().endpoint(), testCase.expectedWs);
  }

  auto transport = std::make_shared<PortableTransport>();
  ClientConfig custom;
  custom.httpUrl = "https://game.invalid/base/";
  custom.wsUrl = "wss://game.invalid/base/";
  custom.graphqlEndpoint = "https://custom.invalid/game-query/";
  custom.wsEndpoint = "wss://custom.invalid/subscriptions/";
  custom.transport = transport;
  CrowdyClient explicitEndpoints(std::move(custom));
  CHECK_EQ(explicitEndpoints.graphqlClient().endpoint(),
           "https://custom.invalid/game-query/");
  CHECK_EQ(explicitEndpoints.websocketEndpoint(),
           "wss://custom.invalid/subscriptions/");
  CHECK_EQ(explicitEndpoints.subscriptions().endpoint(),
           "wss://custom.invalid/subscriptions/");

  ClientConfig customPaths;
  customPaths.httpUrl = "https://game.invalid/root/";
  customPaths.wsUrl = "wss://game.invalid/root/";
  customPaths.graphqlEndpoint = "/v2/query";
  customPaths.wsEndpoint = "/stream";
  customPaths.transport = transport;
  CrowdyClient relativeEndpoints(std::move(customPaths));
  CHECK_EQ(relativeEndpoints.graphqlClient().endpoint(),
           "https://game.invalid/root/v2/query");
  CHECK_EQ(relativeEndpoints.websocketEndpoint(),
           "wss://game.invalid/root/stream");
  CHECK_EQ(relativeEndpoints.subscriptions().endpoint(),
           "wss://game.invalid/root/stream");
}

void testSynchronousGameplayRefresh() {
  auto transport = std::make_shared<PortableTransport>();
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  int statusCalls = 0;
  auto connection = connectNative(client, &statusCalls);
  CHECK_EQ(transport->assignmentCalls, 1);

  GameplayTokenRefreshResult result = client.refreshGameplayToken();
  CHECK(result.ok());
  CHECK(result.tokenInstalled);
  CHECK(result.reconnectAttempted);
  CHECK(result.reconnected);
  CHECK(result.hadActiveReplication);
  CHECK_EQ(result.previousReplicationState,
           replication::ConnState::Connecting);
  CHECK_EQ(result.previousReplicationEndpoint.ip4, "127.0.0.1");
  CHECK_EQ(result.previousReplicationEndpoint.clientPort, 39001);
  CHECK_EQ(result.replicationEndpoint.ip4, "127.0.0.1");
  CHECK_EQ(client.getToken(), kFreshToken);
  CHECK_EQ(result.token.token, kFreshToken);
  CHECK_EQ(transport->assignmentCalls, 2);
  CHECK_EQ(transport->refreshCalls, 1);
  CHECK(client.replication().activeConnection() == connection);
  CHECK(static_cast<bool>(connection->snapshot().handlers.status));

  CHECK_EQ(bearer(transport->requests.at(0)), "Bearer " + kOldToken);
  CHECK_EQ(bearer(transport->requests.at(1)), "Bearer " + kOldToken);
  CHECK_EQ(bearer(transport->requests.at(2)), "Bearer " + kFreshToken);

  connection->pump();
  connection->poll();
  CHECK(statusCalls > 0);
}

#ifndef CROWDY_NO_EXCEPTIONS
void testRefreshFailureRetainsOldToken() {
  auto transport = std::make_shared<PortableTransport>();
  transport->failRefresh = true;
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  auto connection = connectNative(client);

  GameplayTokenRefreshResult result = client.refreshGameplayToken();
  CHECK(!result.ok());
  CHECK_EQ(result.stage, GameplayTokenRefreshStage::Refresh);
  CHECK_EQ(result.errorCode, "UNAUTHENTICATED");
  CHECK_EQ(client.getToken(), kOldToken);
  CHECK(!result.tokenInstalled);
  CHECK(!result.reconnectAttempted);
  CHECK_EQ(connection->state(), replication::ConnState::Closed);

  transport->failRefresh = false;
  CHECK(connection->connect().ok());
  CHECK_EQ(bearer(transport->requests.back()), "Bearer " + kOldToken);
}
#else
void testBlockingRefreshFailureFailsClosedWithoutExceptions() {
  auto transport = std::make_shared<PortableTransport>();
  transport->failRefresh = true;
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);

  // Blocking request() has no typed failure channel in this mode. Verify the
  // compatibility wrapper returns no token and never replaces the old bearer;
  // the async lifecycle test below still checks the exact Refresh-stage error.
  const auto refreshed = client.portal().refresh();
  CHECK(refreshed.token.empty());
  CHECK_EQ(client.getToken(), kOldToken);
  CHECK_EQ(transport->refreshCalls, 1);
}
#endif

void testReconnectFailureRetainsFreshToken() {
  auto transport = std::make_shared<PortableTransport>();
  transport->failReconnect = true;
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  auto connection = connectNative(client);

  GameplayTokenRefreshResult result = client.refreshGameplayToken();
  CHECK(!result.ok());
  CHECK_EQ(result.stage, GameplayTokenRefreshStage::Reconnect);
  CHECK(result.tokenInstalled);
  CHECK(result.reconnectAttempted);
  CHECK(!result.reconnected);
  CHECK_EQ(client.getToken(), kFreshToken);
  CHECK_EQ(connection->state(), replication::ConnState::Failed);

  transport->failReconnect = false;
  CHECK(connection->connect().ok());
  CHECK_EQ(bearer(transport->requests.back()), "Bearer " + kFreshToken);
}

void testAsyncGameplayRefreshUsesPoll() {
  auto transport = std::make_shared<PortableTransport>();
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  auto connection = connectNative(client);

  bool called = false;
  GameplayTokenRefreshResult result;
  client.refreshGameplayTokenAsync(
      [&](GameplayTokenRefreshResult value) {
        called = true;
        result = std::move(value);
      });
  CHECK(!called);
  CHECK_EQ(connection->state(), replication::ConnState::Closed);
  CHECK_EQ(client.getToken(), kOldToken);

  client.poll();
  CHECK(called);
  CHECK(result.ok());
  CHECK(result.reconnected);
  CHECK_EQ(client.getToken(), kFreshToken);
}

void testAsyncRefreshFailureUsesPollAndRetainsOldToken() {
  auto transport = std::make_shared<PortableTransport>();
  transport->failRefresh = true;
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);
  auto connection = connectNative(client);

  bool called = false;
  GameplayTokenRefreshResult result;
  client.refreshGameplayTokenAsync(
      [&](GameplayTokenRefreshResult value) {
        called = true;
        result = std::move(value);
      });
  CHECK(!called);
  CHECK_EQ(client.getToken(), kOldToken);
  client.poll();

  CHECK(called);
  CHECK(!result.ok());
  CHECK_EQ(result.stage, GameplayTokenRefreshStage::Refresh);
  CHECK_EQ(result.errorCode, "UNAUTHENTICATED");
  CHECK_EQ(client.getToken(), kOldToken);
  CHECK_EQ(connection->state(), replication::ConnState::Closed);
}

void testOverflowingGameplayTokenIdIsRejectedWithoutNarrowing() {
  auto transport = std::make_shared<PortableTransport>();
  transport->refreshGameTokenId = "9223372036854775808";
  CrowdyClient client(portableConfig(transport));
  client.setToken(kOldToken);

  const GameplayTokenRefreshResult result = client.refreshGameplayToken();
  CHECK(!result.ok());
  CHECK_EQ(result.stage, GameplayTokenRefreshStage::Install);
  CHECK_EQ(result.status.code, Errc::InvalidArgument);
  CHECK_EQ(result.errorCode, "GAME_TOKEN_ID_OUT_OF_RANGE");
  CHECK_EQ(result.token.gameTokenId, "9223372036854775808");
  CHECK(!result.token.gameTokenIdInt64().has_value());
  CHECK_EQ(client.getToken(), kOldToken);
}

void testNullableTokenAndProfileFieldsRemainDistinct() {
  const auto tokenJson = graphql::Json::parse(
      R"({"token":"t","gameTokenId":"18446744073709551616","appId":"42","expiresAt":"2030-01-01T00:00:00Z","gameApiUrl":null,"gameApiWsUrl":"","launchUrl":null})");
  const auto token = domains::AppTokenResponse::fromJson(tokenJson);
  CHECK_EQ(token.gameTokenId, "18446744073709551616");
  CHECK(!token.gameTokenIdInt64().has_value());
  CHECK(!token.gameApiUrl.has_value());
  CHECK(token.gameApiUrl.empty());
  CHECK(token.gameApiWsUrl.has_value());
  CHECK(token.gameApiWsUrl.empty());
  CHECK(!token.launchUrl.has_value());

  const auto authJson = graphql::Json::parse(
      R"({"token":"identity","user":{"userId":"9223372036854775808","email":null,"gamertag":""}})");
  const auto auth = domains::AuthResponse::fromJson(authJson);
  CHECK_EQ(auth.userId, "9223372036854775808");
  CHECK(!auth.email.has_value());
  CHECK(auth.email.empty());
  CHECK(auth.gamertag.has_value());
  CHECK(auth.gamertag.empty());
}

void testDurableStoreObservability() {
  auto transport = std::make_shared<DurableTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  session::SaveStateStore save(client, "42");
  CHECK_EQ(save.load().size(), std::size_t{2});
  CHECK(!save.dirty());
  CHECK(!save.lastSavedAt().has_value());
  const std::uint8_t replacement[] = {3, 4};
  save.set(Bytes(replacement, sizeof(replacement)));
  CHECK(save.dirty());
  CHECK_EQ(save.snapshot().at(0), std::uint8_t{3});
  save.save();
  CHECK(!save.dirty());
  CHECK(save.lastSavedAt().has_value());
  const auto updatesAfterSave = transport->updates;
  const std::uint8_t patch[] = {9};
  save.patch(1, Bytes(patch, sizeof(patch)));
  CHECK(save.dirty());
  CHECK_EQ(save.snapshot().at(1), std::uint8_t{9});
  CHECK_EQ(transport->updates, updatesAfterSave);

  session::AvatarStateStore avatar(client, "42", "9");
  avatar.load();
  const auto privateState = avatar.privateState();
  CHECK_EQ(privateState.size(), std::size_t{2});
  CHECK_EQ(privateState.at(0), std::uint8_t{2});
  const std::uint8_t privateReplacement[] = {5, 6};
  avatar.setPrivateState(
      Bytes(privateReplacement, sizeof(privateReplacement)));
  CHECK_EQ(avatar.privateState().at(1), std::uint8_t{6});
}

// CrowdyClient's move assignment is a hand-written list of 30-plus members, and
// two newly added domains were missing from it — which does not fail to compile,
// it produces null accessors on a moved client. Touching every accessor is the
// only thing that can catch the next omission.
//
// It also covers the subtler half: the GraphQL and subscription clients are held
// by shared_ptr, so they SURVIVE a move while any lambda inside them still
// points at the object that was moved from. Provoking a redirect after a move is
// what proves the handlers were rebound rather than left dangling.
void testAMovedClientIsFullyUsable() {
  auto transport = std::make_shared<PortableTransport>();
  CrowdyClient original(portableConfig(transport));
  CrowdyClient client = std::move(original);

  // Each accessor is CALLED, not merely named. Naming one dereferences a null
  // unique_ptr to form a reference and discards it, which does not fault — the
  // first version of this test passed with the move of discovery_ deleted.
  {
    const auto placements = client.discovery().apps({"42"});
    CHECK_EQ(placements.size(), std::size_t{1});
    CHECK_EQ(placements.front().gameApiUrl, "https://ck-or.game.invalid");
    CHECK(placements.front().placed());
  }
  (void)client.realtimeControl().watch({});
  (void)client.auth();
  (void)client.users();
  (void)client.portal();
  (void)client.serverStatus();
  (void)client.chunks();
  (void)client.voxels();
  (void)client.actors();
  (void)client.avatars();
  (void)client.state();
  (void)client.host();
  (void)client.teleport();
  (void)client.teams();
  (void)client.channels();
  (void)client.gameModel();
#ifndef CROWDY_NO_EXCEPTIONS
  (void)client.compute();
  (void)client.crowdyStudio();
#endif
  (void)client.playerCompute();
  (void)client.playerWallet();
  (void)client.marketplace();
  (void)client.playerModel();
  (void)client.gameApps();
  (void)client.platform();
  (void)client.crowdyStudioAgent();
  (void)client.admin();
  (void)client.operator_();
  CHECK_EQ(client.graphqlClient().endpoint(),
           "https://game.invalid/graphql");

  // A target has to be a SIBLING in the same estate; an unrelated origin is
  // refused, which is the point of the bound rather than an obstacle.
  CHECK(!client.moveToDatacenter("https://elsewhere.example.com"));

  // Drive the redirect through a real server response, so the handler the move
  // had to rebind is the thing under test. Calling moveToDatacenter directly
  // would prove nothing: it does not go through the handler at all, and the
  // first version of this test passed with the rebinding deleted.
  transport->redirectAppDiscoveryTo = "https://ck-va.game.invalid";
  const auto afterRedirect = client.discovery().apps({"42"});
  CHECK_EQ(afterRedirect.size(), std::size_t{1});
  CHECK_EQ(client.graphqlClient().endpoint(),
           "https://ck-va.game.invalid/graphql");
}

}  // namespace

int main() {
  testAMovedClientIsFullyUsable();
  testEndpointNormalization();
  testSynchronousGameplayRefresh();
#ifndef CROWDY_NO_EXCEPTIONS
  testRefreshFailureRetainsOldToken();
#else
  testBlockingRefreshFailureFailsClosedWithoutExceptions();
#endif
  testReconnectFailureRetainsFreshToken();
  testAsyncGameplayRefreshUsesPoll();
  testAsyncRefreshFailureUsesPollAndRetainsOldToken();
  testOverflowingGameplayTokenIdIsRejectedWithoutNarrowing();
  testNullableTokenAndProfileFieldsRemainDistinct();
  testDurableStoreObservability();
  std::puts("client_portable_test OK");
  return 0;
}
