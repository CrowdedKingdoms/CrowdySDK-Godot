# Native Agentic Studio API

CrowdyCPP exposes the portable `crowdy.studio-agent/1` runtime without a DOM,
provider client, unrestricted SDK bridge, or raw network/tool authority.

## Layers

- `client.crowdyStudioAgent()` is the exact generated-document GraphQL surface.
  Session, event, descriptor, budget, lease, approval, and run operations use
  the Game API. App policy, sanitized usage, and operator policy/kill
  operations require studio permissions.
- `crowdy::agent::CrowdyStudioAgentGraphQLTransport` maps those documents onto
  the typed `IAgentTransport` contract. Its `subscribeEvents` method uses the
  committed GraphQL-WS document and emits only typed durable events.
- `crowdy::agent::CrowdyStudioAgentController` owns attach epochs, durable
  replay, gap filling, acknowledgements, reducer state, policy/registry
  repinning, approvals, leases, budgets, heartbeat renewal, reconnect fencing,
  and late-result rejection.
- `canonicalAgentToolRegistryV1()` is the immutable canonical 28-tool Game API
  registry. Descriptor and registry SHA-256 values are checked against the
  installed CrowdyJS v12 fixture before use.

## Minimal native loop

```cpp
crowdy::agent::CrowdyStudioAgentControllerOptions options;
options.sessionId = savedSessionId;
options.onStateChange = [](const auto& state) {
  // Copy state into engine/UI models on the poll thread.
};

auto agent =
    client.createCrowdyStudioAgentController(std::move(options));
agent->controller().initialize();

while (running) {
  agent->poll();  // HTTP/WS, gap fill, timers, tools, reducer callbacks
}
```

Callbacks from HTTP, subscriptions, and host tool execution are fenced and
land through a `graphql::Dispatcher`; no engine object is touched on a network
thread. The controller has no internal worker thread.

## Realtime adapter contract

The GraphQL-WS integration implements `IAgentEventSubscriptionAdapter`.
`subscribe()` receives only:

- `sessionId`
- the exclusive `afterSeq` durable cursor
- the exact attached `clientEpoch`

It emits typed `AgentEvent` values and returns an RAII closeable handle.
Delivery may be at least once; the controller deduplicates, orders, fills gaps
through `history`, and acknowledges only contiguous sequences. A GraphQL-WS
reconnect callback starts durable gap fill before the operation replay.
Adapter callbacks land through the shared dispatcher.

When no realtime adapter is supplied, the controller owns a
`PollingAgentEventSubscriptionAdapter`. It replays the same durable history
from `poll()` and is suitable for deterministic tests or platforms awaiting a
WebSocket implementation.

## Studio and player-host seams

Native Studio and game hosts use
`NativeBrowserToolDispatcherAdapter`, the concrete
`IAgentBrowserToolDispatcher` bridge over `NativeToolDispatcherV1`:

- `dispatch(invocation, callback)` executes one exact descriptor-pinned
  `BROWSER` invocation through an allowlisted host service.
- `cancelActive(reason)` aborts pending host work synchronously on human or
  context preemption.
- `clearClosedSession()` may release execute-once records only after the
  session is closed/fenced.

The bridge revalidates descriptor digest, epoch, context, lease, approval,
input/output schema, deadline, and execute-once identity. It converts generic
event JSON field-by-field into closed native variants and canonicalizes the
typed result JSON. Ambiguous effects return `OUTCOME_UNKNOWN` and are never
retried. Once an effectful host dispatch starts, timeout, preemption, context
fencing, or invalid output also returns `OUTCOME_UNKNOWN`; validation and
other definitive failures detected before an effect remain `FAILED`. Studio
project edits and player commands route through the same intent services as
human actions. They never receive a raw `CrowdyClient`, GraphQL executor, UDP
connection, provider credential, shell, filesystem, or generic tool callback.

Studio should also provide:

- `beforeAgentWork(mode)` to flush autosave and fail a human turn on unresolved
  revision conflicts;
- `onPreempt(reason)` to stop local effects before best-effort server cleanup;
- `onLeaseChanged(lease)` to bind/release visible workspace or Play authority.

`CrowdyStudioControllerHostAdapter` and `CrowdyStudioIntegration` provide this
controller-backed native assembly, including final session/epoch/context/lease
checks, exact deployment-plan bindings, editor synchronization, and
destruction-safe ownership. LIVE invoke requires a final typed approval
validator because the Studio controller's deployment approval gate does not
authorize arbitrary runtime calls.

`crowdyStudioAgentRuntimeV1()`, `crowdyStudioAgentStateV1()`, and
`crowdyStudioAgentLocalDiagnosticsV1()` are the reference typed projections
from the headless Studio state into these exact local-tool outputs. The runtime
projection intentionally exposes the CrowdyJS common subset while the native
controller retains its content hash, selected module names, pairing
preference, and start timestamp for local safety checks.

Checkpoint events are observations, not mutation authority.
`crowdyStudioCheckpointEventFromAgentV1()` converts source-free durable
`AgentCheckpoint` metadata into a scope-fenced Studio event, and
`CrowdyStudioController::ingestCheckpointEvent()` verifies the open project
before updating state. The published GraphQL SDL has no generic
checkpoint-list, atomic-patch, or approved-restore root. Those controller
operations require an explicit durable `ICrowdyStudioSynchronizationProvider`;
approved restore additionally requires `ICrowdyStudioApprovalGate`. Missing
capabilities fail with `CrowdyStudioCapabilityUnavailableError`.

See [Native player-host integration](native-player-host.md) for a complete
construction and game-loop example, and
[Native Studio integration](native-studio-integration.md) for the editor and
assembly contracts.

## Safety controls

`pause`, `resume`, `cancelRun`, `stop`, `revokeLease`, and `close` are explicit
controller operations. `stop()` first preempts locally, then best-effort
revokes active leases, cancels the current run, and pauses the session.
Reattach allocates a newer epoch and drops callbacks/results from every older
subscription or tool dispatch.
