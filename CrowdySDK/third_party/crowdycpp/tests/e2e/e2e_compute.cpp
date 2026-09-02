// Compute Modules: the owner drives the full compute surface — create a
// module, deploy the starter Rust source, wait for the on-instance compile,
// bind tick + invoke triggers, enable, synchronously invoke an export, read
// the monitoring surface, and delete. Modules are server-only; the module is
// alwaysOn because a playerless test app never activates lazily, and healthy
// ticks aggregate into per-minute usage (only loads/failures write run rows).
// Mirrors CrowdyJS e2e: compute-module.
// Reference: https://docs.crowdedkingdoms.com/game-api/compute-modules
#include <chrono>
#include <thread>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

constexpr const char* kCargoToml = R"toml([package]
name = "my-module"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[dependencies]
crowdy-compute-sdk = "0.1.0"
)toml";

constexpr const char* kLibRs = R"rs(use std::sync::atomic::{AtomicU64, Ordering};
static TICKS: AtomicU64 = AtomicU64::new(0);

fn on_init() {
    crowdy_compute_sdk::log(1, "module init");
}

fn on_tick(_dt_ms: u32) {
    let n = TICKS.fetch_add(1, Ordering::Relaxed) + 1;
    crowdy_compute_sdk::state_set(&n.to_le_bytes());
}

fn on_invoke(input: &[u8]) -> Vec<u8> {
    input.to_vec()
}

crowdy_compute_sdk::register_module!(init: on_init, tick: on_tick, invoke: on_invoke);
)rs";

int run() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto& og = e2e::ownerGame(cfg);
  auto& compute = og.compute();

  const std::string moduleName = "e2e-counter-" + e2e::runSuffix();

  E2E_SUBTEST("author: upsert module (starts disabled, alwaysOn)");
  {
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["name"] = moduleName;
    input["description"] = "e2e counter";
    input["alwaysOn"] = true;
    graphql::Json mod = compute.upsertModule(input);
    E2E_CHECK(mod["name"].asString() == moduleName);
    E2E_CHECK(mod["enabled"].asBool() == false);
    E2E_CHECK(mod["circuitState"].asString() == "closed");
  }

  E2E_SUBTEST("deploy the starter source and wait for the compile");
  {
    graphql::JVal sourceFiles;
    sourceFiles["Cargo.toml"] = kCargoToml;
    sourceFiles["src/lib.rs"] = kLibRs;
    graphql::Json version = compute.deploySource(cfg.appId, moduleName, sourceFiles);
    E2E_CHECK(version["versionNo"].asInt64() == 1);

    graphql::Json compiled =
        compute.waitForCompile(cfg.appId, moduleName, std::chrono::seconds(180));
    E2E_CHECK(compiled["compileStatus"].asString() == "succeeded");
  }

  E2E_SUBTEST("bind tick + invoke triggers and enable");
  {
    graphql::JVal tick;
    tick["appId"] = cfg.appId;
    tick["moduleName"] = moduleName;
    tick["triggerType"] = "tick";
    tick["tickHz"] = 2.0;
    graphql::Json t1 = compute.upsertTrigger(tick);
    E2E_CHECK(t1["triggerType"].asString() == "tick");

    graphql::JVal inv;
    inv["appId"] = cfg.appId;
    inv["moduleName"] = moduleName;
    inv["triggerType"] = "invoke";
    inv["exportName"] = "echo";
    graphql::Json t2 = compute.upsertTrigger(inv);
    E2E_CHECK(t2["exportName"].asString() == "echo");

    graphql::Json triggers = compute.moduleTriggers(cfg.appId, moduleName);
    E2E_CHECK(triggers.size() == 2);

    graphql::Json enabled = compute.setModuleEnabled(cfg.appId, moduleName, true);
    E2E_CHECK(enabled["enabled"].asBool() == true);
    E2E_CHECK(enabled["circuitState"].asString() == "closed");
  }

  E2E_SUBTEST("synchronous invoke RPC (echo round-trip)");
  {
    graphql::Json result = compute.invoke(cfg.appId, moduleName, "echo", R"({"ping":42})");
    const std::string resultJson = result["resultJson"].asString();
    E2E_CHECK(!resultJson.empty());
    graphql::Json echoed = graphql::Json::parse(resultJson);
    E2E_CHECK(echoed["export"].asString() == "echo");
    E2E_CHECK(echoed["params"]["ping"].asInt64() == 42);
    E2E_CHECK(!echoed["callerUserId"].asString().empty());
    E2E_CHECK(std::strtoll(result["fuelUsed"].asString().c_str(), nullptr, 10) > 0);
  }

  E2E_SUBTEST("observe: init run, stats, guest log line, diagnostics, policy");
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    bool sawInit = false;
    while (!sawInit && std::chrono::steady_clock::now() < deadline) {
      graphql::JVal vars;
      vars["appId"] = cfg.appId;
      vars["moduleName"] = moduleName;
      compute.moduleRuns(vars).forEach([&](graphql::Json r) {
        if (r["triggerSource"].asString() == "init" && r["success"].asBool()) sawInit = true;
      });
      if (!sawInit) std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    E2E_CHECK(sawInit);

    graphql::Json stats = compute.moduleStats(cfg.appId);
    E2E_CHECK(stats["totalRuns"].asInt64() >= 1);
    bool inBreakdown = false;
    stats["byModule"].forEach([&](graphql::Json m) {
      if (m["moduleName"].asString() == moduleName) inBreakdown = true;
    });
    E2E_CHECK(inBreakdown);

    bool sawLog = false;
    compute.moduleLogs(cfg.appId, moduleName, 20).forEach([&](graphql::Json l) {
      if (l["message"].asString().find("module init") != std::string::npos) sawLog = true;
    });
    E2E_CHECK(sawLog);

    graphql::Json diag = compute.appDiagnostics(cfg.appId);
    E2E_CHECK(diag["moduleCount"].asInt64() >= 1);
    E2E_CHECK(diag["enabledModuleCount"].asInt64() >= 1);

    graphql::Json policy = compute.modulePolicy(cfg.appId);
    E2E_CHECK(policy["maxTickHz"].asInt64() >= 2);
  }

  E2E_SUBTEST("delete: cascade removes the module (run history retained)");
  {
    graphql::Json deleted = compute.deleteModule(cfg.appId, moduleName);
    E2E_CHECK(deleted.asBool() == true);
    bool stillThere = false;
    compute.modules(cfg.appId).forEach([&](graphql::Json m) {
      if (m["name"].asString() == moduleName) stillThere = true;
    });
    E2E_CHECK(!stillThere);
  }

  std::puts("e2e_compute OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
