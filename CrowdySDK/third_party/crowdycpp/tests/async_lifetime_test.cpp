#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "test_util.hpp"

using namespace crowdy;

namespace {

class UnusedSyncTransport final : public graphql::IHttpTransport {
 public:
  graphql::HttpResponse send(const graphql::HttpRequest&) override {
    return {500, {}};
  }
};

class DeferredTransport final : public graphql::IAsyncHttpTransport {
 public:
  void sendAsync(
      const graphql::HttpRequest&,
      std::function<void(graphql::HttpOutcome)> callback) override {
    pending_.push_back(std::move(callback));
  }

  std::size_t pending() const { return pending_.size(); }

  void complete(graphql::HttpOutcome outcome) {
    CHECK(!pending_.empty());
    auto callback = std::move(pending_.front());
    pending_.erase(pending_.begin());
    callback(std::move(outcome));
  }

 private:
  std::vector<std::function<void(graphql::HttpOutcome)>> pending_;
};

graphql::HttpOutcome refreshSuccess() {
  graphql::HttpOutcome outcome;
  outcome.status = Errc::Ok;
  outcome.response.status = 200;
  outcome.response.body =
      R"({"data":{"refreshAppToken":{"token":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff","gameTokenId":"202","appId":"42","expiresAt":"2030-01-01T00:00:00.000Z","gameApiUrl":null,"gameApiWsUrl":"","launchUrl":null}}})";
  return outcome;
}

ClientConfig configFor(
    const std::shared_ptr<DeferredTransport>& asyncTransport) {
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = std::make_shared<UnusedSyncTransport>();
  config.asyncTransport = asyncTransport;
  return config;
}

void testDestroyedPortalSuppressesRetainedCompletion() {
  auto transport = std::make_shared<DeferredTransport>();
  auto dispatcher = std::make_shared<graphql::Dispatcher>();
  auto auth = std::make_shared<graphql::AuthState>();
  auto gql = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{"https://management.invalid/graphql", 100},
      std::make_shared<UnusedSyncTransport>(), auth);
  gql->setAsyncTransport(transport);
  gql->setDispatcher(dispatcher);

  bool called = false;
  {
    domains::PortalAPI portal(gql, auth, core::unavailableCrypto());
    portal.refreshAsync(
        [&](graphql::GraphQLOutcome, domains::AppTokenResponse) {
          called = true;
        });
    CHECK_EQ(transport->pending(), std::size_t{1});
  }

  transport->complete(refreshSuccess());
  CHECK_EQ(dispatcher->drain(), std::size_t{1});
  CHECK(!called);
  CHECK(!auth->hasToken());
}

void testDestroyedClientSuppressesPortalCompletion() {
  auto transport = std::make_shared<DeferredTransport>();
  std::shared_ptr<graphql::Dispatcher> dispatcher;
  bool called = false;
  {
    auto client = std::make_unique<CrowdyClient>(configFor(transport));
    dispatcher = client->graphqlClient().dispatcher();
    client->portal().refreshAsync(
        [&](graphql::GraphQLOutcome, domains::AppTokenResponse) {
          called = true;
        });
    CHECK_EQ(transport->pending(), std::size_t{1});
  }

  transport->complete(refreshSuccess());
  CHECK_EQ(dispatcher->drain(), std::size_t{0});
  CHECK(!called);
}

void testDestroyedClientSuppressesGameplayRefreshCompletion() {
  auto transport = std::make_shared<DeferredTransport>();
  std::shared_ptr<graphql::Dispatcher> dispatcher;
  bool called = false;
  {
    auto client = std::make_unique<CrowdyClient>(configFor(transport));
    client->setToken(std::string(64, 'o'));
    dispatcher = client->graphqlClient().dispatcher();
    client->refreshGameplayTokenAsync(
        [&](GameplayTokenRefreshResult) { called = true; });
    CHECK_EQ(transport->pending(), std::size_t{1});
  }

  transport->complete(refreshSuccess());
  CHECK_EQ(dispatcher->drain(), std::size_t{0});
  CHECK(!called);
}

void testCloseSuppressesCompletionBeforeDestruction() {
  auto transport = std::make_shared<DeferredTransport>();
  CrowdyClient client(configFor(transport));
  auto dispatcher = client.graphqlClient().dispatcher();
  bool called = false;
  client.portal().refreshAsync(
      [&](graphql::GraphQLOutcome, domains::AppTokenResponse) {
        called = true;
      });
  client.close();

  transport->complete(refreshSuccess());
  CHECK_EQ(dispatcher->drain(), std::size_t{0});
  CHECK(!called);
}

void testFailedAsyncLogoutRetainsToken() {
  auto transport = std::make_shared<DeferredTransport>();
  CrowdyClient client(configFor(transport));
  client.setToken("identity-token");

  bool called = false;
  client.auth().logoutAsync([&](graphql::GraphQLOutcome outcome, bool ok) {
    called = true;
    CHECK(!outcome.ok());
    CHECK(!ok);
  });

  graphql::HttpOutcome failure;
  failure.status = Errc::SocketError;
  failure.errorMessage = "offline";
  transport->complete(std::move(failure));
  client.poll();
  CHECK(called);
  CHECK_EQ(client.getToken(), "identity-token");
}

}  // namespace

int main() {
  testDestroyedPortalSuppressesRetainedCompletion();
  testDestroyedClientSuppressesPortalCompletion();
  testDestroyedClientSuppressesGameplayRefreshCompletion();
  testCloseSuppressesCompletionBeforeDestruction();
  testFailedAsyncLogoutRetainsToken();
  std::puts("async_lifetime_test OK");
  return 0;
}
