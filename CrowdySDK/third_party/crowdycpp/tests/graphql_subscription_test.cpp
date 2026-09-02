#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "crowdy/agent/client_runtime.hpp"
#include "crowdy/agent/transport.hpp"
#include "crowdy/client.hpp"
#include "crowdy/graphql/subscription_client.hpp"
#include "crowdy/graphql/websocket.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::graphql;

namespace {

class FakeConnection final : public IWebSocketConnection {
 public:
  void start(WebSocketEventCallback callback) override {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
    started_ = true;
  }

  Status send(WebSocketFrame frame) override {
    std::lock_guard lock(mutex_);
    if (!sendStatus_.ok()) return sendStatus_;
    sent_.push_back(std::move(frame));
    return Errc::Ok;
  }

  void close(std::uint16_t code, std::string_view reason) override {
    std::lock_guard lock(mutex_);
    closes_.push_back(WebSocketCloseInfo{code, std::string(reason), true});
  }

  void emitOpen() {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Open;
    emit(std::move(event));
  }

  void emitText(std::string text) {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Frame;
    event.frame = WebSocketFrame{WebSocketFrameKind::Text, std::move(text)};
    emit(std::move(event));
  }

  void emitFrame(WebSocketFrameKind kind, std::string payload) {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Frame;
    event.frame = WebSocketFrame{kind, std::move(payload)};
    emit(std::move(event));
  }

  void emitClose(std::uint16_t code, std::string reason = {},
                 bool clean = false) {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Close;
    event.close = WebSocketCloseInfo{code, std::move(reason), clean};
    emit(std::move(event));
  }

  void emitError(WebSocketErrorKind kind, bool retryable,
                 std::string message = "transport failed") {
    WebSocketEvent event;
    event.kind = WebSocketEventKind::Error;
    event.error.kind = kind;
    event.error.status =
        kind == WebSocketErrorKind::Timeout ? Errc::Timeout : Errc::SocketError;
    event.error.message = std::move(message);
    event.error.retryable = retryable;
    emit(std::move(event));
  }

  std::vector<WebSocketFrame> sent() const {
    std::lock_guard lock(mutex_);
    return sent_;
  }

  std::vector<WebSocketCloseInfo> closes() const {
    std::lock_guard lock(mutex_);
    return closes_;
  }

  bool started() const {
    std::lock_guard lock(mutex_);
    return started_;
  }

 private:
  void emit(WebSocketEvent event) {
    WebSocketEventCallback callback;
    {
      std::lock_guard lock(mutex_);
      callback = callback_;
    }
    CHECK(static_cast<bool>(callback));
    callback(std::move(event));
  }

  mutable std::mutex mutex_;
  WebSocketEventCallback callback_;
  std::vector<WebSocketFrame> sent_;
  std::vector<WebSocketCloseInfo> closes_;
  Status sendStatus_ = Errc::Ok;
  bool started_ = false;
};

class FakeTransport final : public IWebSocketTransport {
 public:
  std::shared_ptr<IWebSocketConnection> createConnection(
      const WebSocketConnectRequest& request) override {
    auto connection = std::make_shared<FakeConnection>();
    {
      std::lock_guard lock(mutex_);
      requests_.push_back(request);
      connections_.push_back(connection);
    }
    return connection;
  }

  std::size_t connectionCount() const {
    std::lock_guard lock(mutex_);
    return connections_.size();
  }

  std::shared_ptr<FakeConnection> connection(std::size_t index) const {
    std::lock_guard lock(mutex_);
    CHECK(index < connections_.size());
    return connections_[index];
  }

  WebSocketConnectRequest request(std::size_t index) const {
    std::lock_guard lock(mutex_);
    CHECK(index < requests_.size());
    return requests_[index];
  }

 private:
  mutable std::mutex mutex_;
  std::vector<WebSocketConnectRequest> requests_;
  std::vector<std::shared_ptr<FakeConnection>> connections_;
};

class FakeHttpTransport final : public IHttpTransport {
 public:
  HttpResponse send(const HttpRequest&) override {
    return HttpResponse{200, R"({"data":{"ok":true}})"};
  }
};

class TickingBrowserDispatcher final
    : public agent::IAgentBrowserToolDispatcher {
 public:
  void dispatch(
      agent::AgentToolInvocation,
      agent::AgentCallback<agent::AgentToolResult> callback) override {
    callback(agent::AgentOutcome<agent::AgentToolResult>::failure(
        agent::makeAgentError("AGENT_HOST_UNAVAILABLE",
                              "test dispatcher has no tools")));
  }
  void cancelActive(agent::AgentPreemptionReason) override {
    ++cancellations;
  }
  void clearClosedSession() override { ++clears; }
  void tick() override { ++ticks; }

  int ticks = 0;
  int cancellations = 0;
  int clears = 0;
};

template <typename Predicate>
void waitFor(Predicate predicate, long timeoutMs = 1000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      CHECK(false);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::unique_ptr<GraphQLSubscriptionClient> makeClient(
    const std::shared_ptr<FakeTransport>& transport,
    const std::shared_ptr<AuthState>& auth,
    const std::shared_ptr<Dispatcher>& dispatcher,
    GraphQLSubscriptionOptions options = {}) {
  return std::make_unique<GraphQLSubscriptionClient>(
      GraphQLSubscriptionClientConfig{"https://api.example.test/base/graphql",
                                      options},
      transport, auth, dispatcher);
}

Json parseSentText(const std::shared_ptr<FakeConnection>& connection,
                   std::size_t index) {
  const auto sent = connection->sent();
  CHECK(index < sent.size());
  CHECK(sent[index].kind == WebSocketFrameKind::Text);
  const Json parsed = Json::parse(sent[index].payload);
  CHECK(parsed.ok());
  return parsed;
}

void acknowledge(const std::shared_ptr<FakeConnection>& connection) {
  connection->emitOpen();
  connection->emitText(R"({"type":"connection_ack"})");
}

void testEndpointNormalization() {
  auto check = [](std::string_view input,
                  GraphQLWebSocketEndpointKind kind,
                  std::string_view expected) {
    auto result = normalizeGraphQLWebSocketUrl(input, kind);
    CHECK(result.ok());
    CHECK(result.value() == expected);
  };
  check("http://example.test", GraphQLWebSocketEndpointKind::ApiBase,
        "ws://example.test/graphql");
  check("https://example.test/", GraphQLWebSocketEndpointKind::ApiBase,
        "wss://example.test/graphql");
  check("ws://example.test/api/", GraphQLWebSocketEndpointKind::ApiBase,
        "ws://example.test/api/graphql");
  check("wss://example.test/api/graphql",
        GraphQLWebSocketEndpointKind::ApiBase,
        "wss://example.test/api/graphql");
  check("https://example.test/graphql/graphql/?region=us",
        GraphQLWebSocketEndpointKind::ApiBase,
        "wss://example.test/graphql?region=us");
  check("http://example.test/subscriptions/",
        GraphQLWebSocketEndpointKind::Complete,
        "ws://example.test/subscriptions/");
  check("wss://example.test/subscriptions/",
        GraphQLWebSocketEndpointKind::Complete,
        "wss://example.test/subscriptions/");
  check("https://example.test/custom/events?region=us",
        GraphQLWebSocketEndpointKind::Complete,
        "wss://example.test/custom/events?region=us");
  check("ws://example.test/graphql/graphql/",
        GraphQLWebSocketEndpointKind::Complete,
        "ws://example.test/graphql/graphql/");
  CHECK(!normalizeGraphQLWebSocketUrl("ftp://example.test").ok());
  CHECK(!normalizeGraphQLWebSocketUrl("https://example.test/#fragment").ok());
}

void testDefaultTransportAvailability() {
#ifdef CROWDY_HAS_CURL_WEBSOCKETS
  CHECK(static_cast<bool>(makeCurlWebSocketTransport()) ==
        curlWebSocketTransportAvailable());
#else
  CHECK(!curlWebSocketTransportAvailable());
  CHECK(!makeCurlWebSocketTransport());
#endif
}

void testReuseHttpAuthAndDispatcher() {
  auto auth = std::make_shared<AuthState>();
  auth->setToken("shared-token");
  auto http = std::make_shared<FakeHttpTransport>();
  GraphQLClient httpClient(GraphQLClientConfig{"https://api.example/graphql", 1000},
                           http, auth);
  auto webSocket = std::make_shared<FakeTransport>();
  GraphQLSubscriptionClient subscriptions(
      GraphQLSubscriptionClientConfig{"https://api.example/graphql", {}},
      webSocket, httpClient);
  CHECK(static_cast<bool>(httpClient.dispatcher()));
  CHECK(httpClient.dispatcher() == subscriptions.dispatcher());

  GraphQLSubscriptionCallbacks callbacks;
  auto handle = subscriptions.subscribe(
      "subscription { watch { value } }", JVal(), {}, std::move(callbacks));
  const auto connection = webSocket->connection(0);
  connection->emitOpen();
  Json init = parseSentText(connection, 0);
  CHECK(init["payload"]["Authorization"].asString() ==
        "Bearer shared-token");
  CHECK(handle.active());
}

void testHandshakeAuthParsePingAndDispatcher() {
  auto transport = std::make_shared<FakeTransport>();
  auto auth = std::make_shared<AuthState>();
  auth->setToken("test-token");
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client = makeClient(transport, auth, dispatcher);

  bool nextCalled = false;
  GraphQLSubscriptionOutcome outcome;
  const std::thread::id gameThread = std::this_thread::get_id();
  std::thread::id callbackThread;
  JVal variables;
  variables["appId"] = "42";
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onNext = [&](GraphQLSubscriptionOutcome value) {
    nextCalled = true;
    callbackThread = std::this_thread::get_id();
    outcome = std::move(value);
  };
  auto handle = client->subscribe(
      "subscription Watch($appId: BigInt!) { watch(appId: $appId) { value } }",
      variables, "Watch", std::move(callbacks));
  CHECK(handle.active());
  CHECK_EQ(transport->connectionCount(), std::size_t{1});
  const auto connection = transport->connection(0);
  CHECK(connection->started());
  CHECK(transport->request(0).url ==
        "wss://api.example.test/base/graphql");
  CHECK(transport->request(0).subprotocol == "graphql-transport-ws");

  connection->emitOpen();
  Json init = parseSentText(connection, 0);
  CHECK(init["type"].asString() == "connection_init");
  CHECK(init["payload"]["Authorization"].asString() ==
        "Bearer test-token");

  connection->emitText(R"({"type":"connection_ack"})");
  Json subscribe = parseSentText(connection, 1);
  CHECK(subscribe["type"].asString() == "subscribe");
  CHECK(subscribe["id"].asString() == "1");
  CHECK(subscribe["payload"]["operationName"].asString() == "Watch");
  CHECK(subscribe["payload"]["variables"]["appId"].asString() == "42");

  connection->emitText(R"({"type":"ping","payload":{"nonce":"abc"}})");
  Json pong = parseSentText(connection, 2);
  CHECK(pong["type"].asString() == "pong");
  CHECK(pong["payload"]["nonce"].asString() == "abc");

  connection->emitFrame(WebSocketFrameKind::Ping, "wire-ping");
  const auto sent = connection->sent();
  CHECK_EQ(sent.size(), std::size_t{4});
  CHECK(sent[3].kind == WebSocketFrameKind::Pong);
  CHECK(sent[3].payload == "wire-ping");

  std::thread engineTransportThread([connection] {
    connection->emitText(
        R"({"id":"1","type":"next","payload":{"data":{"watch":{"value":7}}}})");
  });
  engineTransportThread.join();
  CHECK(!nextCalled);
  CHECK_EQ(dispatcher->drain(), std::size_t{1});
  CHECK(nextCalled);
  CHECK(callbackThread == gameThread);
  CHECK(outcome.ok());
  CHECK_EQ(outcome.data["watch"]["value"].asInt64(), std::int64_t{7});
}

void testCancellationSuppressesQueuedDelivery() {
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client =
      makeClient(transport, std::make_shared<AuthState>(), dispatcher);
  bool called = false;
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onNext =
      [&](GraphQLSubscriptionOutcome) { called = true; };
  auto handle = client->subscribe(
      "subscription { watch { value } }", JVal(), {},
      std::move(callbacks));
  const auto connection = transport->connection(0);
  acknowledge(connection);
  connection->emitText(
      R"({"id":"1","type":"next","payload":{"data":{"watch":{"value":1}}}})");
  CHECK(!called);

  handle.cancel();
  CHECK(!handle.active());
  dispatcher->drain();
  CHECK(!called);

  const auto sent = connection->sent();
  CHECK_EQ(sent.size(), std::size_t{3});
  Json complete = Json::parse(sent[2].payload);
  CHECK(complete["type"].asString() == "complete");
  CHECK(complete["id"].asString() == "1");
  CHECK(!connection->closes().empty());
}

void testCancellationSuppressesQueuedTerminalAndReconnectDelivery() {
  {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client =
        makeClient(transport, std::make_shared<AuthState>(), dispatcher);
    bool completed = false;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onComplete = [&] { completed = true; };
    {
      auto handle = client->subscribe(
          "subscription { watch { value } }", JVal(), {},
          std::move(callbacks));
      const auto connection = transport->connection(0);
      acknowledge(connection);
      connection->emitText(R"({"id":"1","type":"complete"})");
      CHECK(!handle.active());
    }
    dispatcher->drain();
    CHECK(!completed);
  }

  {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client =
        makeClient(transport, std::make_shared<AuthState>(), dispatcher);
    bool errored = false;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError = [&](GraphQLSubscriptionError) { errored = true; };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    connection->emitText(
        R"({"id":"1","type":"error","payload":[{"message":"denied","extensions":{"code":"FORBIDDEN"}}]})");
    CHECK(!handle.active());
    handle.cancel();
    dispatcher->drain();
    CHECK(!errored);
  }

  {
    GraphQLSubscriptionOptions options;
    options.initialReconnectDelayMs = 0;
    options.maxReconnectDelayMs = 0;
    options.reconnectJitter = 0;
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    bool reconnected = false;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onReconnect =
        [&](GraphQLReconnectInfo) { reconnected = true; };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto first = transport->connection(0);
    acknowledge(first);
    first->emitClose(1012, "service restart");
    waitFor([&] { return transport->connectionCount() == 2; });
    acknowledge(transport->connection(1));
    handle.cancel();
    dispatcher->drain();
    CHECK(!reconnected);
  }
}

void testCancellationWaitsForInFlightDelivery() {
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client =
      makeClient(transport, std::make_shared<AuthState>(), dispatcher);

  std::promise<void> callbackStarted;
  auto callbackStartedFuture = callbackStarted.get_future();
  std::promise<void> releaseCallback;
  auto releaseCallbackFuture = releaseCallback.get_future().share();
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onNext = [&](GraphQLSubscriptionOutcome) {
    callbackStarted.set_value();
    releaseCallbackFuture.wait();
  };
  auto handle = client->subscribe(
      "subscription { watch { value } }", JVal(), {},
      std::move(callbacks));
  const auto connection = transport->connection(0);
  acknowledge(connection);
  connection->emitText(
      R"({"id":"1","type":"next","payload":{"data":{"watch":{"value":1}}}})");

  auto draining = std::async(std::launch::async,
                             [&] { return dispatcher->drain(); });
  callbackStartedFuture.wait();
  std::promise<void> cancelStarted;
  auto cancelStartedFuture = cancelStarted.get_future();
  auto cancelling = std::async(std::launch::async, [&] {
    cancelStarted.set_value();
    handle.cancel();
  });
  cancelStartedFuture.wait();
  CHECK(cancelling.wait_for(std::chrono::milliseconds(25)) ==
        std::future_status::timeout);
  releaseCallback.set_value();
  cancelling.get();
  CHECK_EQ(draining.get(), std::size_t{1});
  CHECK(!handle.active());
}

void testServerCompleteAndCloseMapping() {
  {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client =
        makeClient(transport, std::make_shared<AuthState>(), dispatcher);
    bool completed = false;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onComplete = [&] { completed = true; };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    connection->emitText(R"({"id":"1","type":"complete"})");
    CHECK(!handle.active());
    CHECK(!completed);
    dispatcher->drain();
    CHECK(completed);
  }

  {
    GraphQLSubscriptionOptions options;
    options.maxReconnectAttempts = 3;
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    connection->emitClose(4403, "forbidden");
    CHECK(!handle.active());
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::Authorization);
    CHECK_EQ(got.closeCode, std::uint16_t{4403});
    CHECK(got.terminal);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK_EQ(transport->connectionCount(), std::size_t{1});
  }
}

void testReconnectReplayAndStaleConnectionFencing() {
  GraphQLSubscriptionOptions options;
  options.initialReconnectDelayMs = 1;
  options.maxReconnectDelayMs = 1;
  options.reconnectJitter = 0;
  options.reconnectRandomSeed = 7;
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client = makeClient(transport, std::make_shared<AuthState>(),
                           dispatcher, options);

  std::size_t reconnects = 0;
  std::size_t nextCount = 0;
  GraphQLReconnectInfo reconnectInfo;
  GraphQLSubscriptionError terminalError;
  bool errored = false;
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onNext =
      [&](GraphQLSubscriptionOutcome) { ++nextCount; };
  callbacks.onError = [&](GraphQLSubscriptionError error) {
    terminalError = std::move(error);
    errored = true;
  };
  callbacks.onReconnect = [&](GraphQLReconnectInfo info) {
    ++reconnects;
    reconnectInfo = std::move(info);
  };
  auto handle = client->subscribe(
      "subscription { watch { value } }", JVal(), {},
      std::move(callbacks));
  const auto first = transport->connection(0);
  acknowledge(first);
  first->emitClose(1012, "service restart");

  waitFor([&] { return transport->connectionCount() == 2; });
  const auto second = transport->connection(1);
  acknowledge(second);
  CHECK_EQ(second->sent().size(), std::size_t{2});
  CHECK_EQ(reconnects, std::size_t{0});

  // A late terminal event from the replaced connection must not kill the
  // healthy replayed operation.
  first->emitClose(4401, "UNAUTHENTICATED");
  first->emitError(WebSocketErrorKind::Protocol, false, "late old error");
  CHECK(handle.active());
  CHECK(!errored);

  second->emitText(
      R"({"id":"1","type":"next","payload":{"data":{"watch":{"value":2}}}})");
  CHECK_EQ(nextCount, std::size_t{0});
  dispatcher->drain();
  CHECK_EQ(reconnects, std::size_t{1});
  CHECK_EQ(reconnectInfo.attempt, std::size_t{1});
  CHECK(reconnectInfo.operationId == "1");
  CHECK_EQ(nextCount, std::size_t{1});
  CHECK(!errored);
  (void)terminalError;
}

void testTerminalGraphqlErrors() {
  struct Case {
    const char* code;
    GraphQLSubscriptionErrorKind kind;
  };
  const Case cases[] = {
      {"AUTH_REQUIRED", GraphQLSubscriptionErrorKind::Authentication},
      {"APP_ID_REQUIRED", GraphQLSubscriptionErrorKind::AppScope},
      {"AGENT_CLIENT_EPOCH_STALE",
       GraphQLSubscriptionErrorKind::StaleClientEpoch},
  };

  for (const auto& testCase : cases) {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client =
        makeClient(transport, std::make_shared<AuthState>(), dispatcher);
    GraphQLSubscriptionError got;
    bool called = false;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError = [&](GraphQLSubscriptionError error) {
      got = std::move(error);
      called = true;
    };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    connection->emitText(
        std::string(
            R"({"id":"1","type":"error","payload":[{"message":"terminal","extensions":{"code":")") +
        testCase.code + R"("}}]})");
    CHECK(!handle.active());
    CHECK(!called);
    dispatcher->drain();
    CHECK(called);
    CHECK(got.kind == testCase.kind);
    CHECK(got.code == testCase.code);
    CHECK(got.terminal);
    CHECK(!got.retryable);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK_EQ(transport->connectionCount(), std::size_t{1});
    CHECK(isTerminalSubscriptionErrorCode(testCase.code));
  }
}

// The websocket path used to run its own copy of the error parser, and the copy
// had already lost `blame`. Both carriers read the same extensions now, so the
// same refusal means the same thing however it arrived.
void testSubscriptionErrorsCarryBlameAndRetryAfterMs() {
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<Dispatcher>();
  auto client = makeClient(transport, std::make_shared<AuthState>(), dispatcher);
  GraphQLSubscriptionError got;
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onError = [&](GraphQLSubscriptionError error) { got = std::move(error); };
  auto handle = client->subscribe("subscription { watch { value } }", JVal(), {},
                                  std::move(callbacks));
  const auto connection = transport->connection(0);
  acknowledge(connection);
  connection->emitText(
      R"({"id":"1","type":"error","payload":[{"message":"Too many calls","extensions":)"
      R"({"code":"RATE_LIMITED","blame":"BUDGET","retryAfterMs":4200}}]})");
  dispatcher->drain();

  CHECK_EQ(got.errors.size(), std::size_t{1});
  CHECK(got.errors[0].blame == "BUDGET");
  CHECK(got.errors[0].retryAfterMs.has_value());
  CHECK_EQ(*got.errors[0].retryAfterMs, std::int64_t{4200});
}

void testFrameAndUtf8Limits() {
  GraphQLSubscriptionOptions options;
  options.maxFrameBytes = 256;
  options.maxReconnectAttempts = 0;

  {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    connection->emitText(std::string(257, 'x'));
    CHECK(!handle.active());
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::FrameTooLarge);
    CHECK_EQ(got.closeCode, std::uint16_t{1009});
  }

  {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    const auto connection = transport->connection(0);
    acknowledge(connection);
    std::string invalid = R"({"type":"ping","payload":")";
    invalid.push_back(static_cast<char>(0xc3));
    invalid += R"("})";
    connection->emitText(std::move(invalid));
    CHECK(!handle.active());
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::InvalidUtf8);
    CHECK_EQ(got.closeCode, std::uint16_t{1007});
  }
}

void testAcknowledgementTimeoutAndReconnectCap() {
  {
    GraphQLSubscriptionOptions options;
    options.acknowledgementTimeoutMs = 5;
    options.maxReconnectAttempts = 0;
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    transport->connection(0)->emitOpen();
    waitFor([&] { return !handle.active(); });
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::Timeout);
    CHECK(got.status.code == Errc::Timeout);
  }

  {
    GraphQLSubscriptionOptions options;
    options.initialReconnectDelayMs = 0;
    options.maxReconnectDelayMs = 0;
    options.reconnectJitter = 0;
    options.maxReconnectAttempts = 1;
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<Dispatcher>();
    auto client = makeClient(transport, std::make_shared<AuthState>(),
                             dispatcher, options);
    GraphQLSubscriptionError got;
    GraphQLSubscriptionCallbacks callbacks;
    callbacks.onError =
        [&](GraphQLSubscriptionError error) { got = std::move(error); };
    auto handle = client->subscribe(
        "subscription { watch { value } }", JVal(), {},
        std::move(callbacks));
    transport->connection(0)->emitError(WebSocketErrorKind::Connection, true);
    waitFor([&] { return transport->connectionCount() == 2; });
    transport->connection(1)->emitError(WebSocketErrorKind::Connection, true);
    waitFor([&] { return !handle.active(); });
    dispatcher->drain();
    CHECK(got.kind == GraphQLSubscriptionErrorKind::ReconnectExhausted);
    CHECK(got.code == "RECONNECT_EXHAUSTED");
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK_EQ(transport->connectionCount(), std::size_t{2});
  }
}

void testCrowdyClientInjectionAndPoll() {
  auto webSocket = std::make_shared<FakeTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.example.test/api";
  config.transport = std::make_shared<FakeHttpTransport>();
  config.webSocketTransport = webSocket;
  config.webSocket.initialReconnectDelayMs = 0;
  CrowdyClient client(std::move(config));
  client.setToken("game-token");

  bool called = false;
  std::thread::id callbackThread;
  GraphQLSubscriptionCallbacks callbacks;
  callbacks.onNext = [&](GraphQLSubscriptionOutcome) {
    called = true;
    callbackThread = std::this_thread::get_id();
  };
  auto handle = client.subscriptions().subscribe(
      "subscription { watch { value } }", JVal(), {},
      std::move(callbacks));
  const auto connection = webSocket->connection(0);
  acknowledge(connection);
  std::thread transportThread([connection] {
    connection->emitText(
        R"({"id":"1","type":"next","payload":{"data":{"watch":{"value":9}}}})");
  });
  transportThread.join();
  CHECK(!called);
  client.poll();
  CHECK(called);
  CHECK(callbackThread == std::this_thread::get_id());
  CHECK(client.subscriptions().endpoint() ==
        "wss://game.example.test/api/graphql");
  CHECK(handle.active());
}

void testTypedGameModelActivePlayerCountChanged() {
  auto webSocket = std::make_shared<FakeTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.example.test";
  config.transport = std::make_shared<FakeHttpTransport>();
  config.webSocketTransport = webSocket;
  config.webSocket.initialReconnectDelayMs = 0;
  config.webSocket.maxReconnectDelayMs = 0;
  config.webSocket.reconnectJitter = 0;
  CrowdyClient client(std::move(config));

  std::size_t deliveries = 0;
  std::size_t errors = 0;
  std::size_t reconnects = 0;
  bool completed = false;
  domains::GameModelActivePlayerCountChange received;
  GraphQLSubscriptionError receivedError;
  GraphQLReconnectInfo reconnectInfo;
  domains::GameModelActivePlayerCountChangedCallbacks callbacks;
  callbacks.next =
      [&](domains::GameModelActivePlayerCountChange value) {
        ++deliveries;
        received = std::move(value);
      };
  callbacks.error = [&](GraphQLSubscriptionError error) {
    ++errors;
    receivedError = std::move(error);
  };
  callbacks.complete = [&] { completed = true; };
  callbacks.reconnect = [&](GraphQLReconnectInfo info) {
    ++reconnects;
    reconnectInfo = std::move(info);
  };

  auto handle = client.gameModel().activePlayerCountChanged(
      "42", std::move(callbacks));
  const auto first = webSocket->connection(0);
  acknowledge(first);
  const Json subscribe = parseSentText(first, 1);
  CHECK(subscribe["payload"]["operationName"].asString() ==
        "GameModelActivePlayerCountChanged");
  CHECK(subscribe["payload"]["variables"]["appId"].asString() == "42");
  const std::string document = subscribe["payload"]["query"].asString();
  CHECK(document.find(
            "subscription GameModelActivePlayerCountChanged") !=
        std::string::npos);
  CHECK(document.find("gameModelActivePlayerCountChanged(appId: $appId)") !=
        std::string::npos);
  CHECK(document.find("previousCount") != std::string::npos);
  CHECK(document.find("currentCount") != std::string::npos);
  CHECK(document.find("delta") != std::string::npos);
  CHECK(document.find("revision") != std::string::npos);
  CHECK(document.find("observedAt") != std::string::npos);
  CHECK(document.find("query GameModelActivePlayerCount(") ==
        std::string::npos);

  first->emitText(
      R"({"id":"1","type":"next","payload":{"data":{"gameModelActivePlayerCountChanged":{"appId":"42","previousCount":7,"currentCount":9,"delta":2,"revision":"184467440737095516160000","observedAt":"2026-07-24T00:00:00.000Z"}}}})");
  CHECK_EQ(deliveries, std::size_t{0});
  client.poll();
  CHECK_EQ(deliveries, std::size_t{1});
  CHECK_EQ(errors, std::size_t{0});
  CHECK(received.appId == "42");
  CHECK_EQ(received.previousCount, 7);
  CHECK_EQ(received.currentCount, 9);
  CHECK_EQ(received.delta, 2);
  CHECK(received.revision == "184467440737095516160000");
  CHECK(received.observedAt == "2026-07-24T00:00:00.000Z");

  first->emitText(
      R"({"id":"1","type":"next","payload":{"data":{"gameModelActivePlayerCountChanged":{"appId":"42","previousCount":9,"currentCount":8,"delta":-1,"revision":"not-decimal","observedAt":"2026-07-24T00:00:01.000Z"}}}})");
  client.poll();
  CHECK_EQ(deliveries, std::size_t{1});
  CHECK_EQ(errors, std::size_t{1});
  CHECK(receivedError.kind == GraphQLSubscriptionErrorKind::Protocol);
  CHECK(receivedError.status.code == Errc::Malformed);
  CHECK(receivedError.code == "INVALID_ACTIVE_PLAYER_COUNT_CHANGE");
  CHECK(receivedError.terminal);
  CHECK(handle.active());

  first->emitClose(1012, "service restart");
  waitFor([&] { return webSocket->connectionCount() == 2; });
  const auto second = webSocket->connection(1);
  acknowledge(second);
  const Json replay = parseSentText(second, 1);
  CHECK(replay["payload"]["operationName"].asString() ==
        "GameModelActivePlayerCountChanged");
  client.poll();
  CHECK_EQ(reconnects, std::size_t{1});
  CHECK_EQ(reconnectInfo.attempt, std::size_t{1});
  CHECK(reconnectInfo.operationId == "1");
  CHECK(handle.active());

  second->emitText(R"({"id":"1","type":"complete"})");
  CHECK(!handle.active());
  CHECK(!completed);
  client.poll();
  CHECK(completed);
}

void testTypedGameModelContainerChanged() {
  auto webSocket = std::make_shared<FakeTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.example.test";
  config.transport = std::make_shared<FakeHttpTransport>();
  config.webSocketTransport = webSocket;
  CrowdyClient client(std::move(config));

  bool called = false;
  domains::GameModelContainerChange received;
  domains::GameModelContainerChangedCallbacks callbacks;
  callbacks.next = [&](domains::GameModelContainerChange value) {
    called = true;
    received = std::move(value);
  };
  callbacks.error = [](GraphQLSubscriptionError) { CHECK(false); };
  auto handle = client.gameModel().containerChanged(
      "42", "SharedDoor", "session-1", std::move(callbacks));
  const auto connection = webSocket->connection(0);
  acknowledge(connection);
  const Json subscribe = parseSentText(connection, 1);
  CHECK(subscribe["payload"]["operationName"].asString() ==
        "GameModelContainerChanged");
  CHECK(subscribe["payload"]["variables"]["appId"].asString() == "42");
  CHECK(subscribe["payload"]["variables"]["typeName"].asString() ==
        "SharedDoor");

  connection->emitText(
      R"({"id":"1","type":"next","payload":{"data":{"gameModelContainerChanged":{"appId":"42","containerId":"container-1","typeName":"SharedDoor","sessionId":"session-1","source":"function","functionName":"open","changedKeys":["open"],"occurredAt":"2026-07-24T00:00:00.000Z"}}}})");
  CHECK(!called);
  client.poll();
  CHECK(called);
  CHECK(received.appId == "42");
  CHECK(received.containerId == "container-1");
  CHECK(received.typeName == "SharedDoor");
  CHECK_EQ(received.changedKeys.size(), std::size_t{1});
  CHECK(handle.active());
}

void testTypedAgentGraphqlSubscription() {
  auto webSocket = std::make_shared<FakeTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.example.test";
  config.transport = std::make_shared<FakeHttpTransport>();
  config.webSocketTransport = webSocket;
  CrowdyClient client(std::move(config));
  agent::CrowdyStudioAgentGraphQLTransport transport(
      client.crowdyStudioAgent(), client.subscriptions());

  bool called = false;
  bool failed = false;
  agent::AgentEvent event;
  auto handle = transport.subscribeEvents(
      {"session-1", "0", "1"},
      {[&](agent::AgentEvent value) {
         called = true;
         event = std::move(value);
       },
       [&](agent::AgentError) { failed = true; },
       [] {},
       [] {}});
  const auto connection = webSocket->connection(0);
  acknowledge(connection);
  const Json subscribe = parseSentText(connection, 1);
  CHECK(subscribe["payload"]["operationName"].asString() ==
        "CrowdyStudioAgentEvents");
  CHECK(subscribe["payload"]["variables"]["clientEpoch"].asString() == "1");

  connection->emitText(
      R"({"id":"1","type":"next","payload":{"data":{"crowdyStudioAgentEvents":{"__typename":"AgentLifecycleEvent","protocolVersion":"crowdy.agent-event/1","eventId":"event-1","sessionId":"session-1","seq":"1","type":"MODE_SELECTED","runId":null,"version":"crowdy.agent-event/1","createdAt":"2026-07-24T00:00:00.000Z","lifecycleMode":"ASK","lifecycleClientEpoch":null,"lifecycleReplayAfterSeq":null,"lifecycleReason":null,"lifecycleContextVersion":null}}}})");
  CHECK(!called);
  transport.poll();
  CHECK(called);
  CHECK(!failed);
  CHECK(event.seq == "1");
  CHECK(event.type == agent::AgentEventType::ModeSelected);

  bool cancelledDelivery = false;
  auto cancelled = transport.subscribeEvents(
      {"session-1", "1", "1"},
      {[&](agent::AgentEvent) { cancelledDelivery = true; },
       [&](agent::AgentError) { cancelledDelivery = true; },
       [&] { cancelledDelivery = true; },
       [&] { cancelledDelivery = true; }});
  connection->emitText(
      R"({"id":"2","type":"next","payload":{"data":{"crowdyStudioAgentEvents":{"__typename":"AgentLifecycleEvent","protocolVersion":"crowdy.agent-event/1","eventId":"event-2","sessionId":"session-1","seq":"2","type":"MODE_SELECTED","runId":null,"version":"crowdy.agent-event/1","createdAt":"2026-07-24T00:00:00.000Z","lifecycleMode":"ASK","lifecycleClientEpoch":null,"lifecycleReplayAfterSeq":null,"lifecycleReason":null,"lifecycleContextVersion":null}}}})");
  cancelled->close();
  transport.poll();
  CHECK(!cancelledDelivery);

  handle->close();
  client.poll();
}

void testControllerFactoryOwnsAdaptersAndPumpsTools() {
  auto webSocket = std::make_shared<FakeTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.example.test";
  config.transport = std::make_shared<FakeHttpTransport>();
  config.webSocketTransport = webSocket;
  CrowdyClient client(std::move(config));

  TickingBrowserDispatcher tools;
  agent::CrowdyStudioAgentControllerOptions options;
  options.sessionId = "session-1";
  options.browserDispatcher = &tools;
  auto runtime =
      client.createCrowdyStudioAgentController(std::move(options));
  CHECK(runtime);
  CHECK(runtime->controller().state().connection ==
        agent::AgentConnectionState::Disconnected);
  CHECK_EQ(runtime->poll(), std::size_t{0});
  CHECK_EQ(tools.ticks, 1);
  runtime.reset();
  CHECK_EQ(tools.cancellations, 1);
}

}  // namespace

int main() {
  testEndpointNormalization();
  testDefaultTransportAvailability();
  testReuseHttpAuthAndDispatcher();
  testHandshakeAuthParsePingAndDispatcher();
  testCancellationSuppressesQueuedDelivery();
  testCancellationSuppressesQueuedTerminalAndReconnectDelivery();
  testCancellationWaitsForInFlightDelivery();
  testServerCompleteAndCloseMapping();
  testReconnectReplayAndStaleConnectionFencing();
  testTerminalGraphqlErrors();
  testSubscriptionErrorsCarryBlameAndRetryAfterMs();
  testFrameAndUtf8Limits();
  testAcknowledgementTimeoutAndReconnectCap();
  testCrowdyClientInjectionAndPoll();
  testTypedGameModelActivePlayerCountChanged();
  testTypedGameModelContainerChanged();
  testTypedAgentGraphqlSubscription();
  testControllerFactoryOwnsAdaptersAndPumpsTools();
  std::printf("graphql_subscription_test passed\n");
  return 0;
}
