// Optional live graphql-transport-ws evidence. Exercises both the generic
// subscription client and the typed game-model container metadata feed, then
// uses the metadata event to refresh a separate pull-based ContainerMirror.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

graphql::GraphQLSubscriptionError outcomeError(
    const graphql::GraphQLSubscriptionOutcome& outcome) {
  graphql::GraphQLSubscriptionError error;
  error.status = outcome.status;
  error.kind = graphql::GraphQLSubscriptionErrorKind::GraphQL;
  error.errors = outcome.errors;
  error.code = outcome.errors.empty() ? "GRAPHQL_SUBSCRIPTION_ERROR"
                                      : outcome.errors.front().code;
  error.message = outcome.errors.empty()
                      ? "generic container subscription returned no data"
                      : outcome.errors.front().message;
  error.terminal = true;
  return error;
}

bool hasChangedKey(const domains::GameModelContainerChange& change,
                   std::string_view key) {
  return std::find(change.changedKeys.begin(), change.changedKeys.end(), key) !=
         change.changedKeys.end();
}

}  // namespace

int main() try {
  const auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  e2e::requireFlag("CROWDY_E2E_WEBSOCKET");

  auto player = e2e::provisionPlayer(cfg, "graphql-ws");
  if (!player.game->config().webSocketTransport &&
      !graphql::curlWebSocketTransportAvailable()) {
    std::puts(
        "CROWDY_E2E_WEBSOCKET requested, but this client has no injected or "
        "default GraphQL WebSocket transport; skipping");
    return 77;
  }
  auto& admin = e2e::ownerGame(cfg);
  const std::string typeName = e2e::kitPrefix("E2eWsBox");

  E2E_SUBTEST("seed one container type for live metadata events");
  graphql::JVal type;
  type["typeName"] = typeName;
  type["displayName"] = "E2E WebSocket box";
  type["instantiableBy"] = "member";
  graphql::JVal property;
  property["containerTypeName"] = typeName;
  property["key"] = "value";
  property["valueType"] = "int";
  property["defaultValueJson"] = "0";
  graphql::JVal seed;
  seed["appId"] = cfg.appId;
  seed["containerTypes"] = graphql::JVal::array({type});
  seed["propertyDefinitions"] = graphql::JVal::array({property});
  admin.gameModel().seed(seed);

  graphql::JVal create;
  create["appId"] = cfg.appId;
  create["typeName"] = typeName;
  create["displayName"] = "graphql-ws-" + e2e::runSuffix();
  const auto container = player.game->gameModel().createContainer(create);
  const std::string containerId = container["containerId"].asString();
  E2E_CHECK(!containerId.empty());

  session::ContainerMirror mirror(*player.game, cfg.appId);
  E2E_CHECK(mirror.watch(containerId).value["value"].asInt64() == 0);

  bool genericSeen = false;
  bool typedSeen = false;
  std::optional<graphql::GraphQLSubscriptionError> genericFailure;
  std::optional<graphql::GraphQLSubscriptionError> typedFailure;

  E2E_SUBTEST("subscribe through generic and typed WebSocket clients");
  graphql::JVal variables;
  variables["appId"] = cfg.appId;
  variables["typeName"] = typeName;
  graphql::GraphQLSubscriptionCallbacks genericCallbacks;
  genericCallbacks.onNext =
      [&](graphql::GraphQLSubscriptionOutcome outcome) {
        if (!outcome.ok()) {
          genericFailure = outcomeError(outcome);
          e2e::printSubscriptionError(*genericFailure,
                                      "generic WebSocket payload");
          return;
        }
        const auto change = outcome.data["gameModelContainerChanged"];
        if (change["containerId"].asString() != containerId) return;
        bool valueChanged = false;
        change["changedKeys"].forEach([&](graphql::Json key) {
          valueChanged = valueChanged || key.asString() == "value";
        });
        genericSeen = valueChanged;
      };
  genericCallbacks.onError =
      [&](graphql::GraphQLSubscriptionError error) {
        e2e::printSubscriptionError(error, "generic WebSocket transport");
        genericFailure = std::move(error);
      };
  auto generic = player.game->subscriptions().subscribe(
      "subscription E2eContainerFeed($appId: BigInt!, $typeName: String) {"
      " gameModelContainerChanged(appId: $appId, typeName: $typeName) {"
      "  appId containerId typeName sessionId source functionName changedKeys"
      "  occurredAt"
      " }"
      "}",
      variables, "E2eContainerFeed", std::move(genericCallbacks));

  domains::GameModelContainerChangedCallbacks typedCallbacks;
  typedCallbacks.next = [&](domains::GameModelContainerChange change) {
    if (change.containerId != containerId ||
        !hasChangedKey(change, "value")) {
      return;
    }
    typedSeen = true;
    mirror.refresh(change.containerId);
  };
  typedCallbacks.error = [&](graphql::GraphQLSubscriptionError error) {
    e2e::printSubscriptionError(error, "typed container WebSocket transport");
    typedFailure = std::move(error);
  };
  auto typed = player.game->gameModel().containerChanged(
      cfg.appId, typeName, {}, std::move(typedCallbacks));
  E2E_CHECK(generic.active());
  E2E_CHECK(typed.active());

  E2E_SUBTEST("mutations reach both feeds and drive mirror pull");
  for (int attempt = 1;
       attempt <= 20 && (!genericSeen || !typedSeen) && !genericFailure &&
           !typedFailure;
       ++attempt) {
    graphql::JVal set;
    set["appId"] = cfg.appId;
    set["containerId"] = containerId;
    set["key"] = "value";
    set["valueType"] = "int";
    set["valueJson"] = std::to_string(attempt);
    player.game->gameModel().setProperty(set);

    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < until &&
           (!genericSeen || !typedSeen) && !genericFailure && !typedFailure) {
      player.game->poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  player.game->poll();
  E2E_CHECK(!genericFailure);
  E2E_CHECK(!typedFailure);
  E2E_CHECK(genericSeen);
  E2E_CHECK(typedSeen);
  E2E_CHECK(mirror.get(containerId) != nullptr);
  E2E_CHECK(mirror.get(containerId)->value["value"].asInt64() > 0);

  generic.cancel();
  typed.cancel();
  player.game->poll();
  E2E_CHECK(!generic.active());
  E2E_CHECK(!typed.active());
  std::puts("e2e_graphql_websocket passed");
  return 0;
} catch (const graphql::CrowdyGraphQLError& error) {
  e2e::printGraphQLError(error, "GraphQL WebSocket e2e request");
  return 1;
} catch (const graphql::CrowdyError& error) {
  std::fprintf(stderr, "SDK error: code=%s message=%s\n",
               error.code().c_str(), error.what());
  return 1;
} catch (const std::exception& error) {
  std::fprintf(stderr, "exception: %s\n", error.what());
  return 1;
}
