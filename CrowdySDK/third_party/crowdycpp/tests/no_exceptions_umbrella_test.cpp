#include <crowdy/crowdy.hpp>

#include "test_util.hpp"

#ifndef CROWDY_NO_EXCEPTIONS
#error "no_exceptions_umbrella_test must compile with CROWDY_NO_EXCEPTIONS"
#endif

int main() {
  const auto endpoint = crowdy::graphql::normalizeGraphQLWebSocketUrl(
      "https://api.example.test");
  CHECK(endpoint.ok());
  CHECK_EQ(endpoint.value(), "wss://api.example.test/graphql");
  crowdy::studio::StudioLayoutController layout;
  layout.setVisible(crowdy::studio::StudioPaneId::Agent, true);
  CHECK(layout.getState().isVisible(
      crowdy::studio::StudioPaneId::Agent));
  std::puts("no_exceptions_umbrella_test OK");
  return 0;
}
