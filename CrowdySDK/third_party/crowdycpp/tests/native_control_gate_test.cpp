#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/core/clock.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/player_host/adapter.hpp"
#include "crowdy/player_host/control_gate.hpp"
#include "crowdy/player_host/lease_manager.hpp"
#include "crowdy/player_host/schemas.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::player_host;

namespace {

constexpr std::int64_t kBaseTime = 1'784'851'200'000LL;

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

PlayerHostCapabilitiesV1 makeCapabilities(std::int64_t now) {
  PlayerHostCapabilitiesV1 value;
  value.game_id = "native-control-gate-test";
  value.revision = "capability-1";
  value.controlled_entity_id = "player-1";
  value.observation = {
      .max_age_ms = 2'000,
      .max_nearby_actors = 8,
      .max_nearby_voxels = 8,
  };
  value.advertised_at = iso(now);
  return value;
}

AgentControlLeaseV1 localLease(std::int64_t now, std::string id) {
  AgentControlLeaseV1 value;
  value.lease_id = std::move(id);
  value.kind = LeaseKindV1::Play;
  value.status = LeaseStatusV1::Active;
  value.client_epoch = "1";
  value.scopes = {LeaseScopeV1::Observe, LeaseScopeV1::Locomotion,
                  LeaseScopeV1::Interact};
  value.holder = "Current player";
  value.controlled_entity_id = "player-1";
  value.host_capability_revision = "capability-1";
  value.context_version = "context-1";
  value.granted_at = iso(now);
  value.expires_at = iso(now + 60'000);
  return value;
}

agent::AgentLease remoteLease(const AgentControlLeaseV1& source) {
  agent::AgentLease value;
  value.leaseId = source.lease_id;
  value.kind = agent::AgentLeaseKind::Play;
  value.status = agent::AgentLeaseStatus::Active;
  value.clientEpoch = source.client_epoch;
  for (const auto scope : source.scopes) {
    value.scopes.emplace_back(toString(scope));
  }
  value.holder = source.holder;
  value.controlledEntityId = source.controlled_entity_id;
  value.hostCapabilityRevision = source.host_capability_revision;
  value.contextVersion = source.context_version;
  value.grantedAt = source.granted_at;
  value.expiresAt = source.expires_at;
  return value;
}

class FakePlayerHost final : public PlayerHostAdapterV1 {
 public:
  FakePlayerHost(std::vector<std::string>& events, FakeClock& clock)
      : events_(events), clock_(clock) {}

  void capabilities(CancellationTokenV1,
                    CapabilitiesCallbackV1 callback) override {
    callback(AdapterResultV1<PlayerHostCapabilitiesV1>::success(
        makeCapabilities(clock_.epoch)));
  }

  void observe(const ObserveRequestV1&, CancellationTokenV1,
               ObservationCallbackV1 callback) override {
    callback(AdapterResultV1<GameObservationV1>::failure(
        {.code = "NOT_USED",
         .message = "observation is not used by control-gate tests",
         .retryable = false,
         .remediation = std::nullopt,
         .field = std::nullopt,
         .required_scope = std::nullopt}));
  }

  void dispatch(const GameCommandV1&, const ValidatedGateV1&,
                CancellationTokenV1, CommandCallbackV1 callback) override {
    callback(AdapterResultV1<GameCommandResultV1>::failure(
        {.code = "NOT_USED",
         .message = "dispatch is not used by control-gate tests",
         .retryable = false,
         .remediation = std::nullopt,
         .field = std::nullopt,
         .required_scope = std::nullopt}));
  }

  void clearAgentIntent(PreemptionReasonV1 reason) noexcept override {
    events_.push_back("clear:" + std::string(preemptionReasonName(reason)));
  }

 private:
  std::vector<std::string>& events_;
  FakeClock& clock_;
};

class FakeController final : public INativePlayerControlGateController {
 public:
  explicit FakeController(std::vector<std::string>& events)
      : events_(events) {}

  std::vector<agent::AgentLease> leases;
  bool throw_on_leases = false;
  bool throw_on_revoke = false;
  bool throw_on_pause = false;
  bool throw_on_stop = false;

  std::vector<agent::AgentLease> playerControlLeases() const override {
    if (throw_on_leases) throw std::runtime_error("state unavailable");
    return leases;
  }

  void revokePlayerControlLease(std::string lease_id,
                                PreemptionReasonV1 reason) override {
    events_.push_back("remote:revoke:" + lease_id + ":" +
                      std::string(preemptionReasonName(reason)));
    if (throw_on_revoke) throw std::runtime_error("remote revoke failed");
  }

  void pausePlayerControl() override {
    events_.push_back("remote:pause");
    if (throw_on_pause) throw std::runtime_error("remote pause failed");
  }

  void stopPlayerControl() override {
    events_.push_back("remote:stop");
    if (throw_on_stop) throw std::runtime_error("remote stop failed");
  }

 private:
  std::vector<std::string>& events_;
};

struct ReadyControl {
  FakeClock clock;
  std::vector<std::string> events;
  FakePlayerHost host;
  std::string context = "context-1";
  AgentControlLeaseManager manager;
  FakeController controller;

  explicit ReadyControl(std::string initial_lease_id = "lease-1")
      : host(events, clock),
        manager(host, managerOptions()),
        controller(events) {
    CHECK(!manager.attach("1"));
    std::optional<AdapterResultV1<PlayerHostCapabilitiesV1>> refreshed;
    manager.refreshCapabilities(
        [&](auto result) { refreshed = std::move(result); });
    CHECK(refreshed && refreshed->ok());
    activate(std::move(initial_lease_id));
  }

  AgentControlLeaseManagerOptionsV1 managerOptions() {
    AgentControlLeaseManagerOptionsV1 options;
    options.clock = &clock;
    options.context_version = [this] { return context; };
    return options;
  }

  void activate(std::string id) {
    auto lease = localLease(clock.epoch, std::move(id));
    CHECK(!manager.grantLease(lease));
    controller.leases = {remoteLease(lease)};
  }
};

std::size_t countEvent(const std::vector<std::string>& events,
                       std::string_view event) {
  return static_cast<std::size_t>(
      std::count(events.begin(), events.end(), event));
}

NativePlayerControlGateOptionsV1 gateOptions(const FakeClock& clock) {
  return {
      .clock = &clock,
      .human_input_active_ms = 150,
      .on_preempt = {},
  };
}

std::string fixtureText(std::string_view name) {
  const std::string path =
      std::string(CROWDY_PARITY_FIXTURE_DIR) + "/" + std::string(name);
  std::ifstream input(path, std::ios::binary);
  CHECK(input.good());
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

PreemptionReasonV1 fixtureReason(std::string_view value) {
  for (const auto reason : kClosedPreemptionReasonsV1) {
    if (preemptionReasonName(reason) == value) return reason;
  }
  CHECK(false);
  return PreemptionReasonV1::HUMAN_STOP;
}

std::vector<std::string> fixtureStrings(const graphql::Json& values) {
  CHECK(values.isArray());
  std::vector<std::string> result;
  values.forEach(
      [&](const graphql::Json& value) { result.push_back(value.asString()); });
  return result;
}

std::string normalizedFixtureEvent(std::string value) {
  if (value.rfind("clear:", 0) == 0) {
    const auto separator = value.find(':', 6);
    if (separator != std::string::npos) {
      return "clear:" + value.substr(separator + 1);
    }
  }
  if (value.rfind("revoke:", 0) == 0) {
    const auto separator = value.find(':', 7);
    if (separator != std::string::npos) {
      return "remote:revoke:" + value.substr(separator + 1);
    }
  }
  if (value.rfind("pause:", 0) == 0) return "remote:pause";
  if (value.rfind("stop:", 0) == 0) return "remote:stop";
  return value;
}

void checkFixtureOrder(const std::vector<std::string>& actual,
                       const graphql::Json& expected) {
  auto normalized = fixtureStrings(expected);
  for (auto& event : normalized) {
    event = normalizedFixtureEvent(std::move(event));
  }
  CHECK(actual == normalized);
}

std::string snapshotToken(
    const NativePlayerControlGateSnapshotV1& value) {
  return std::string("snapshot:") +
         (value.bound ? "bound:" : "unbound:") +
         (value.active_lease ? value.active_lease->leaseId : "none") + ":" +
         (value.last_preemption
              ? std::string(preemptionReasonName(*value.last_preemption))
              : "none") +
         ":" + (value.human_input_active ? "human:" : "idle:") +
         (value.offline_stop ? "offline-stop" : "online");
}

void checkFixtureSnapshot(
    const NativePlayerControlGateSnapshotV1& actual,
    const graphql::Json& expected) {
  CHECK(actual.bound == expected["bound"].asBool());
  const auto expectedLease = expected["activeLease"];
  if (!expectedLease.ok() || expectedLease.isNull()) {
    CHECK(!actual.active_lease);
  } else {
    CHECK(actual.active_lease.has_value());
    const auto& lease = *actual.active_lease;
    CHECK(lease.leaseId == expectedLease["leaseId"].asString());
    CHECK(expectedLease["kind"].asStringView() == "PLAY");
    CHECK(lease.kind == agent::AgentLeaseKind::Play);
    CHECK(expectedLease["status"].asStringView() == "ACTIVE");
    CHECK(lease.status == agent::AgentLeaseStatus::Active);
    CHECK(lease.clientEpoch == expectedLease["clientEpoch"].asString());
    CHECK(lease.scopes == fixtureStrings(expectedLease["scopes"]));
    CHECK(lease.holder == expectedLease["holder"].asString());
    CHECK(lease.controlledEntityId ==
          expectedLease["controlledEntityId"].asString());
    CHECK(lease.hostCapabilityRevision ==
          expectedLease["hostCapabilityRevision"].asString());
    CHECK(lease.contextVersion ==
          expectedLease["contextVersion"].asString());
    CHECK(lease.grantedAt == expectedLease["grantedAt"].asString());
    CHECK(lease.expiresAt == expectedLease["expiresAt"].asString());
  }
  const auto lastPreemption = expected["lastPreemption"];
  if (lastPreemption.ok()) {
    CHECK(actual.last_preemption ==
          std::optional<PreemptionReasonV1>{
              fixtureReason(lastPreemption.asStringView())});
  } else {
    CHECK(!actual.last_preemption);
  }
  CHECK(actual.human_input_active ==
        expected["humanInputActive"].asBool());
  CHECK(actual.offline_stop == expected["offlineStop"].asBool());
}

void checkFixtureSnapshots(
    const std::vector<NativePlayerControlGateSnapshotV1>& actual,
    const graphql::Json& expected) {
  CHECK(actual.size() == expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    checkFixtureSnapshot(actual[index], expected.at(index));
  }
}

void testHumanHooksWindowAndDuplicateSuppression() {
  ReadyControl ready;
  NativePlayerControlGate gate([](PreemptionReasonV1) {},
                               gateOptions(ready.clock));
  std::vector<NativePlayerControlGateSnapshotV1> snapshots;
  auto unsubscribe =
      gate.subscribe([&](const auto& value) { snapshots.push_back(value); });
  CHECK_EQ(snapshots.size(), 1U);
  CHECK(!snapshots.back().bound);

  auto unbind = gate.bind(ready.manager, ready.controller);
  CHECK(snapshots.back().bound);
  CHECK(snapshots.back().active_lease);
  CHECK_EQ(snapshots.back().active_lease->leaseId, "lease-1");

  gate.onHumanKeyboardInput();
  ready.events.push_back("human-handler");
  CHECK_EQ(ready.events[0], "clear:HUMAN_INPUT");
  CHECK_EQ(ready.events[1], "remote:revoke:lease-1:HUMAN_INPUT");
  CHECK_EQ(ready.events[2], "human-handler");
  CHECK(!gate.snapshot().active_lease);
  CHECK(gate.snapshot().human_input_active);
  CHECK_EQ(*gate.snapshot().last_preemption,
           PreemptionReasonV1::HUMAN_INPUT);

  gate.onHumanPointerInput();
  gate.onHumanMovementInput();
  CHECK_EQ(countEvent(ready.events, "clear:HUMAN_INPUT"), 1U);
  CHECK_EQ(countEvent(ready.events,
                      "remote:revoke:lease-1:HUMAN_INPUT"),
           1U);

  ready.clock.advance(149);
  CHECK(gate.humanInputActive());
  ready.clock.advance(1);
  CHECK(!gate.humanInputActive());

  ready.activate("lease-pointer");
  gate.onHumanPointerInput();
  CHECK_EQ(ready.events[ready.events.size() - 2], "clear:HUMAN_INPUT");
  CHECK_EQ(ready.events.back(),
           "remote:revoke:lease-pointer:HUMAN_INPUT");

  ready.activate("lease-movement");
  gate.onHumanMovementInput();
  CHECK_EQ(ready.events[ready.events.size() - 2], "clear:HUMAN_INPUT");
  CHECK_EQ(ready.events.back(),
           "remote:revoke:lease-movement:HUMAN_INPUT");

  ready.activate("lease-escape");
  gate.onHumanKeyboardInput(NativePlayerControlKeyboardInputV1::Escape);
  CHECK_EQ(ready.events[ready.events.size() - 2], "clear:ESCAPE");
  CHECK_EQ(ready.events.back(), "remote:revoke:lease-escape:ESCAPE");

  unsubscribe();
  const auto snapshot_count = snapshots.size();
  gate.onHumanPointerInput();
  CHECK_EQ(snapshots.size(), snapshot_count);
  unbind();
}

void testPauseStopOfflineAndRemoteFailures() {
  ReadyControl ready;
  NativePlayerControlGate gate([](PreemptionReasonV1) {},
                               gateOptions(ready.clock));
  auto unbind = gate.bind(ready.manager, ready.controller);

  gate.pause();
  CHECK_EQ(ready.events[0], "clear:HUMAN_STOP");
  CHECK_EQ(ready.events[1], "remote:pause");
  CHECK(!gate.snapshot().active_lease);
  CHECK(!gate.snapshot().offline_stop);

  ready.activate("lease-pause-failure");
  ready.controller.throw_on_pause = true;
  gate.pause();
  CHECK_EQ(ready.events[ready.events.size() - 2], "clear:HUMAN_STOP");
  CHECK_EQ(ready.events.back(), "remote:pause");

  ready.activate("lease-stop-failure");
  ready.controller.throw_on_stop = true;
  gate.stop();
  CHECK_EQ(ready.events[ready.events.size() - 2], "clear:HUMAN_STOP");
  CHECK_EQ(ready.events.back(), "remote:stop");
  CHECK(!gate.snapshot().active_lease);
  CHECK(gate.snapshot().offline_stop);
  CHECK_EQ(*gate.snapshot().last_preemption,
           PreemptionReasonV1::HUMAN_STOP);

  auto reset_offline = gate.bind(ready.manager, ready.controller);
  CHECK(!gate.snapshot().offline_stop);
  reset_offline();
  unbind();

  std::vector<std::string> offline_events;
  NativePlayerControlGate offline(
      [&](PreemptionReasonV1 reason) {
        offline_events.push_back(
            "fallback:" + std::string(preemptionReasonName(reason)));
      },
      gateOptions(ready.clock));
  offline.stop();
  CHECK_EQ(offline_events.front(), "fallback:HUMAN_STOP");
  CHECK(offline.snapshot().offline_stop);
  CHECK(!offline.snapshot().bound);
}

void testSafetyTransitionsAndExactRebind() {
  ReadyControl ready;
  NativePlayerControlGate gate([](PreemptionReasonV1) {},
                               gateOptions(ready.clock));
  auto unbind = gate.bind(ready.manager, ready.controller);

  struct Transition {
    std::string id;
    PreemptionReasonV1 reason;
    std::function<void()> invoke;
  };
  std::vector<Transition> remote_transitions = {
      {"death", PreemptionReasonV1::DEATH, [&] { gate.onDeath(); }},
      {"permission", PreemptionReasonV1::PERMISSION_CHANGED,
       [&] { gate.onPermissionChanged(); }},
      {"admission", PreemptionReasonV1::ADMISSION_CHANGED,
       [&] { gate.onAdmissionChanged(); }},
      {"context", PreemptionReasonV1::CONTEXT_CHANGED,
       [&] { gate.onContextChanged(); }},
      {"target", PreemptionReasonV1::CONTROL_TARGET_CHANGED,
       [&] { gate.onControlTargetChanged(); }},
  };
  for (auto& transition : remote_transitions) {
    const auto lease_id = "lease-" + transition.id;
    ready.activate(lease_id);
    transition.invoke();
    CHECK_EQ(ready.events[ready.events.size() - 2],
             "clear:" +
                 std::string(preemptionReasonName(transition.reason)));
    CHECK_EQ(ready.events.back(),
             "remote:revoke:" + lease_id + ":" +
                 std::string(preemptionReasonName(transition.reason)));
    CHECK_EQ(*gate.snapshot().last_preemption, transition.reason);
    CHECK(!gate.snapshot().active_lease);
  }

  ready.activate("lease-disconnect");
  const auto before_disconnect = ready.events.size();
  gate.onDisconnected();
  CHECK_EQ(ready.events[before_disconnect], "clear:DISCONNECTED");
  CHECK_EQ(ready.events.size(), before_disconnect + 1);

  ready.activate("lease-background");
  const auto before_background = ready.events.size();
  gate.onBackgrounded();
  CHECK_EQ(ready.events[before_background], "clear:DISCONNECTED");
  CHECK_EQ(ready.events.size(), before_background + 1);

  ready.activate("lease-reattach");
  const auto before_reattach = ready.events.size();
  gate.onClientReattached();
  CHECK_EQ(ready.events[before_reattach], "clear:CLIENT_REATTACHED");
  CHECK_EQ(ready.events.size(), before_reattach + 1);

  ready.controller.leases = {
      remoteLease(localLease(ready.clock.epoch, "lease-old-epoch"))};
  CHECK(!ready.manager.attach("2"));
  CHECK(!gate.snapshot().active_lease);
  unbind();

  ReadyControl first;
  ReadyControl second;
  second.activate("lease-second");
  NativePlayerControlGate rebound([](PreemptionReasonV1) {},
                                  gateOptions(first.clock));
  auto old_unbind = rebound.bind(first.manager, first.controller);
  auto current_unbind = rebound.bind(second.manager, second.controller);
  CHECK_EQ(first.events.back(), "clear:CLIENT_REATTACHED");
  CHECK(rebound.snapshot().bound);
  CHECK_EQ(rebound.snapshot().active_lease->leaseId, "lease-second");

  const auto second_events = second.events.size();
  old_unbind();
  CHECK(rebound.snapshot().bound);
  CHECK_EQ(second.events.size(), second_events);

  current_unbind();
  CHECK(!rebound.snapshot().bound);
  CHECK_EQ(second.events.back(), "clear:DISCONNECTED");
}

void testStaleLeaseControllerFailureAndNoController() {
  ReadyControl ready;
  ready.manager.preempt(PreemptionReasonV1::HUMAN_STOP);
  ready.events.clear();
  ready.controller.throw_on_revoke = true;
  NativePlayerControlGate gate([](PreemptionReasonV1) {},
                               gateOptions(ready.clock));
  auto unbind = gate.bind(ready.manager, ready.controller);
  CHECK(gate.snapshot().active_lease);

  gate.onDeath();
  CHECK_EQ(ready.events[0], "clear:DEATH");
  CHECK_EQ(ready.events[1], "remote:revoke:lease-1:DEATH");
  CHECK(!gate.snapshot().active_lease);

  const auto state_failure_lease =
      localLease(ready.clock.epoch, "lease-state-failure");
  ready.controller.leases = {remoteLease(state_failure_lease)};
  CHECK(gate.snapshot().active_lease);
  ready.controller.throw_on_leases = true;
  const auto before_state_failure = ready.events.size();
  gate.onHumanPointerInput();
  CHECK_EQ(ready.events[before_state_failure], "clear:HUMAN_INPUT");
  CHECK_EQ(ready.events[before_state_failure + 1],
           "remote:revoke:lease-state-failure:HUMAN_INPUT");
  ready.controller.throw_on_leases = false;
  CHECK(!gate.snapshot().active_lease);
  unbind();

  std::vector<std::string> events;
  NativePlayerControlGate no_controller(
      [&](PreemptionReasonV1 reason) {
        events.push_back(std::string(preemptionReasonName(reason)));
      },
      gateOptions(ready.clock));
  no_controller.onContextChanged();
  CHECK_EQ(events.back(), "CONTEXT_CHANGED");
  CHECK_EQ(*no_controller.snapshot().last_preemption,
           PreemptionReasonV1::CONTEXT_CHANGED);
}

void testSubscriptionsExceptionsAndDestruction() {
  ReadyControl ready;
  std::size_t preempt_callbacks = 0;
  auto gate = std::make_unique<NativePlayerControlGate>(
      [](PreemptionReasonV1) {},
      NativePlayerControlGateOptionsV1{
          .clock = &ready.clock,
          .human_input_active_ms = 150,
          .on_preempt =
              [&](PreemptionReasonV1) {
                ++preempt_callbacks;
                throw std::runtime_error("observer failed");
              },
      });
  auto unbind = gate->bind(ready.manager, ready.controller);
  std::size_t observed = 0;
  auto unsubscribe = gate->subscribe([&](const auto&) { ++observed; });
  auto throwing_unsubscribe = gate->subscribe(
      [](const auto&) { throw std::runtime_error("listener failed"); });
  CHECK_EQ(observed, 1U);

  gate->onDeath();
  CHECK_EQ(preempt_callbacks, 1U);
  CHECK(observed >= 2U);
  unsubscribe();
  const auto before = observed;
  gate->onPermissionChanged();
  CHECK_EQ(observed, before);

  gate->destroy();
  CHECK(!gate->snapshot().bound);
  CHECK_EQ(*gate->snapshot().last_preemption,
           PreemptionReasonV1::DISCONNECTED);
  throwing_unsubscribe();
  gate.reset();
  unbind();
}

void testReentrantListenerCannotTargetReplacementController() {
  ReadyControl first;
  ReadyControl second;
  second.activate("lease-replacement");
  NativePlayerControlGate gate([](PreemptionReasonV1) {},
                               gateOptions(first.clock));
  auto old_unbind = gate.bind(first.manager, first.controller);
  NativePlayerControlGate::Unbind replacement_unbind;
  bool replaced = false;
  auto unsubscribe = gate.subscribe([&](const auto& state) {
    if (!replaced && state.last_preemption &&
        *state.last_preemption == PreemptionReasonV1::HUMAN_INPUT) {
      replaced = true;
      replacement_unbind = gate.bind(second.manager, second.controller);
    }
  });

  gate.onHumanKeyboardInput();
  CHECK(replaced);
  CHECK_EQ(first.events[0], "clear:HUMAN_INPUT");
  CHECK_EQ(first.events[1], "clear:CLIENT_REATTACHED");
  CHECK_EQ(first.events.size(), 2U);
  CHECK(second.events.empty());
  CHECK_EQ(gate.snapshot().active_lease->leaseId, "lease-replacement");

  unsubscribe();
  replacement_unbind();
  old_unbind();
}

NativePlayerControlGateOptionsV1 fixtureGateOptions(
    const FakeClock& clock, std::vector<std::string>& events) {
  return {
      .clock = &clock,
      .human_input_active_ms = 150,
      .on_preempt =
          [&](PreemptionReasonV1 reason) {
            events.push_back(
                "onPreempt:" +
                std::string(preemptionReasonName(reason)));
          },
  };
}

void initializeFixtureManager(
    AgentControlLeaseManager& manager, FakeClock& clock,
    FakeController& controller, std::string lease_id) {
  CHECK(!manager.attach("1"));
  std::optional<AdapterResultV1<PlayerHostCapabilitiesV1>> refreshed;
  manager.refreshCapabilities(
      [&](auto result) { refreshed = std::move(result); });
  CHECK(refreshed && refreshed->ok());
  auto lease = localLease(clock.epoch, std::move(lease_id));
  CHECK(!manager.grantLease(lease));
  controller.leases = {remoteLease(lease)};
}

void testSharedControlGateDefaultsAndWindowFixture(
    const graphql::Json& fixture) {
  const auto expected = fixture["defaults"];
  CHECK(expected["humanInputActiveMs"].asInt64() == 150);
  ReadyControl ready("lease-human");
  NativePlayerControlGate gate(
      [&](PreemptionReasonV1 reason) {
        ready.events.push_back(
            "fallback:" +
            std::string(preemptionReasonName(reason)));
      },
      fixtureGateOptions(ready.clock, ready.events));
  checkFixtureSnapshot(gate.snapshot(), expected["initialSnapshot"]);

  std::vector<NativePlayerControlGateSnapshotV1> emissions;
  bool record = false;
  auto unsubscribe = gate.subscribe([&](const auto& value) {
    emissions.push_back(value);
    if (record) ready.events.push_back(snapshotToken(value));
  });
  checkFixtureSnapshot(emissions.back(), expected["subscribedSnapshot"]);
  auto unbind = gate.bind(ready.manager, ready.controller);
  checkFixtureSnapshot(gate.snapshot(), expected["boundSnapshot"]);

  ready.events.clear();
  emissions.clear();
  record = true;
  gate.onHumanKeyboardInput();
  ready.events.push_back("human-handler");
  checkFixtureOrder(ready.events, expected["humanInputOrder"]);
  checkFixtureSnapshots(emissions, expected["humanInputEmissions"]);
  checkFixtureSnapshot(gate.snapshot(), expected["afterHumanInput"]);
  CHECK(gate.humanInputActive() ==
        expected["window"]["activeAtZero"].asBool());
  ready.clock.advance(149);
  CHECK(gate.humanInputActive() ==
        expected["window"]["activeAt149Ms"].asBool());
  ready.clock.advance(1);
  CHECK(gate.humanInputActive() ==
        expected["window"]["activeAt150Ms"].asBool());

  unsubscribe();
  unbind();
}

void testSharedControlGateRebindStopFixture(
    const graphql::Json& fixture) {
  const auto expected = fixture["rebindOfflineStop"];
  FakeClock clock;
  std::vector<std::string> events;
  std::string context = "context-1";
  FakePlayerHost firstHost(events, clock);
  FakePlayerHost secondHost(events, clock);
  AgentControlLeaseManagerOptionsV1 managerOptions;
  managerOptions.clock = &clock;
  managerOptions.context_version = [&] { return context; };
  AgentControlLeaseManager firstManager(firstHost, managerOptions);
  AgentControlLeaseManager secondManager(secondHost, managerOptions);
  FakeController firstController(events);
  FakeController secondController(events);
  initializeFixtureManager(
      firstManager, clock, firstController, "lease-first");
  initializeFixtureManager(
      secondManager, clock, secondController, "lease-second");

  NativePlayerControlGate gate(
      [&](PreemptionReasonV1 reason) {
        events.push_back(
            "fallback:" +
            std::string(preemptionReasonName(reason)));
      },
      fixtureGateOptions(clock, events));
  std::vector<NativePlayerControlGateSnapshotV1> emissions;
  bool record = false;
  auto unsubscribe = gate.subscribe([&](const auto& value) {
    emissions.push_back(value);
    if (record) events.push_back(snapshotToken(value));
  });
  auto firstUnbind = gate.bind(firstManager, firstController);

  events.clear();
  emissions.clear();
  record = true;
  auto secondUnbind = gate.bind(secondManager, secondController);
  checkFixtureOrder(events, expected["rebindOrder"]);
  checkFixtureSnapshots(emissions, expected["rebindEmissions"]);
  checkFixtureSnapshot(gate.snapshot(), expected["reboundSnapshot"]);

  events.clear();
  emissions.clear();
  secondController.throw_on_stop = true;
  gate.stop();
  checkFixtureOrder(events, expected["stopOrder"]);
  checkFixtureSnapshots(emissions, expected["stopEmissions"]);
  checkFixtureSnapshot(gate.snapshot(), expected["stoppedSnapshot"]);

  auto resetLease = localLease(clock.epoch, "lease-reset");
  CHECK(!secondManager.grantLease(resetLease));
  secondController.leases = {remoteLease(resetLease)};
  events.clear();
  emissions.clear();
  auto resetUnbind = gate.bind(secondManager, secondController);
  checkFixtureSnapshot(
      gate.snapshot(), expected["reboundAfterStopSnapshot"]);

  events.clear();
  emissions.clear();
  resetUnbind();
  checkFixtureOrder(events, expected["unbindOrder"]);
  checkFixtureSnapshots(emissions, expected["unbindEmissions"]);
  checkFixtureSnapshot(gate.snapshot(), expected["unboundSnapshot"]);

  firstUnbind();
  secondUnbind();
  unsubscribe();
}

void invokeFixtureHook(NativePlayerControlGate& gate,
                       std::string_view action) {
  if (action == "keyboard") {
    gate.onHumanKeyboardInput();
  } else if (action == "escape") {
    gate.onHumanKeyboardInput(
        NativePlayerControlKeyboardInputV1::Escape);
  } else if (action == "pointer") {
    gate.onHumanPointerInput();
  } else if (action == "movement") {
    gate.onHumanMovementInput();
  } else if (action == "pause") {
    gate.pause();
  } else if (action == "stop") {
    gate.stop();
  } else if (action == "death") {
    gate.onDeath();
  } else if (action == "contextChanged") {
    gate.onContextChanged();
  } else if (action == "permissionChanged") {
    gate.onPermissionChanged();
  } else if (action == "controlTargetChanged") {
    gate.onControlTargetChanged();
  } else if (action == "disconnected") {
    gate.onDisconnected();
  } else if (action == "pagehide" || action == "offline" ||
             action == "backgrounded") {
    gate.onBackgrounded();
  } else {
    CHECK(false);
  }
}

void testSharedControlGateHooksFixture(
    const graphql::Json& fixture) {
  fixture["imperativeHooks"].forEach(
      [&](const graphql::Json& fixtureCase) {
        const std::string action = fixtureCase["action"].asString();
        ReadyControl ready("lease-" + action);
        NativePlayerControlGate gate(
            [&](PreemptionReasonV1 reason) {
              ready.events.push_back(
                  "fallback:" +
                  std::string(preemptionReasonName(reason)));
            },
            fixtureGateOptions(ready.clock, ready.events));
        auto unbind = gate.bind(ready.manager, ready.controller);
        ready.events.clear();
        invokeFixtureHook(gate, action);
        checkFixtureOrder(ready.events, fixtureCase["order"]);
        checkFixtureSnapshot(gate.snapshot(), fixtureCase["snapshot"]);
        CHECK(gate.snapshot().last_preemption ==
              std::optional<PreemptionReasonV1>{
                  fixtureReason(
                      fixtureCase["expectedReason"].asStringView())});
        unbind();
      });
}

void testSharedControlGateReasonVocabularyFixture(
    const graphql::Json& fixture) {
  std::vector<std::string> nativeReasons;
  for (const auto reason : kClosedPreemptionReasonsV1) {
    nativeReasons.emplace_back(preemptionReasonName(reason));
  }
  CHECK(nativeReasons == fixtureStrings(fixture["reasonVocabulary"]));

  std::size_t index = 0;
  fixture["imperativeReasons"].forEach(
      [&](const graphql::Json& fixtureCase) {
        const auto reason =
            fixtureReason(fixtureCase["reason"].asStringView());
        ReadyControl ready(
            "lease-reason-" + std::to_string(index++));
        NativePlayerControlGate gate(
            [&](PreemptionReasonV1 fallbackReason) {
              ready.events.push_back(
                  "fallback:" +
                  std::string(
                      preemptionReasonName(fallbackReason)));
            },
            fixtureGateOptions(ready.clock, ready.events));
        auto unbind = gate.bind(ready.manager, ready.controller);
        ready.events.clear();
        gate.preempt(reason);
        checkFixtureOrder(ready.events, fixtureCase["order"]);
        checkFixtureSnapshot(gate.snapshot(), fixtureCase["snapshot"]);
        unbind();
      });
  CHECK(index == kClosedPreemptionReasonsV1.size());
}

void testSharedControlGateFixture() {
  const graphql::Json fixture = graphql::Json::parse(
      fixtureText("crowdyjs-player-control-gate.v1.json"));
  CHECK(fixture.ok());
  CHECK(fixture["fixtureVersion"].asInt64() == 1);
  CHECK(fixture["contractVersion"].asStringView() ==
        "crowdy.player-control-gate/1");
  CHECK(fixture["crowdyJs"]["version"].asStringView() == "15.0.0");
  CHECK(fixture["crowdyJs"]["commit"].asStringView() ==
        "2b0a5a5aa269c3822a5db8fdde689519bee5fe86");
  CHECK(fixture["construction"]["clearAgentIntentRequired"].asBool());
  testSharedControlGateDefaultsAndWindowFixture(fixture);
  testSharedControlGateRebindStopFixture(fixture);
  testSharedControlGateHooksFixture(fixture);
  testSharedControlGateReasonVocabularyFixture(fixture);
}

void testMissingFallbackClearFailsClosed() {
  ReadyControl ready;
  bool rejected = false;
  try {
    NativePlayerControlGate gate(
        NativePlayerControlGate::ClearAgentIntent{},
        gateOptions(ready.clock));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);
}

}  // namespace

int main() {
  testHumanHooksWindowAndDuplicateSuppression();
  testPauseStopOfflineAndRemoteFailures();
  testSafetyTransitionsAndExactRebind();
  testStaleLeaseControllerFailureAndNoController();
  testSubscriptionsExceptionsAndDestruction();
  testReentrantListenerCannotTargetReplacementController();
  testSharedControlGateFixture();
  testMissingFallbackClearFailsClosed();
  std::printf("native_control_gate_test passed\n");
  return 0;
}
