#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

bool decimalAtLeast(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return left.size() > right.size();
  return left >= right;
}

class AgentRuntimeHarness {
 public:
  virtual ~AgentRuntimeHarness() = default;
  virtual agent::CrowdyStudioAgentController& controller() = 0;
  virtual std::size_t poll() = 0;
};

class WebSocketAgentRuntimeHarness final : public AgentRuntimeHarness {
 public:
  explicit WebSocketAgentRuntimeHarness(
      std::unique_ptr<agent::CrowdyStudioAgentControllerRuntime> runtime)
      : runtime_(std::move(runtime)) {}
  agent::CrowdyStudioAgentController& controller() override {
    return runtime_->controller();
  }
  std::size_t poll() override { return runtime_->poll(); }

 private:
  std::unique_ptr<agent::CrowdyStudioAgentControllerRuntime> runtime_;
};

class PollingAgentRuntimeHarness final : public AgentRuntimeHarness {
 public:
  PollingAgentRuntimeHarness(
      domains::CrowdyStudioAgentAPI& api,
      agent::CrowdyStudioAgentControllerOptions options)
      : transport_(api),
        controller_(bind(api, std::move(options), transport_)) {}
  agent::CrowdyStudioAgentController& controller() override {
    return controller_;
  }
  std::size_t poll() override { return controller_.poll(); }

 private:
  static agent::CrowdyStudioAgentControllerOptions bind(
      domains::CrowdyStudioAgentAPI& api,
      agent::CrowdyStudioAgentControllerOptions options,
      agent::CrowdyStudioAgentGraphQLTransport& transport) {
    options.transport = &transport;
    options.subscriptionAdapter = nullptr;
    if (!options.dispatcher) options.dispatcher = api.dispatcher();
    return options;
  }

  agent::CrowdyStudioAgentGraphQLTransport transport_;
  agent::CrowdyStudioAgentController controller_;
};

std::unique_ptr<AgentRuntimeHarness> createAgentRuntime(
    CrowdyClient& game, agent::CrowdyStudioAgentControllerOptions options) {
  if (game.config().webSocketTransport ||
      graphql::curlWebSocketTransportAvailable()) {
    return std::make_unique<WebSocketAgentRuntimeHarness>(
        game.createCrowdyStudioAgentController(std::move(options)));
  }
  return std::make_unique<PollingAgentRuntimeHarness>(
      game.crowdyStudioAgent(), std::move(options));
}

template <typename Predicate>
bool pumpUntil(AgentRuntimeHarness& runtime,
               Predicate&& done, int timeoutMs = 10'000) {
  const auto started = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - started <
         std::chrono::milliseconds(timeoutMs)) {
    runtime.poll();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  runtime.poll();
  return done();
}

template <typename Start>
bool waitForAgentVoid(AgentRuntimeHarness& runtime,
                      Start&& start, const char* context,
                      int timeoutMs = 10'000) {
  bool finished = false;
  std::optional<agent::AgentError> failure;
  std::forward<Start>(start)(
      [&](agent::AgentOutcome<agent::AgentVoid> outcome) {
        if (!outcome.ok()) failure = *outcome.error;
        finished = true;
      });
  if (!pumpUntil(runtime, [&] { return finished; }, timeoutMs)) {
    std::fprintf(stderr, "%s timed out\n", context);
    return false;
  }
  if (failure) {
    e2e::printAgentError(*failure, context);
    return false;
  }
  return true;
}

agent::CrowdyStudioAgentControllerOptions createOptions(
    agent::AgentMode mode, std::string_view appId, std::string_view suffix,
    std::optional<std::string> projectId = std::nullopt,
    std::optional<std::string> gridId = std::nullopt) {
  const std::string modeName(agent::toString(mode));
  agent::AgentCreateSessionInput input;
  input.appId = appId;
  input.projectId = std::move(projectId);
  input.gridId = std::move(gridId);
  input.mode = mode;
  input.providerDataConsent = false;
  input.idempotencyKey =
      "cpp-agent-create-" + modeName + "-" + std::string(suffix);

  agent::CrowdyStudioAgentControllerOptions options;
  options.createSession = std::move(input);
  options.clientInstanceId = e2e::clientInstanceUuid();
  options.heartbeatIntervalMs = 100;
  options.heartbeatStaleMs = 2'000;
  options.workspaceRenewIntervalMs = 1'000;
  options.createIdempotencyKey =
      [prefix = "cpp-agent-" + modeName + "-" + std::string(suffix),
       sequence = 0U](std::string_view operation) mutable {
        return prefix + "-" + std::string(operation) + "-" +
               std::to_string(++sequence);
      };
  return options;
}

agent::CrowdyStudioAgentControllerOptions attachOptions(
    std::string sessionId, std::string_view suffix) {
  agent::CrowdyStudioAgentControllerOptions options;
  options.sessionId = std::move(sessionId);
  options.clientInstanceId = e2e::clientInstanceUuid();
  options.heartbeatIntervalMs = 100;
  options.heartbeatStaleMs = 2'000;
  options.workspaceRenewIntervalMs = 1'000;
  options.createIdempotencyKey =
      [prefix = "cpp-agent-replay-" + std::string(suffix),
       sequence = 0U](std::string_view operation) mutable {
        return prefix + "-" + std::string(operation) + "-" +
               std::to_string(++sequence);
      };
  return options;
}

class PendingPlayerHost final : public player_host::PlayerHostAdapterV1 {
 public:
  std::vector<std::string> events;
  std::optional<player_host::PreemptionReasonV1> clearedReason;
  player_host::CapabilitiesCallbackV1 pendingCapabilities;
  player_host::CancellationTokenV1 cancellation;

  void capabilities(
      player_host::CancellationTokenV1 token,
      player_host::CapabilitiesCallbackV1 callback) override {
    events.push_back("dispatch");
    cancellation = token;
    pendingCapabilities = std::move(callback);
  }

  void observe(const player_host::ObserveRequestV1&,
               player_host::CancellationTokenV1,
               player_host::ObservationCallbackV1 callback) override {
    player_host::AgentErrorV1 error;
    error.code = "AGENT_HOST_UNAVAILABLE";
    error.message = "unexpected observe in cancellation e2e";
    callback(player_host::AdapterResultV1<
             player_host::GameObservationV1>::failure(
        std::move(error)));
  }

  void dispatch(const player_host::GameCommandV1&,
                const player_host::ValidatedGateV1&,
                player_host::CancellationTokenV1,
                player_host::CommandCallbackV1 callback) override {
    player_host::AgentErrorV1 error;
    error.code = "AGENT_HOST_UNAVAILABLE";
    error.message = "unexpected command in cancellation e2e";
    callback(player_host::AdapterResultV1<
             player_host::GameCommandResultV1>::failure(
        std::move(error)));
  }

  void clearAgentIntent(
      player_host::PreemptionReasonV1 reason) noexcept override {
    events.push_back("clear");
    clearedReason = reason;
  }
};

const agent::NativeLocalToolContractV1* nativeContract(
    std::string_view name) {
  const auto contracts = agent::nativeLocalToolContractsV1();
  const auto found = std::find_if(
      contracts.begin(), contracts.end(),
      [&](const auto& value) { return value.name == name; });
  return found == contracts.end() ? nullptr : &*found;
}

}  // namespace

int main() try {
  const auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  e2e::requireFlag("CROWDY_E2E_AGENT");
  const std::string projectId =
      e2e::envOr("CROWDY_E2E_AGENT_PROJECT_ID");
  const std::string gridId = e2e::envOr("CROWDY_E2E_STUDIO_GRID_ID");
  if (projectId.empty() || gridId.empty()) {
    std::puts(
        "CROWDY_E2E_AGENT_PROJECT_ID/STUDIO_GRID_ID not configured; skipping");
    return 77;
  }

  auto& game = e2e::ownerGame(cfg);
  if (!game.config().webSocketTransport &&
      !graphql::curlWebSocketTransportAvailable()) {
    std::puts(
        "No default GraphQL WebSocket transport is available; exercising the "
        "controller's durable HTTP polling fallback");
  }
  const std::string suffix = e2e::runSuffix();

  E2E_SUBTEST("controller factory creates, attaches, replays, and heartbeats");
  std::string liveSessionId;
  std::string liveClientEpoch;
  std::string liveContextVersion;
  std::optional<player_host::AgentErrorV1> hostAttachError;
  PendingPlayerHost host;
  player_host::AgentControlLeaseManagerOptionsV1 managerOptions;
  managerOptions.context_version = [&] { return liveContextVersion; };
  player_host::AgentControlLeaseManager manager(host,
                                                 std::move(managerOptions));

  agent::NativeToolDispatcherOptionsV1 nativeOptions;
  nativeOptions.session_id = [&]() -> std::optional<std::string> {
    return liveSessionId.empty()
               ? std::nullopt
               : std::optional<std::string>(liveSessionId);
  };
  nativeOptions.client_epoch = [&]() -> std::optional<std::string> {
    return liveClientEpoch.empty()
               ? std::nullopt
               : std::optional<std::string>(liveClientEpoch);
  };
  nativeOptions.context_version = [&] { return liveContextVersion; };
  nativeOptions.mode = [] { return agent::NativeAgentModeV1::Ask; };
  nativeOptions.validate_argument_hash = [](const auto&) { return true; };
  agent::NativeToolDispatcherV1 nativeDispatcher(
      manager, nullptr, std::move(nativeOptions));
  agent::NativeBrowserToolDispatcherAdapter browserDispatcher(
      nativeDispatcher);

  auto askOptions =
      createOptions(agent::AgentMode::Ask, cfg.appId, suffix + "-ask");
  E2E_CHECK(askOptions.clientInstanceId.size() == 36);
  E2E_CHECK(askOptions.clientInstanceId[14] == '4');
  E2E_CHECK(askOptions.clientInstanceId[19] == '8' ||
            askOptions.clientInstanceId[19] == '9' ||
            askOptions.clientInstanceId[19] == 'a' ||
            askOptions.clientInstanceId[19] == 'b');
  askOptions.browserDispatcher = &browserDispatcher;
  askOptions.onStateChange =
      [&](const agent::CrowdyStudioAgentState& state) {
        if (!state.session) return;
        liveSessionId = state.session->sessionId;
        liveContextVersion = state.session->contextVersion;
      };
  askOptions.onEpochAttached = [&](std::string_view epoch) {
    liveClientEpoch = epoch;
    hostAttachError = manager.attach(liveClientEpoch);
  };

  auto askRuntime = createAgentRuntime(game, std::move(askOptions));
  E2E_CHECK(waitForAgentVoid(
      *askRuntime,
      [&](auto callback) {
        askRuntime->controller().initialize(std::move(callback));
      },
      "initialize ASK controller"));
  if (hostAttachError) {
    std::fprintf(stderr, "native host attach: code=%s message=%s\n",
                 hostAttachError->code.c_str(),
                 hostAttachError->message.c_str());
  }
  E2E_CHECK(!hostAttachError);
  E2E_CHECK(askRuntime->controller().state().connection ==
            agent::AgentConnectionState::Connected);
  E2E_CHECK(askRuntime->controller().state().session.has_value());
  E2E_CHECK(askRuntime->controller().state().session->mode ==
            agent::AgentMode::Ask);

  E2E_SUBTEST(
      "live epoch binds native dispatcher cancellation to human takeover");
  const auto* capabilityContract = nativeContract("game.capabilities.get");
  E2E_CHECK(capabilityContract != nullptr);
  agent::NativeToolInvocationV1 capabilityCall;
  capabilityCall.session_id = liveSessionId;
  capabilityCall.run_id = "cpp-e2e-local-run-" + suffix;
  capabilityCall.tool_call_id = "cpp-e2e-capabilities-" + suffix;
  capabilityCall.name = std::string(capabilityContract->name);
  capabilityCall.version = std::string(capabilityContract->version);
  capabilityCall.descriptor_digest =
      std::string(capabilityContract->descriptor_digest);
  capabilityCall.arguments = agent::NoArgumentsV1{};
  capabilityCall.argument_hash =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  capabilityCall.context_version = liveContextVersion;
  capabilityCall.client_epoch = liveClientEpoch;
  capabilityCall.deadline = "2099-01-01T00:00:00.000Z";

  std::optional<agent::NativeToolResultV1> cancelledCapability;
  nativeDispatcher.dispatch(
      std::move(capabilityCall),
      [&](agent::NativeToolResultV1 result) {
        host.events.push_back("callback");
        cancelledCapability = std::move(result);
      });
  E2E_CHECK(!cancelledCapability);
  E2E_CHECK(host.events.size() == 1 && host.events[0] == "dispatch");
  askRuntime->controller().preemptForHumanEdit();
  E2E_CHECK(cancelledCapability.has_value());
  E2E_CHECK(cancelledCapability->status ==
            agent::NativeToolResultStatusV1::Cancelled);
  E2E_CHECK(cancelledCapability->error.has_value());
  E2E_CHECK(cancelledCapability->error->code == "AGENT_CANCELLED");
  E2E_CHECK(host.cancellation.cancelled());
  E2E_CHECK(host.clearedReason ==
            player_host::PreemptionReasonV1::HUMAN_EDIT);
  E2E_CHECK(host.events.size() >= 3);
  E2E_CHECK(host.events[1] == "clear");
  E2E_CHECK(host.events[2] == "callback");

  const std::string firstEpoch =
      *askRuntime->controller().state().clientEpoch;
  E2E_CHECK(waitForAgentVoid(
      *askRuntime,
      [&](auto callback) {
        askRuntime->controller().reconnect(std::move(callback));
      },
      "reattach ASK controller"));
  const std::string reattachedEpoch =
      *askRuntime->controller().state().clientEpoch;
  E2E_CHECK(reattachedEpoch != firstEpoch);
  E2E_CHECK(decimalAtLeast(reattachedEpoch, firstEpoch));
  E2E_CHECK(waitForAgentVoid(
      *askRuntime,
      [&](auto callback) {
        askRuntime->controller().resume(std::move(callback));
      },
      "resume preempted ASK controller"));

  E2E_CHECK(pumpUntil(
      *askRuntime,
      [&] {
        return askRuntime->controller().state().lastContiguousSeq != "0";
      },
      5'000));
  const std::string replayFloor =
      askRuntime->controller().state().lastContiguousSeq;
  const std::string askSessionId =
      askRuntime->controller().state().session->sessionId;
  askRuntime.reset();

  auto replayOptions = attachOptions(askSessionId, suffix + "-ask");
  auto replayRuntime = createAgentRuntime(game, std::move(replayOptions));
  E2E_CHECK(waitForAgentVoid(
      *replayRuntime,
      [&](auto callback) {
        replayRuntime->controller().initialize(std::move(callback));
      },
      "initialize replay controller"));
  E2E_CHECK(replayRuntime->controller().state().connection ==
            agent::AgentConnectionState::Connected);
  E2E_CHECK(replayRuntime->controller().state().clientEpoch.has_value());
  E2E_CHECK(*replayRuntime->controller().state().clientEpoch !=
            reattachedEpoch);
  E2E_CHECK(decimalAtLeast(
      replayRuntime->controller().state().lastContiguousSeq, replayFloor));
  E2E_CHECK(waitForAgentVoid(
      *replayRuntime,
      [&](auto callback) {
        replayRuntime->controller().resume(std::move(callback));
      },
      "resume replay-attached ASK controller"));

  if (e2e::envFlag("CROWDY_E2E_AGENT_RUN")) {
    bool finished = false;
    std::optional<agent::AgentError> failure;
    std::optional<agent::AgentRun> run;
    replayRuntime->controller().sendMessage(
        "Reply with one short readiness sentence.",
        [&](agent::AgentOutcome<agent::AgentRun> outcome) {
          if (outcome.ok()) {
            run = std::move(*outcome.value);
          } else {
            failure = *outcome.error;
          }
          finished = true;
        });
    E2E_CHECK(pumpUntil(*replayRuntime, [&] { return finished; }, 30'000));
    if (failure) e2e::printAgentError(*failure, "send ASK message");
    E2E_CHECK(!failure);
    E2E_CHECK(run && !run->runId.empty());
  }
  E2E_CHECK(waitForAgentVoid(
      *replayRuntime,
      [&](auto callback) {
        replayRuntime->controller().close(std::move(callback));
      },
      "close ASK controller"));

  E2E_SUBTEST("controller factory binds BUILD to an exact saved project");
  auto buildOptions = createOptions(
      agent::AgentMode::Build, cfg.appId, suffix + "-build", projectId,
      gridId);
  auto buildRuntime = createAgentRuntime(game, std::move(buildOptions));
  E2E_CHECK(waitForAgentVoid(
      *buildRuntime,
      [&](auto callback) {
        buildRuntime->controller().initialize(std::move(callback));
      },
      "initialize BUILD controller"));
  E2E_CHECK(buildRuntime->controller().state().session->mode ==
            agent::AgentMode::Build);
  E2E_CHECK(buildRuntime->controller().state().session->projectId ==
            projectId);
  E2E_CHECK(waitForAgentVoid(
      *buildRuntime,
      [&](auto callback) {
        buildRuntime->controller().close(std::move(callback));
      },
      "close BUILD controller"));

  if (e2e::envFlag("CROWDY_E2E_AGENT_PLAY")) {
    const std::string controlled =
        e2e::envOr("CROWDY_E2E_AGENT_CONTROLLED_ENTITY_ID");
    const std::string capability =
        e2e::envOr("CROWDY_E2E_AGENT_HOST_CAPABILITY_REVISION");
    if (controlled.empty() || capability.empty()) {
      std::puts(
          "Play host entity/capability vars missing; skipping Play subtest");
    } else {
      E2E_SUBTEST(
          "controller factory grants Play then performs takeover revoke");
      auto playOptions = createOptions(
          agent::AgentMode::Play, cfg.appId, suffix + "-play", std::nullopt,
          gridId);
      auto playRuntime = createAgentRuntime(game, std::move(playOptions));
      E2E_CHECK(waitForAgentVoid(
          *playRuntime,
          [&](auto callback) {
            playRuntime->controller().initialize(std::move(callback));
          },
          "initialize PLAY controller"));

      bool granted = false;
      std::optional<agent::AgentError> grantFailure;
      std::optional<agent::AgentLease> lease;
      playRuntime->controller().grantPlayLease(
          {"observe", "locomotion"}, 30, controlled, capability,
          [&](agent::AgentOutcome<agent::AgentLease> outcome) {
            if (outcome.ok()) {
              lease = std::move(*outcome.value);
            } else {
              grantFailure = *outcome.error;
            }
            granted = true;
          });
      E2E_CHECK(pumpUntil(*playRuntime, [&] { return granted; }));
      if (grantFailure) {
        e2e::printAgentError(*grantFailure, "grant Play lease");
      }
      E2E_CHECK(!grantFailure);
      E2E_CHECK(lease &&
                lease->status == agent::AgentLeaseStatus::Active);
      E2E_CHECK(pumpUntil(
          *playRuntime,
          [&] {
            return playRuntime->controller()
                .state()
                .lastHeartbeatAt.has_value();
          },
          5'000));

      E2E_CHECK(waitForAgentVoid(
          *playRuntime,
          [&](auto callback) {
            playRuntime->controller().revokeLease(
                lease->leaseId, agent::AgentPreemptionReason::HumanInput,
                std::move(callback));
          },
          "revoke Play lease for human takeover"));
      E2E_CHECK(waitForAgentVoid(
          *playRuntime,
          [&](auto callback) {
            playRuntime->controller().close(std::move(callback));
          },
          "close PLAY controller"));
    }
  }

  if (e2e::envFlag("CROWDY_E2E_AGENT_POLICY_KILL")) {
    if (cfg.operatorEmail.empty()) {
      std::puts("CROWDY_E2E_OPERATOR_EMAIL missing; skipping kill subtest");
    } else {
      E2E_SUBTEST("publish and release an operator policy kill");
      auto operatorClient =
          e2e::identityClient(cfg, cfg.operatorEmail);
      auto& operatorApi = operatorClient->crowdyStudioAgent();
      graphql::JVal kill;
      kill["appId"] = cfg.appId;
      kill["killed"] = true;
      kill["reasonCode"] = "AGENT_OPERATOR_KILLED";
      kill["reason"] = "CrowdyCPP explicit e2e policy-kill check";
      kill["idempotencyKey"] = "cpp-agent-kill-" + suffix;
      const bool killed =
          operatorApi.setOperatorAppKill(kill)["operatorKillSwitch"]
              .asBool(false);

      graphql::JVal release;
      release["appId"] = cfg.appId;
      release["killed"] = false;
      release["reasonCode"] = "AGENT_OPERATOR_KILLED";
      release["reason"] = "CrowdyCPP e2e policy-kill release";
      release["idempotencyKey"] = "cpp-agent-kill-release-" + suffix;
      const bool released =
          !operatorApi.setOperatorAppKill(release)["operatorKillSwitch"]
               .asBool(true);
      E2E_CHECK(killed);
      E2E_CHECK(released);
    }
  }

  std::puts("e2e_agentic_studio passed");
  return 0;
} catch (const graphql::CrowdyGraphQLError& error) {
  e2e::printGraphQLError(error, "Agentic Studio GraphQL error");
  return 1;
} catch (const agent::CrowdyAgentError& error) {
  e2e::printAgentError(error.value(), "Agentic Studio controller error");
  return 1;
} catch (const graphql::CrowdyError& error) {
  std::fprintf(stderr, "SDK error: code=%s message=%s\n",
               error.code().c_str(), error.what());
  return 1;
} catch (const std::exception& error) {
  std::fprintf(stderr, "exception: %s\n", error.what());
  return 1;
}
