#include <concepts>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "crowdy/domains/marketplace.hpp"
#include "crowdy/domains/player_compute.hpp"
#include "crowdy/domains/player_model.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/domains/types.hpp"

namespace {

using crowdy::domains::AuthResponse;
using crowdy::domains::ClientArtifactBytes;
using crowdy::domains::MarketplaceAPI;
using crowdy::domains::PlayerComputeAPI;
using crowdy::domains::PlayerModelAPI;
using crowdy::domains::PortalAPI;
using crowdy::graphql::GraphQLCallback;
using crowdy::graphql::JVal;
using crowdy::graphql::Json;

static_assert(std::same_as<
              decltype(std::declval<AuthResponse>().gameTokenId),
              std::string>);
static_assert(std::same_as<
              decltype(std::declval<ClientArtifactBytes>().bytes),
              std::vector<std::uint8_t>>);
static_assert(std::same_as<
              decltype(std::declval<ClientArtifactBytes>().fuelPerDispatch),
              std::string>);
static_assert(std::same_as<
              decltype(std::declval<ClientArtifactBytes>().contractJson),
              std::optional<std::string>>);

template <typename API>
concept CompletePlayerModelTwins =
    requires(API& api, std::string_view id, const JVal& input,
             GraphQLCallback callback) {
      { api.containers(id, id) } -> std::same_as<Json>;
      { api.containersAsync(id, id, callback) } -> std::same_as<void>;
      { api.container(input) } -> std::same_as<Json>;
      { api.containerAsync(input, callback) } -> std::same_as<void>;
      { api.createContainer(input) } -> std::same_as<Json>;
      { api.createContainerAsync(input, callback) } -> std::same_as<void>;
      { api.setProperty(input) } -> std::same_as<Json>;
      { api.setPropertyAsync(input, callback) } -> std::same_as<void>;
      { api.deleteContainer(input) } -> std::same_as<Json>;
      { api.deleteContainerAsync(input, callback) } -> std::same_as<void>;
      { api.automations(id, id) } -> std::same_as<Json>;
      { api.automationsAsync(id, id, callback) } -> std::same_as<void>;
      { api.createAutomation(input) } -> std::same_as<Json>;
      { api.createAutomationAsync(input, callback) } -> std::same_as<void>;
      { api.setAutomationEnabled(input) } -> std::same_as<Json>;
      { api.setAutomationEnabledAsync(input, callback) } -> std::same_as<void>;
      { api.deleteAutomation(input) } -> std::same_as<Json>;
      { api.deleteAutomationAsync(input, callback) } -> std::same_as<void>;
    };

static_assert(CompletePlayerModelTwins<PlayerModelAPI>);

template <typename API>
concept PlayerComputeParity =
    requires(API& api, std::string_view id, GraphQLCallback callback,
             API::ArtifactBytesCallback artifactCallback) {
      // Four arguments proves the synchronous paramsJson default remains.
      { api.invoke(id, id, id, id) } -> std::same_as<Json>;
      { api.invokeAsync(id, id, id, id, "{}", callback) } ->
          std::same_as<void>;
      // Three arguments proves the optional version id remains omitted.
      { api.artifactBytes(id, id, id) } ->
          std::same_as<ClientArtifactBytes>;
      { api.artifactBytesAsync(id, id, id, artifactCallback) } ->
          std::same_as<void>;
    };

static_assert(PlayerComputeParity<PlayerComputeAPI>);

template <typename API>
concept MarketplaceArtifactParity =
    requires(API& api, const JVal& variables,
             API::ArtifactBytesCallback callback) {
      { api.clientArtifactBytes(variables) } ->
          std::same_as<ClientArtifactBytes>;
      { api.clientArtifactBytesAsync(variables, callback) } ->
          std::same_as<void>;
    };

static_assert(MarketplaceArtifactParity<MarketplaceAPI>);

using AuthorizeAppSignature = Json (PortalAPI::*)(
    std::string_view,
    std::optional<std::vector<std::string>>) const;
static_assert(std::same_as<
              decltype(static_cast<AuthorizeAppSignature>(
                  &PortalAPI::authorizeApp)),
              AuthorizeAppSignature>);
static_assert(requires(PortalAPI& api, std::string_view appId) {
  // One argument proves omitted scopes still use the server-side baseline.
  { api.authorizeApp(appId) } -> std::same_as<Json>;
});

}  // namespace

int main() {
  std::printf("public_api_parity_test passed\n");
  return 0;
}
