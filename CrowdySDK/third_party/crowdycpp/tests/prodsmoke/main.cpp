// Read-only smoke test of the 0.20.0 connection machinery against a live tier.
//
// Everything here is a GET-shaped query with no credential: appDiscovery takes no
// token by design, because a cold client has to ask where its app lives before it
// has anything to authenticate with.
//
//   ./prod_smoke https://ck.prod.cp.cks-env.com 77330192913920
//
// What it proves that a unit test cannot: that the estate rule accepts the real
// pair of hostnames this tier uses. A guard that refused
// ck.prod.cp.cks-env.com -> ck-or.prod.cp.cks-env.com would pass every fixture I
// wrote and then decline every redirect in production.
#include <cstdio>
#include <string>
#include <vector>

#include <crowdy/crowdy.hpp>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
  if (!ok) ++failures;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string origin =
      argc > 1 ? argv[1] : "https://ck.prod.cp.cks-env.com";
  const std::string appId = argc > 2 ? argv[2] : "77330192913920";

  std::printf("shared origin: %s\napp: %s\n\n", origin.c_str(), appId.c_str());

  crowdy::ClientConfig config;
  config.httpUrl = origin;
  // The shared origin is also the way back, which is the whole point of it.
  config.discoveryUrl = origin;
  crowdy::CrowdyClient client(std::move(config));

  std::printf("1. appDiscovery against the shared origin (no token)\n");
  std::vector<crowdy::domains::AppEndpoint> placements;
#ifndef CROWDY_NO_EXCEPTIONS
  try {
    placements = client.discovery().apps({appId});
  } catch (const crowdy::graphql::CrowdyError& error) {
    std::printf("  FAIL  threw %s: %s\n", error.code().c_str(), error.what());
    return 1;
  }
#else
  placements = client.discovery().apps({appId});
#endif
  check(placements.size() == 1, "exactly one placement returned");
  if (placements.empty()) return 1;
  const auto& placement = placements.front();
  std::printf("        appId=%s datacenter=%s\n        gameApiUrl=%s\n        gameApiWsUrl=%s\n",
              placement.appId.c_str(), placement.datacenterCode.c_str(),
              placement.gameApiUrl.c_str(), placement.gameApiWsUrl.c_str());
  check(placement.appId == appId, "appId round-trips as a decimal string");
  check(placement.placed(), "placed() is true for a placed app");
  check(!placement.datacenterCode.empty(), "datacenterCode is populated");

  std::printf("\n2. estate rule accepts this tier's real hostnames\n");
  const bool sameEstate =
      crowdy::graphql::isSameEstate(origin, placement.gameApiUrl);
  check(sameEstate,
        "shared origin -> per-datacenter origin is within one estate");
  check(!crowdy::graphql::isSameEstate(origin, "https://evil.example.com"),
        "an unrelated origin is still refused");

  std::printf("\n3. move every transport to the app's own datacenter\n");
  const std::string before = client.graphqlClient().endpoint();
  const bool moved =
      client.moveToDatacenter(placement.gameApiUrl, placement.gameApiWsUrl);
  check(moved, "moveToDatacenter accepted the discovered endpoint");
  const std::string after = client.graphqlClient().endpoint();
  std::printf("        %s\n     -> %s\n", before.c_str(), after.c_str());
  check(after != before, "the GraphQL endpoint actually changed");
  check(after.rfind(placement.gameApiUrl, 0) == 0,
        "the new endpoint is under the discovered origin");
  check(!client.moveToDatacenter(placement.gameApiUrl, placement.gameApiWsUrl),
        "moving to the current endpoint reports no move");

  std::printf("\n4. the moved client still works (per-datacenter origin serves)\n");
#ifndef CROWDY_NO_EXCEPTIONS
  try {
    const auto again = client.discovery().apps({appId});
    check(again.size() == 1, "appDiscovery answers from the datacenter origin");
    check(!again.empty() && again.front().datacenterCode ==
                                placement.datacenterCode,
          "both origins agree on where the app lives");
  } catch (const crowdy::graphql::CrowdyError& error) {
    std::printf("  FAIL  threw %s: %s\n", error.code().c_str(), error.what());
    ++failures;
  }
#endif

  std::printf("\n5. an unplaced app is a legitimate answer, not an error\n");
#ifndef CROWDY_NO_EXCEPTIONS
  try {
    const auto missing = client.discovery().app("1");
    check(!missing.placed(), "an unknown app reports placed() == false");
  } catch (const crowdy::graphql::CrowdyError& error) {
    std::printf("  note  server rejected the probe id: %s (%s)\n",
                error.code().c_str(), error.what());
  }
#endif

  std::printf("\n6. a real server error is not mistaken for a redirect\n");
#ifndef CROWDY_NO_EXCEPTIONS
  // gameClientBootstrap needs a bearer token, so this is a genuine
  // UNAUTHENTICATED from the live server rather than a fixture. It must arrive as
  // a plain CrowdyGraphQLError: not CrowdyAppUnavailableError, and not something
  // the redirect path tried to follow. It also has no `retryable` extension,
  // which is the case that must default to true rather than to false.
  const std::string bootstrap =
      "query { gameClientBootstrap(appId: \"" + appId +
      "\") { gameApiUrl gameApiWsUrl discoveryUrl } }";
  const std::string endpointBefore = client.graphqlClient().endpoint();
  bool sawPlainError = false;
  try {
    (void)client.graphqlClient().request(bootstrap);
    std::printf("  note  it answered without a token; skipping\n");
  } catch (const crowdy::graphql::CrowdyAppUnavailableError& error) {
    std::printf("  FAIL  typed as APP_UNAVAILABLE: %s\n", error.what());
    ++failures;
  } catch (const crowdy::graphql::CrowdyGraphQLError& error) {
    sawPlainError = true;
    std::printf("        code=%s\n", error.code().c_str());
    check(error.code() == "UNAUTHENTICATED",
          "surfaced with the server's own code");
    check(!error.errors().empty() && error.errors().front().retryable,
          "retryable defaults to true when the server omits it");
    check(!error.errors().empty() && error.errors().front().gameApiUrl.empty(),
          "no move target was invented");
  }
  check(sawPlainError, "the unauthenticated call was refused as expected");
  check(client.graphqlClient().endpoint() == endpointBefore,
        "the endpoint did not move on an unrelated error");
#endif

  std::printf("\n%s (%d failure(s))\n", failures == 0 ? "PROD SMOKE OK" : "PROD SMOKE FAILED",
              failures);
  return failures == 0 ? 0 : 1;
}
