# Native player-host integration

`PlayerHostAdapterV1` is the only boundary that executes local game intent.
Implement it over the same movement, inventory, interaction, crafting, combat,
chat, and travel services used by human controls. Do not give it a
`CrowdyClient`, transport, socket, filesystem, shell, provider, or generic
tool executor.

```cpp
class MyPlayerHost final : public crowdy::player_host::PlayerHostAdapterV1 {
 public:
  void capabilities(crowdy::player_host::CancellationTokenV1 cancel,
                    crowdy::player_host::CapabilitiesCallbackV1 done) override;
  void observe(const crowdy::player_host::ObserveRequestV1& request,
               crowdy::player_host::CancellationTokenV1 cancel,
               crowdy::player_host::ObservationCallbackV1 done) override;
  void dispatch(const crowdy::player_host::GameCommandV1& command,
                const crowdy::player_host::ValidatedGateV1& gate,
                crowdy::player_host::CancellationTokenV1 cancel,
                crowdy::player_host::CommandCallbackV1 done) override;
  void clearAgentIntent(
      crowdy::player_host::PreemptionReasonV1 reason) noexcept override;
};
```

Wire the authority gate, typed native router, and controller adapter:

```cpp
MyPlayerHost host;
crowdy::player_host::AgentControlLeaseManager leases(
    host,
    {.context_version = [&] { return currentGameContextVersion(); }});

crowdy::agent::NativeToolDispatcherV1 nativeTools(
    leases, studioHost,
    {.session_id = [&] { return attachedSessionId(); },
     .client_epoch = [&] { return attachedClientEpoch(); },
     .context_version = [&] { return currentGameContextVersion(); },
     .mode = [&] { return currentNativeAgentMode(); },
     .is_lease_active = [&](std::string_view id, auto kind) {
       return visibleLeaseIsActive(id, kind);
     },
     .validate_argument_hash = validateCanonicalArgumentHash,
     .validate_approval_grant = validateExactApprovalGrant});

crowdy::agent::NativeBrowserToolDispatcherAdapter localTools(
    nativeTools);

crowdy::agent::CrowdyStudioAgentControllerOptions options;
options.sessionId = savedSessionId;
options.browserDispatcher = &localTools;
options.onEpochAttached = [&](std::string_view epoch) {
  leases.attach(std::string(epoch));
};
options.onLeaseChanged = [&](const crowdy::agent::AgentLease& lease) {
  applyVisiblePlayLease(leases, lease); // exact field/scope conversion
};

auto agent = client.createCrowdyStudioAgentController(std::move(options));
agent->controller().initialize();

while (running) {
  agent->poll(); // WS/HTTP delivery, controller timers, native tool deadlines
}
```

For a full native editor, `CrowdyStudioIntegration` owns this dispatcher chain
and optional Agent runtime while retaining an injected editor and typed
project/runtime services. It still borrows `AgentControlLeaseManager`; destroy
the integration before the manager and its `PlayerHostAdapterV1`. See
[Native Studio integration](native-studio-integration.md).

Human input must synchronously preempt local intent before any asynchronous
cleanup:

Wrap the lease manager and controller with `NativePlayerControlGate`. The
required fallback clear callback keeps Stop effective before Studio attaches
or after the network/controller is gone; construction rejects an empty
callback:

```cpp
crowdy::player_host::NativePlayerControlGateOptionsV1 gateOptions;
gateOptions.clock = &engineClock;
gateOptions.human_input_active_ms = 150;

crowdy::player_host::NativePlayerControlGate controlGate(
    [&](auto reason) { host.clearAgentIntent(reason); }, gateOptions);
auto unbindControl = controlGate.bind(leases, agent->controller());

auto unsubscribeControl = controlGate.subscribe([](const auto& state) {
  // state.bound, state.active_lease, state.last_preemption,
  // state.human_input_active, state.offline_stop
});
```

The engine calls the imperative hooks before continuing its normal human-input
path. They return `void`, never consume an input object, and never synthesize
input:

```cpp
void onKeyboard(const EngineKeyEvent& event) {
  controlGate.onHumanKeyboardInput(
      event.key == Key::Escape
          ? crowdy::player_host::NativePlayerControlKeyboardInputV1::Escape
          : crowdy::player_host::NativePlayerControlKeyboardInputV1::Input);
  gameplayKeyboard(event); // Always receives the original event.
}

void onPointerDown(const EnginePointerEvent& event) {
  controlGate.onHumanPointerInput();
  gameplayPointerDown(event);
}

void onLookOrMovementInput(const EngineInputEvent& event) {
  controlGate.onHumanMovementInput();
  gameplayMovement(event);
}

// Engine lifecycle and authority notifications:
controlGate.onBackgrounded();          // pagehide/hidden native equivalent
controlGate.onDisconnected();
controlGate.onClientReattached();
controlGate.onDeath();
controlGate.onContextChanged();
controlGate.onPermissionChanged();
controlGate.onAdmissionChanged();
controlGate.onControlTargetChanged();

// User controls:
controlGate.pause();
controlGate.stop(); // Clears locally even with no controller/network.
```

The active-input window defaults to 150 ms and uses the injected monotonic
clock. `snapshot()` and `humanInputActive()` evaluate it on demand, matching
CrowdyJS 12.1; the gate has no timer thread and needs no `tick()`. The existing
lease manager/tool dispatcher still needs its normal game-thread pump.

For every takeover, local intent and the local lease are cleared before any
best-effort remote revoke, Pause, or Stop. A locally revoked lease id is
suppressed even if stale controller state still reports it as active, so
network errors cannot restore control or trigger duplicate remote revocation.
Controller calls, state access, preemption observers, and state listeners are
exception-isolated from imperative safety hooks.

`bind()` also accepts `INativePlayerControlGateController` for engine-owned
controller wrappers. The returned unbind function is pair-specific: an old
unbind cannot detach a replacement manager/controller pair. Keep both bound
objects alive until unbinding; declare the unbind handle after them or call it
explicitly before either is destroyed. Gate destruction performs a final local
`DISCONNECTED` preemption, unbinds, and makes outstanding unsubscribe/unbind
functions harmless.

`crowdyjs-player-control-gate.v1.json` is generated from the exact pinned
CrowdyJS build. The native fixture test replays its initial/bound snapshots,
listener and local-clear ordering, 150 ms boundary, rebind/unbind transitions,
offline Stop, every imperative hook, and all 16 preemption reasons.

`NativeBrowserToolDispatcherAdapter` validates canonical input schemas,
converts JSON field-by-field into closed C++ variants, propagates
cancellation, timing, context, output, and typed errors, and converts output
back to canonical validated JSON. Unknown/server/provider tools fail closed.
Call `clearClosedSession()` only after the attached session is closed or
fenced; otherwise an effect could execute twice.
