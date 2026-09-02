// Mirrors Management API e2e: operator platform policy (READ-ONLY). Gated on
// CROWDY_E2E_OPERATOR_EMAIL (an is_operator account); skips (77) otherwise.
//
// As of the unified galaxy API (v13) the operator surface is reduced to the
// platform-wide compute ceilings — dedicated environments, change orders,
// secrets, releases, and audit moved to the separate infra-control-plane
// service. This exercises the surviving read only. NO mutations.
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

int runAll() {
  auto cfg = e2e::requireConfig();
  if (cfg.operatorEmail.empty()) {
    std::puts("CROWDY_E2E_OPERATOR_EMAIL not configured; skipping");
    return 77;
  }
  auto op = e2e::identityClient(cfg, cfg.operatorEmail);

  E2E_SUBTEST("computePlatformCeilings");
  graphql::Json ceilings = op->operator_().computePlatformCeilings();
  // The row exists (individual knobs may be null when there is no operator
  // override, so only assert the object shape).
  E2E_CHECK(ceilings.isObject());

  std::puts("e2e_operator OK");
  return 0;
}

}  // namespace

int main() {
  try {
    return runAll();
  } catch (const graphql::CrowdyError& e) {
    std::fprintf(stderr, "FATAL [%s]: %s\n", e.code().c_str(), e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FATAL: %s\n", e.what());
    return 1;
  }
}
