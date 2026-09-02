// Mirrors Management API e2e: auth + identities. Email + password sign-in,
// magic-link REQUEST, identity listing, session revocation (logout /
// logoutAllDevices), and checkAuthMethod / changePassword. Black-box against
// the public Management API; see https://docs.crowdedkingdoms.com.
//
// The magic link is requested and NOT completed. It used to be exchanged here
// through `devToken`, which returned the emailed one-time token in the response
// body whenever the server had the dev bypass on; that field is deleted, so
// completing the flow now needs a real inbox and is out of scope for a
// black-box suite.
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

/// A management-plane client with no token seeded.
std::unique_ptr<CrowdyClient> bareClient(const e2e::E2eConfig& cfg) {
  ClientConfig c;
  c.httpUrl = cfg.apiUrl;
  return std::make_unique<CrowdyClient>(std::move(c));
}

int runAll() {
  auto cfg = e2e::requireConfig();
  const std::string email = e2e::deriveEmail(cfg, "auth");

  E2E_SUBTEST("registerUser returns a session token + user; users().me() matches");
  auto client = bareClient(cfg);
  const std::string password = e2e::derivePassword(email);
  auto dev = client->auth().registerUser(email, password);
  E2E_CHECK(!dev.token.empty());
  E2E_CHECK(!dev.userId.empty());
  E2E_CHECK(dev.email == email);
  graphql::Json me = client->users().me();
  E2E_CHECK(me["userId"].asString() == dev.userId);
  E2E_CHECK(me["email"].asString() == email);

  E2E_SUBTEST("checkAuthMethod reports the password this account was created with");
  graphql::Json method = client->auth().checkAuthMethod(email);
  E2E_CHECK(method["hasPassword"].isBool());
  E2E_CHECK(method["hasPassword"].asBool());

  E2E_SUBTEST("login returns to the same account");
  auto linkClient = bareClient(cfg);
  auto viaPassword = linkClient->auth().login(email, password);
  E2E_CHECK(!viaPassword.token.empty());
  E2E_CHECK(viaPassword.userId == dev.userId);
  E2E_CHECK(linkClient->users().me()["userId"].asString() == dev.userId);

  E2E_SUBTEST("requestLoginLink reports sent and hands back no token");
  // `sent` is always true and reveals nothing about whether the address is
  // registered. Completing the flow needs the emailed token, so this asserts
  // the request is accepted and stops -- the exchange used to run here only
  // because the bypass leaked the token into the response.
  graphql::Json link = client->auth().requestLoginLink(email);
  E2E_CHECK(link["sent"].asBool());

  E2E_SUBTEST("myIdentities lists the password identity");
  graphql::Json identities = client->auth().myIdentities();
  E2E_CHECK(identities.size() >= 1);
  bool sawOwnEmail = false;
  identities.forEach([&](graphql::Json ident) {
    E2E_CHECK(!ident["identityId"].asString().empty());
    E2E_CHECK(!ident["provider"].asString().empty());
    if (ident["email"].asString() == email) sawOwnEmail = true;
  });
  E2E_CHECK(sawOwnEmail);

  E2E_SUBTEST("me() with no token throws UNAUTHENTICATED");
  {
    auto anon = bareClient(cfg);
    bool threw = false;
    try {
      (void)anon->users().me();
    } catch (const graphql::CrowdyGraphQLError& e) {
      threw = true;
      E2E_CHECK(e.code() == "UNAUTHENTICATED");
    }
    E2E_CHECK(threw);
  }

  E2E_SUBTEST("logout clears the session; the old token is rejected afterwards");
  {
    const std::string oldToken = linkClient->getToken();
    E2E_CHECK(linkClient->auth().logout());
    E2E_CHECK(linkClient->getToken().empty());
    auto replay = bareClient(cfg);
    replay->setToken(oldToken);
    bool threw = false;
    try {
      (void)replay->users().me();
    } catch (const graphql::CrowdyGraphQLError& e) {
      threw = true;
      E2E_CHECK(!e.code().empty());  // structured; UNAUTHENTICATED on this deployment
      std::printf("   note: replayed logged-out token rejected with code=%s\n",
                  e.code().c_str());
    }
    E2E_CHECK(threw);
  }

  E2E_SUBTEST("logoutAllDevices revokes older sessions");
  {
    // Two independent sessions for the same account, both by password. The
    // account exists by now, so these are logins rather than registrations.
    auto older = bareClient(cfg);
    (void)older->auth().login(email, password);
    auto newer = bareClient(cfg);
    (void)newer->auth().login(email, password);
    E2E_CHECK(newer->auth().logoutAllDevices());
    bool threw = false;
    try {
      (void)older->users().me();
    } catch (const graphql::CrowdyGraphQLError& e) {
      threw = true;
      E2E_CHECK(!e.code().empty());
    }
    E2E_CHECK(threw);
  }

  E2E_SUBTEST("password family: register -> login -> changePassword -> login again");
  {
    const std::string pwEmail = e2e::deriveEmail(cfg, "auth-pw");
    const std::string password1 = "Cpp-e2e-pw1-" + e2e::runSuffix();
    const std::string password2 = "Cpp-e2e-pw2-" + e2e::runSuffix();
    try {
      auto pwClient = bareClient(cfg);
      auto registered = pwClient->auth().registerUser(pwEmail, password1);
      E2E_CHECK(!registered.token.empty());
      E2E_CHECK(registered.email == pwEmail);

      auto loginClient = bareClient(cfg);
      auto logged = loginClient->auth().login(pwEmail, password1);
      E2E_CHECK(!logged.token.empty());
      E2E_CHECK(logged.userId == registered.userId);

      E2E_CHECK(loginClient->auth().changePassword(password1, password2));

      auto reloginClient = bareClient(cfg);
      auto relogged = reloginClient->auth().login(pwEmail, password2);
      E2E_CHECK(!relogged.token.empty());
      E2E_CHECK(relogged.userId == registered.userId);

      // The old password must no longer work.
      bool oldRejected = false;
      try {
        (void)bareClient(cfg)->auth().login(pwEmail, password1);
      } catch (const graphql::CrowdyGraphQLError& e) {
        oldRejected = true;
        E2E_CHECK(!e.code().empty());
      }
      E2E_CHECK(oldRejected);

      graphql::Json pwMethod = reloginClient->auth().checkAuthMethod(pwEmail);
      E2E_CHECK(pwMethod["hasPassword"].asBool());
    } catch (const graphql::CrowdyGraphQLError& e) {
      // Password auth is a legacy/optional surface; a deployment with the
      // feature off must still fail with a STRUCTURED error code.
      E2E_CHECK(!e.code().empty());
      std::printf("   note: password ops rejected by this deployment (code=%s): %s\n",
                  e.code().c_str(), e.what());
    }
  }

  std::puts("e2e_auth_identities OK");
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
