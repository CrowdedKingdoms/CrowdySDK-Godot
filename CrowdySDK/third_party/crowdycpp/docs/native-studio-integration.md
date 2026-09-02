# Native Crowdy Studio integration

CrowdyCPP supplies the headless Studio state machine and narrow native
integration contracts. Engines own the window, panes, text editor, language
service, renderer, and input system.

## Editor boundary

Implement `crowdy::studio::ICrowdyStudioEditorAdapter`. The adapter receives
only:

- synchronized in-memory project, personal-library, and common-file buffers;
- the selected file and ordered open-file set;
- callbacks for project-buffer edits, diagnostics, open, close, and failure;
- relayout and disposal notifications.

`CrowdyStudioEditorBridge` connects those callbacks to the same
`CrowdyStudioController::updateFile`, `openFile`, `closeFile`, and
`setLocalDiagnostics` paths used by a human UI. Reference buffers are marked
read-only. The interface has no filesystem, shell, GraphQL, network, or
`CrowdyClient` handle.

```cpp
class EngineEditor final
    : public crowdy::studio::ICrowdyStudioEditorAdapter {
 public:
  crowdy::studio::CrowdyStudioEditorMode mode() const noexcept override;
  void setCallbacks(
      crowdy::studio::CrowdyStudioEditorCallbacks callbacks) override;
  void synchronize(
      const crowdy::studio::CrowdyStudioEditorSnapshot& snapshot) override;
  void relayout() override;
  void dispose() noexcept override;
};
```

Editor callbacks run on the engine thread chosen by the adapter. Invoke them
on the game/UI thread because `CrowdyStudioController` is single-threaded.
Callbacks retained after `dispose()` are fenced.

## Complete assembly

`CrowdyClient::createCrowdyStudioIntegration` constructs owned project and
PlayerCompute adapters, the Studio runtime/controller, editor bridge,
controller-backed Studio host, native dispatcher, browser-dispatch bridge, and
optional durable Agent controller in dependency-safe order.

```cpp
crowdy::studio::CrowdyStudioIntegrationOptions options;
options.studio = {.appId = appId, .gridId = gridId};
options.editor = std::make_shared<EngineEditor>();
options.clientRuntime = engineClientArtifactRuntime;
options.layoutStorage = engineSettingsStore;
options.playerHost = &enginePlayerHost;  // externally owned, outlives assembly
options.crypto = engineCryptoOwner;    // shared ownership

crowdy::agent::CrowdyStudioAgentControllerOptions agent;
agent.sessionId = savedSessionId;
options.agent = std::move(agent);       // omit for manual Studio only

auto studio =
    client.createCrowdyStudioIntegration(std::move(options));
studio->initialize();

while (running) {
  studio->poll();  // nonblocking callbacks + Agent/native deadlines
  if (engineStudioMaintenancePhase) {
    studio->runStudioMaintenance();  // save/HTTP/compile polling may block
  }
}
```

`poll()` always pumps both the client/platform dispatcher and the optional
Agent runtime. It never runs Studio autosave, monitor HTTP, compile polling, or
sleep. `runStudioMaintenance()` is the explicit serialized maintenance lane.
When Agent HTTP has no injected async transport, `CrowdyClient` installs a
single worker-thread adapter around `IHttpTransport`; custom synchronous
transports must therefore honor the interface's thread-safety requirement.
Call it from a deliberately blocking engine phase, or from a worker only when
all Studio controller access is serialized onto that worker; never run it
concurrently with editor/controller access. Potentially blocking native Studio
tools use the same lane by default; advanced hosts can inject
`studioHost.schedule` to route them onto their own serial executor.

Keep `CrowdyClient` open while the assembly uses its shared HTTP/GraphQL
dispatcher. Destroying the client first remains memory-safe but closes those
shared transports. An externally injected raw crypto provider is not assumed
owned: pass `options.crypto` so the assembly retains it.

Lower-level hosts can call `CrowdyStudioIntegration::create` with owned
`ICrowdyStudioProjectProvider` and `ICrowdyStudioRuntime` implementations.
The existing direct constructors remain available.

[`examples/native_studio_shell.cpp`](../examples/native_studio_shell.cpp) is a
credential-free, engine-neutral wiring example. It uses an in-memory editor and
layout store, a typed intent-only player host, explicit nonblocking and
maintenance lanes, input forwarding through `NativePlayerControlGate`, and
ordered disposal without granting DOM, filesystem, or raw GraphQL authority to
an adapter.

## Native Studio tools

`CrowdyStudioControllerHostAdapter` implements all 11 native Studio tools:

- `studio.context.get`, `studio.state.get`, `project.select`;
- `workspace.tab.open`, `workspace.tab.close`, `diagnostics.local.get`;
- `runtime.status.get`, `runtime.test_draft`, `runtime.deploy_live`;
- `runtime.invoke`, `runtime.stop`.

Session, epoch, context, lease, cancellation, and approval metadata are
revalidated at the final host boundary. Draft/live plans use the controller's
current complete plan; live deployment additionally binds the exact revision,
target set, pairing preference, and project content hash. LIVE invoke requires
an explicit final approval validator. Provider/gate failures before an external
effect are `FAILED`. Only an operation that reached its exact effect boundary
can become `OUTCOME_UNKNOWN`; draft submission, deploy, invoke, stop, and
restore all use that fencing rule. Those results are never retried.

`crowdyjs-studio-host-tools.v1.json` is generated by executing all 11 handlers
from the exact pinned CrowdyJS build. The native fixture test replays every
typed input and setup transition through the concrete controller host and
compares canonical output/status projections, including required approval,
pre-dispatch cancellation, and effectful outcome-unknown fencing.

## Concrete layout, wallet, and control ownership

The integration owns `StudioLayoutController`, its exact
`AgentControlLeaseManager` on the `playerHost` path, and
`NativePlayerControlGate`. Engines inject layout storage and the typed player
host, then forward keyboard, pointer, movement, lifecycle, permission, and
controlled-entity events through `controlGate()`. Use `layout()`,
`layoutSnapshot()`, `leaseSnapshot()`, and `controlSnapshot()` for typed state.
The gate is unbound and destroyed before the optional Agent controller; native
dispatch is destroyed before the lease manager and Studio controller.

`CrowdyClient::createCrowdyStudioIntegration()` also installs the owned
read-only PlayerWallet adapter when `observePlayerWallet` is true (the
default). Supply `walletProvider` to override it or set the flag false to omit
wallet observation. Wallet read failures only clear the optional snapshot;
they never block editing, saving, compilation, or deployment.

