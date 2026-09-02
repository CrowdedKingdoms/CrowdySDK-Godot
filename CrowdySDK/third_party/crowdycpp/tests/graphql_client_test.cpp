#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/graphql/json.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::graphql;

namespace {

// A synchronous transport that returns a canned response, or throws.
class FakeSyncTransport final : public IHttpTransport {
 public:
  HttpResponse response;
#ifndef CROWDY_NO_EXCEPTIONS
  bool throwTimeout = false;
  bool throwNetwork = false;
#else
  Status failure;
#endif

  HttpResponse send(const HttpRequest&) override {
#ifndef CROWDY_NO_EXCEPTIONS
    if (throwTimeout) throw CrowdyTimeoutError("timed out");
    if (throwNetwork) throw CrowdyNetworkError("connection refused");
#endif
    return response;
  }

#ifdef CROWDY_NO_EXCEPTIONS
  HttpOutcome sendOutcome(const HttpRequest&) noexcept override {
    if (!failure.ok()) return {failure, {}, "injected transport failure"};
    return {Errc::Ok, response, {}};
  }
#endif
};

class SlowSyncTransport final : public IHttpTransport {
 public:
  HttpResponse send(const HttpRequest&) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return {200, R"({"data":{"ok":true}})"};
  }
};

// An async transport that either calls back immediately or holds the callback
// until fire() is called (to test deferred delivery through the dispatcher).
class FakeAsyncTransport final : public IAsyncHttpTransport {
 public:
  HttpOutcome outcome;
  bool defer = false;
  std::function<void(HttpOutcome)> saved;
#ifndef CROWDY_NO_EXCEPTIONS
  bool retainThenThrow = false;
#endif

  void sendAsync(const HttpRequest&, std::function<void(HttpOutcome)> cb) override {
#ifndef CROWDY_NO_EXCEPTIONS
    if (retainThenThrow) {
      saved = cb;
      throw CrowdyNetworkError("async start failed");
    }
#endif
    if (defer) {
      saved = std::move(cb);
    } else {
      cb(outcome);
    }
  }

  void fire() { saved(outcome); }
};

class UrlRecordingTransport final : public IHttpTransport {
 public:
  std::vector<std::string> urls;
  HttpResponse response{200, R"({"data":{"v":"ok"}})"};

  HttpResponse send(const HttpRequest& request) override {
    urls.push_back(request.url);
    return response;
  }

#ifdef CROWDY_NO_EXCEPTIONS
  HttpOutcome sendOutcome(const HttpRequest& request) noexcept override {
    urls.push_back(request.url);
    return {Errc::Ok, response, {}};
  }
#endif
};

std::shared_ptr<GraphQLClient> makeClient(std::shared_ptr<IHttpTransport> sync) {
  return std::make_shared<GraphQLClient>(GraphQLClientConfig{"http://test/graphql", 1000},
                                         std::move(sync), std::make_shared<AuthState>());
}

HttpOutcome httpOk(int status, std::string body) {
  HttpOutcome o;
  o.status = Errc::Ok;
  o.response = HttpResponse{status, std::move(body)};
  return o;
}

void testAsyncSuccess() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome = httpOk(200, R"({"data":{"foo":{"x":7}}})");
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  bool called = false;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    got = std::move(out);
    called = true;
  });

  CHECK(called);
  CHECK(got.ok());
  CHECK_EQ(got.data["foo"]["x"].asInt64(), 7);
}

void testAsyncGraphqlErrors() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome = httpOk(200, R"({"errors":[{"message":"nope","extensions":{"code":"FORBIDDEN"}}]})");
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::GraphQL);
  CHECK_EQ(got.errors.size(), std::size_t{1});
  CHECK(got.errors[0].code == "FORBIDDEN");
  CHECK(got.errors[0].message == "nope");
}

// The server carries the wait for a rate-limit refusal in extensions.
// A client that can classify the refusal but not time it has to guess.
void testRetryAfterMsIsReadFromExtensions() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome = httpOk(200,
                          R"({"errors":[{"message":"Too many calls","extensions":)"
                          R"({"code":"RATE_LIMITED","blame":"BUDGET","retryAfterMs":4200}}]})");
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK_EQ(got.errors.size(), std::size_t{1});
  CHECK(got.errors[0].code == "RATE_LIMITED");
  CHECK(got.errors[0].blame == "BUDGET");
  CHECK(got.errors[0].retryAfterMs.has_value());
  CHECK_EQ(*got.errors[0].retryAfterMs, std::int64_t{4200});
}

// "Retry immediately" and "the server said nothing" are different instructions,
// so a zero must not be reachable by an absent key. This is the whole reason the
// field is optional rather than an int defaulting to 0.
void testRetryAfterMsDistinguishesZeroFromAbsent() {
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  auto async = std::make_shared<FakeAsyncTransport>();
  client->setAsyncTransport(async);

  auto errorFor = [&](const char* body) {
    async->outcome = httpOk(200, body);
    GraphQLOutcome got;
    client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });
    CHECK_EQ(got.errors.size(), std::size_t{1});
    return got.errors[0];
  };

  const GraphQLErrorDetail absent =
      errorFor(R"({"errors":[{"message":"nope","extensions":{"code":"FORBIDDEN"}}]})");
  CHECK(!absent.retryAfterMs.has_value());

  const GraphQLErrorDetail zero = errorFor(
      R"({"errors":[{"message":"now","extensions":{"code":"RATE_LIMITED","retryAfterMs":0}}]})");
  CHECK(zero.retryAfterMs.has_value());
  CHECK_EQ(*zero.retryAfterMs, std::int64_t{0});

  // A non-numeric value is the server saying nothing intelligible, which is
  // nearer to absent than to zero.
  const GraphQLErrorDetail wrongType = errorFor(
      R"({"errors":[{"message":"?","extensions":{"code":"RATE_LIMITED","retryAfterMs":"soon"}}]})");
  CHECK(!wrongType.retryAfterMs.has_value());
}

void testAsyncHttpError() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome = httpOk(500, "internal error, not json");
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::Http);
  CHECK_EQ(got.httpStatus, 500);
}

void testAsyncProtocolError() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome = httpOk(200, "this is not json");
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::Protocol);
}

void testAsyncTransportFailure() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->outcome.status = Errc::Timeout;
  async->outcome.errorMessage = "timed out";
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::Timeout);
  CHECK(got.errorMessage == "timed out");
}

// The pump: a completion is not delivered until drain() runs on the poll thread.
void testDispatcherDefersDelivery() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->defer = true;
  async->outcome = httpOk(200, R"({"data":{"ok":true}})");
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);
  client->setDispatcher(dispatcher);

  bool called = false;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome) { called = true; });
  CHECK(!called);  // request started, transport has not completed

  async->fire();
  CHECK(!called);  // transport completed, but delivery is queued on the dispatcher

  const std::size_t ran = dispatcher->drain();
  CHECK(called);
  CHECK_EQ(ran, std::size_t{1});
}

// With no async transport, requestAsync falls back to the sync transport inline.
void testInlineFallback() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->response = HttpResponse{200, R"({"data":{"n":42}})"};
  auto client = makeClient(sync);

  GraphQLOutcome got;
  bool called = false;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    got = std::move(out);
    called = true;
  });

  CHECK(called);
  CHECK(got.ok());
  CHECK_EQ(got.data["n"].asInt64(), 42);
}

void testThreadedAdapterStartsWithoutBlockingCaller() {
  auto threaded =
      makeThreadedAsyncTransport(std::make_shared<SlowSyncTransport>());
  std::atomic<bool> called{false};
  HttpOutcome outcome;
  const auto started = std::chrono::steady_clock::now();
  threaded->sendAsync({}, [&](HttpOutcome value) {
    outcome = std::move(value);
    called.store(true, std::memory_order_release);
  });
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(elapsed < std::chrono::milliseconds(50));
  for (int attempt = 0;
       attempt < 200 &&
       !called.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(called.load(std::memory_order_acquire));
  CHECK(outcome.status.ok());
  CHECK_EQ(outcome.response.status, 200);
}

#ifndef CROWDY_NO_EXCEPTIONS
void testInlineFallbackTransportThrow() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->throwTimeout = true;
  auto client = makeClient(sync);

  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) { got = std::move(out); });

  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::Timeout);
}

void testAsyncStartThrowDeliversOnce() {
  auto async = std::make_shared<FakeAsyncTransport>();
  async->retainThenThrow = true;
  auto client = makeClient(std::make_shared<FakeSyncTransport>());
  client->setAsyncTransport(async);

  int calls = 0;
  GraphQLOutcome got;
  client->requestAsync("query", JVal(), {}, [&](GraphQLOutcome out) {
    ++calls;
    got = std::move(out);
  });
  CHECK_EQ(calls, 1);
  CHECK(!got.ok());
  CHECK(got.kind == GraphQLErrorKind::Network);

  async->outcome = httpOk(200, R"({"data":{"late":true}})");
  async->fire();
  CHECK_EQ(calls, 1);
}
#endif

// The blocking request() still works and still throws, derived from the same
// logic as the async path.
// setEndpoint's RETURN VALUE is load-bearing, not a convenience: the
// datacenter-redirect retry only retries when the endpoint actually moved.
// If a no-op move reported success, a server that redirects to the URL the
// client is already on would produce an endless retry against itself.
void testSetEndpointReportsWhetherItMoved() {
  auto transport = std::make_shared<UrlRecordingTransport>();
  auto client = makeClient(transport);
  CHECK_EQ(client->endpoint(), "http://test/graphql");

  CHECK(!client->setEndpoint(""));
  CHECK(!client->setEndpoint("http://test/graphql"));
  CHECK_EQ(client->endpoint(), "http://test/graphql");

  CHECK(client->setEndpoint("http://moved/graphql"));
  CHECK_EQ(client->endpoint(), "http://moved/graphql");

  (void)client->request("query");
  CHECK_EQ(transport->urls.size(), std::size_t{1});
  CHECK_EQ(transport->urls[0], "http://moved/graphql");
}

void testSyncRequestReturnsData() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->response = HttpResponse{200, R"({"data":{"v":"hello"}})"};
  auto client = makeClient(sync);

  Json data = client->request("query");
  CHECK(data["v"].asString() == "hello");
}

#ifndef CROWDY_NO_EXCEPTIONS
void testSyncRequestThrowsGraphql() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->response = HttpResponse{200, R"({"errors":[{"message":"denied","extensions":{"code":"UNAUTHENTICATED"}}]})"};
  auto client = makeClient(sync);

  bool threw = false;
  try {
    client->request("query");
  } catch (const CrowdyGraphQLError& e) {
    threw = true;
    CHECK(e.code() == "UNAUTHENTICATED");
  }
  CHECK(threw);
}
#else
void testSyncRequestReturnsInvalidOnFailure() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->response = HttpResponse{
      200,
      R"({"errors":[{"message":"denied","extensions":{"code":"UNAUTHENTICATED"}}]})"};
  auto client = makeClient(sync);
  CHECK(!client->request("query").ok());
  sync->response = HttpResponse{503, "unavailable"};
  CHECK(!client->request("query").ok());
  sync->failure = Errc::Timeout;
  CHECK(!client->request("query").ok());
}
#endif

#if defined(CROWDY_NO_EXCEPTIONS) && defined(CROWDY_TEST_WITH_CURL)
void testDefaultCurlFailureReturnsInvalidJson() {
  auto client = std::make_shared<GraphQLClient>(
      GraphQLClientConfig{"http://127.0.0.1:1/graphql", 250},
      makeCurlTransport(), std::make_shared<AuthState>());
  CHECK(!client->request("query Unreachable { unreachable }").ok());

  GraphQLOutcome asyncOutcome;
  client->requestAsync(
      "query Unreachable { unreachable }", JVal(), {},
      [&](GraphQLOutcome outcome) { asyncOutcome = std::move(outcome); });
  CHECK(!asyncOutcome.ok());
  CHECK(asyncOutcome.kind == GraphQLErrorKind::Network ||
        asyncOutcome.kind == GraphQLErrorKind::Timeout);
}
#endif

#ifndef CROWDY_NO_EXCEPTIONS
void testSyncRequestThrowsHttp() {
  auto sync = std::make_shared<FakeSyncTransport>();
  sync->response = HttpResponse{503, "unavailable"};
  auto client = makeClient(sync);

  bool threw = false;
  try {
    client->request("query");
  } catch (const CrowdyHttpError& e) {
    threw = true;
    CHECK_EQ(e.status(), 503);
  }
  CHECK(threw);
}
#endif

}  // namespace

int main() {
  testAsyncSuccess();
  testAsyncGraphqlErrors();
  testRetryAfterMsIsReadFromExtensions();
  testRetryAfterMsDistinguishesZeroFromAbsent();
  testAsyncHttpError();
  testAsyncProtocolError();
  testAsyncTransportFailure();
  testDispatcherDefersDelivery();
  testInlineFallback();
  testThreadedAdapterStartsWithoutBlockingCaller();
#ifndef CROWDY_NO_EXCEPTIONS
  testInlineFallbackTransportThrow();
  testAsyncStartThrowDeliversOnce();
#endif
  testSetEndpointReportsWhetherItMoved();
  testSyncRequestReturnsData();
#ifndef CROWDY_NO_EXCEPTIONS
  testSyncRequestThrowsGraphql();
  testSyncRequestThrowsHttp();
#else
  testSyncRequestReturnsInvalidOnFailure();
#endif
#if defined(CROWDY_NO_EXCEPTIONS) && defined(CROWDY_TEST_WITH_CURL)
  testDefaultCurlFailureReturnsInvalidJson();
#endif
  std::printf("graphql_client_test passed\n");
  return 0;
}
