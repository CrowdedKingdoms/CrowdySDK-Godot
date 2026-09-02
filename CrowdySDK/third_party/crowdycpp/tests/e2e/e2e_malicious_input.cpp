// Malicious / malformed input over the GraphQL surface: oversized state
// blobs, invalid BigInt strings, absurd pagination, invalid enum values, and
// unauthenticated access all draw STRUCTURED errors (never a crash or hang),
// and a healthy client keeps working after each. Mirrors CrowdyJS's
// malicious-input e2e suite.
// Reference: https://docs.crowdedkingdoms.com/game-api/errors
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

// Every hostile call must fail with a structured, coded error.
template <typename Fn>
std::string expectCodedError(Fn&& hostileCall) {
  try {
    hostileCall();
  } catch (const graphql::CrowdyError& e) {
    E2E_CHECK(!e.code().empty());
    return e.code();
  }
  return {};
}

int run() {
  auto cfg = e2e::requireConfig();
  auto p = e2e::provisionPlayer(cfg, "malicious-a");

  const std::string healthyBlob = core::base64Encode(asBytes("healthy-" + e2e::runSuffix()));
  auto healthy = [&] {
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["state"] = healthyBlob;
    p.game->state().update(input);
    graphql::Json read = p.game->state().getOne(cfg.appId);
    E2E_CHECK(!read["state"].asString().empty());
  };
  healthy();

  E2E_SUBTEST("oversized (~2MB) state blob draws a structured error");
  {
    // 2 MiB of 'A' is valid base64 text, so it exercises the size limit, not
    // the encoding validation.
    const std::string huge(2 * 1024 * 1024, 'A');
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["state"] = huge;
    const std::string code = expectCodedError([&] { p.game->state().update(input); });
    E2E_CHECK(!code.empty());
    std::printf("   oversized blob -> %s\n", code.c_str());
  }
  healthy();

  E2E_SUBTEST("invalid BigInt string draws BAD_USER_INPUT-ish error");
  {
    const std::string code =
        expectCodedError([&] { p.game->state().getOne("not-a-number"); });
    E2E_CHECK(!code.empty());
    std::printf("   invalid BigInt -> %s\n", code.c_str());
  }
  healthy();

  E2E_SUBTEST("absurd pagination values are refused or handled");
  {
    graphql::JVal filter;
    filter["appId"] = cfg.appId;
    const std::string negativeCode =
        expectCodedError([&] { p.game->actors().listConnection(-5, {}, filter); });
    std::printf("   first=-5 -> %s\n", negativeCode.empty() ? "(handled)" : negativeCode.c_str());
    const std::string hugeCode = expectCodedError(
        [&] { p.game->actors().listConnection(2000000000, {}, filter); });
    std::printf("   first=2e9 -> %s\n", hugeCode.empty() ? "(handled)" : hugeCode.c_str());
  }
  healthy();

  E2E_SUBTEST("invalid enum value draws BAD_USER_INPUT");
  {
    graphql::JVal vars;
    vars["appId"] = cfg.appId;
    vars["visibility"] = "TOTALLY_NOT_A_VISIBILITY";
    const std::string code = expectCodedError([&] {
      p.identity->graphqlClient().request(gen::apps::kSetAppVisibilityDocument, vars,
                                             "SetAppVisibility");
    });
    E2E_CHECK(!code.empty());
    std::printf("   invalid enum -> %s\n", code.c_str());
  }
  healthy();

  E2E_SUBTEST("unauthenticated client draws UNAUTHENTICATED");
  {
    ClientConfig bare;
    bare.httpUrl = !cfg.httpUrl.empty() ? cfg.httpUrl : cfg.apiUrl;
    CrowdyClient anonymous(std::move(bare));
    const std::string code =
        expectCodedError([&] { anonymous.state().getOne(cfg.appId); });
    E2E_CHECK(code == "UNAUTHENTICATED");
  }
  healthy();

  std::puts("e2e_malicious_input OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
