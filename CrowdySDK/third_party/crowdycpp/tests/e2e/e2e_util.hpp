#pragma once

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/crowdy.hpp"
#include "crowdy/session/world_session.hpp"

/// Shared harness for the env-gated e2e suites. Everything is BLACK-BOX:
/// provisioning happens through the public API the way a real integrator does
/// it (owner sign-in -> access tier -> grantAppAccess) — no database access
/// anywhere.
///
/// Required environment:
///   CROWDY_E2E_API_URL          API base URL (shared entry origin).
///                               CROWDY_E2E_MANAGEMENT_URL is still read as a
///                               fallback: it names the same origin now, and
///                               ignoring it would make every suite exit 77 —
///                               a skip, which reads as a pass.
///   CROWDY_E2E_HTTP_URL         per-game API base URL (optional; falls back
///                               to the gameApiUrl minted with the app token)
///   CROWDY_E2E_EMAIL            base sign-in email; suites derive fresh
///                               accounts by plus-addressing it
///                               (user+<suite>-<n>@domain)
///   CROWDY_E2E_APP_ID           app id (decimal string)
///   CROWDY_E2E_OWNER_EMAIL      an account with manage_apps +
///                               manage_access_tiers on the app (entitles the
///                               derived players, deploys kit blueprints)
///
/// Optional:
///   CROWDY_E2E_EMAIL_2          fixed second email (legacy two-client mode)
///   CROWDY_E2E_APP_ID_2         a second app on the same deployment
///                               (cross-app isolation suites)
///   CROWDY_E2E_OPERATOR_EMAIL   an is_operator account (operator suite)
///   CROWDY_E2E_MULTI_SERVER=1   deployment runs 2+ replication servers
///   CROWDY_E2E_CLAIM_CHUNK_X/Y/Z
///                               reserved free chunk for SELF_CLAIM coverage
///   CROWDY_E2E_STUDIO_GRID_ID   owned grid for Studio draft coverage
///   CROWDY_E2E_AGENT=1          enable Agentic Studio public-API coverage
///   CROWDY_E2E_AGENT_PROJECT_ID saved project for BUILD-mode coverage
///   CROWDY_E2E_AGENT_PLAY=1     enable Play lease grant/revoke coverage
///   CROWDY_E2E_STUDIO_APPROVED_RESTORE_CAPABILITY=1
///                               assert that a separately injected durable
///                               synchronization + approval provider exists
///   CROWDY_E2E_AGENT_POLICY_KILL=1
///                               explicitly enable temporary operator kill
///   CROWDY_E2E_WEBSOCKET=1      enable optional live GraphQL-WS coverage
///   CROWDY_E2E_SLOW=1           enable slow suites (soak, TTL waits)
///
/// The suites register their own accounts, so the server needs no special
/// configuration; production-style
/// magic-link flows are covered by the auth suite itself. Tests exit 77
/// (ctest SKIP_RETURN_CODE) when their required variables are unset.
namespace e2e {

#define E2E_CHECK(cond)                                                            \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "E2E_CHECK failed: %s at %s:%d\n", #cond, __FILE__,     \
                   __LINE__);                                                      \
      std::exit(1);                                                                \
    }                                                                              \
  } while (0)

#define E2E_SUBTEST(name) std::printf("-- %s\n", name)

inline std::string envOr(const char* name, const char* fallback = "") {
  const char* v = std::getenv(name);
  return v ? v : fallback;
}

inline bool envFlag(const char* name) {
  const std::string v = envOr(name);
  return v == "1" || v == "true" || v == "yes";
}

/// Generate a canonical RFC 4122 version-4 UUID for APIs whose UUID inputs are
/// distinct from the Replication API's 32-character ActorUuid.
inline std::string clientInstanceUuid() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (auto& byte : bytes) {
    byte = static_cast<unsigned char>(random());
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

  constexpr char hex[] = "0123456789abcdef";
  std::string uuid;
  uuid.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      uuid.push_back('-');
    }
    uuid.push_back(hex[(bytes[index] >> 4U) & 0x0fU]);
    uuid.push_back(hex[bytes[index] & 0x0fU]);
  }
  return uuid;
}

inline void printGraphQLError(
    const crowdy::graphql::CrowdyGraphQLError& error,
    const char* context = "GraphQL error") {
  std::fprintf(stderr, "%s: %s\n", context, error.what());
  for (const auto& detail : error.errors()) {
    std::fprintf(stderr,
                 "  code=%s path=%s message=%s remediation=%s\n",
                 detail.code.c_str(), detail.path.c_str(),
                 detail.message.c_str(), detail.remediation.c_str());
  }
}

inline void printSubscriptionError(
    const crowdy::graphql::GraphQLSubscriptionError& error,
    const char* context = "GraphQL subscription error") {
  std::fprintf(stderr,
               "%s: code=%s kind=%d retryable=%s terminal=%s message=%s\n",
               context, error.code.c_str(), static_cast<int>(error.kind),
               error.retryable ? "true" : "false",
               error.terminal ? "true" : "false", error.message.c_str());
  for (const auto& detail : error.errors) {
    std::fprintf(stderr,
                 "  code=%s path=%s message=%s remediation=%s\n",
                 detail.code.c_str(), detail.path.c_str(),
                 detail.message.c_str(), detail.remediation.c_str());
  }
}

inline void printAgentError(const crowdy::agent::AgentError& error,
                            const char* context = "Agent error") {
  std::fprintf(
      stderr,
      "%s: code=%s retryable=%s message=%s remediation=%s field=%s scope=%s\n",
      context, error.code.c_str(), error.retryable ? "true" : "false",
      error.message.c_str(),
      error.remediation ? error.remediation->c_str() : "",
      error.field ? error.field->c_str() : "",
      error.requiredScope ? error.requiredScope->c_str() : "");
}

struct E2eConfig {
  /// The origin identity and administration run against — the shared entry
  /// name in a real deployment. One API, so this differs from httpUrl only in
  /// that httpUrl may name the app's OWN datacenter instance.
  std::string apiUrl;
  std::string httpUrl;
  std::string email;
  std::string email2;
  std::string appId;
  std::string appId2;
  std::string ownerEmail;
  std::string operatorEmail;
};

/// Load the config or skip the test (exit 77).
inline E2eConfig requireConfig(bool needSecondPlayer = false) {
  E2eConfig cfg;
  // CROWDY_E2E_MANAGEMENT_URL still works: it names the same origin now, and
  // dropping it would make every suite exit 77 on a harness that still sets it.
  // A skipped suite reports as a skip, not as a failure, so that rename would
  // have quietly stopped testing rather than told anyone.
  cfg.apiUrl = envOr("CROWDY_E2E_API_URL");
  if (cfg.apiUrl.empty()) cfg.apiUrl = envOr("CROWDY_E2E_MANAGEMENT_URL");
  cfg.httpUrl = envOr("CROWDY_E2E_HTTP_URL");
  cfg.email = envOr("CROWDY_E2E_EMAIL");
  cfg.email2 = envOr("CROWDY_E2E_EMAIL_2");
  cfg.appId = envOr("CROWDY_E2E_APP_ID");
  cfg.appId2 = envOr("CROWDY_E2E_APP_ID_2");
  cfg.ownerEmail = envOr("CROWDY_E2E_OWNER_EMAIL");
  cfg.operatorEmail = envOr("CROWDY_E2E_OPERATOR_EMAIL");
  if (cfg.apiUrl.empty() || cfg.email.empty() || cfg.appId.empty() ||
      (needSecondPlayer && cfg.email2.empty() && cfg.ownerEmail.empty())) {
    std::puts("CROWDY_E2E_* not configured; skipping");
    std::exit(77);
  }
  return cfg;
}

/// Skip (exit 77) unless the owner account is configured. Most suites need
/// it for self-provisioning.
inline void requireOwner(const E2eConfig& cfg) {
  if (cfg.ownerEmail.empty()) {
    std::puts("CROWDY_E2E_OWNER_EMAIL not configured; skipping");
    std::exit(77);
  }
}

inline void requireFlag(const char* flag) {
  if (!envFlag(flag)) {
    std::printf("%s not set; skipping\n", flag);
    std::exit(77);
  }
}

/// A per-process unique suffix so reruns on a shared app never collide
/// (fresh derived accounts, fresh kit typePrefixes, fresh org slugs).
inline const std::string& runSuffix() {
  static const std::string suffix = [] {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms % 100000000);
  }();
  return suffix;
}

/// Plus-address a derived account into the base email:
/// alice@x.com + "voxel-1" -> alice+voxel-1-<runSuffix>@x.com
/// The password for a derived e2e address.
///
/// These accounts are created by the suite with `registerUser`, one per run, so
/// the password only has to be reproducible within the run and satisfy the
/// server's 8-character floor. Derived from the address rather than stored.
inline std::string derivePassword(const std::string& email) {
  return "Aa1!e2e-" + email;
}

inline std::string deriveEmail(const E2eConfig& cfg, const std::string& tag) {
  const auto at = cfg.email.find('@');
  return cfg.email.substr(0, at) + "+" + tag + "-" + runSuffix() + cfg.email.substr(at);
}

/// A CamelCase-safe unique kit typePrefix for this run: "Ct" + digits is a
/// valid GraphQL type-name fragment.
inline std::string kitPrefix(const char* base) { return std::string(base) + runSuffix(); }

/// Run-suffixed names leave residue on the shared app (every kit run deploys
/// fresh automations, and apps cap automations — e.g. 100). Prune automations
/// from PREVIOUS runs: any name embedding a >=6-digit run marker at least an
/// hour older than this process's runSuffix (so concurrently running suites,
/// whose markers are seconds apart, are never touched). Call it as admin
/// before deploying automation-bearing blueprints; failures are non-fatal.
inline void pruneStaleAutomations(crowdy::CrowdyClient& adminGame, const std::string& appId) {
  constexpr long long kWrap = 100000000;         // runSuffix() = epochMs % 1e8
  constexpr long long kMinAgeMs = 60LL * 60000;  // 1 hour
  const long long now = std::strtoll(runSuffix().c_str(), nullptr, 10);
  try {
    adminGame.gameModel().automationsList(appId).forEach([&](crowdy::graphql::Json a) {
      const std::string name = a["name"].asString();
      // Longest digit run in the name is the candidate run marker.
      std::string best, cur;
      for (char c : name) {
        if (c >= '0' && c <= '9') {
          cur += c;
        } else {
          if (cur.size() > best.size()) best = cur;
          cur.clear();
        }
      }
      if (cur.size() > best.size()) best = cur;
      if (best.size() < 6) return;  // no run marker: not e2e residue
      const long long marker = std::strtoll(best.c_str(), nullptr, 10);
      const long long age = ((now - marker) % kWrap + kWrap) % kWrap;
      if (age < kMinAgeMs || age > kWrap - kMinAgeMs) return;  // current or clock-wrapped
      try {
        adminGame.gameModel().deleteAutomation(appId, name);
      } catch (const std::exception&) { /* another run may have pruned it already */
      }
    });
  } catch (const std::exception& e) {
    std::printf("(automation prune skipped: %s)\n", e.what());
  }
}

// ---------------------------------------------------------------------------
// Clients and players
// ---------------------------------------------------------------------------

/// One signed-in player with an app-scoped game client and (optionally) a
/// connected native-UDP replication session.
struct Player {
  std::unique_ptr<crowdy::CrowdyClient> identity;
  std::unique_ptr<crowdy::CrowdyClient> game;
  crowdy::domains::AppTokenResponse appToken;
  std::string userId;
  std::string email;
  std::shared_ptr<crowdy::replication::Connection> conn;

  crowdy::replication::TokenInfo tokenInfo() const {
    crowdy::replication::TokenInfo t;
    t.token = appToken.token;
    t.gameTokenId = appToken.gameTokenIdInt64().value_or(0);
    t.expiresAtEpochMs =
        crowdy::core::parseIso8601Millis(appToken.expiresAt.data(), appToken.expiresAt.size());
    return t;
  }
};

/// Sign in an identity client (session token, shared origin).
inline std::unique_ptr<crowdy::CrowdyClient> identityClient(const E2eConfig& cfg,
                                                            const std::string& email,
                                                            std::string* userId = nullptr) {
  crowdy::ClientConfig c;
  c.httpUrl = cfg.apiUrl;
  auto client = std::make_unique<crowdy::CrowdyClient>(std::move(c));
  // registerUser, not a bypass. The address carries a per-run suffix, which is
  // what makes this the CREATE case -- `register` returns a session only for an
  // address it has never seen; one that already exists gets the password
  // attached pending confirmation and no token.
  auto auth = client->auth().registerUser(email, derivePassword(email));
  E2E_CHECK(!auth.token.empty());
  if (userId) *userId = auth.userId;
  return client;
}

/// The owner's identity client (entitles players, administers the app).
/// Skips when no owner is configured.
inline crowdy::CrowdyClient& owner(const E2eConfig& cfg) {
  static std::unique_ptr<crowdy::CrowdyClient> cached;
  if (!cached) {
    requireOwner(cfg);
    cached = identityClient(cfg, cfg.ownerEmail);
  }
  return *cached;
}

/// The owner's per-game client (app token; used for kit deploys and
/// game-plane admin such as grid grants).
inline crowdy::CrowdyClient& ownerGame(const E2eConfig& cfg) {
  static std::unique_ptr<crowdy::CrowdyClient> cached;
  if (!cached) {
    auto minted = owner(cfg).portal().mintAppToken(cfg.appId);
    crowdy::ClientConfig c;
    c.httpUrl = !cfg.httpUrl.empty()
                    ? cfg.httpUrl
                    : (!minted.gameApiUrl.empty()
                           ? minted.gameApiUrl.valueOrEmpty()
                           : cfg.apiUrl);
    cached = std::make_unique<crowdy::CrowdyClient>(std::move(c));
    cached->setToken(minted.token);
  }
  return *cached;
}

/// Find-or-create the e2e access tier carrying every runtime permission.
/// Idempotent across runs (keyed by name, not suffix).
inline std::string ensureEntitledTier(const E2eConfig& cfg, const std::string& appId) {
  static constexpr const char* kTierName = "crowdycpp-e2e";
  auto& adminClient = owner(cfg);
  crowdy::graphql::Json tiers = adminClient.admin().appAccess().tiers(appId);
  std::string tierId;
  tiers.forEach([&](crowdy::graphql::Json t) {
    if (!tierId.empty()) return;
    if (t["name"].asString() == kTierName && t["permissionKeys"].size() >= 4)
      tierId = t["tierId"].asString();
  });
  if (!tierId.empty()) return tierId;

  crowdy::graphql::JVal input;
  input["appId"] = appId;
  input["name"] = kTierName;
  input["isFree"] = true;
  input["description"] = "CrowdyCPP e2e tier: all runtime permissions";
  input["permissionKeys"] = crowdy::graphql::JVal::array(
      {crowdy::graphql::JVal("access"), crowdy::graphql::JVal("teleport"),
       crowdy::graphql::JVal("update_voxel_data"), crowdy::graphql::JVal("use_voice_chat")});
  crowdy::graphql::Json created = adminClient.admin().appAccess().createTier(input);
  tierId = created["tierId"].asString();
  E2E_CHECK(!tierId.empty());
  return tierId;
}

/// Sign in `email`, entitle it on the e2e tier via the owner, mint the app
/// token, and build the per-game client. Fully black-box.
inline Player provisionPlayerEmail(const E2eConfig& cfg, const std::string& email,
                                   const std::string& appId) {
  Player p;
  p.email = email;
  p.identity = identityClient(cfg, email, &p.userId);

  if (!cfg.ownerEmail.empty()) {
    const std::string tierId = ensureEntitledTier(cfg, appId);
    crowdy::graphql::JVal grant;
    grant["appId"] = appId;
    grant["userId"] = p.userId;
    grant["tierId"] = tierId;
    owner(cfg).admin().appAccess().grant(grant);
  }

  p.appToken = p.identity->portal().mintAppToken(appId);
  E2E_CHECK(p.appToken.token.size() == 64);

  const std::string gameUrl =
      !cfg.httpUrl.empty() ? cfg.httpUrl
                           : (!p.appToken.gameApiUrl.empty()
                                  ? p.appToken.gameApiUrl.valueOrEmpty()
                                  : cfg.apiUrl);
  crowdy::ClientConfig gameCfg;
  gameCfg.httpUrl = gameUrl;
  // The shared origin the app token came back with: where re-discovery goes
  // when the instance in gameApiUrl stops answering.
  gameCfg.discoveryUrl = !p.appToken.discoveryUrl.empty()
                             ? p.appToken.discoveryUrl.valueOrEmpty()
                             : cfg.apiUrl;
  if (!p.appToken.gameApiWsUrl.empty()) {
    gameCfg.wsUrl = p.appToken.gameApiWsUrl.valueOrEmpty();
  }
  p.game = std::make_unique<crowdy::CrowdyClient>(std::move(gameCfg));
  p.game->setToken(p.appToken.token);
  return p;
}

/// Provision a fresh derived player for this suite run: `tag` names it
/// (e.g. "voxel-a"). Requires the owner for entitlement.
inline Player provisionPlayer(const E2eConfig& cfg, const std::string& tag) {
  requireOwner(cfg);
  return provisionPlayerEmail(cfg, deriveEmail(cfg, tag), cfg.appId);
}

/// Legacy fixed-account sign-in (kept for the original suites): uses the
/// pre-entitled CROWDY_E2E_EMAIL / _EMAIL_2 accounts when no owner is
/// configured, otherwise self-provisions them too.
inline Player signIn(const E2eConfig& cfg, const std::string& email) {
  return provisionPlayerEmail(cfg, email, cfg.appId);
}

/// Assign a replication server and open the native UDP connection
/// (owned-thread mode), waiting for session-ready.
inline void connectUdp(Player& p, const E2eConfig& cfg, const std::string& appId = {}) {
  crowdy::replication::Config rc;
  const std::string& app = appId.empty() ? cfg.appId : appId;
  rc.appId = std::strtoll(app.c_str(), nullptr, 10);
  rc.token = p.tokenInfo();
  p.conn = p.game->replication().connect(rc);
  // Server-status heartbeats can transiently lapse on small deployments;
  // retry assignment for up to ~30 s before declaring failure.
  bool connected = p.conn->connect().ok();
  for (int i = 0; i < 10 && !connected; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    connected = p.conn->connect().ok();
  }
  E2E_CHECK(connected);
  for (int i = 0; i < 100 && p.conn->state() != crowdy::replication::ConnState::Connected; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    p.conn->poll();
  }
  E2E_CHECK(p.conn->state() == crowdy::replication::ConnState::Connected);
  // Announce ourselves: the server replies to the socket address it hears
  // from, so a receive-only client must send at least one datagram (a cheap
  // presence heartbeat) before any notification can reach it.
  p.conn->sendHeartbeat({0, 0, 0}, crowdy::core::generateActorUuid());
}

/// Warm up the server-side permission window for `chunk`. The first spatial
/// messages after connect can be denied (UNAUTHORIZED) until the session's
/// grid-permission window loads — documented "first-chunk" behavior
/// (https://docs.crowdedkingdoms.com/replication-api/troubleshooting). Send
/// actor updates until one is acknowledged so later assertions observe steady
/// state. Returns true once acknowledged.
inline bool warmUp(crowdy::replication::Connection& conn, const crowdy::wire::ChunkCoord& chunk,
                   int budgetMs = 15000) {
  const auto uuid = crowdy::core::generateActorUuid();
  const std::uint8_t pose[] = {0};
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(budgetMs)) {
    auto outcome = conn.sendActorUpdateAndWait(
        {chunk, uuid, crowdy::Bytes(pose, sizeof(pose)), 8, crowdy::wire::DecayRate::None}, 1000);
    if (outcome.acknowledged) return true;
  }
  return false;
}

/// Poll until `done()` or the timeout elapses; returns done().
template <typename Fn>
inline bool pollUntil(crowdy::replication::Connection& conn, Fn&& done, int timeoutMs = 8000) {
  const auto start = std::chrono::steady_clock::now();
  while (!done()) {
    conn.poll();
    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs))
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

/// Repeatedly run `attempt()` (a send/refresh action) then poll both
/// connections until `done()`; UDP is best-effort so single sends may drop.
template <typename Attempt, typename Done>
inline bool retryUntil(Attempt&& attempt, Done&& done, int attempts = 40, int perWaitMs = 400) {
  for (int i = 0; i < attempts; ++i) {
    attempt();
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(perWaitMs)) {
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  return done();
}

}  // namespace e2e
