// Engine-neutral Crowdy Studio shell wiring. This example intentionally uses
// no credentials and never initializes the remote project service: it shows
// ownership, scheduling, and input boundaries that an engine supplies.
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <crowdy/crowdy.hpp>

namespace {

class NoNetworkTransport final : public crowdy::graphql::IHttpTransport {
 public:
  crowdy::graphql::HttpResponse send(
      const crowdy::graphql::HttpRequest&) override {
    return {503, R"({"errors":[{"message":"example is offline"}]})"};
  }
};

// Receives only synchronized buffers and narrow editor callbacks. It has no
// DOM, filesystem, CrowdyClient, transport, or raw GraphQL authority.
class InMemoryEditor final
    : public crowdy::studio::ICrowdyStudioEditorAdapter {
 public:
  crowdy::studio::CrowdyStudioEditorMode mode() const noexcept override {
    return crowdy::studio::CrowdyStudioEditorMode::Text;
  }

  void setCallbacks(
      crowdy::studio::CrowdyStudioEditorCallbacks callbacks) override {
    callbacks_ = std::move(callbacks);
  }

  void synchronize(
      const crowdy::studio::CrowdyStudioEditorSnapshot& snapshot) override {
    snapshot_ = snapshot;
  }

  void relayout() override {}

  void dispose() noexcept override {
    callbacks_ = {};
    snapshot_ = {};
  }

 private:
  crowdy::studio::CrowdyStudioEditorCallbacks callbacks_;
  crowdy::studio::CrowdyStudioEditorSnapshot snapshot_;
};

// The host maps typed commands to the same in-memory intent state that human
// controls would use. It exposes no input injection or generic SDK call.
class InMemoryPlayerHost final
    : public crowdy::player_host::PlayerHostAdapterV1 {
 public:
  void capabilities(
      crowdy::player_host::CancellationTokenV1 cancellation,
      crowdy::player_host::CapabilitiesCallbackV1 callback) override {
    if (cancellation.cancelled()) {
      callback(failure<crowdy::player_host::PlayerHostCapabilitiesV1>(
          "AGENT_CANCELLED", "capability request was cancelled"));
      return;
    }
    crowdy::player_host::PlayerHostCapabilitiesV1 capabilities;
    capabilities.game_id = "native-studio-shell";
    capabilities.revision = "in-memory-intents/1";
    capabilities.controlled_entity_id = "local-player";
    capabilities.observation.max_age_ms = 2'000;
    capabilities.advertised_at = "2026-07-24T00:00:00.000Z";
    callback(crowdy::player_host::AdapterResultV1<
             crowdy::player_host::PlayerHostCapabilitiesV1>::success(
        std::move(capabilities)));
  }

  void observe(
      const crowdy::player_host::ObserveRequestV1&,
      crowdy::player_host::CancellationTokenV1,
      crowdy::player_host::ObservationCallbackV1 callback) override {
    callback(failure<crowdy::player_host::GameObservationV1>(
        "AGENT_HOST_UNAVAILABLE",
        "the minimal shell does not publish observations"));
  }

  void dispatch(
      const crowdy::player_host::GameCommandV1& command,
      const crowdy::player_host::ValidatedGateV1&,
      crowdy::player_host::CancellationTokenV1 cancellation,
      crowdy::player_host::CommandCallbackV1 callback) override {
    using namespace crowdy::player_host;
    if (cancellation.cancelled()) {
      callback(failure<GameCommandResultV1>(
          "AGENT_CANCELLED", "intent was cancelled"));
      return;
    }

    GameCommandResultV1 result;
    result.status = CommandResultStatusV1::Succeeded;
    result.command_kind = commandKind(command);
    if (const auto* move = std::get_if<MoveCommandV1>(&command)) {
      movement_ = move->intensity;
    } else if (std::holds_alternative<StopCommandV1>(command)) {
      movement_ = 0;
    } else {
      callback(failure<GameCommandResultV1>(
          "AGENT_SCOPE_DENIED",
          "the minimal shell exposes only movement and stop intents"));
      return;
    }
    callback(AdapterResultV1<GameCommandResultV1>::success(
        std::move(result)));
  }

  void clearAgentIntent(
      crowdy::player_host::PreemptionReasonV1) noexcept override {
    movement_ = 0;
  }

 private:
  template <typename T>
  static crowdy::player_host::AdapterResultV1<T> failure(
      std::string code, std::string message) {
    crowdy::player_host::AgentErrorV1 error;
    error.code = std::move(code);
    error.message = std::move(message);
    return crowdy::player_host::AdapterResultV1<T>::failure(
        std::move(error));
  }

  double movement_ = 0;
};

}  // namespace

int main() {
  auto transport = std::make_shared<NoNetworkTransport>();
  crowdy::ClientConfig config;
  config.httpUrl = "https://offline.invalid";
  config.transport = transport;
  crowdy::CrowdyClient client(std::move(config));

  auto editor = std::make_shared<InMemoryEditor>();
  auto layout =
      std::make_shared<crowdy::studio::InMemoryCrowdyStudioLayoutStorage>();
  InMemoryPlayerHost playerHost;

  crowdy::studio::CrowdyStudioIntegrationOptions options;
  options.studio.appId = "1";
  options.studio.gridId = "1";
  options.editor = editor;
  options.layoutStorage = layout;
  options.playerHost = &playerHost;  // Must outlive the integration.
  options.observePlayerWallet = false;

  auto studio =
      client.createCrowdyStudioIntegration(std::move(options));
  studio->layout().setVisible(
      crowdy::studio::StudioPaneId::Agent, true);

  bool running = true;
  bool maintenanceDue = true;
  while (running) {
    (void)studio->poll();  // Nonblocking callbacks and deadline progress.

    if (maintenanceDue) {
      // Schedule this only on a serialized lane where project saves, HTTP,
      // compile polling, and runtime effects are allowed to block.
      (void)studio->runStudioMaintenance();
      maintenanceDue = false;
    }

    // Forward existing engine input events; the gate observes takeover intent
    // but never consumes, synthesizes, or injects an input event.
    studio->controlGate().onHumanKeyboardInput();
    studio->controlGate().onHumanPointerInput();
    studio->controlGate().onHumanMovementInput();
    running = false;
  }

  studio->dispose();
  studio.reset();  // Destroy before the borrowed player host and client.
  client.close();
  std::puts("native_studio_shell completed without credentials");
  return 0;
}
