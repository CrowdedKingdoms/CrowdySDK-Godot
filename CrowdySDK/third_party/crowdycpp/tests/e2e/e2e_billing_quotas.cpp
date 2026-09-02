// Mirrors Management API e2e: billing + quotas + usage. Owner-surface reads
// of the org wallet, per-app budget round-trip, billing tier catalogs, quota
// set/effective/list/delete round-trip, and the usage reporting queries.
// Budget + quota mutations run against a suite-created app (additive; never
// mutates the configured e2e app). See https://docs.crowdedkingdoms.com for
// the billing and quota surfaces.
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

std::string bigIntStr(const graphql::Json& v) {
  if (v.isString()) return v.asString();
  return std::to_string(v.asInt64());
}

/// ISO-8601 UTC timestamp `days` days in the past (for DateTime! params).
std::string isoDaysAgo(int days) {
  const auto t = std::chrono::system_clock::now() - std::chrono::hours(24 * days);
  const std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
  return buf;
}

int runAll() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto& own = e2e::owner(cfg);

  graphql::Json e2eApp = own.admin().apps().get(cfg.appId);
  const std::string orgId = bigIntStr(e2eApp["orgId"]);
  E2E_CHECK(!orgId.empty());

  E2E_SUBTEST("walletBalance + walletTransactions(+Connection)");
  graphql::Json wallet = own.admin().billing().walletBalance(orgId);
  E2E_CHECK(bigIntStr(wallet["orgId"]) == orgId);
  E2E_CHECK(!wallet["currency"].asString().empty());
  (void)wallet["balanceCents"];  // may be zero
  graphql::Json txns = own.admin().billing().walletTransactions(orgId, 10);
  E2E_CHECK(txns.isArray());
  graphql::Json txnConn = own.admin().billing().walletTransactionsConnection(orgId, 10);
  E2E_CHECK(txnConn["edges"].isArray());
  E2E_CHECK(txnConn["totalCount"].asInt64() >= 0);

  E2E_SUBTEST("create a suite app for budget/quota mutations");
  graphql::JVal appInput;
  appInput["orgId"] = orgId;
  appInput["name"] = "CrowdyCPP E2E Billing " + e2e::runSuffix();
  appInput["slug"] = "cpp-e2e-billing-" + e2e::runSuffix();
  appInput["description"] = "CrowdyCPP billing/quotas e2e app";
  graphql::Json suiteApp = own.admin().apps().create(appInput);
  const std::string suiteAppId = bigIntStr(suiteApp["appId"]);
  E2E_CHECK(!suiteAppId.empty() && suiteAppId != "0");

  E2E_SUBTEST("appBudgets + setAppBudget round-trip");
  graphql::Json budget = own.admin().billing().setAppBudget(orgId, suiteAppId, "250000");
  E2E_CHECK(bigIntStr(budget["appId"]) == suiteAppId);
  E2E_CHECK(bigIntStr(budget["monthlyLimitCents"]) == "250000");
  graphql::Json budgets = own.admin().billing().appBudgets(orgId);
  bool sawBudget = false;
  budgets.forEach([&](graphql::Json b) {
    if (bigIntStr(b["appId"]) == suiteAppId) {
      sawBudget = true;
      E2E_CHECK(bigIntStr(b["monthlyLimitCents"]) == "250000");
    }
  });
  E2E_CHECK(sawBudget);
  graphql::Json budgetOne = own.admin().billing().appBudget(orgId, suiteAppId);
  E2E_CHECK(bigIntStr(budgetOne["monthlyLimitCents"]) == "250000");

  // NOTE (unified galaxy API, v13): the per-environment capacity billing-tier
  // catalogs (buddy/graphql/postgres) were retired with dedicated customer
  // environments.

  E2E_SUBTEST("quotas: set -> effective -> forOrg/forApp -> delete");
  graphql::JVal quotaInput;
  quotaInput["appId"] = suiteAppId;
  quotaInput["metric"] = "api_requests";
  quotaInput["limitValue"] = "123456";
  quotaInput["period"] = "per_minute";
  quotaInput["actionOnExceed"] = "throttle";
  graphql::Json quota = own.admin().quotas().set(quotaInput);
  const std::string quotaId = bigIntStr(quota["quotaId"]);
  E2E_CHECK(!quotaId.empty() && quotaId != "0");
  E2E_CHECK(quota["metric"].asString() == "api_requests");
  E2E_CHECK(bigIntStr(quota["limitValue"]) == "123456");

  graphql::JVal effVars;
  effVars["metric"] = "api_requests";
  effVars["appId"] = suiteAppId;
  graphql::Json effective = own.admin().quotas().effective(effVars);
  E2E_CHECK(bigIntStr(effective["quotaId"]) == quotaId);
  E2E_CHECK(bigIntStr(effective["limitValue"]) == "123456");

  graphql::Json forApp = own.admin().quotas().forApp(suiteAppId);
  bool sawInApp = false;
  forApp.forEach([&](graphql::Json q) {
    if (bigIntStr(q["quotaId"]) == quotaId) sawInApp = true;
  });
  E2E_CHECK(sawInApp);
  // forOrg lists org-scoped rules; an app-scoped rule may or may not roll up,
  // so only assert the read returns a well-formed list.
  graphql::Json forOrg = own.admin().quotas().forOrg(orgId);
  E2E_CHECK(forOrg.isArray());

  E2E_CHECK(own.admin().quotas().remove(quotaId).asBool());
  graphql::Json afterDelete = own.admin().quotas().forApp(suiteAppId);
  afterDelete.forEach([&](graphql::Json q) {
    E2E_CHECK(bigIntStr(q["quotaId"]) != quotaId);
  });

  E2E_SUBTEST("usage reads: orgSummary / appSummary / playerPulse / orgProjection");
  graphql::Json orgSummary = own.admin().usage().orgSummary(orgId, isoDaysAgo(7));
  E2E_CHECK(bigIntStr(orgSummary["orgId"]) == orgId);
  E2E_CHECK(orgSummary["totalOps"].asInt64() >= 0);
  graphql::JVal appUsageVars;
  appUsageVars["orgId"] = orgId;
  appUsageVars["appId"] = cfg.appId;
  appUsageVars["since"] = isoDaysAgo(7);
  graphql::Json appSummary = own.admin().usage().appSummary(appUsageVars);
  E2E_CHECK(bigIntStr(appSummary["appId"]) == cfg.appId);
  E2E_CHECK(appSummary["topGraphqlOperations"].isArray());
  graphql::Json pulse = own.admin().usage().playerPulse(orgId);
  E2E_CHECK(pulse["orgLivePlayers"].asInt64() >= 0);
  E2E_CHECK(pulse["globalLivePlayers"].asInt64() >= 0);
  graphql::Json projection = own.admin().usage().orgProjection(orgId);
  E2E_CHECK(projection["daysElapsed"].asInt64() >= 0);
  E2E_CHECK(projection["apps"].isArray());

  // Tidy up: archive the suite app (its budget row is inert on an archived
  // app; there is no delete-budget operation on the public surface).
  E2E_CHECK(own.admin().apps().archive(suiteAppId)["status"].asString() == "ARCHIVED");

  std::puts("e2e_billing_quotas OK");
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
