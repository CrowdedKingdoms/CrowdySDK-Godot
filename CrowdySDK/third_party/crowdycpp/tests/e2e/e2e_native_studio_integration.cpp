#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

#define LIVE_CHECK(condition)                                                \
  do {                                                                       \
    if (!(condition)) {                                                      \
      throw std::runtime_error(                                              \
          std::string("live check failed: ") + #condition);                 \
    }                                                                        \
  } while (false)

constexpr const char* kCargoToml = R"toml([package]
name = "crowdy-native-studio-e2e"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]
)toml";

studio::CrowdyStudioProjectFile projectFile(
    std::string path, std::string content) {
  studio::CrowdyStudioProjectFile file;
  file.target = studio::CrowdyStudioTarget::Server;
  file.path = std::move(path);
  file.content = std::move(content);
  return file;
}

class LiveEditor final : public studio::ICrowdyStudioEditorAdapter {
 public:
  studio::CrowdyStudioEditorMode mode() const noexcept override {
    return studio::CrowdyStudioEditorMode::Text;
  }

  void setCallbacks(
      studio::CrowdyStudioEditorCallbacks callbacks) override {
    callbacks_ = std::move(callbacks);
  }

  void synchronize(
      const studio::CrowdyStudioEditorSnapshot& snapshot) override {
    snapshot_ = snapshot;
  }

  void relayout() override {}

  void dispose() noexcept override {
    disposed_ = true;
    callbacks_ = {};
    snapshot_ = {};
  }

  void edit(studio::CrowdyStudioTarget target, std::string path,
            std::string content) {
    LIVE_CHECK(callbacks_.onProjectFileChange);
    callbacks_.onProjectFileChange(
        target, std::move(path), std::move(content));
  }

  bool disposed() const noexcept { return disposed_; }

 private:
  studio::CrowdyStudioEditorCallbacks callbacks_;
  studio::CrowdyStudioEditorSnapshot snapshot_;
  bool disposed_ = false;
};

class LivePlayerHost final : public player_host::PlayerHostAdapterV1 {
 public:
  LivePlayerHost(std::string controlledEntity, std::string revision)
      : controlledEntity_(std::move(controlledEntity)),
        revision_(std::move(revision)) {}

  void capabilities(
      player_host::CancellationTokenV1 cancellation,
      player_host::CapabilitiesCallbackV1 callback) override {
    if (cancellation.cancelled()) {
      callback(failure<player_host::PlayerHostCapabilitiesV1>(
          "AGENT_CANCELLED", "capability request was cancelled"));
      return;
    }
    player_host::PlayerHostCapabilitiesV1 capabilities;
    capabilities.game_id = "crowdycpp-native-studio-e2e";
    capabilities.revision = revision_;
    capabilities.controlled_entity_id = controlledEntity_;
    capabilities.commands = {
        {.kind = player_host::CommandKindV1::Move,
         .tool_name = "game.control.move",
         .required_scope = player_host::LeaseScopeV1::Locomotion,
         .risk = player_host::ToolRiskV1::WORLD_CONTROL,
         .approval = player_host::ApprovalPolicyV1::None,
         .rate_limit_per_second = 30},
        {.kind = player_host::CommandKindV1::Stop,
         .tool_name = "game.control.stop",
         .required_scope = std::nullopt,
         .risk = player_host::ToolRiskV1::WORLD_CONTROL,
         .approval = player_host::ApprovalPolicyV1::None,
         .rate_limit_per_second = 100},
    };
    capabilities.observation.max_age_ms = 2'000;
    capabilities.observation.max_nearby_actors = 8;
    capabilities.observation.max_nearby_voxels = 8;
    capabilities.advertised_at = "2026-07-24T00:00:00.000Z";
    callback(player_host::AdapterResultV1<
             player_host::PlayerHostCapabilitiesV1>::success(
        std::move(capabilities)));
  }

  void observe(
      const player_host::ObserveRequestV1&,
      player_host::CancellationTokenV1,
      player_host::ObservationCallbackV1 callback) override {
    callback(failure<player_host::GameObservationV1>(
        "AGENT_HOST_UNAVAILABLE",
        "observation is outside this integration lifecycle test"));
  }

  void dispatch(
      const player_host::GameCommandV1& command,
      const player_host::ValidatedGateV1&,
      player_host::CancellationTokenV1 cancellation,
      player_host::CommandCallbackV1 callback) override {
    if (cancellation.cancelled()) {
      callback(failure<player_host::GameCommandResultV1>(
          "AGENT_CANCELLED", "command was cancelled"));
      return;
    }
    player_host::GameCommandResultV1 result;
    result.status = player_host::CommandResultStatusV1::Succeeded;
    result.command_kind = player_host::commandKind(command);
    callback(player_host::AdapterResultV1<
             player_host::GameCommandResultV1>::success(
        std::move(result)));
  }

  void clearAgentIntent(
      player_host::PreemptionReasonV1 reason) noexcept override {
    ++clearCount_;
    clearedReason_ = reason;
  }

  int clearCount() const noexcept { return clearCount_; }
  std::optional<player_host::PreemptionReasonV1> clearedReason() const {
    return clearedReason_;
  }

 private:
  template <typename T>
  static player_host::AdapterResultV1<T> failure(
      std::string code, std::string message) {
    player_host::AgentErrorV1 error;
    error.code = std::move(code);
    error.message = std::move(message);
    return player_host::AdapterResultV1<T>::failure(std::move(error));
  }

  std::string controlledEntity_;
  std::string revision_;
  int clearCount_ = 0;
  std::optional<player_host::PreemptionReasonV1> clearedReason_;
};

template <typename Predicate>
bool pumpUntil(studio::CrowdyStudioIntegration& integration,
               Predicate&& done, int timeoutMs = 20'000) {
  const auto started = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - started <
         std::chrono::milliseconds(timeoutMs)) {
    integration.poll();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  integration.poll();
  return done();
}

struct AsyncVoid {
  bool finished = false;
  std::optional<agent::AgentError> failure;
};

template <typename Start>
void waitForAgentVoid(
    studio::CrowdyStudioIntegration& integration, Start&& start,
    const char* context, int timeoutMs = 20'000) {
  auto state = std::make_shared<AsyncVoid>();
  std::forward<Start>(start)(
      [state](agent::AgentOutcome<agent::AgentVoid> outcome) {
        if (!outcome.ok()) state->failure = *outcome.error;
        state->finished = true;
      });
  if (!pumpUntil(integration, [&] { return state->finished; }, timeoutMs)) {
    throw std::runtime_error(std::string(context) + " timed out");
  }
  if (state->failure) {
    e2e::printAgentError(*state->failure, context);
    throw std::runtime_error(std::string(context) + " failed");
  }
}

agent::CrowdyStudioAgentControllerOptions buildAgentOptions(
    std::string_view appId, std::string projectId, std::string gridId,
    std::string_view suffix) {
  agent::AgentCreateSessionInput create;
  create.appId = appId;
  create.projectId = std::move(projectId);
  create.gridId = std::move(gridId);
  create.mode = agent::AgentMode::Build;
  create.providerDataConsent = false;
  create.idempotencyKey =
      "cpp-native-integration-create-" + std::string(suffix);

  agent::CrowdyStudioAgentControllerOptions options;
  options.createSession = std::move(create);
  options.clientInstanceId = e2e::clientInstanceUuid();
  options.heartbeatIntervalMs = 100;
  options.heartbeatStaleMs = 2'000;
  options.workspaceRenewIntervalMs = 1'000;
  options.createIdempotencyKey =
      [prefix = "cpp-native-integration-" + std::string(suffix),
       sequence = 0U](std::string_view operation) mutable {
        return prefix + "-" + std::string(operation) + "-" +
               std::to_string(++sequence);
      };
  return options;
}

agent::NativeToolInvocationV1 runtimeStatusInvocation(
    const studio::CrowdyStudioIntegration& integration,
    std::string_view suffix) {
  const auto* controller = integration.agentController();
  LIVE_CHECK(controller != nullptr);
  LIVE_CHECK(controller->state().session.has_value());
  LIVE_CHECK(controller->state().clientEpoch.has_value());
  const auto contracts = agent::nativeLocalToolContractsV1();
  const auto found = std::find_if(
      contracts.begin(), contracts.end(), [](const auto& value) {
        return value.name == "runtime.status.get";
      });
  LIVE_CHECK(found != contracts.end());

  agent::NativeToolInvocationV1 invocation;
  invocation.session_id = controller->state().session->sessionId;
  invocation.run_id = "cpp-native-integration-status-" +
                      std::string(suffix);
  invocation.tool_call_id = invocation.run_id + "-tool";
  invocation.name = std::string(found->name);
  invocation.version = std::string(found->version);
  invocation.descriptor_digest =
      std::string(found->descriptor_digest);
  invocation.arguments = agent::NoArgumentsV1{};
  invocation.argument_hash =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  invocation.context_version =
      controller->state().session->contextVersion;
  invocation.client_epoch = controller->state().clientEpoch;
  invocation.deadline = "2099-01-01T00:00:00.000Z";
  return invocation;
}

bool archiveProject(domains::CrowdyStudioAPI& api, std::string_view appId,
                    std::string_view gridId, std::string_view projectId,
                    std::string_view suffix) noexcept {
  try {
    auto project = api.getProject(
        {std::string(appId), std::string(gridId)}, projectId);
    if (!project.archived) {
      project = api.setProjectArchived({
          .appId = std::string(appId),
          .projectId = std::string(projectId),
          .expectedRevisionId = project.revision.id,
          .archived = true,
          .idempotencyKey =
              "cpp-native-integration-archive-" + std::string(suffix),
      });
    }
    return project.archived;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "native Studio cleanup archive failed: %s\n",
                 error.what());
    return false;
  }
}

void stopAndClose(studio::CrowdyStudioIntegration& integration) noexcept {
  try {
    if (integration.studio().getState().project) {
      (void)integration.studio().stopProject();
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "native Studio cleanup stop failed: %s\n",
                 error.what());
  }
  if (auto* controller = integration.agentController()) {
    if (!controller->state().clientEpoch) {
      integration.dispose();
      return;
    }
    auto state = std::make_shared<AsyncVoid>();
    try {
      controller->close(
          [state](agent::AgentOutcome<agent::AgentVoid> outcome) {
            if (!outcome.ok()) state->failure = *outcome.error;
            state->finished = true;
          });
    } catch (const std::exception& error) {
      std::fprintf(stderr, "native Studio cleanup close failed: %s\n",
                   error.what());
      integration.dispose();
      return;
    }
    (void)pumpUntil(integration, [&] { return state->finished; }, 10'000);
    if (state->failure) {
      e2e::printAgentError(
          *state->failure, "native Studio cleanup close");
    }
  }
  integration.dispose();
}

}  // namespace

int main() {
  const auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  e2e::requireFlag("CROWDY_E2E_AGENT");
  e2e::requireFlag("CROWDY_E2E_AGENT_PLAY");

  const std::string gridId =
      e2e::envOr("CROWDY_E2E_STUDIO_GRID_ID");
  const std::string controlledEntity =
      e2e::envOr("CROWDY_E2E_AGENT_CONTROLLED_ENTITY_ID");
  const std::string capabilityRevision =
      e2e::envOr("CROWDY_E2E_AGENT_HOST_CAPABILITY_REVISION");
  if (gridId.empty() || controlledEntity.empty() ||
      capabilityRevision.empty()) {
    std::puts(
        "Studio grid/Play host capability variables missing; skipping");
    return 77;
  }

  auto& game = e2e::ownerGame(cfg);
  auto& api = game.crowdyStudio();
  const std::string suffix = e2e::runSuffix();
  const std::string module = "native_studio_e2e_" + suffix;
  std::string projectId;
  std::shared_ptr<LiveEditor> editor;
  std::unique_ptr<LivePlayerHost> playerHost;
  std::unique_ptr<studio::CrowdyStudioIntegration> integration;

  try {
    E2E_SUBTEST("create a disposable project for the integration");
    studio::CreateCrowdyStudioProjectInput create;
    create.appId = cfg.appId;
    create.gridId = gridId;
    create.kind = studio::CrowdyStudioProjectKind::Server;
    create.metadata.name = "CrowdyCPP native integration " + suffix;
    create.metadata.description = "0.15 integration e2e";
    create.metadata.serverModuleName = module;
    create.idempotencyKey =
        "cpp-native-integration-project-" + suffix;
    create.files = {
        projectFile("Cargo.toml", kCargoToml),
        projectFile(
            "src/lib.rs",
            "#[no_mangle]\n"
            "pub extern \"C\" fn init() {}\n\n"
            "pub fn invoke(input: &[u8]) -> Vec<u8> { input.to_vec() }\n"),
    };
    auto created = api.createProject(create);
    projectId = created.projectId;
    LIVE_CHECK(!projectId.empty());

    E2E_SUBTEST("factory initializes editor and layout");
    editor = std::make_shared<LiveEditor>();
    auto layout =
        std::make_shared<studio::InMemoryCrowdyStudioLayoutStorage>();
    playerHost = std::make_unique<LivePlayerHost>(
        controlledEntity, capabilityRevision);

    studio::CrowdyStudioIntegrationOptions options;
    options.studio.appId = cfg.appId;
    options.studio.gridId = gridId;
    options.studio.initialProjectId = projectId;
    options.studio.autosaveMs = 0;
    options.studio.compilePollMs = 500;
    options.studio.compilePollLimit = 120;
    options.editor = editor;
    options.layoutStorage = layout;
    options.playerHost = playerHost.get();
    options.nativeTools.validate_argument_hash =
        [](const agent::NativeToolInvocationV1&) { return true; };
    options.agent =
        buildAgentOptions(cfg.appId, projectId, gridId, suffix);

    integration =
        game.createCrowdyStudioIntegration(std::move(options));
    integration->initializeStudio();

    E2E_SUBTEST("edit in memory and save only on maintenance lane");
    const std::string edited =
        "#[no_mangle]\n"
        "pub extern \"C\" fn init() {}\n\n"
        "pub fn invoke(input: &[u8]) -> Vec<u8> {\n"
        "    let mut output = input.to_vec();\n"
        "    output.push(15);\n"
        "    output\n"
        "}\n";
    const std::string beforeRevision =
        integration->studio().getState().project->revision.id;
    editor->edit(
        studio::CrowdyStudioTarget::Server, "src/lib.rs", edited);
    LIVE_CHECK(integration->studio().getState().saveState ==
               studio::CrowdyStudioSaveState::Saving);
    integration->poll();
    LIVE_CHECK(integration->studio().getState().project->revision.id ==
               beforeRevision);
    (void)integration->runStudioMaintenance();
    LIVE_CHECK(integration->studio().getState().saveState ==
               studio::CrowdyStudioSaveState::Saved);
    const std::string savedRevision =
        integration->studio().getState().project->revision.id;
    LIVE_CHECK(savedRevision != beforeRevision);
    const auto saved =
        api.getProject({cfg.appId, gridId}, projectId);
    const auto savedFile = std::find_if(
        saved.files.begin(), saved.files.end(), [](const auto& file) {
          return file.target == studio::CrowdyStudioTarget::Server &&
                 file.path == "src/lib.rs";
        });
    LIVE_CHECK(savedFile != saved.files.end());
    LIVE_CHECK(savedFile->content == edited);

    E2E_SUBTEST("attach BUILD after the edited revision is durable");
    integration->initializeAgent();
    LIVE_CHECK(pumpUntil(
        *integration,
        [&] {
          const auto* controller = integration->agentController();
          return controller &&
                 (controller->state().connection ==
                      agent::AgentConnectionState::Connected ||
                  controller->state().lastError.has_value());
        },
        30'000));
    auto* agentController = integration->agentController();
    LIVE_CHECK(agentController != nullptr);
    if (agentController->state().lastError) {
      e2e::printAgentError(
          *agentController->state().lastError,
          "native Studio BUILD attach");
    }
    LIVE_CHECK(agentController->state().connection ==
               agent::AgentConnectionState::Connected);
    LIVE_CHECK(agentController->state().session->mode ==
               agent::AgentMode::Build);
    LIVE_CHECK(agentController->state().session->projectId ==
               projectId);

    E2E_SUBTEST("BUILD context dispatches a Studio host status tool");
    const auto plan = integration->studio().makeDeploymentPlan();
    LIVE_CHECK(plan.expectedRevisionId == savedRevision);
    auto statusResult =
        std::make_shared<std::optional<agent::NativeToolResultV1>>();
    integration->nativeTools().dispatch(
        runtimeStatusInvocation(*integration, suffix),
        [statusResult](agent::NativeToolResultV1 result) {
          *statusResult = std::move(result);
        });
    if (!statusResult->has_value()) {
      LIVE_CHECK(integration->runStudioMaintenance(1) == 1);
    }
    LIVE_CHECK(statusResult->has_value());
    LIVE_CHECK((*statusResult)->status ==
               agent::NativeToolResultStatusV1::Succeeded);
    LIVE_CHECK((*statusResult)->output.has_value());
    LIVE_CHECK(std::holds_alternative<
               agent::StudioRuntimeStatusV1>(
        *(*statusResult)->output));

    E2E_SUBTEST("run the exact saved draft through explicit maintenance");
    const auto draft = integration->studio().testDraftPlan(plan);
    if (draft.status !=
        studio::CrowdyStudioDeployResult::Status::Running) {
      std::fprintf(
          stderr, "native Studio draft failed: %s\n",
          draft.message.c_str());
    }
    const bool admissionPending =
        draft.message.find("awaiting admission") != std::string::npos;
    LIVE_CHECK(
        draft.status ==
            studio::CrowdyStudioDeployResult::Status::Running ||
        admissionPending);
    if (draft.status ==
        studio::CrowdyStudioDeployResult::Status::Running) {
      LIVE_CHECK(integration->studio().getState().runtimeSync.state ==
                 studio::CrowdyStudioRuntimeSyncState::RunningSaved);
    } else {
      std::puts(
          "Draft reached the configured code-admission gate; runtime start "
          "is intentionally pending operator approval");
    }

    E2E_SUBTEST("grant native Play and take over through the control gate");
    waitForAgentVoid(
        *integration,
        [&](auto callback) {
          agentController->setMode(
              agent::AgentMode::Play, std::move(callback));
        },
        "switch integration session to PLAY");
    struct PlayState {
      bool finished = false;
      agent::AgentOutcome<agent::AgentLease> outcome;
    };
    auto play = std::make_shared<PlayState>();
    agentController->grantPlayLease(
        {"observe", "locomotion"}, 30, controlledEntity,
        capabilityRevision,
        [play](
            agent::AgentOutcome<agent::AgentLease> outcome) {
          play->outcome = std::move(outcome);
          play->finished = true;
        });
    LIVE_CHECK(pumpUntil(*integration, [&] { return play->finished; }));
    if (play->outcome.error) {
      e2e::printAgentError(
          *play->outcome.error, "grant integration Play lease");
    }
    LIVE_CHECK(play->outcome.ok());
    LIVE_CHECK(play->outcome.value->status ==
               agent::AgentLeaseStatus::Active);
    LIVE_CHECK(integration->leaseSnapshot().lease.has_value());
    LIVE_CHECK(integration->leaseSnapshot().lease->lease_id ==
               play->outcome.value->leaseId);
    LIVE_CHECK(integration->controlSnapshot().active_lease.has_value());

    integration->controlGate().onHumanMovementInput();
    LIVE_CHECK(playerHost->clearCount() >= 1);
    LIVE_CHECK(playerHost->clearedReason() ==
               player_host::PreemptionReasonV1::HUMAN_INPUT);
    LIVE_CHECK(!integration->leaseSnapshot().lease.has_value());
    LIVE_CHECK(!integration->controlSnapshot().active_lease.has_value());
    LIVE_CHECK(pumpUntil(
        *integration,
        [&] {
          const auto& leases = agentController->state().leases;
          const auto found = std::find_if(
              leases.begin(), leases.end(), [&](const auto& lease) {
                return lease.leaseId ==
                       play->outcome.value->leaseId;
              });
          return found != leases.end() &&
                 found->status != agent::AgentLeaseStatus::Active;
        }));

    E2E_SUBTEST("approved restore capability detection");
    if (e2e::envFlag(
            "CROWDY_E2E_STUDIO_APPROVED_RESTORE_CAPABILITY")) {
      throw std::runtime_error(
          "deployment advertised approved restore, but this checkout has "
          "no injected synchronization/approval provider");
    }
    std::puts(
        "No injected synchronization/approval provider advertised; "
        "approved restore live subtest skipped (offline gate covers it)");

    stopAndClose(*integration);
    integration.reset();
    LIVE_CHECK(editor->disposed());
    LIVE_CHECK(archiveProject(
        api, cfg.appId, gridId, projectId, suffix));
    std::puts("e2e_native_studio_integration passed");
    return 0;
  } catch (const graphql::CrowdyGraphQLError& error) {
    e2e::printGraphQLError(
        error, "native Studio integration GraphQL error");
  } catch (const agent::CrowdyAgentError& error) {
    e2e::printAgentError(
        error.value(), "native Studio integration Agent error");
  } catch (const std::exception& error) {
    std::fprintf(
        stderr, "native Studio integration exception: %s\n",
        error.what());
  }

  if (integration) {
    stopAndClose(*integration);
    integration.reset();
  }
  if (!projectId.empty()) {
    (void)archiveProject(
        api, cfg.appId, gridId, projectId, suffix);
  }
  return 1;
}
