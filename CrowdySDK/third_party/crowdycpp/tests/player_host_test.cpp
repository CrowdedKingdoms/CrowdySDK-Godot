#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/agent/native_browser_dispatcher.hpp"
#include "crowdy/agent/native_tool_dispatcher.hpp"
#include "crowdy/core/clock.hpp"
#include "crowdy/player_host/adapter.hpp"
#include "crowdy/player_host/lease_manager.hpp"
#include "crowdy/player_host/schemas.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::agent;
using namespace crowdy::player_host;

namespace {

constexpr std::int64_t kBaseTime = 1'753'300'800'000LL;

std::string iso(std::int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1'000);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer),
                "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec,
                static_cast<long long>(epoch_ms % 1'000));
  return buffer;
}

class FakeClock final : public core::IClock {
 public:
  std::int64_t epoch = kBaseTime;
  std::int64_t monotonic = 10'000;
  std::int64_t epochMillis() const override { return epoch; }
  std::int64_t monotonicMillis() const override { return monotonic; }
  void advance(std::int64_t milliseconds) {
    epoch += milliseconds;
    monotonic += milliseconds;
  }
};

std::optional<LeaseScopeV1> scopeFor(CommandKindV1 kind) {
  for (const auto& schema : kGameCommandSchemasV1) {
    if (schema.kind == kind) return schema.default_scope;
  }
  return std::nullopt;
}

PlayerHostCapabilitiesV1 makeCapabilities(std::int64_t now,
                                          std::uint32_t move_rate = 100) {
  PlayerHostCapabilitiesV1 value;
  value.game_id = "native-test-game";
  value.revision = "capability-7";
  value.controlled_entity_id = "player-7";
  for (const auto kind : kCommandKindsV1) {
    CommandCapabilityV1 command;
    command.kind = kind;
    command.tool_name = std::string(toolName(kind));
    command.required_scope = scopeFor(kind);
    command.risk = gen::CrowdyStudioAgentToolRisk::WORLD_CONTROL;
    command.approval = kind == CommandKindV1::CombatAttack
                           ? ApprovalPolicyV1::Conditional
                           : ApprovalPolicyV1::None;
    command.rate_limit_per_second =
        kind == CommandKindV1::Move ? move_rate : 100;
    value.commands.push_back(std::move(command));
  }
  value.observation = {.max_age_ms = 2'000,
                       .max_nearby_actors = 16,
                       .max_nearby_voxels = 32};
  value.advertised_at = iso(now);
  return value;
}

GameObservationV1 makeObservation(
    std::int64_t now, ActorKindV1 target_kind = ActorKindV1::Mob,
    bool alive = true, std::string id = "observation-1") {
  GameObservationV1 value;
  value.observation_id = std::move(id);
  value.capability_revision = "capability-7";
  value.controlled_entity_id = "player-7";
  value.observed_at = iso(now);
  value.expires_at = iso(now + 2'000);
  value.player.position = {
      "922337203685477580812345.123456789", "64", "-12.5"};
  value.player.velocity = {"0", "0", "0"};
  value.player.look = {"0", "0"};
  value.player.health = alive ? "20" : "0";
  value.player.alive = alive;
  value.controlled_entity.kind = ControlledEntityKindV1::Player;
  value.controlled_entity.position = value.player.position;
  value.controlled_entity.velocity = {"0", "0", "0"};
  value.target = ObservationTargetV1{
      .target_id = "target-1",
      .kind = ObservationTargetKindV1::Actor,
      .distance = "2.25",
  };
  value.nearby_actors.push_back(ObservationActorV1{
      .actor_id = "target-1",
      .kind = target_kind,
      .position = {"1", "64", "1"},
      .distance = "2.25",
      .disposition = ActorDispositionV1::Hostile,
      .label = std::nullopt,
      .health = std::nullopt,
  });
  return value;
}

AgentControlLeaseV1 makeLease(std::int64_t now,
                              std::vector<LeaseScopeV1> scopes = {
                              LeaseScopeV1::Observe,
                              LeaseScopeV1::Locomotion,
                              LeaseScopeV1::Interact,
                              LeaseScopeV1::Craft,
                              LeaseScopeV1::Combat,
                              LeaseScopeV1::Communicate,
                              LeaseScopeV1::Travel,
                              LeaseScopeV1::Grid,
                              LeaseScopeV1::TrustConsent,
                              LeaseScopeV1::Commerce,
                          }) {
  AgentControlLeaseV1 value;
  value.lease_id = "lease-1";
  value.client_epoch = "1";
  value.scopes = std::move(scopes);
  value.holder = "Current player";
  value.controlled_entity_id = "player-7";
  value.host_capability_revision = "capability-7";
  value.context_version = "context-1";
  value.granted_at = iso(now);
  value.expires_at = iso(now + 60'000);
  return value;
}

PlannedCommandV1 planned(std::string observation_id = "observation-1") {
  return PlannedCommandV1{.observation_id = std::move(observation_id),
                          .capability_revision = "capability-7",
                          .controlled_entity_id = "player-7"};
}

std::vector<GameCommandV1> allCommands() {
  return {
      MoveCommandV1{.planned = planned(),
                    .direction = MoveDirectionV1::Forward,
                    .intensity = 1,
                    .duration_ms = 100},
      LookCommandV1{.planned = planned(),
                    .delta_yaw = 10,
                    .delta_pitch = -5},
      InventorySelectCommandV1{.planned = planned(), .slot = 1},
      InventoryConsumeCommandV1{
          .planned = planned(), .slot = 1, .quantity = 1},
      InventoryTransferCommandV1{
          .planned = planned(),
          .direction = InventoryTransferDirectionV1::ToContainer,
          .slot = 1,
          .quantity = 1,
          .container_ref = "target-1"},
      InteractCommandV1{.planned = planned(),
                        .action = InteractActionV1::Use,
                        .target_ref = "target-1",
                        .inventory_slot = std::nullopt},
      CraftCommandV1{
          .planned = planned(), .recipe_id = "plank", .quantity = 1},
      MountCommandV1{.planned = planned(),
                     .action = MountActionV1::Mount,
                     .mount_ref = "target-1"},
      CombatAttackCommandV1{.planned = planned(),
                            .target_ref = "target-1",
                            .attack = CombatAttackV1::Primary},
      ChatSendCommandV1{.planned = planned(),
                        .channel = ChatChannelV1::Local,
                        .text = "hello"},
      TravelTeleportCommandV1{.planned = planned(),
                              .destination_ref = "spawn"},
      StopCommandV1{},
  };
}

class FakePlayerHost final : public PlayerHostAdapterV1 {
 public:
  PlayerHostCapabilitiesV1 capability_value = makeCapabilities(kBaseTime);
  GameObservationV1 observation_value = makeObservation(kBaseTime);
  std::vector<std::string> events;
  std::vector<CommandKindV1> dispatched;
  std::vector<ValidatedGateV1> gates;
  bool hold_dispatch = false;
  bool ambiguous_dispatch = false;
  bool definitive_failure = false;
  std::optional<CommandCallbackV1> pending;

  void capabilities(CancellationTokenV1,
                    CapabilitiesCallbackV1 callback) override {
    callback(AdapterResultV1<PlayerHostCapabilitiesV1>::success(
        capability_value));
  }

  void observe(const ObserveRequestV1&, CancellationTokenV1,
               ObservationCallbackV1 callback) override {
    callback(
        AdapterResultV1<GameObservationV1>::success(observation_value));
  }

  void dispatch(const GameCommandV1& command, const ValidatedGateV1& gate,
                CancellationTokenV1, CommandCallbackV1 callback) override {
    dispatched.push_back(commandKind(command));
    gates.push_back(gate);
    events.push_back("dispatch:" + std::string(toString(commandKind(command))));
    if (hold_dispatch) {
      pending = std::move(callback);
      return;
    }
    if (ambiguous_dispatch) {
      callback(AdapterResultV1<GameCommandResultV1>::failure(
          {.code = "AGENT_TOOL_OUTCOME_UNKNOWN",
           .message = "effect may have occurred",
           .retryable = false,
           .remediation = std::nullopt,
           .field = std::nullopt,
           .required_scope = std::nullopt},
          true));
      return;
    }
    if (definitive_failure) {
      callback(AdapterResultV1<GameCommandResultV1>::failure(
          {.code = "AGENT_TOOL_FAILED",
           .message = "intent service rejected command",
           .retryable = false,
           .remediation = std::nullopt,
           .field = std::nullopt,
           .required_scope = std::nullopt}));
      return;
    }
    GameCommandResultV1 result;
    result.status = CommandResultStatusV1::Succeeded;
    result.command_kind = commandKind(command);
    if (const auto* value = plannedCommand(command)) {
      result.observation_id = value->observation_id;
    }
    callback(
        AdapterResultV1<GameCommandResultV1>::success(std::move(result)));
  }

  void clearAgentIntent(PreemptionReasonV1 reason) noexcept override {
    events.push_back("clear:" + std::string(gen::toString(reason)));
  }

  void completePendingSuccess() {
    CHECK(pending.has_value());
    auto callback = std::move(*pending);
    pending.reset();
    GameCommandResultV1 result;
    result.status = CommandResultStatusV1::Succeeded;
    result.command_kind = dispatched.back();
    result.observation_id = "observation-1";
    callback(
        AdapterResultV1<GameCommandResultV1>::success(std::move(result)));
  }
};

AgentControlLeaseManagerOptionsV1 managerOptions(
    const core::IClock* clock, std::function<std::string()> context,
    std::uint32_t heartbeat_timeout_ms = 5'000,
    std::function<bool(const AgentControlDispatchV1&)> approval = {}) {
  AgentControlLeaseManagerOptionsV1 options;
  options.clock = clock;
  options.context_version = std::move(context);
  options.heartbeat_timeout_ms = heartbeat_timeout_ms;
  options.validate_approval_grant = std::move(approval);
  return options;
}

struct ReadyHost {
  FakeClock clock;
  FakePlayerHost host;
  std::string context = "context-1";
  AgentControlLeaseManager manager;

  explicit ReadyHost(
      std::uint32_t move_rate = 100,
      std::vector<LeaseScopeV1> scopes = makeLease(kBaseTime).scopes)
      : manager(
            host,
            managerOptions(
                &clock, [this] { return context; }, 5'000,
                [](const AgentControlDispatchV1& input) {
                  return input.approval_grant == "approved";
                })) {
    host.capability_value = makeCapabilities(clock.epoch, move_rate);
    CHECK(!manager.attach("1"));
    std::optional<AdapterResultV1<PlayerHostCapabilitiesV1>> refreshed;
    manager.refreshCapabilities(
        [&](auto result) { refreshed = std::move(result); });
    CHECK(refreshed && refreshed->ok());
    CHECK(!manager.grantLease(makeLease(clock.epoch, std::move(scopes))));
    observe();
  }

  void observe() {
    std::optional<AdapterResultV1<GameObservationV1>> observed;
    manager.observe(
        {.detail = ObserveDetailV1::Tactical,
         .max_nearby_actors = 4,
         .max_nearby_voxels = 4},
        AgentObservationDispatchV1{.client_epoch = "1",
                                   .lease_id = "lease-1"},
        [&](auto result) { observed = std::move(result); });
    CHECK(observed && observed->ok());
  }

  AdapterResultV1<GameCommandResultV1> dispatch(
      std::string id, GameCommandV1 command,
      std::optional<std::string> approval = std::nullopt,
      std::string epoch = "1") {
    std::optional<AdapterResultV1<GameCommandResultV1>> completed;
    manager.dispatch(
        {.tool_call_id = std::move(id),
         .client_epoch = std::move(epoch),
         .lease_id = "lease-1",
         .approval_grant = std::move(approval),
         .command = std::move(command)},
        [&](auto result) { completed = std::move(result); });
    CHECK(completed.has_value());
    return std::move(*completed);
  }
};

void testSchemasAndEveryCommandMapping() {
  CHECK_EQ(kClosedPreemptionReasonsV1.size(), 16U);
  CHECK_EQ(kLeaseScopesV1.size(), 10U);
  CHECK_EQ(kMandatoryGameToolSurfacesV1.size(), 14U);
  CHECK_EQ(kGameCommandSchemasV1.size(), 12U);
  CHECK(validateDecimalStringV1(
      "922337203685477580812345.123456789", "world.x"));
  CHECK(!validateDecimalStringV1("01", "world.x"));

  auto caps = makeCapabilities(kBaseTime);
  CHECK(validatePlayerHostCapabilitiesV1(caps));
  for (const auto kind : kCommandKindsV1) {
    CHECK(commandKindForToolName(toolName(kind)).has_value());
    CHECK_EQ(*commandKindForToolName(toolName(kind)), kind);
  }
  caps.commands[0].tool_name = "game.control.look";
  CHECK(!validatePlayerHostCapabilitiesV1(caps));

  ReadyHost ready;
  std::size_t index = 0;
  for (auto command : allCommands()) {
    const auto kind = commandKind(command);
    const auto surface = std::find_if(
        kMandatoryGameToolSurfacesV1.begin(),
        kMandatoryGameToolSurfacesV1.end(),
        [&](const GameToolSurfaceV1& value) {
          return value.command_kind && *value.command_kind == kind;
        });
    CHECK(surface != kMandatoryGameToolSurfacesV1.end());
    CHECK_EQ(std::string(toolName(kind)), std::string(surface->name));
    const auto result =
        ready.dispatch("mapping-" + std::to_string(index), std::move(command));
    CHECK(result.ok());
    CHECK_EQ(result.value->status, CommandResultStatusV1::Succeeded);
    CHECK_EQ(result.value->command_kind, kind);
    ++index;
  }
  CHECK_EQ(index, 12U);
  CHECK_EQ(ready.host.dispatched.size(), 11U);
  CHECK_EQ(ready.host.gates.size(), 11U);
  for (const auto& gate : ready.host.gates) {
    CHECK(validateValidatedGateV1(gate));
    CHECK_EQ(gate.client_epoch, "1");
    CHECK_EQ(*gate.lease_id, "lease-1");
    CHECK_EQ(gate.context_version, "context-1");
    CHECK_EQ(*gate.observation_id, "observation-1");
    CHECK_EQ(gate.scopes, makeLease(kBaseTime).scopes);
  }
}

void testObservationBoundsAndCapabilitiesValidation() {
  FakeClock clock;
  FakePlayerHost host;
  std::string context = "context-1";
  AgentControlLeaseManager manager(
      host, managerOptions(&clock, [&] { return context; }));
  CHECK(!manager.attach("1"));

  host.capability_value.commands[0].tool_name = "game.control.look";
  std::optional<AdapterResultV1<PlayerHostCapabilitiesV1>> invalid_caps;
  manager.refreshCapabilities(
      [&](auto result) { invalid_caps = std::move(result); });
  CHECK(invalid_caps && !invalid_caps->ok());
  CHECK_EQ(invalid_caps->error->code, "AGENT_TOOL_OUTPUT_INVALID");

  host.capability_value = makeCapabilities(clock.epoch);
  std::optional<AdapterResultV1<PlayerHostCapabilitiesV1>> valid_caps;
  manager.refreshCapabilities(
      [&](auto result) { valid_caps = std::move(result); });
  CHECK(valid_caps && valid_caps->ok());

  std::optional<AdapterResultV1<GameObservationV1>> too_large;
  manager.observe(
      {.detail = ObserveDetailV1::Tactical,
       .max_nearby_actors = 17,
       .max_nearby_voxels = 1},
      AgentObservationDispatchV1{.client_epoch = "1",
                                 .lease_id = std::nullopt},
      [&](auto result) { too_large = std::move(result); });
  CHECK(too_large && !too_large->ok());
  CHECK_EQ(too_large->error->code, "AGENT_TOOL_INPUT_INVALID");

  host.observation_value.nearby_actors.push_back(
      host.observation_value.nearby_actors.front());
  std::optional<AdapterResultV1<GameObservationV1>> exceeded;
  manager.observe(
      {.detail = ObserveDetailV1::Standard,
       .max_nearby_actors = 1,
       .max_nearby_voxels = 0},
      AgentObservationDispatchV1{.client_epoch = "1",
                                 .lease_id = std::nullopt},
      [&](auto result) { exceeded = std::move(result); });
  CHECK(exceeded && !exceeded->ok());
  CHECK_EQ(exceeded->error->code, "AGENT_TOOL_OUTPUT_INVALID");
}

void testScopesConditionalApprovalTargetsEpochAndContext() {
  ReadyHost missing_scope(100, {LeaseScopeV1::Observe});
  auto missing = missing_scope.dispatch(
      "scope", MoveCommandV1{.planned = planned(),
                             .direction = MoveDirectionV1::Forward,
                             .intensity = 1,
                             .duration_ms = 100});
  CHECK(!missing.ok());
  CHECK_EQ(missing.error->code, "AGENT_LEASE_SCOPE_MISSING");
  CHECK_EQ(*missing.error->required_scope, "locomotion");

  ReadyHost combat;
  combat.host.observation_value =
      makeObservation(combat.clock.epoch, ActorKindV1::Player, true,
                      "observation-player");
  combat.observe();
  auto player_attack = CombatAttackCommandV1{
      .planned = planned("observation-player"),
      .target_ref = "target-1",
      .attack = CombatAttackV1::Primary};
  auto needs_approval =
      combat.dispatch("combat-no-approval", player_attack);
  CHECK(!needs_approval.ok());
  CHECK_EQ(needs_approval.error->code, "AGENT_APPROVAL_REQUIRED");
  auto approved =
      combat.dispatch("combat-approved", player_attack, "approved");
  CHECK(approved.ok());

  auto stale_target = player_attack;
  stale_target.target_ref = "not-observed";
  auto stale = combat.dispatch("stale-target", stale_target, "approved");
  CHECK(!stale.ok());
  CHECK_EQ(stale.error->code, "AGENT_OBSERVATION_STALE");

  auto stale_epoch =
      combat.dispatch("stale-epoch", player_attack, "approved", "2");
  CHECK(!stale_epoch.ok());
  CHECK_EQ(stale_epoch.error->code, "AGENT_CLIENT_EPOCH_STALE");

  combat.context = "context-2";
  auto stale_context =
      combat.dispatch("stale-context", player_attack, "approved");
  CHECK(!stale_context.ok());
  CHECK_EQ(stale_context.error->code, "AGENT_CONTEXT_CHANGED");
  CHECK(combat.manager.snapshot().last_preemption.has_value());
  CHECK_EQ(*combat.manager.snapshot().last_preemption,
           PreemptionReasonV1::CONTEXT_CHANGED);
}

void testSerializedRatesAndHeartbeatExpiry() {
  ReadyHost ready(1);
  auto move = MoveCommandV1{.planned = planned(),
                            .direction = MoveDirectionV1::Forward,
                            .intensity = 1,
                            .duration_ms = 100};
  CHECK(ready.dispatch("rate-1", move).ok());
  auto limited = ready.dispatch("rate-2", move);
  CHECK(!limited.ok());
  CHECK_EQ(limited.error->code, "AGENT_RATE_LIMITED");
  CHECK(limited.error->retryable);
  ready.clock.advance(1'000);
  ready.host.observation_value =
      makeObservation(ready.clock.epoch, ActorKindV1::Mob, true,
                      "observation-rate-2");
  ready.observe();
  move.planned.observation_id = "observation-rate-2";
  CHECK(ready.dispatch("rate-3", move).ok());

  FakeClock clock;
  FakePlayerHost host;
  std::string context = "context-1";
  AgentControlLeaseManager expiring(
      host, managerOptions(&clock, [&] { return context; }, 100));
  CHECK(!expiring.attach("1"));
  std::optional<AdapterResultV1<PlayerHostCapabilitiesV1>> refreshed;
  expiring.refreshCapabilities(
      [&](auto result) { refreshed = std::move(result); });
  CHECK(refreshed && refreshed->ok());
  CHECK(!expiring.grantLease(makeLease(clock.epoch)));
  CHECK(!expiring.heartbeat(
      {.lease_id = "lease-1",
       .client_epoch = "1",
       .context_version = "context-1",
       .controlled_entity_id = "player-7",
       .host_capability_revision = "capability-7"}));
  clock.advance(101);
  expiring.tick();
  CHECK(!expiring.snapshot().lease.has_value());
  CHECK_EQ(*expiring.snapshot().last_preemption,
           PreemptionReasonV1::LEASE_EXPIRED);
}

void testCapabilityEntityAndPolicyPreemption() {
  ReadyHost target;
  target.host.capability_value.controlled_entity_id = "vehicle-9";
  target.host.capability_value.revision = "capability-8";
  std::optional<AdapterResultV1<PlayerHostCapabilitiesV1>> refreshed;
  target.manager.refreshCapabilities(
      [&](auto result) { refreshed = std::move(result); });
  CHECK(refreshed && refreshed->ok());
  CHECK(!target.manager.snapshot().lease);
  CHECK_EQ(*target.manager.snapshot().last_preemption,
           PreemptionReasonV1::CONTROL_TARGET_CHANGED);

  ReadyHost revision;
  revision.host.capability_value.revision = "capability-8";
  refreshed.reset();
  revision.manager.refreshCapabilities(
      [&](auto result) { refreshed = std::move(result); });
  CHECK(refreshed && refreshed->ok());
  CHECK_EQ(*revision.manager.snapshot().last_preemption,
           PreemptionReasonV1::CONTEXT_CHANGED);

  ReadyHost confused;
  confused.host.capability_value.observation.max_age_ms = 1'000;
  refreshed.reset();
  confused.manager.refreshCapabilities(
      [&](auto result) { refreshed = std::move(result); });
  CHECK(refreshed && !refreshed->ok());
  CHECK_EQ(refreshed->error->code, "AGENT_HOST_CAPABILITY_CHANGED");
  CHECK(confused.manager.snapshot().lease.has_value());

  ReadyHost permission;
  permission.manager.onPermissionChanged();
  CHECK_EQ(*permission.manager.snapshot().last_preemption,
           PreemptionReasonV1::PERMISSION_CHANGED);
  ReadyHost admission;
  admission.manager.onAdmissionChanged();
  CHECK_EQ(*admission.manager.snapshot().last_preemption,
           PreemptionReasonV1::ADMISSION_CHANGED);
}

void testHumanTakeoverOfflineStopDeathAndDisconnect() {
  ReadyHost ready;
  ready.host.hold_dispatch = true;
  std::optional<AdapterResultV1<GameCommandResultV1>> completed;
  ready.manager.dispatch(
      {.tool_call_id = "pending",
       .client_epoch = "1",
       .lease_id = "lease-1",
       .approval_grant = std::nullopt,
       .command = MoveCommandV1{.planned = planned(),
                                .direction = MoveDirectionV1::Forward,
                                .intensity = 1,
                                .duration_ms = 100}},
      [&](auto result) {
        ready.host.events.push_back("callback");
        completed = std::move(result);
      });
  CHECK(!completed.has_value());
  ready.manager.onHumanInput();
  CHECK(completed && completed->ok());
  CHECK_EQ(completed->value->status,
           CommandResultStatusV1::OutcomeUnknown);
  const auto clear = std::find(ready.host.events.begin(),
                               ready.host.events.end(), "clear:HUMAN_INPUT");
  const auto callback = std::find(ready.host.events.begin(),
                                  ready.host.events.end(), "callback");
  CHECK(clear != ready.host.events.end());
  CHECK(callback != ready.host.events.end());
  CHECK(clear < callback);
  const auto event_count = ready.host.events.size();
  ready.host.completePendingSuccess();
  CHECK_EQ(ready.host.events.size(), event_count);

  ready.manager.disconnect();
  std::optional<AdapterResultV1<GameCommandResultV1>> stopped;
  ready.manager.dispatch(
      {.tool_call_id = "offline-stop",
       .client_epoch = "999",
       .lease_id = std::nullopt,
       .approval_grant = std::nullopt,
       .command = StopCommandV1{}},
      [&](auto result) { stopped = std::move(result); });
  CHECK(stopped && stopped->ok());
  CHECK_EQ(stopped->value->status, CommandResultStatusV1::Succeeded);
  CHECK_EQ(ready.host.events.back(), "clear:HUMAN_STOP");

  ReadyHost dead;
  dead.host.observation_value =
      makeObservation(dead.clock.epoch, ActorKindV1::Mob, false,
                      "observation-dead");
  dead.observe();
  auto death = dead.dispatch(
      "death", MoveCommandV1{.planned = planned("observation-dead"),
                             .direction = MoveDirectionV1::Forward,
                             .intensity = 1,
                             .duration_ms = 100});
  CHECK(!death.ok());
  CHECK_EQ(death.error->code, "AGENT_PREEMPTED");
  CHECK_EQ(*dead.manager.snapshot().last_preemption,
           PreemptionReasonV1::DEATH);

  ReadyHost disconnected;
  disconnected.manager.disconnect();
  std::optional<AdapterResultV1<GameCommandResultV1>> denied;
  disconnected.manager.dispatch(
      {.tool_call_id = "after-disconnect",
       .client_epoch = "1",
       .lease_id = "lease-1",
       .approval_grant = std::nullopt,
       .command = MoveCommandV1{.planned = planned(),
                                .direction = MoveDirectionV1::Forward,
                                .intensity = 1,
                                .duration_ms = 100}},
      [&](auto result) { denied = std::move(result); });
  CHECK(denied && !denied->ok());
  CHECK_EQ(denied->error->code, "AGENT_DISCONNECTED");
  CHECK_EQ(*disconnected.manager.snapshot().last_preemption,
           PreemptionReasonV1::DISCONNECTED);
}

void testAmbiguousOutcomesExecuteOnceAndLateResults() {
  ReadyHost ambiguous;
  ambiguous.host.ambiguous_dispatch = true;
  auto move = MoveCommandV1{.planned = planned(),
                            .direction = MoveDirectionV1::Forward,
                            .intensity = 1,
                            .duration_ms = 100};
  auto unknown = ambiguous.dispatch("ambiguous", move);
  CHECK(unknown.ok());
  CHECK_EQ(unknown.value->status,
           CommandResultStatusV1::OutcomeUnknown);
  const auto dispatch_count = ambiguous.host.dispatched.size();
  auto replay = ambiguous.dispatch("ambiguous", move);
  CHECK(replay.ok());
  CHECK_EQ(replay.value->status,
           CommandResultStatusV1::OutcomeUnknown);
  CHECK_EQ(ambiguous.host.dispatched.size(), dispatch_count);

  std::optional<AdapterResultV1<GameCommandResultV1>> conflict;
  ambiguous.manager.dispatch(
      {.tool_call_id = "ambiguous",
       .client_epoch = "1",
       .lease_id = "lease-1",
       .approval_grant = std::nullopt,
       .command = LookCommandV1{.planned = planned(),
                                .delta_yaw = 1,
                                .delta_pitch = 1}},
      [&](auto result) { conflict = std::move(result); });
  CHECK(conflict && !conflict->ok());
  CHECK_EQ(conflict->error->code, "AGENT_IDEMPOTENCY_CONFLICT");

  ReadyHost late;
  late.host.hold_dispatch = true;
  std::size_t callback_count = 0;
  std::optional<AdapterResultV1<GameCommandResultV1>> late_result;
  late.manager.dispatch(
      {.tool_call_id = "late",
       .client_epoch = "1",
       .lease_id = "lease-1",
       .approval_grant = std::nullopt,
       .command = move},
      [&](auto result) {
        ++callback_count;
        late_result = std::move(result);
      });
  late.manager.onContextChanged();
  CHECK(late_result && late_result->ok());
  CHECK_EQ(late_result->value->status,
           CommandResultStatusV1::OutcomeUnknown);
  late.host.completePendingSuccess();
  CHECK_EQ(callback_count, 1U);
}

StudioRuntimeStatusV1 runtimeStatus() {
  StudioRuntimeStatusV1 value;
  value.phase = StudioRuntimePhaseV1::Running;
  value.saved_revision = "1";
  value.running_revision = "1";
  value.sync = StudioRuntimeSyncV1::RunningSaved;
  value.target = StudioTargetV1::Server;
  value.draft = true;
  return value;
}

class FakeStudioHost final : public agent::CrowdyStudioHostAdapter {
 public:
  std::vector<agent::StudioNativeToolKindV1> dispatched;
  std::vector<std::string> events;
  bool hold = false;
  bool definitive_failure = false;
  bool ambiguous_failure = false;
  bool invalid_output = false;
  std::optional<agent::StudioToolCallbackV1> pending;
  std::optional<agent::StudioNativeToolOutputV1> pending_output;

  void dispatch(agent::StudioNativeToolKindV1 kind,
                const agent::StudioNativeToolRequestV1& request,
                const agent::ValidatedStudioGateV1&,
                CancellationTokenV1 cancellation,
                agent::StudioToolCallbackV1 callback) override {
    dispatched.push_back(kind);
    events.push_back("dispatch");
    agent::StudioNativeToolOutputV1 output;
    switch (kind) {
      case agent::StudioNativeToolKindV1::ContextGet: {
        agent::StudioContextV1 value;
        value.app_ref = "42";
        value.grid_ref = "500";
        value.context_version = "context-1";
        value.runtime = runtimeStatus();
        output = std::move(value);
        break;
      }
      case agent::StudioNativeToolKindV1::StateGet: {
        agent::StudioStateV1 value;
        value.runtime = runtimeStatus();
        output = std::move(value);
        break;
      }
      case agent::StudioNativeToolKindV1::ProjectSelect:
        output = agent::StudioProjectSelectResultV1{
            .selected_project_ref =
                std::get<agent::StudioProjectSelectRequestV1>(request)
                    .project_ref,
            .revision = "1"};
        break;
      case agent::StudioNativeToolKindV1::WorkspaceTabOpen:
      case agent::StudioNativeToolKindV1::WorkspaceTabClose:
        output = agent::StudioOkV1{.ok = true};
        break;
      case agent::StudioNativeToolKindV1::DiagnosticsLocalGet:
        output = agent::StudioDiagnosticsV1{};
        break;
      case agent::StudioNativeToolKindV1::RuntimeStatusGet:
        output = runtimeStatus();
        break;
      case agent::StudioNativeToolKindV1::RuntimeTestDraft:
        output = agent::StudioRuntimePlanResultV1{
            .runtime = runtimeStatus(),
            .targets =
                std::get<agent::StudioRuntimeTestDraftRequestV1>(request)
                    .targets};
        break;
      case agent::StudioNativeToolKindV1::RuntimeDeployLive:
        {
          auto runtime = runtimeStatus();
          runtime.draft = false;
          output = agent::StudioRuntimePlanResultV1{
              .runtime = std::move(runtime),
              .targets =
                  std::get<agent::StudioRuntimeDeployLiveRequestV1>(request)
                      .targets};
        }
        break;
      case agent::StudioNativeToolKindV1::RuntimeInvoke:
        output = agent::StudioRuntimeInvokeResultV1{
            .result_type = agent::StudioRuntimeResultTypeV1::Text,
            .result = "{\"ok\":true}",
            .fuel_used = "4",
            .duration_us = 2};
        break;
      case agent::StudioNativeToolKindV1::RuntimeStop:
        output = agent::StudioRuntimeStopResultV1{
            .server_stopped = true,
            .client_stopped = true,
            .failures = {}};
        break;
    }
    if (!definitive_failure &&
        (kind == agent::StudioNativeToolKindV1::RuntimeTestDraft ||
         kind == agent::StudioNativeToolKindV1::RuntimeDeployLive ||
         kind == agent::StudioNativeToolKindV1::RuntimeInvoke)) {
      cancellation.markEffectStarted();
    }
    if (definitive_failure || ambiguous_failure) {
      callback(AdapterResultV1<agent::StudioNativeToolOutputV1>::failure(
          {.code = ambiguous_failure ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                                     : "AGENT_TOOL_FAILED",
           .message = ambiguous_failure ? "effect may have occurred"
                                        : "host rejected before effect",
           .retryable = false,
           .remediation = std::nullopt,
           .field = std::nullopt,
           .required_scope = std::nullopt},
          ambiguous_failure));
      return;
    }
    if (invalid_output) {
      if (auto* plan =
              std::get_if<agent::StudioRuntimePlanResultV1>(&output)) {
        plan->targets.clear();
      } else if (auto* context =
                     std::get_if<agent::StudioContextV1>(&output)) {
        context->context_version.clear();
      }
    }
    if (hold) {
      pending = std::move(callback);
      pending_output = std::move(output);
    } else {
      callback(AdapterResultV1<agent::StudioNativeToolOutputV1>::success(
          std::move(output)));
    }
  }

  void clearAgentOperation(PreemptionReasonV1 reason) noexcept override {
    events.push_back("clear:" + std::string(gen::toString(reason)));
  }

  void completePending() {
    CHECK(pending && pending_output);
    auto callback = std::move(*pending);
    auto output = std::move(*pending_output);
    pending.reset();
    pending_output.reset();
    callback(AdapterResultV1<agent::StudioNativeToolOutputV1>::success(
        std::move(output)));
  }
};

const agent::NativeLocalToolContractV1& localContract(std::string_view name) {
  const auto contracts = agent::nativeLocalToolContractsV1();
  const auto found =
      std::find_if(contracts.begin(), contracts.end(),
                   [&](const auto& value) { return value.name == name; });
  CHECK(found != contracts.end());
  return *found;
}

agent::NativeToolInvocationV1 invocation(
    std::string name, agent::NativeToolArgumentsV1 arguments,
    const FakeClock& clock, std::string id,
    std::optional<std::string> lease_id = std::nullopt,
    std::optional<std::string> approval = std::nullopt) {
  const auto& descriptor = localContract(name);
  agent::NativeToolInvocationV1 value;
  value.session_id = "session-1";
  value.run_id = "run-1";
  value.tool_call_id = std::move(id);
  value.name = std::move(name);
  value.version = std::string(descriptor.version);
  value.descriptor_digest = std::string(descriptor.descriptor_digest);
  value.arguments = std::move(arguments);
  value.argument_hash =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  value.context_version = "context-1";
  value.client_epoch = "1";
  value.lease_id = std::move(lease_id);
  value.approval_grant = std::move(approval);
  value.deadline = iso(clock.epoch + 120'000);
  return value;
}

void testNativeDispatcherGameAndStudioRouting() {
  ReadyHost ready;
  FakeStudioHost studio;
  agent::NativeAgentModeV1 mode = agent::NativeAgentModeV1::Build;
  agent::NativeToolDispatcherV1 dispatcher(
      ready.manager, &studio,
      {.clock = &ready.clock,
       .session_id = [] { return std::optional<std::string>("session-1"); },
       .client_epoch = [] { return std::optional<std::string>("1"); },
       .context_version = [&] { return ready.context; },
       .mode = [&] { return mode; },
       .is_lease_active =
           [](std::string_view id, LeaseKindV1 kind) {
             return (kind == LeaseKindV1::Workspace &&
                     id == "workspace-lease") ||
                    (kind == LeaseKindV1::Play && id == "lease-1");
           },
       .validate_argument_hash = [](const auto&) { return true; },
       .validate_approval_grant =
           [](const agent::NativeToolInvocationV1& value) {
             return value.approval_grant == "approved";
           }});

  std::size_t game_contracts = 0;
  for (const auto& value : agent::nativeLocalToolContractsV1()) {
    if (value.name.rfind("game.", 0) == 0) ++game_contracts;
  }
  CHECK_EQ(game_contracts, 14U);
  CHECK_EQ(agent::nativeLocalToolContractsV1().size(), 25U);

  struct StudioCase {
    std::string name;
    agent::NativeToolArgumentsV1 arguments;
    std::optional<std::string> lease;
    std::optional<std::string> approval;
  };
  std::vector<StudioCase> cases;
  cases.push_back({"studio.context.get", agent::NoArgumentsV1{},
                   std::nullopt, std::nullopt});
  cases.push_back({"studio.state.get", agent::NoArgumentsV1{}, std::nullopt,
                   std::nullopt});
  cases.push_back(
      {"project.select",
       agent::StudioProjectSelectRequestV1{.project_ref = "project-1"},
       std::nullopt, std::nullopt});
  cases.push_back(
      {"workspace.tab.open",
       agent::StudioFileTabRequestV1{
           .source = agent::StudioFileSourceV1::Project,
           .target = agent::StudioTargetV1::Server,
           .path = "src/lib.rs",
           .reference_ref = std::nullopt},
       std::nullopt, std::nullopt});
  cases.push_back(
      {"workspace.tab.close",
       agent::StudioFileTabRequestV1{
           .source = agent::StudioFileSourceV1::Project,
           .target = agent::StudioTargetV1::Server,
           .path = "src/lib.rs",
           .reference_ref = std::nullopt},
       std::nullopt, std::nullopt});
  cases.push_back({"diagnostics.local.get", agent::NoArgumentsV1{},
                   std::nullopt, std::nullopt});
  cases.push_back({"runtime.status.get", agent::NoArgumentsV1{},
                   std::nullopt, std::nullopt});
  cases.push_back(
      {"runtime.test_draft",
       agent::StudioRuntimeTestDraftRequestV1{
           .expected_revision = "1",
           .targets = {agent::StudioTargetV1::Server}},
       "workspace-lease", std::nullopt});
  cases.push_back(
      {"runtime.deploy_live",
       agent::StudioRuntimeDeployLiveRequestV1{
           .expected_revision = "1",
           .project_content_hash =
               "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
           .targets = {agent::StudioTargetV1::Server},
           .pairing_preference =
               agent::StudioPairingPreferenceV1::None,
           .draft = false},
       "workspace-lease", "approved"});
  cases.push_back(
      {"runtime.invoke",
       agent::StudioRuntimeInvokeRequestV1{
           .export_name = "invoke",
           .environment = agent::StudioRuntimeEnvironmentV1::Live,
           .params = {{.name = "enabled",
                       .type =
                           agent::StudioRuntimeParameterTypeV1::Boolean,
                       .value = "true"}}},
       "lease-1", "approved"});
  cases.push_back({"runtime.stop", agent::NoArgumentsV1{}, std::nullopt,
                   std::nullopt});

  std::size_t index = 0;
  for (auto& item : cases) {
    std::optional<agent::NativeToolResultV1> completed;
    dispatcher.dispatch(
        invocation(item.name, std::move(item.arguments), ready.clock,
                   "studio-" + std::to_string(index++), item.lease,
                   item.approval),
        [&](auto result) { completed = std::move(result); });
    CHECK(completed.has_value());
    CHECK_EQ(completed->status,
             agent::NativeToolResultStatusV1::Succeeded);
    CHECK(completed->output.has_value());
  }
  CHECK_EQ(studio.dispatched.size(), 11U);

  mode = agent::NativeAgentModeV1::Ask;
  std::optional<agent::NativeToolResultV1> capability_result;
  const auto capability_call =
      invocation("game.capabilities.get", agent::NoArgumentsV1{},
                 ready.clock, "game-capabilities");
  dispatcher.dispatch(
      capability_call,
      [&](auto result) { capability_result = std::move(result); });
  CHECK(capability_result);
  CHECK_EQ(capability_result->status,
           agent::NativeToolResultStatusV1::Succeeded);
  ready.context = "context-after-result";
  std::optional<agent::NativeToolResultV1> capability_replay;
  dispatcher.dispatch(
      capability_call,
      [&](auto result) { capability_replay = std::move(result); });
  CHECK(capability_replay);
  CHECK_EQ(capability_replay->status,
           agent::NativeToolResultStatusV1::Succeeded);
  ready.context = "context-1";

  std::optional<agent::NativeToolResultV1> observation_result;
  dispatcher.dispatch(
      invocation("game.observe",
                 ObserveRequestV1{.detail = ObserveDetailV1::Minimal,
                                  .max_nearby_actors = 1,
                                  .max_nearby_voxels = 0},
                 ready.clock, "game-observe", "lease-1"),
      [&](auto result) { observation_result = std::move(result); });
  CHECK(observation_result);
  CHECK_EQ(observation_result->status,
           agent::NativeToolResultStatusV1::Succeeded);

  mode = agent::NativeAgentModeV1::Play;
  std::size_t command_index = 0;
  for (auto command : allCommands()) {
    const auto kind = commandKind(command);
    std::optional<agent::NativeToolResultV1> command_result;
    dispatcher.dispatch(
        invocation(std::string(toolName(kind)), std::move(command),
                   ready.clock,
                   "dispatcher-command-" +
                       std::to_string(command_index++),
                   kind == CommandKindV1::Stop
                       ? std::nullopt
                       : std::optional<std::string>("lease-1")),
        [&](auto result) { command_result = std::move(result); });
    CHECK(command_result);
    CHECK_EQ(command_result->status,
             agent::NativeToolResultStatusV1::Succeeded);
  }
  CHECK_EQ(command_index, 12U);
}

void testNativeDispatcherFencesLateResultsAndStopsOffline() {
  ReadyHost ready;
  FakeStudioHost studio;
  agent::NativeAgentModeV1 mode = agent::NativeAgentModeV1::Ask;
  agent::NativeToolDispatcherV1 dispatcher(
      ready.manager, &studio,
      {.clock = &ready.clock,
       .session_id = [] { return std::optional<std::string>("session-1"); },
       .client_epoch = [] { return std::optional<std::string>("1"); },
       .context_version = [&] { return ready.context; },
       .mode = [&] { return mode; },
       .is_lease_active =
           [](std::string_view id, LeaseKindV1 kind) {
             return kind == LeaseKindV1::Workspace &&
                    id == "workspace-lease";
           },
       .validate_argument_hash = [](const auto&) { return true; },
       .validate_approval_grant = {}});

  auto unknown =
      invocation("studio.context.get", agent::NoArgumentsV1{}, ready.clock,
                 "server-tool");
  unknown.name = "workspace.file.read";
  unknown.descriptor_digest =
      "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  std::optional<agent::NativeToolResultV1> unknown_result;
  dispatcher.dispatch(
      std::move(unknown),
      [&](auto result) { unknown_result = std::move(result); });
  CHECK(unknown_result);
  CHECK_EQ(unknown_result->status,
           agent::NativeToolResultStatusV1::Failed);
  CHECK_EQ(unknown_result->error->code, "AGENT_TOOL_UNKNOWN");

  studio.hold = true;
  std::optional<agent::NativeToolResultV1> late;
  dispatcher.dispatch(
      invocation("runtime.status.get", agent::NoArgumentsV1{}, ready.clock,
                 "late-studio"),
      [&](auto result) { late = std::move(result); });
  CHECK(!late);
  ready.context = "context-2";
  studio.completePending();
  CHECK(late);
  CHECK_EQ(late->status, agent::NativeToolResultStatusV1::Failed);
  CHECK_EQ(late->error->code, "AGENT_CONTEXT_STALE");

  ready.context = "context-1";
  mode = agent::NativeAgentModeV1::Build;
  studio.hold = true;
  std::optional<agent::NativeToolResultV1> fenced_effect;
  dispatcher.dispatch(
      invocation(
          "runtime.test_draft",
          agent::StudioRuntimeTestDraftRequestV1{
              .expected_revision = "1",
              .targets = {agent::StudioTargetV1::Server}},
          ready.clock, "fenced-effect", "workspace-lease"),
      [&](auto result) { fenced_effect = std::move(result); });
  CHECK(!fenced_effect);
  ready.context = "context-2";
  studio.completePending();
  CHECK(fenced_effect);
  CHECK_EQ(fenced_effect->status,
           agent::NativeToolResultStatusV1::OutcomeUnknown);
  CHECK_EQ(fenced_effect->error->code, "AGENT_TOOL_OUTCOME_UNKNOWN");
  CHECK(!fenced_effect->error->retryable);

  ready.context = "context-1";
  studio.hold = false;
  auto expired_call = invocation(
      "runtime.test_draft",
      agent::StudioRuntimeTestDraftRequestV1{
          .expected_revision = "1",
          .targets = {agent::StudioTargetV1::Server}},
      ready.clock, "expired-before-dispatch", "workspace-lease");
  expired_call.deadline = iso(ready.clock.epoch - 1);
  const auto dispatches_before_expired = studio.dispatched.size();
  std::optional<agent::NativeToolResultV1> expired;
  dispatcher.dispatch(
      std::move(expired_call),
      [&](auto result) { expired = std::move(result); });
  CHECK(expired);
  CHECK_EQ(expired->status, agent::NativeToolResultStatusV1::Failed);
  CHECK_EQ(expired->error->code, "AGENT_TOOL_TIMEOUT");
  CHECK_EQ(studio.dispatched.size(), dispatches_before_expired);

  studio.definitive_failure = true;
  std::optional<agent::NativeToolResultV1> definitive;
  dispatcher.dispatch(
      invocation(
          "runtime.test_draft",
          agent::StudioRuntimeTestDraftRequestV1{
              .expected_revision = "1",
              .targets = {agent::StudioTargetV1::Server}},
          ready.clock, "definitive-pre-effect", "workspace-lease"),
      [&](auto result) { definitive = std::move(result); });
  CHECK(definitive);
  CHECK_EQ(definitive->status, agent::NativeToolResultStatusV1::Failed);
  CHECK_EQ(definitive->error->code, "AGENT_TOOL_FAILED");
  studio.definitive_failure = false;

  studio.ambiguous_failure = true;
  std::optional<agent::NativeToolResultV1> ambiguous;
  dispatcher.dispatch(
      invocation(
          "runtime.test_draft",
          agent::StudioRuntimeTestDraftRequestV1{
              .expected_revision = "1",
              .targets = {agent::StudioTargetV1::Server}},
          ready.clock, "ambiguous-after-dispatch", "workspace-lease"),
      [&](auto result) { ambiguous = std::move(result); });
  CHECK(ambiguous);
  CHECK_EQ(ambiguous->status,
           agent::NativeToolResultStatusV1::OutcomeUnknown);
  CHECK_EQ(ambiguous->error->code, "AGENT_TOOL_OUTCOME_UNKNOWN");
  CHECK(!ambiguous->error->retryable);
  studio.ambiguous_failure = false;

  studio.invalid_output = true;
  std::optional<agent::NativeToolResultV1> invalid_effect_output;
  dispatcher.dispatch(
      invocation(
          "runtime.test_draft",
          agent::StudioRuntimeTestDraftRequestV1{
              .expected_revision = "1",
              .targets = {agent::StudioTargetV1::Server}},
          ready.clock, "invalid-effect-output", "workspace-lease"),
      [&](auto result) { invalid_effect_output = std::move(result); });
  CHECK(invalid_effect_output);
  CHECK_EQ(invalid_effect_output->status,
           agent::NativeToolResultStatusV1::OutcomeUnknown);
  CHECK_EQ(invalid_effect_output->error->code,
           "AGENT_TOOL_OUTPUT_INVALID");
  CHECK(!invalid_effect_output->error->retryable);
  studio.invalid_output = false;

  studio.hold = true;
  std::optional<agent::NativeToolResultV1> cancelled;
  dispatcher.dispatch(
      invocation(
          "runtime.test_draft",
          agent::StudioRuntimeTestDraftRequestV1{
              .expected_revision = "1",
              .targets = {agent::StudioTargetV1::Server}},
          ready.clock, "cancelled-studio", "workspace-lease"),
      [&](auto result) {
        studio.events.push_back("callback");
        cancelled = std::move(result);
      });
  dispatcher.cancelActive(PreemptionReasonV1::ESCAPE);
  CHECK(cancelled);
  CHECK_EQ(cancelled->status,
           agent::NativeToolResultStatusV1::OutcomeUnknown);
  CHECK_EQ(cancelled->error->code, "AGENT_TOOL_OUTCOME_UNKNOWN");
  CHECK(!cancelled->error->retryable);
  const auto clear = std::find(studio.events.begin(), studio.events.end(),
                               "clear:ESCAPE");
  const auto callback =
      std::find(studio.events.begin(), studio.events.end(), "callback");
  CHECK(clear != studio.events.end());
  CHECK(callback != studio.events.end());
  CHECK(clear < callback);
  studio.completePending();

  mode = agent::NativeAgentModeV1::Play;
  ready.manager.disconnect();
  std::optional<agent::NativeToolResultV1> stopped;
  auto stop =
      invocation("game.control.stop", GameCommandV1{StopCommandV1{}},
                 ready.clock, "dispatcher-offline-stop");
  stop.client_epoch = "999";
  stop.context_version = "stale";
  stop.deadline = "expired";
  dispatcher.dispatch(
      std::move(stop), [&](auto result) { stopped = std::move(result); });
  CHECK(stopped);
  CHECK_EQ(stopped->status,
           agent::NativeToolResultStatusV1::Succeeded);
}

agent::AgentToolInvocation publicInvocation(
    std::string name, std::string arguments, const FakeClock& clock,
    std::string id, std::optional<std::string> lease = std::nullopt) {
  const auto& descriptor =
      agent::canonicalAgentToolRegistryV1().require(name, "1.0.0");
  agent::AgentToolInvocation value;
  value.sessionId = "session-1";
  value.runId = "run-1";
  value.toolCallId = std::move(id);
  value.name = std::move(name);
  value.version = "1.0.0";
  value.descriptorDigest = descriptor.descriptorDigest;
  value.argumentsJson = std::move(arguments);
  value.arguments = graphql::Json::parse(value.argumentsJson);
  value.argumentHash =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  value.contextVersion = "context-1";
  value.clientEpoch = "1";
  value.leaseId = std::move(lease);
  value.deadline = iso(clock.epoch + 120'000);
  return value;
}

void testNativeBrowserDispatcherAdapterConversionAndLifecycle() {
  ReadyHost ready;
  FakeStudioHost studio;
  agent::NativeAgentModeV1 mode = agent::NativeAgentModeV1::Ask;
  agent::NativeToolDispatcherV1 native(
      ready.manager, &studio,
      {.clock = &ready.clock,
       .session_id = [] { return std::optional<std::string>("session-1"); },
       .client_epoch = [] { return std::optional<std::string>("1"); },
       .context_version = [&] { return ready.context; },
       .mode = [&] { return mode; },
       .is_lease_active =
           [](std::string_view id, LeaseKindV1 kind) {
             return (kind == LeaseKindV1::Play && id == "lease-1") ||
                    (kind == LeaseKindV1::Workspace &&
                     id == "workspace-lease");
           },
       .validate_argument_hash = [](const auto&) { return true; },
       .validate_approval_grant = {}});
  agent::NativeBrowserToolDispatcherAdapter adapter(
      native, {.clock = &ready.clock});

  std::optional<agent::AgentOutcome<agent::AgentToolResult>> capabilities;
  adapter.dispatch(
      publicInvocation("game.capabilities.get", "{}", ready.clock,
                       "public-capabilities"),
      [&](auto value) { capabilities = std::move(value); });
  CHECK(capabilities && capabilities->ok());
  CHECK_EQ(capabilities->value->status,
           agent::AgentToolResultStatus::Succeeded);
  CHECK(capabilities->value->outputJson.has_value());
  const auto capabilityJson =
      graphql::Json::parse(*capabilities->value->outputJson);
  CHECK(capabilityJson["contractVersion"].asString() ==
        "crowdy.player-host/1");
  CHECK_EQ(capabilityJson["commands"].size(), std::size_t{12});
  CHECK(adapter.has("public-capabilities"));

  mode = agent::NativeAgentModeV1::Play;
  std::optional<agent::AgentOutcome<agent::AgentToolResult>> moved;
  adapter.dispatch(
      publicInvocation(
          "game.control.move",
          R"({"observationId":"observation-1","capabilityRevision":"capability-7","controlledEntityId":"player-7","direction":"FORWARD","intensity":1,"durationMs":100})",
          ready.clock, "public-move", "lease-1"),
      [&](auto value) { moved = std::move(value); });
  CHECK(moved && moved->ok());
  CHECK_EQ(moved->value->status,
           agent::AgentToolResultStatus::Succeeded);
  const auto moveJson = graphql::Json::parse(*moved->value->outputJson);
  CHECK(moveJson["commandKind"].asString() == "MOVE");
  CHECK(moveJson["status"].asString() == "SUCCEEDED");

  auto malformed = publicInvocation(
      "game.control.move",
      R"({"observationId":"observation-1","capabilityRevision":"capability-7","controlledEntityId":"player-7","direction":"FORWARD","intensity":1,"durationMs":100,"appId":"2"})",
      ready.clock, "public-malformed", "lease-1");
  std::optional<agent::AgentOutcome<agent::AgentToolResult>> rejected;
  adapter.dispatch(std::move(malformed),
                   [&](auto value) { rejected = std::move(value); });
  CHECK(rejected && rejected->ok());
  CHECK_EQ(rejected->value->status, agent::AgentToolResultStatus::Failed);
  CHECK_EQ(rejected->value->error->code, "AGENT_TOOL_INPUT_INVALID");

  studio.hold = true;
  mode = agent::NativeAgentModeV1::Build;
  auto pending = publicInvocation(
      "runtime.test_draft",
      R"({"expectedRevision":"1","targets":["SERVER"]})", ready.clock,
      "public-cancel");
  pending.leaseId = "workspace-lease";
  std::optional<agent::AgentOutcome<agent::AgentToolResult>> cancelled;
  adapter.dispatch(std::move(pending),
                   [&](auto value) { cancelled = std::move(value); });
  adapter.cancelActive(agent::AgentPreemptionReason::Escape);
  CHECK(cancelled && cancelled->ok());
  CHECK_EQ(cancelled->value->status,
           agent::AgentToolResultStatus::OutcomeUnknown);
  CHECK_EQ(cancelled->value->error->code,
           "AGENT_TOOL_OUTCOME_UNKNOWN");
  CHECK(!cancelled->value->error->retryable);
  studio.completePending();
  adapter.tick();
  adapter.clearClosedSession();
}

}  // namespace

int main() {
  testSchemasAndEveryCommandMapping();
  testObservationBoundsAndCapabilitiesValidation();
  testScopesConditionalApprovalTargetsEpochAndContext();
  testSerializedRatesAndHeartbeatExpiry();
  testCapabilityEntityAndPolicyPreemption();
  testHumanTakeoverOfflineStopDeathAndDisconnect();
  testAmbiguousOutcomesExecuteOnceAndLateResults();
  testNativeDispatcherGameAndStudioRouting();
  testNativeDispatcherFencesLateResultsAndStopsOffline();
  testNativeBrowserDispatcherAdapterConversionAndLifecycle();
  std::printf("player_host_test passed\n");
  return 0;
}
