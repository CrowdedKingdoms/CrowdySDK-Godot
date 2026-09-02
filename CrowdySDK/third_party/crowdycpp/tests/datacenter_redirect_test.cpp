// Ports CrowdyJS test/unit/datacenter-redirect.test.mjs case for case.
//
// An app lives in ONE datacenter because Citus distributes on app_id, so a
// request answered elsewhere is not merely slower — it reads a different
// database. WRONG_DATACENTER moves the client silently; APP_UNAVAILABLE is a
// typed refusal with deliberately no endpoint. The distinction is the whole
// point: mistaking the second for the first produces a client that thinks it
// moved and did not.
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/graphql/datacenter_redirect.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::graphql;

namespace {

std::string wrongDatacenterBody() {
  return R"({"errors":[{"message":"App 42 is served from datacenter 'or', not 'va'.",)"
         R"("extensions":{"code":"WRONG_DATACENTER","appId":"42",)"
         R"("appDatacenter":"or","servedBy":"va",)"
         R"("gameApiUrl":"https://ck-or.prod.cp.cks-env.com",)"
         R"("gameApiWsUrl":"wss://ck-or.prod.cp.cks-env.com"}}]})";
}

std::string appUnavailableBody() {
  return R"({"errors":[{"message":"This app is temporarily offline.",)"
         R"("extensions":{"code":"APP_UNAVAILABLE","appId":"42",)"
         R"("appDatacenter":"or","servedBy":"va","retryable":true}}]})";
}

/// Replays a queue of responses and records every URL it was asked for.
class ScriptedTransport final : public IHttpTransport {
 public:
  std::vector<std::string> bodies;
  std::vector<std::string> urls;
  std::vector<std::string> authorizations;  // one per request; "" = no header
  std::size_t next = 0;
  /// Runs while the request is in flight, before its response is delivered.
  /// A test uses this to act as a CONCURRENT request whose redirect completes
  /// first - the only way to script that race deterministically.
  std::function<void(const HttpRequest&)> onSend;

  HttpResponse send(const HttpRequest& request) override {
    urls.push_back(request.url);
    std::string authorization;
    for (const auto& header : request.headers) {
      if (header.first == "Authorization") authorization = header.second;
    }
    authorizations.push_back(std::move(authorization));
    if (onSend) onSend(request);
    const std::string body =
        next < bodies.size() ? bodies[next] : bodies.back();
    ++next;
    return HttpResponse{200, body};
  }

#ifdef CROWDY_NO_EXCEPTIONS
  HttpOutcome sendOutcome(const HttpRequest& request) noexcept override {
    return {Errc::Ok, send(request), {}};
  }
#endif
};

std::shared_ptr<GraphQLClient> makeClient(
    const std::shared_ptr<IHttpTransport>& transport) {
  return std::make_shared<GraphQLClient>(
      GraphQLClientConfig{"https://ck-va.prod.cp.cks-env.com/graphql", 1000},
      transport, std::make_shared<AuthState>());
}

std::vector<GraphQLErrorDetail> errorsFrom(const std::string& body) {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {body};
  auto client = makeClient(transport);
  std::vector<GraphQLErrorDetail> captured;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    captured = out.errors;
  });
  return captured;
}

void testMoveFromErrorsReadsTheEndpoint() {
  const auto move = moveFromErrors(errorsFrom(wrongDatacenterBody()));
  CHECK(move.has_value());
  CHECK_EQ(move->gameApiUrl, "https://ck-or.prod.cp.cks-env.com");
  CHECK_EQ(move->gameApiWsUrl, "wss://ck-or.prod.cp.cks-env.com");
  CHECK_EQ(move->appId, "42");
  CHECK_EQ(move->appDatacenter, "or");
}

void testMoveFromErrorsScansPastALeadingUnrelatedError() {
  // A partially resolved query reports whatever failed first, so the routing
  // error is not guaranteed to lead. Reading only errors[0] would miss it.
  const std::string body =
      R"({"errors":[{"message":"nope","extensions":{"code":"FORBIDDEN"}},)"
      R"({"message":"wrong dc","extensions":{"code":"WRONG_DATACENTER",)"
      R"("appId":"42","appDatacenter":"or",)"
      R"("gameApiUrl":"https://ck-or.prod.cp.cks-env.com"}}]})";
  const auto move = moveFromErrors(errorsFrom(body));
  CHECK(move.has_value());
  CHECK_EQ(move->gameApiUrl, "https://ck-or.prod.cp.cks-env.com");
}

void testMoveFromErrorsRefusesOnAppUnavailable() {
  const auto errors = errorsFrom(appUnavailableBody());
  CHECK(!moveFromErrors(errors).has_value());
  CHECK(isAppUnavailable(errors));
}

void testMoveFromErrorsRefusesWrongDatacenterWithNoEndpoint() {
  const std::string body =
      R"({"errors":[{"message":"wrong dc","extensions":{)"
      R"("code":"WRONG_DATACENTER","appId":"42","appDatacenter":"or"}}]})";
  CHECK(!moveFromErrors(errorsFrom(body)).has_value());
}

void testTransportMovesAndRetriesOnceAndTheRetrySucceeds() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);

  int moves = 0;
  client->setWrongDatacenterHandler([&](const DatacenterMove& move) {
    ++moves;
    return client->setEndpoint(move.gameApiUrl + "/graphql");
  });

  Json data = client->request("query");
  CHECK_EQ(data["v"].asString(), "ok");
  CHECK_EQ(moves, 1);
  CHECK_EQ(transport->urls.size(), std::size_t{2});
  CHECK_EQ(transport->urls[0], "https://ck-va.prod.cp.cks-env.com/graphql");
  CHECK_EQ(transport->urls[1], "https://ck-or.prod.cp.cks-env.com/graphql");
}

void testTransportDoesNotRetryWhenTheHandlerDeclines() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);
  client->setWrongDatacenterHandler([](const DatacenterMove&) { return false; });

#ifndef CROWDY_NO_EXCEPTIONS
  bool threw = false;
  try {
    (void)client->request("query");
  } catch (const CrowdyGraphQLError& error) {
    threw = true;
    CHECK_EQ(error.code(), "WRONG_DATACENTER");
  }
  CHECK(threw);
#else
  CHECK(!client->request("query").ok());
#endif
  CHECK_EQ(transport->urls.size(), std::size_t{1});
}

// The SYNC path is bounded by construction — it re-issues exactly once, in
// straight-line code. The ASYNC path is bounded by a flag, and a flag can be
// wrong, so it needs its own case: script nothing but redirects and assert the
// handler is still called once. Removing the guard makes only this test fail,
// which is how it was found to be the one worth writing.
void testAsyncPathRetriesAtMostOnce() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody()};  // every response redirects
  auto client = makeClient(transport);

  int moves = 0;
  client->setWrongDatacenterHandler([&](const DatacenterMove& move) {
    ++moves;
    // A fresh URL each time, so setEndpoint keeps reporting a real move and
    // cannot be what stops the loop.
    return client->setEndpoint(move.gameApiUrl + "/graphql?attempt=" +
                               std::to_string(moves));
  });

  int calls = 0;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    ++calls;
    CHECK(!out.ok());
  });
  CHECK_EQ(calls, 1);
  CHECK_EQ(moves, 1);
  CHECK_EQ(transport->urls.size(), std::size_t{2});
}

void testTransportRetriesAtMostOnce() {
  // Two datacenters that disagree about an app would otherwise bounce one
  // query between them forever.
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), wrongDatacenterBody(),
                       R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);

  int moves = 0;
  client->setWrongDatacenterHandler([&](const DatacenterMove& move) {
    ++moves;
    return client->setEndpoint(move.gameApiUrl + "/graphql?attempt=" +
                               std::to_string(moves));
  });

#ifndef CROWDY_NO_EXCEPTIONS
  bool threw = false;
  try {
    (void)client->request("query");
  } catch (const CrowdyGraphQLError&) {
    threw = true;
  }
  CHECK(threw);
#else
  CHECK(!client->request("query").ok());
#endif
  CHECK_EQ(moves, 1);
  CHECK_EQ(transport->urls.size(), std::size_t{2});
}

#ifndef CROWDY_NO_EXCEPTIONS
void testHandlerThatThrowsSurfacesTheOriginalRefusal() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);
  client->setWrongDatacenterHandler([](const DatacenterMove&) -> bool {
    throw std::runtime_error("handler blew up");
  });

  bool threw = false;
  try {
    (void)client->request("query");
  } catch (const CrowdyGraphQLError& error) {
    threw = true;
    CHECK_EQ(error.code(), "WRONG_DATACENTER");
  }
  CHECK(threw);
  CHECK_EQ(transport->urls.size(), std::size_t{1});
}

// A handler can move this endpoint and THEN throw: the HTTP move succeeded and
// the WebSocket or session move did not. That torn state must surface the
// original refusal loudly, not read as "already moved, retry quietly" - a
// quiet success would hide a client querying one datacenter and playing in
// another.
void testHandlerThatMovesThenThrowsStillSurfacesTheOriginalRefusal() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);
  client->setWrongDatacenterHandler([&](const DatacenterMove& move) -> bool {
    client->setEndpoint(move.gameApiUrl + "/graphql");
    throw std::runtime_error("the socket move blew up");
  });

  bool threw = false;
  try {
    (void)client->request("query");
  } catch (const CrowdyGraphQLError& error) {
    threw = true;
    CHECK_EQ(error.code(), "WRONG_DATACENTER");
  }
  CHECK(threw);
  CHECK_EQ(transport->urls.size(), std::size_t{1});
}

void testAppUnavailableThrowsATypedErrorAHostCanShowAPlayer() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {appUnavailableBody()};
  auto client = makeClient(transport);

  bool threw = false;
  try {
    (void)client->request("query");
  } catch (const CrowdyAppUnavailableError& error) {
    threw = true;
    CHECK_EQ(error.code(), "APP_UNAVAILABLE");
    CHECK_EQ(error.appId(), "42");
    CHECK_EQ(error.appDatacenter(), "or");
    CHECK_EQ(error.servedBy(), "va");
    CHECK(error.retryable());
    // The server's own words: it knows why the app is down and the client
    // does not, so a host shows this rather than inventing generic text.
    CHECK_EQ(std::string(error.what()), "This app is temporarily offline.");
  }
  CHECK(threw);
}

void testRetryableIsFalseOnlyWhenTheServerSaysSo() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {
      R"({"errors":[{"message":"gone","extensions":{"code":"APP_UNAVAILABLE",)"
      R"("retryable":false}}]})"};
  auto client = makeClient(transport);
  bool threw = false;
  try {
    (void)client->request("query");
  } catch (const CrowdyAppUnavailableError& error) {
    threw = true;
    CHECK(!error.retryable());
  }
  CHECK(threw);
}
#endif

void testAppUnavailableNeverLeaksAnEndpointToMoveTo() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {appUnavailableBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);

  int moves = 0;
  client->setWrongDatacenterHandler([&](const DatacenterMove&) {
    ++moves;
    return true;
  });
#ifndef CROWDY_NO_EXCEPTIONS
  try {
    (void)client->request("query");
  } catch (const CrowdyGraphQLError&) {
  }
#else
  (void)client->request("query");
#endif
  CHECK_EQ(moves, 0);
  CHECK_EQ(transport->urls.size(), std::size_t{1});
  CHECK_EQ(client->endpoint(), "https://ck-va.prod.cp.cks-env.com/graphql");
}

void testAnOrdinaryErrorIsUntouchedByAnyOfThis() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {
      R"({"errors":[{"message":"nope","extensions":{"code":"FORBIDDEN"}}]})",
      R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);
  int moves = 0;
  client->setWrongDatacenterHandler([&](const DatacenterMove&) {
    ++moves;
    return true;
  });
#ifndef CROWDY_NO_EXCEPTIONS
  bool threw = false;
  try {
    (void)client->request("query");
  } catch (const CrowdyGraphQLError& error) {
    threw = true;
    CHECK_EQ(error.code(), "FORBIDDEN");
  }
  CHECK(threw);
#else
  CHECK(!client->request("query").ok());
#endif
  CHECK_EQ(moves, 0);
  CHECK_EQ(transport->urls.size(), std::size_t{1});
}

// Requests to one endpoint can be in flight together, and every one of them is
// answered with the same redirect. The first to complete moves the client; the
// handler then tells the others "no move" only because the target is already
// current. Those requests must retry too - at the endpoint the client now sits
// on - rather than surface the redirect to their callers. This is the startup
// shape of any host that fans out queries before first contact resolved the
// app's datacenter.
void testARequestOvertakenByAnotherRedirectStillRetries() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);

  int moves = 0;
  client->setWrongDatacenterHandler([&](const DatacenterMove& move) {
    ++moves;
    return client->setEndpoint(move.gameApiUrl + "/graphql");
  });

  // The concurrent winner: its redirect lands while this request is in
  // flight, so by the time this one's redirect is offered to the handler the
  // client already sits on the target.
  transport->onSend = [&](const HttpRequest&) {
    if (transport->urls.size() == 1) {
      client->setEndpoint("https://ck-or.prod.cp.cks-env.com/graphql");
    }
  };

  int calls = 0;
  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    ++calls;
    got = std::move(out);
  });

  CHECK_EQ(calls, 1);
  CHECK(got.ok());
  CHECK_EQ(got.data["v"].asString(), "ok");
  // The handler was offered the redirect and reported no move; the retry
  // happened anyway, and at the moved-to endpoint rather than the refused one.
  CHECK_EQ(moves, 1);
  CHECK_EQ(transport->urls.size(), std::size_t{2});
  CHECK_EQ(transport->urls[0], "https://ck-va.prod.cp.cks-env.com/graphql");
  CHECK_EQ(transport->urls[1], "https://ck-or.prod.cp.cks-env.com/graphql");
}

// The same race through the blocking path, which has its own retry expression.
void testSyncRequestOvertakenByAnotherRedirectStillRetries() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);

  int moves = 0;
  client->setWrongDatacenterHandler([&](const DatacenterMove& move) {
    ++moves;
    return client->setEndpoint(move.gameApiUrl + "/graphql");
  });
  transport->onSend = [&](const HttpRequest&) {
    if (transport->urls.size() == 1) {
      client->setEndpoint("https://ck-or.prod.cp.cks-env.com/graphql");
    }
  };

  Json data = client->request("query");
  CHECK_EQ(data["v"].asString(), "ok");
  CHECK_EQ(moves, 1);
  CHECK_EQ(transport->urls.size(), std::size_t{2});
  CHECK_EQ(transport->urls[1], "https://ck-or.prod.cp.cks-env.com/graphql");
}

// The retry re-issues the same operation, so it must carry the credential the
// attempt carried. The shared AuthState can hold a DIFFERENT caller's bearer by
// the time the redirect is delivered - engines multiplex token planes over one
// client and switch the state per call - and a retry rebuilt from it would run
// the operation as someone else.
void testTheRetryKeepsTheAttemptsBearer() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);
  client->auth().setToken("attempt-bearer");
  client->setWrongDatacenterHandler([&](const DatacenterMove& move) {
    return client->setEndpoint(move.gameApiUrl + "/graphql");
  });
  // A concurrent caller switches the shared auth state while the request is in
  // flight.
  transport->onSend = [&](const HttpRequest&) {
    if (transport->urls.size() == 1) client->auth().setToken("other-callers-bearer");
  };

  Json data = client->request("query");
  CHECK_EQ(data["v"].asString(), "ok");
  CHECK_EQ(transport->authorizations.size(), std::size_t{2});
  CHECK_EQ(transport->authorizations[0], "Bearer attempt-bearer");
  CHECK_EQ(transport->authorizations[1], "Bearer attempt-bearer");
}

void testAsyncRetryKeepsTheAttemptsBearer() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);
  client->auth().setToken("attempt-bearer");
  client->setWrongDatacenterHandler([&](const DatacenterMove& move) {
    return client->setEndpoint(move.gameApiUrl + "/graphql");
  });
  transport->onSend = [&](const HttpRequest&) {
    if (transport->urls.size() == 1) client->auth().setToken("other-callers-bearer");
  };

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    got = std::move(out);
  });
  CHECK(got.ok());
  CHECK_EQ(transport->authorizations.size(), std::size_t{2});
  CHECK_EQ(transport->authorizations[1], "Bearer attempt-bearer");
}

void testAsyncPathMovesAndRetriesOnce() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->bodies = {wrongDatacenterBody(), R"({"data":{"v":"ok"}})"};
  auto client = makeClient(transport);
  client->setWrongDatacenterHandler([&](const DatacenterMove& move) {
    return client->setEndpoint(move.gameApiUrl + "/graphql");
  });

  int calls = 0;
  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    ++calls;
    got = std::move(out);
  });
  // The caller is told once, about the retry's result, not about the redirect.
  CHECK_EQ(calls, 1);
  CHECK(got.ok());
  CHECK_EQ(got.data["v"].asString(), "ok");
  CHECK_EQ(transport->urls.size(), std::size_t{2});
}

}  // namespace

int main() {
  testMoveFromErrorsReadsTheEndpoint();
  testMoveFromErrorsScansPastALeadingUnrelatedError();
  testMoveFromErrorsRefusesOnAppUnavailable();
  testMoveFromErrorsRefusesWrongDatacenterWithNoEndpoint();
  testTransportMovesAndRetriesOnceAndTheRetrySucceeds();
  testTransportDoesNotRetryWhenTheHandlerDeclines();
  testTransportRetriesAtMostOnce();
  testAsyncPathRetriesAtMostOnce();
#ifndef CROWDY_NO_EXCEPTIONS
  testHandlerThatThrowsSurfacesTheOriginalRefusal();
  testHandlerThatMovesThenThrowsStillSurfacesTheOriginalRefusal();
  testAppUnavailableThrowsATypedErrorAHostCanShowAPlayer();
  testRetryableIsFalseOnlyWhenTheServerSaysSo();
#endif
  testAppUnavailableNeverLeaksAnEndpointToMoveTo();
  testAnOrdinaryErrorIsUntouchedByAnyOfThis();
  testARequestOvertakenByAnotherRedirectStillRetries();
  testSyncRequestOvertakenByAnotherRedirectStillRetries();
  testTheRetryKeepsTheAttemptsBearer();
  testAsyncRetryKeepsTheAttemptsBearer();
  testAsyncPathMovesAndRetriesOnce();
  std::puts("datacenter_redirect_test OK");
  return 0;
}
