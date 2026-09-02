// Mirrors Management API e2e: cross-tenant isolation. An OUTSIDER account
// (fresh, no org membership) must be denied on every owner-org admin surface
// with a structured FORBIDDEN-ish code, no mutation may take effect, and the
// public surface (platform config, marketplace) must still work. Account-level
// isolation only; org-token scope needs credentials this suite does not mint.
// See https://docs.crowdedkingdoms.com for the permission model.
#include <functional>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

std::string bigIntStr(const graphql::Json& v) {
  if (v.isString()) return v.asString();
  return std::to_string(v.asInt64());
}

bool isForbiddenIsh(const std::string& code) {
  return code == "FORBIDDEN" || code == "UNAUTHENTICATED" || code == "SCOPE_MISSING";
}

/// Run `fn` and require a structured FORBIDDEN-ish denial.
void expectDenied(const char* what, const std::function<void()>& fn) {
  try {
    fn();
  } catch (const graphql::CrowdyGraphQLError& e) {
    if (!isForbiddenIsh(e.code())) {
      std::fprintf(stderr, "%s: expected FORBIDDEN-ish, got [%s] %s\n", what,
                   e.code().c_str(), e.what());
      std::exit(1);
    }
    std::printf("   denied as expected: %s (code=%s)\n", what, e.code().c_str());
    return;
  }
  std::fprintf(stderr, "%s: expected a denial but the call succeeded\n", what);
  std::exit(1);
}

int runAll() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto& own = e2e::owner(cfg);

  graphql::Json e2eApp = own.admin().apps().get(cfg.appId);
  const std::string ownerOrgId = bigIntStr(e2eApp["orgId"]);
  E2E_CHECK(!ownerOrgId.empty());

  std::string outsiderId;
  auto outsider =
      e2e::identityClient(cfg, e2e::deriveEmail(cfg, "outsider"), &outsiderId);
  // Sanity: the outsider really has no org membership.
  E2E_CHECK(outsider->admin().organizations().mine().size() == 0);

  E2E_SUBTEST("owner-org reads are denied");
  // Observed public contract: organization(id) is documented as "requires a
  // valid session token" — the basic org profile is readable by ANY signed-in
  // account (it backs marketplace org pages). Assert that documented shape
  // here; the privileged reads below must still be denied.
  graphql::Json orgProfile = outsider->admin().organizations().get(ownerOrgId);
  E2E_CHECK(bigIntStr(orgProfile["orgId"]) == ownerOrgId);
  E2E_CHECK(!orgProfile["slug"].asString().empty());
  std::puts("   note: organization(id) is public-by-session per its schema contract");
  expectDenied("organizations().members(ownerOrgId)", [&] {
    (void)outsider->admin().organizations().members(ownerOrgId);
  });
  expectDenied("billing().walletBalance(ownerOrgId)", [&] {
    (void)outsider->admin().billing().walletBalance(ownerOrgId);
  });
  expectDenied("billing().appBudgets(ownerOrgId)", [&] {
    (void)outsider->admin().billing().appBudgets(ownerOrgId);
  });

  E2E_SUBTEST("owner-app mutations are denied and take no effect");
  const std::string rogueTierName = "cpp-e2e-rogue-tier-" + e2e::runSuffix();
  expectDenied("appAccess().createTier on the owner's app", [&] {
    graphql::JVal tierInput;
    tierInput["appId"] = cfg.appId;
    tierInput["name"] = rogueTierName;
    tierInput["isFree"] = true;
    (void)outsider->admin().appAccess().createTier(tierInput);
  });
  expectDenied("appAccess().grant on the owner's app", [&] {
    graphql::JVal grantInput;
    grantInput["appId"] = cfg.appId;
    grantInput["userId"] = outsiderId;
    (void)outsider->admin().appAccess().grant(grantInput);
  });

  // Verify through the owner that the tier mutation did not land.
  graphql::Json tiers = own.admin().appAccess().tiers(cfg.appId);
  tiers.forEach([&](graphql::Json t) {
    E2E_CHECK(t["name"].asString() != rogueTierName);
  });
  // Observed platform behavior: fresh accounts receive a SYSTEM auto-grant on
  // the open/free e2e app (grantedBy="system"), so the outsider may
  // legitimately hold a row here. The denied grantAppAccess would have written
  // grantedBy=<outsiderId>; assert no such self-granted row exists.
  graphql::Json outsiderAccess = outsider->admin().appAccess().myAccess(cfg.appId);
  if (outsiderAccess.isNull()) {
    std::puts("   note: outsider has no access row on the e2e app");
  } else {
    E2E_CHECK(outsiderAccess["grantedBy"].asString() != outsiderId);
    std::printf("   note: outsider holds a %s auto-grant (grantedBy=%s), not the denied grant\n",
                outsiderAccess["status"].asString().c_str(),
                outsiderAccess["grantedBy"].asString().c_str());
  }

  // Org-token scope isolation is intentionally NOT exercised here: minting an
  // org API token requires org admin credentials this suite does not have.
  // Account-level isolation is the contract under test.

  E2E_SUBTEST("public surface still works for the outsider");
  graphql::Json platformCfg = outsider->platform().config();
  E2E_CHECK(platformCfg.ok());
  E2E_CHECK(platformCfg["freeAppsPerOrg"].asInt64() >= 0);
  graphql::Json marketplace = outsider->admin().apps().marketplace();
  E2E_CHECK(marketplace["items"].isArray());
  E2E_CHECK(marketplace["pageInfo"]["totalCount"].asInt64() >= 0);

  std::puts("e2e_cross_tenant OK");
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
