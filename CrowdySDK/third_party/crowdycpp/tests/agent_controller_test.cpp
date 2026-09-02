#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/agent/controller.hpp"
#include "test_util.hpp"

using namespace crowdy;

namespace {

class FakeClock final : public core::IClock {
 public:
  std::int64_t epoch = 1'800'000'000'000;
  std::int64_t monotonic = 1'000;
  std::int64_t epochMillis() const override { return epoch; }
  std::int64_t monotonicMillis() const override { return monotonic; }
  void advance(std::int64_t milliseconds) {
    epoch += milliseconds;
    monotonic += milliseconds;
  }
};

agent::AgentSession makeSession(std::string digest) {
  agent::AgentSession session;
  session.sessionId = "session-1";
  session.appId = "42";
  session.projectId = "project-1";
  session.gridId = "500";
  session.mode = agent::AgentMode::Ask;
  session.status = agent::AgentSessionStatus::Active;
  session.requestedModel = "fake/model";
  session.registryDigest = std::move(digest);
  session.providerPolicyVersion = "provider-1";
  session.appPolicyVersion = "app-1";
  session.contextVersion = "context-1";
  session.currentClientEpoch = "0";
  session.lastEventSeq = "0";
  session.createdAt = "2026-07-24T00:00:00Z";
  session.updatedAt = session.createdAt;
  return session;
}

class FakeTransport final : public agent::IAgentTransport {
 public:
  explicit FakeTransport(
      std::shared_ptr<const agent::AgentToolRegistry> toolRegistry)
      : registry(std::move(toolRegistry)),
        session(makeSession(this->registry->registryDigest())) {}

  std::shared_ptr<const agent::AgentToolRegistry> registry;
  agent::AgentSession session;
  std::vector<agent::AgentEvent> durable;
  std::vector<std::string> acknowledgements;
  std::vector<agent::AgentToolResult> toolResults;
  std::vector<agent::AgentPreemptionReason> revocations;
  std::optional<agent::AgentError> heartbeatError;
  int heartbeatCount = 0;
  int attachEpoch = 0;
  int approveCount = 0;
  bool holdAcknowledgements = false;
  struct PendingAcknowledgement {
    std::string throughSeq;
    agent::AgentCallback<std::string> callback;
  };
  std::vector<PendingAcknowledgement> pendingAcknowledgements;

  void getSession(std::string,
                  agent::AgentCallback<agent::AgentSession> callback) override {
    callback(agent::AgentOutcome<agent::AgentSession>::success(session));
  }

  void listSessions(
      std::string, std::optional<std::string>, int,
      agent::AgentCallback<agent::AgentConnection<agent::AgentSession>>
          callback) override {
    agent::AgentConnection<agent::AgentSession> page;
    page.edges.push_back({"cursor-1", session});
    page.nodes.push_back(session);
    callback(agent::AgentOutcome<
             agent::AgentConnection<agent::AgentSession>>::success(
        std::move(page)));
  }

  void history(
      std::string, std::string afterSeq, int first,
      agent::AgentCallback<agent::AgentHistoryPage> callback) override {
    agent::AgentHistoryPage page;
    for (const auto& event : durable) {
      if (std::stoull(event.seq) <= std::stoull(afterSeq)) continue;
      if (static_cast<int>(page.events.size()) >= first) {
        page.hasMore = true;
        break;
      }
      page.edges.push_back({event.seq, event});
      page.events.push_back(event);
    }
    callback(agent::AgentOutcome<agent::AgentHistoryPage>::success(
        std::move(page)));
  }

  void toolDescriptors(
      std::string,
      agent::AgentCallback<agent::AgentDescriptorSet> callback) override {
    callback(agent::AgentOutcome<agent::AgentDescriptorSet>::success(
        {registry->registryDigest(), registry}));
  }

  void budget(std::string,
              agent::AgentCallback<agent::AgentBudget> callback) override {
    agent::AgentBudget budget;
    budget.platformFunded = true;
    budget.payer = "PLATFORM";
    budget.dimensions.push_back(
        {"TOOL_CALLS", "SESSION", "20", "0", "0", "20", "calls"});
    callback(agent::AgentOutcome<agent::AgentBudget>::success(
        std::move(budget)));
  }

  void createSession(
      agent::AgentCreateSessionInput input,
      agent::AgentCallback<agent::AgentSession> callback) override {
    session.appId = input.appId;
    session.projectId = input.projectId;
    session.gridId = input.gridId;
    session.mode = input.mode;
    callback(agent::AgentOutcome<agent::AgentSession>::success(session));
  }

  void attachClient(
      std::string, std::optional<std::string>, std::string,
      agent::AgentCallback<agent::AgentClientAttachment> callback) override {
    ++attachEpoch;
    session.currentClientEpoch = std::to_string(attachEpoch);
    session.clientEpoch = session.currentClientEpoch;
    session.lastEventSeq =
        durable.empty() ? "0" : durable.back().seq;
    callback(agent::AgentOutcome<agent::AgentClientAttachment>::success(
        {session, session.currentClientEpoch, "0"}));
  }

  void setMode(agent::AgentMutationContext, agent::AgentMode mode,
               agent::AgentCallback<agent::AgentSession> callback) override {
    session.mode = mode;
    callback(agent::AgentOutcome<agent::AgentSession>::success(session));
  }

  void acknowledgeEvents(
      agent::AgentMutationContext, std::string throughSeq,
      agent::AgentCallback<std::string> callback) override {
    acknowledgements.push_back(throughSeq);
    if (holdAcknowledgements) {
      pendingAcknowledgements.push_back(
          {std::move(throughSeq), std::move(callback)});
      return;
    }
    callback(agent::AgentOutcome<std::string>::success(
        std::move(throughSeq)));
  }

  void completeAcknowledgement(std::size_t index) {
    CHECK(index < pendingAcknowledgements.size());
    auto pending = std::move(pendingAcknowledgements[index]);
    pending.callback(agent::AgentOutcome<std::string>::success(
        std::move(pending.throughSeq)));
  }

  void heartbeat(
      agent::AgentMutationContext,
      agent::AgentCallback<agent::AgentHeartbeat> callback) override {
    ++heartbeatCount;
    if (heartbeatError) {
      callback(agent::AgentOutcome<agent::AgentHeartbeat>::failure(
          *heartbeatError));
      return;
    }
    agent::AgentHeartbeat heartbeat;
    heartbeat.serverTime = "2026-07-24T00:00:00Z";
    heartbeat.playLeaseFreshUntil = "2026-07-24T00:00:05Z";
    heartbeat.workspaceLeaseExpiresAt = "2026-07-24T00:00:30Z";
    callback(agent::AgentOutcome<agent::AgentHeartbeat>::success(
        std::move(heartbeat)));
  }

  void sendMessage(
      agent::AgentMutationContext, std::string,
      agent::AgentCallback<agent::AgentRun> callback) override {
    agent::AgentRun run;
    run.runId = "run-1";
    run.status = agent::AgentRunStatus::Queued;
    callback(agent::AgentOutcome<agent::AgentRun>::success(std::move(run)));
  }

  void approveTool(
      agent::AgentMutationContext, std::string toolCallId,
      std::string argumentHash,
      agent::AgentCallback<agent::AgentApproval> callback) override {
    ++approveCount;
    agent::AgentApproval approval;
    approval.approvalId = "approval-1";
    approval.toolCallId = std::move(toolCallId);
    approval.argumentHash = std::move(argumentHash);
    approval.status = agent::AgentApprovalStatus::Granted;
    approval.safeSummary = "Approved";
    approval.expiresAt = "2027-07-24T00:00:00Z";
    approval.approved = true;
    callback(agent::AgentOutcome<agent::AgentApproval>::success(
        std::move(approval)));
  }

  void rejectTool(
      agent::AgentMutationContext, std::string toolCallId,
      std::string argumentHash, std::optional<std::string>,
      agent::AgentCallback<agent::AgentApproval> callback) override {
    agent::AgentApproval approval;
    approval.approvalId = "approval-1";
    approval.toolCallId = std::move(toolCallId);
    approval.argumentHash = std::move(argumentHash);
    approval.status = agent::AgentApprovalStatus::Denied;
    approval.safeSummary = "Denied";
    approval.expiresAt = "2027-07-24T00:00:00Z";
    approval.rejected = true;
    callback(agent::AgentOutcome<agent::AgentApproval>::success(
        std::move(approval)));
  }

  void browserToolResult(
      agent::AgentMutationContext, agent::AgentToolResult result,
      agent::AgentCallback<agent::AgentToolCallAck> callback) override {
    toolResults.push_back(result);
    agent::AgentToolCallAck ack;
    ack.toolCallId = result.toolCallId;
    ack.toolName = "studio.context.get";
    ack.status = result.status == agent::AgentToolResultStatus::OutcomeUnknown
                     ? agent::AgentToolCallStatus::OutcomeUnknown
                     : agent::AgentToolCallStatus::Succeeded;
    ack.argumentHash =
        "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    ack.accepted = true;
    callback(agent::AgentOutcome<agent::AgentToolCallAck>::success(
        std::move(ack)));
  }

  void grantLease(
      agent::AgentMutationContext context, std::vector<std::string> scopes,
      int, std::string controlledEntityId,
      std::string hostCapabilityRevision,
      agent::AgentCallback<agent::AgentLease> callback) override {
    agent::AgentLease lease;
    lease.leaseId = "play-1";
    lease.kind = agent::AgentLeaseKind::Play;
    lease.status = agent::AgentLeaseStatus::Active;
    lease.clientEpoch = context.clientEpoch;
    lease.scopes = std::move(scopes);
    lease.holder = "Current player";
    lease.controlledEntityId = std::move(controlledEntityId);
    lease.hostCapabilityRevision = std::move(hostCapabilityRevision);
    lease.contextVersion = session.contextVersion;
    lease.grantedAt = "2026-07-24T00:00:00Z";
    lease.expiresAt = "2026-07-24T00:10:00Z";
    callback(agent::AgentOutcome<agent::AgentLease>::success(
        std::move(lease)));
  }

  void revokeLease(
      agent::AgentMutationContext context, std::string leaseId,
      agent::AgentPreemptionReason reason,
      agent::AgentCallback<agent::AgentLease> callback) override {
    revocations.push_back(reason);
    agent::AgentLease lease;
    lease.leaseId = std::move(leaseId);
    lease.kind = agent::AgentLeaseKind::Workspace;
    lease.status = agent::AgentLeaseStatus::Revoked;
    lease.clientEpoch = context.clientEpoch;
    lease.holder = "Current player";
    lease.contextVersion = session.contextVersion;
    lease.grantedAt = "2026-07-24T00:00:00Z";
    lease.expiresAt = "2026-07-24T00:00:30Z";
    lease.revokedReason = reason;
    callback(agent::AgentOutcome<agent::AgentLease>::success(
        std::move(lease)));
  }

  void pause(agent::AgentMutationContext,
             agent::AgentCallback<agent::AgentSession> callback) override {
    session.status = agent::AgentSessionStatus::Paused;
    callback(agent::AgentOutcome<agent::AgentSession>::success(session));
  }

  void resume(agent::AgentMutationContext,
              agent::AgentCallback<agent::AgentSession> callback) override {
    session.status = agent::AgentSessionStatus::Active;
    callback(agent::AgentOutcome<agent::AgentSession>::success(session));
  }

  void cancelRun(
      agent::AgentMutationContext, std::string runId,
      agent::AgentCallback<agent::AgentRun> callback) override {
    agent::AgentRun run;
    run.runId = std::move(runId);
    run.status = agent::AgentRunStatus::Cancelled;
    run.cancelled = true;
    callback(agent::AgentOutcome<agent::AgentRun>::success(std::move(run)));
  }

  void closeSession(
      agent::AgentMutationContext,
      agent::AgentCallback<agent::AgentSession> callback) override {
    session.status = agent::AgentSessionStatus::Closed;
    callback(agent::AgentOutcome<agent::AgentSession>::success(session));
  }

};

class FakeSubscriptionAdapter final
    : public agent::IAgentEventSubscriptionAdapter {
 public:
  struct Slot {
    agent::AgentEventSubscriptionRequest request;
    agent::AgentEventSubscriptionCallbacks callbacks;
    bool closed = false;
  };

  class Handle final : public agent::IAgentEventSubscription {
   public:
    explicit Handle(std::shared_ptr<Slot> slot) : slot_(std::move(slot)) {}
    void close() override { slot_->closed = true; }

   private:
    std::shared_ptr<Slot> slot_;
  };

  std::vector<std::shared_ptr<Slot>> slots;

  std::unique_ptr<agent::IAgentEventSubscription> subscribe(
      agent::AgentEventSubscriptionRequest request,
      agent::AgentEventSubscriptionCallbacks callbacks) override {
    auto slot = std::make_shared<Slot>();
    slot->request = std::move(request);
    slot->callbacks = std::move(callbacks);
    slots.push_back(slot);
    return std::make_unique<Handle>(slot);
  }

  void emit(std::size_t index, agent::AgentEvent event,
            bool force = false) {
    CHECK(index < slots.size());
    if (force || !slots[index]->closed) {
      slots[index]->callbacks.next(std::move(event));
    }
  }
};

class FakeBrowserDispatcher final : public agent::IAgentBrowserToolDispatcher {
 public:
  std::vector<agent::AgentToolInvocation> invocations;
  std::vector<agent::AgentCallback<agent::AgentToolResult>> callbacks;
  int cancellations = 0;

  void dispatch(
      agent::AgentToolInvocation invocation,
      agent::AgentCallback<agent::AgentToolResult> callback) override {
    invocations.push_back(std::move(invocation));
    callbacks.push_back(std::move(callback));
  }
  void cancelActive(agent::AgentPreemptionReason) override {
    ++cancellations;
  }
  void clearClosedSession() override {}
};

void pump(agent::CrowdyStudioAgentController& controller, int times = 12) {
  for (int index = 0; index < times; ++index) controller.poll();
}

agent::AgentEvent lifecycleEvent(std::string seq,
                                 agent::AgentEventType type) {
  agent::AgentEvent event;
  event.eventId = "event-" + seq;
  event.sessionId = "session-1";
  event.seq = std::move(seq);
  event.type = type;
  event.version = "crowdy.agent-event/1";
  event.createdAt = "2026-07-24T00:00:00Z";
  event.payload = agent::AgentLifecycleEventPayload{};
  return event;
}

agent::AgentEvent messageEvent(std::string seq, std::string id,
                               std::string role, std::string content,
                               agent::AgentEventType type) {
  auto event = lifecycleEvent(std::move(seq), type);
  agent::AgentMessageEventPayload payload;
  payload.message.messageId = std::move(id);
  payload.message.role = std::move(role);
  payload.message.content = std::move(content);
  payload.message.createdAt = event.createdAt;
  payload.chunk = type == agent::AgentEventType::AssistantChunk;
  event.payload = std::move(payload);
  return event;
}

struct Harness {
  std::shared_ptr<const agent::AgentToolRegistry> registry =
      std::make_shared<const agent::AgentToolRegistry>(
          agent::AgentToolRegistry::fromFixtureJson(
              agent::canonicalAgentToolFixtureJsonV1()));
  FakeTransport transport{registry};
  FakeSubscriptionAdapter subscriptions;
  FakeBrowserDispatcher browser;
  FakeClock clock;
  std::shared_ptr<graphql::Dispatcher> dispatcher =
      std::make_shared<graphql::Dispatcher>();
  std::vector<agent::AgentPreemptionReason> preemptions;
  std::unique_ptr<agent::CrowdyStudioAgentController> controller;

  Harness() {
    agent::CrowdyStudioAgentControllerOptions options;
    options.transport = &transport;
    options.subscriptionAdapter = &subscriptions;
    options.dispatcher = dispatcher;
    options.clock = &clock;
    options.sessionId = "session-1";
    options.browserDispatcher = &browser;
    options.heartbeatIntervalMs = 5;
    options.heartbeatStaleMs = 20;
    options.workspaceRenewIntervalMs = 5;
    options.createIdempotencyKey = [](std::string_view operation) {
      return "key:" + std::string(operation);
    };
    options.onPreempt = [&](agent::AgentPreemptionReason reason) {
      preemptions.push_back(reason);
    };
    controller =
        std::make_unique<agent::CrowdyStudioAgentController>(std::move(options));
  }

  void initialize() {
    bool complete = false;
    controller->initialize([&](agent::AgentOutcome<agent::AgentVoid> outcome) {
      CHECK(outcome.ok());
      complete = true;
    });
    pump(*controller);
    CHECK(complete);
    CHECK(controller->state().connection ==
          agent::AgentConnectionState::Connected);
  }
};

void testReplayGapDedupAndAck() {
  Harness harness;
  harness.initialize();
  harness.transport.durable = {
      messageEvent("1", "message-1", "USER", "Build weather",
                   agent::AgentEventType::UserMessage),
      messageEvent("2", "chunk-1", "ASSISTANT", "Working",
                   agent::AgentEventType::AssistantChunk),
      messageEvent("3", "message-2", "ASSISTANT", "Done",
                   agent::AgentEventType::AssistantMessage),
  };
  harness.subscriptions.emit(0, harness.transport.durable[2]);
  pump(*harness.controller);
  CHECK(harness.controller->state().lastContiguousSeq == "3");
  CHECK_EQ(harness.controller->state().events.size(), std::size_t{3});
  CHECK_EQ(harness.controller->state().messages.size(), std::size_t{2});
  CHECK(harness.controller->state().streamingText.empty());
  CHECK(!harness.transport.acknowledgements.empty());
  CHECK(harness.transport.acknowledgements.back() == "3");

  harness.subscriptions.emit(0, harness.transport.durable[1]);
  pump(*harness.controller);
  CHECK_EQ(harness.controller->state().events.size(), std::size_t{3});
}

void testReconnectFencesStaleEpoch() {
  Harness harness;
  harness.initialize();
  CHECK(harness.controller->state().clientEpoch == "1");
  harness.subscriptions.slots[0]->callbacks.reconnect();
  pump(*harness.controller);
  CHECK(harness.controller->state().connection ==
        agent::AgentConnectionState::Connected);
  CHECK(!harness.controller->state().lastError);

  harness.controller->reconnect();
  pump(*harness.controller);
  CHECK(harness.controller->state().clientEpoch == "2");
  CHECK_EQ(harness.subscriptions.slots.size(), std::size_t{2});
  CHECK(harness.subscriptions.slots[0]->closed);

  auto mode = lifecycleEvent("1", agent::AgentEventType::ModeSelected);
  agent::AgentLifecycleEventPayload payload;
  payload.mode = agent::AgentMode::Build;
  mode.payload = payload;
  harness.subscriptions.emit(0, mode, true);
  pump(*harness.controller);
  CHECK(harness.controller->state().session->mode == agent::AgentMode::Ask);
  harness.subscriptions.emit(1, mode);
  pump(*harness.controller);
  CHECK(harness.controller->state().session->mode == agent::AgentMode::Build);
}

void testStaleAcknowledgementDoesNotStallReattachedGeneration() {
  Harness harness;
  harness.initialize();
  harness.transport.holdAcknowledgements = true;

  harness.subscriptions.emit(
      0, lifecycleEvent("1", agent::AgentEventType::ModeSelected));
  pump(*harness.controller);
  CHECK_EQ(harness.transport.pendingAcknowledgements.size(), std::size_t{1});
  CHECK(harness.controller->state().lastAcknowledgedSeq == "0");

  harness.controller->reconnect();
  pump(*harness.controller);
  CHECK(harness.controller->state().clientEpoch == "2");
  CHECK_EQ(harness.subscriptions.slots.size(), std::size_t{2});
  CHECK(harness.controller->state().lastAcknowledgedSeq == "1");

  harness.subscriptions.emit(
      1, lifecycleEvent("2", agent::AgentEventType::ModeSelected));
  pump(*harness.controller);
  CHECK_EQ(harness.transport.pendingAcknowledgements.size(), std::size_t{2});
  CHECK(harness.transport.acknowledgements.back() == "2");

  harness.transport.completeAcknowledgement(0);
  pump(*harness.controller);
  CHECK(harness.controller->state().lastAcknowledgedSeq == "1");

  harness.transport.completeAcknowledgement(1);
  pump(*harness.controller);
  CHECK(harness.controller->state().lastAcknowledgedSeq == "2");
}

void testApprovalBudgetAndPolicyKill() {
  Harness harness;
  harness.initialize();

  auto approvalEvent =
      lifecycleEvent("1", agent::AgentEventType::ApprovalRequested);
  agent::AgentApprovalEventPayload approvalPayload;
  approvalPayload.approval.approvalId = "approval-1";
  approvalPayload.approval.toolCallId = "tool-1";
  approvalPayload.approval.argumentHash =
      "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
  approvalPayload.approval.status = agent::AgentApprovalStatus::Pending;
  approvalPayload.approval.safeSummary = "Deploy revision 7";
  approvalPayload.approval.expiresAt = "2027-07-24T00:00:00Z";
  approvalEvent.payload = approvalPayload;
  harness.subscriptions.emit(0, approvalEvent);
  pump(*harness.controller);
  CHECK_EQ(harness.controller->state().approvals.size(), std::size_t{1});

  bool mismatch = false;
  harness.controller->approveTool(
      "tool-1",
      "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
      [&](agent::AgentOutcome<agent::AgentVoid> outcome) {
        mismatch = !outcome.ok() &&
                   outcome.error->code == "AGENT_APPROVAL_MISMATCH";
      });
  CHECK(mismatch);
  harness.controller->approveTool("tool-1");
  pump(*harness.controller);
  CHECK_EQ(harness.transport.approveCount, 1);

  auto budgetEvent =
      lifecycleEvent("2", agent::AgentEventType::BudgetUpdated);
  agent::AgentBudgetEventPayload budgetPayload;
  budgetPayload.budget.platformFunded = true;
  budgetPayload.budget.payer = "PLATFORM";
  budgetPayload.budget.dimensions.push_back(
      {"TOOL_CALLS", "SESSION", "5", "1", "3", "1", "calls"});
  budgetEvent.payload = budgetPayload;
  harness.subscriptions.emit(0, budgetEvent);
  pump(*harness.controller);
  CHECK(harness.controller->state().budget->dimensions[0].remaining == "1");

  harness.transport.heartbeatError = agent::makeAgentError(
      "AGENT_OPERATOR_KILLED", "Platform agent kill is active");
  harness.controller->setMode(agent::AgentMode::Play);
  pump(*harness.controller);
  CHECK(harness.controller->state().connection ==
        agent::AgentConnectionState::Disconnected);
  CHECK(harness.controller->state().lastError->code ==
        "AGENT_OPERATOR_KILLED");
  CHECK(std::find(harness.preemptions.begin(), harness.preemptions.end(),
                  agent::AgentPreemptionReason::OperatorKill) !=
        harness.preemptions.end());
}

void testWorkspaceRenewalAndHumanEdit() {
  Harness harness;
  harness.initialize();
  harness.controller->setMode(agent::AgentMode::Build);
  pump(*harness.controller);

  auto leaseEvent =
      lifecycleEvent("1", agent::AgentEventType::LeaseGranted);
  agent::AgentLeaseEventPayload leasePayload;
  leasePayload.lease.leaseId = "workspace-1";
  leasePayload.lease.kind = agent::AgentLeaseKind::Workspace;
  leasePayload.lease.status = agent::AgentLeaseStatus::Active;
  leasePayload.lease.clientEpoch = "1";
  leasePayload.lease.scopes = {"studio.project.write.server"};
  leasePayload.lease.holder = "Current player";
  leasePayload.lease.expectedProjectRevision = "1";
  leasePayload.lease.contextVersion = "context-1";
  leasePayload.lease.grantedAt = "2026-07-24T00:00:00Z";
  leasePayload.lease.expiresAt = "2026-07-24T00:00:30Z";
  leaseEvent.payload = leasePayload;
  harness.subscriptions.emit(0, leaseEvent);
  pump(*harness.controller);
  const auto before = harness.transport.heartbeatCount;
  harness.clock.advance(6);
  pump(*harness.controller);
  CHECK(harness.transport.heartbeatCount > before);

  harness.controller->preemptForHumanEdit();
  pump(*harness.controller);
  CHECK(!harness.transport.revocations.empty());
  CHECK(harness.transport.revocations.back() ==
        agent::AgentPreemptionReason::HumanEdit);
  CHECK(harness.browser.cancellations > 0);
}

void testOutcomeUnknownAndLateResultFencing() {
  Harness harness;
  harness.initialize();
  const auto& descriptor =
      harness.registry->require("studio.context.get", "1.0.0");

  auto dispatched =
      lifecycleEvent("1", agent::AgentEventType::ToolDispatched);
  agent::AgentToolEventPayload tool;
  tool.toolCallId = "tool-1";
  tool.name = descriptor.descriptor->name;
  tool.version = descriptor.descriptor->version;
  tool.status = agent::AgentToolCallStatus::Dispatched;
  agent::AgentToolInvocation invocation;
  invocation.sessionId = "session-1";
  invocation.runId = "run-1";
  invocation.toolCallId = "tool-1";
  invocation.name = tool.name;
  invocation.version = tool.version;
  invocation.descriptorDigest = descriptor.descriptorDigest;
  invocation.argumentsJson = "{}";
  invocation.arguments = graphql::Json::parse("{}");
  invocation.argumentHash =
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  invocation.contextVersion = "context-1";
  invocation.clientEpoch = "1";
  invocation.deadline = "2027-07-24T00:00:00Z";
  tool.invocation = invocation;
  dispatched.payload = tool;
  harness.subscriptions.emit(0, dispatched);
  pump(*harness.controller);
  CHECK_EQ(harness.browser.callbacks.size(), std::size_t{1});

  agent::AgentToolResult unknown;
  unknown.toolCallId = "tool-1";
  unknown.status = agent::AgentToolResultStatus::OutcomeUnknown;
  unknown.error = agent::makeAgentError(
      "AGENT_TOOL_OUTCOME_UNKNOWN", "Acknowledgement lost");
  unknown.observedContextVersion = "context-1";
  unknown.startedAt = "2026-07-24T00:00:00Z";
  unknown.finishedAt = "2026-07-24T00:00:01Z";
  harness.browser.callbacks[0](
      agent::AgentOutcome<agent::AgentToolResult>::success(unknown));
  pump(*harness.controller);
  CHECK_EQ(harness.transport.toolResults.size(), std::size_t{1});
  CHECK(harness.transport.toolResults[0].status ==
        agent::AgentToolResultStatus::OutcomeUnknown);

  auto lateEvent = dispatched;
  lateEvent.seq = "2";
  lateEvent.eventId = "event-2";
  auto& latePayload = std::get<agent::AgentToolEventPayload>(
      lateEvent.payload);
  latePayload.toolCallId = "tool-2";
  latePayload.invocation->toolCallId = "tool-2";
  harness.subscriptions.emit(0, lateEvent);
  pump(*harness.controller);
  CHECK_EQ(harness.browser.callbacks.size(), std::size_t{2});
  harness.controller->preemptForHumanEdit();

  auto late = unknown;
  late.toolCallId = "tool-2";
  harness.browser.callbacks[1](
      agent::AgentOutcome<agent::AgentToolResult>::success(late));
  pump(*harness.controller);
  CHECK_EQ(harness.transport.toolResults.size(), std::size_t{1});
}

void testPollingFallbackAndLifecycleControls() {
  const auto registry = std::make_shared<const agent::AgentToolRegistry>(
      agent::AgentToolRegistry::fromFixtureJson(
          agent::canonicalAgentToolFixtureJsonV1()));
  FakeTransport transport(registry);
  transport.durable.push_back(
      messageEvent("1", "message-1", "USER", "hello",
                   agent::AgentEventType::UserMessage));
  agent::PollingAgentEventSubscriptionAdapter polling(transport, 10);
  int delivered = 0;
  auto subscription = polling.subscribe(
      {"session-1", "0", "1"},
      {[&](agent::AgentEvent event) {
         ++delivered;
         CHECK(event.seq == "1");
       },
       [&](agent::AgentError) { CHECK(false); },
       [] {},
       [] {}});
  polling.poll();
  polling.poll();
  CHECK_EQ(delivered, 1);
  subscription->close();

  Harness harness;
  harness.initialize();
  agent::AgentRun current;
  current.runId = "run-1";
  current.status = agent::AgentRunStatus::Running;
  harness.transport.session.currentRun = current;
  harness.controller->grantPlayLease(
      {"observe", "locomotion"}, 60, "player-1", "capability-1");
  pump(*harness.controller);
  CHECK_EQ(harness.controller->state().leases.size(), std::size_t{1});

  bool stopped = false;
  harness.controller->stop(
      [&](agent::AgentOutcome<agent::AgentVoid> outcome) {
        stopped = outcome.ok();
      });
  pump(*harness.controller);
  CHECK(stopped);
  CHECK(harness.controller->state().session->status ==
        agent::AgentSessionStatus::Paused);
  CHECK(harness.controller->state().leases.empty());

  bool resumed = false;
  harness.controller->resume(
      [&](agent::AgentOutcome<agent::AgentVoid> outcome) {
        resumed = outcome.ok();
      });
  pump(*harness.controller);
  CHECK(resumed);
  CHECK(harness.controller->state().session->status ==
        agent::AgentSessionStatus::Active);

  bool closed = false;
  harness.controller->close(
      [&](agent::AgentOutcome<agent::AgentVoid> outcome) {
        closed = outcome.ok();
      });
  pump(*harness.controller);
  CHECK(closed);
  CHECK(harness.controller->state().connection ==
        agent::AgentConnectionState::Disconnected);
  CHECK(!harness.controller->state().clientEpoch);
}

}  // namespace

int main() {
  testReplayGapDedupAndAck();
  testReconnectFencesStaleEpoch();
  testStaleAcknowledgementDoesNotStallReattachedGeneration();
  testApprovalBudgetAndPolicyKill();
  testWorkspaceRenewalAndHumanEdit();
  testOutcomeUnknownAndLateResultFencing();
  testPollingFallbackAndLifecycleControls();
  std::printf("agent_controller_test passed\n");
  return 0;
}
