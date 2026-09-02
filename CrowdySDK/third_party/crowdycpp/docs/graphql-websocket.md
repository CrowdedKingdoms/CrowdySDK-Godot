# GraphQL WebSocket examples

CrowdyCPP implements `graphql-transport-ws`. The client authenticates in
`connection_init`, replays active operations after bounded reconnects, and
delivers every callback through the shared `Dispatcher`.

## Typed game-model changes

```cpp
crowdy::domains::GameModelContainerChangedCallbacks callbacks;
callbacks.next = [&](crowdy::domains::GameModelContainerChange change) {
  // Metadata-only push: pull caller-visible state after receipt.
  auto state = game.gameModel().containerState(
      change.appId, change.containerId);
  applyState(state);
};
callbacks.error = [](crowdy::graphql::GraphQLSubscriptionError error) {
  log(error.code, error.message);
};
callbacks.reconnect = [](crowdy::graphql::GraphQLReconnectInfo info) {
  // Durable reads remain authoritative after a best-effort feed reconnect.
};

auto changes = game.gameModel().containerChanged(
    appId, "Door", sessionId, std::move(callbacks));

while (running) game.poll();
// changes.cancel() is explicit; destruction also cancels.
```

## Durable Agentic Studio events

The lifetime-safe controller factory owns the generated HTTP transport and
typed event adapter together:

```cpp
crowdy::agent::CrowdyStudioAgentControllerOptions options;
options.sessionId = savedSessionId;
options.onStateChange = renderAgentState;

auto agent = game.createCrowdyStudioAgentController(std::move(options));
agent->controller().initialize();
while (running) agent->poll();
```

`CrowdyAgentGraphQLTransport::subscribeEvents` maps the committed
`CrowdyStudioAgentEvents` operation into typed `AgentEvent` values. On a
socket replacement it invokes the reconnect callback before replay, allowing
the controller to query durable history and fill gaps. The returned handle is
RAII-cancelled and suppresses already-queued callbacks after cancellation.

## Generic operations

Use `client.subscriptions()` only for GraphQL subscriptions without a typed
domain wrapper:

```cpp
crowdy::graphql::GraphQLSubscriptionCallbacks callbacks;
callbacks.onNext = [](crowdy::graphql::GraphQLSubscriptionOutcome outcome) {
  if (outcome.ok()) consume(outcome.data);
};

auto handle = game.subscriptions().subscribe(
    "subscription Watch($appId: BigInt!) { customFeed(appId: $appId) { id } }",
    crowdy::graphql::JVal::object({{"appId", appId}}),
    "Watch", std::move(callbacks));
```

Direct `GraphQLSubscriptionClient` construction treats its URL as an API base
by default. Set `endpointKind` to
`GraphQLWebSocketEndpointKind::Complete` for a custom route such as
`wss://host/subscriptions/`; its path, query, and trailing slash are preserved.
`CrowdyClient::wsEndpoint` selects this complete-endpoint behavior
automatically.

Engine integrations may inject `IWebSocketTransport`; libcurl 8.13+ WebSockets
are optional. Older curl versions are deliberately not enabled because their
fragmented-message metadata cannot satisfy this backend's reassembly contract.
A curl-free build retains the same API and reports a typed transport
unavailable error until an engine transport is supplied.
