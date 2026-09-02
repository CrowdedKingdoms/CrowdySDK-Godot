#include <cstdio>
#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "crowdy/client.hpp"
#include "crowdy/graphql/http.hpp"
#include "test_util.hpp"

using namespace crowdy;

namespace {

class CaptureTransport final : public graphql::IHttpTransport {
 public:
  graphql::HttpResponse response;
  graphql::HttpRequest last;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    last = request;
    return response;
  }
};

class ImmediateAsyncTransport final : public graphql::IAsyncHttpTransport {
 public:
  graphql::HttpRequest last;
  graphql::HttpOutcome outcome;

  void sendAsync(const graphql::HttpRequest& request,
                 std::function<void(graphql::HttpOutcome)> callback) override {
    last = request;
    callback(outcome);
  }
};

void respond(CaptureTransport& transport, std::string_view root,
             std::string_view payload = "{}") {
  transport.response = {
      200, std::string(R"({"data":{")") + std::string(root) + "\":" +
               std::string(payload) + "}}"};
}

void checkCall(CaptureTransport& transport, std::string_view base,
               std::string_view operation, std::string_view root,
               const std::function<void()>& call) {
  respond(transport, root);
  call();
  CHECK(transport.last.url == std::string(base) + "/graphql");
  CHECK(transport.last.body.find(
            std::string(R"("operationName":")") +
            std::string(operation) + "\"") != std::string::npos);
}

void testEveryNamedOperationUsesTheOneOrigin() {
  auto capture = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = capture;
  CrowdyClient client(std::move(config));
  auto& api = client.crowdyStudioAgent();
  graphql::JVal input;
  input["sessionId"] = "session-1";
  input["clientEpoch"] = "1";
  input["idempotencyKey"] = "key-1";

  checkCall(*capture, "https://game.invalid", "CrowdyStudioAgentSession",
            "crowdyStudioAgentSession",
            [&] { (void)api.session("session-1"); });
  checkCall(*capture, "https://game.invalid", "CrowdyStudioAgentSessions",
            "crowdyStudioAgentSessions",
            [&] { (void)api.sessions("42"); });
  checkCall(*capture, "https://game.invalid", "CrowdyStudioAgentHistory",
            "crowdyStudioAgentHistory",
            [&] { (void)api.history("session-1", "0", 100); });
  checkCall(*capture, "https://game.invalid",
            "CrowdyStudioAgentToolDescriptors",
            "crowdyStudioAgentToolDescriptors",
            [&] { (void)api.toolDescriptors("session-1"); });
  checkCall(*capture, "https://game.invalid", "CrowdyStudioAgentBudget",
            "crowdyStudioAgentBudget",
            [&] { (void)api.budget("session-1"); });

  const std::pair<std::string_view, std::function<void()>> mutations[] = {
      {"CrowdyStudioAgentCreateSession",
       [&] { (void)api.createSession(input); }},
      {"CrowdyStudioAgentAttachClient",
       [&] { (void)api.attachClient(input); }},
      {"CrowdyStudioAgentSetMode", [&] { (void)api.setMode(input); }},
      {"CrowdyStudioAgentAcknowledgeEvents",
       [&] { (void)api.acknowledgeEvents(input); }},
      {"CrowdyStudioAgentHeartbeat", [&] { (void)api.heartbeat(input); }},
      {"CrowdyStudioAgentSendMessage",
       [&] { (void)api.sendMessage(input); }},
      {"CrowdyStudioAgentApproveTool",
       [&] { (void)api.approveTool(input); }},
      {"CrowdyStudioAgentRejectTool",
       [&] { (void)api.rejectTool(input); }},
      {"CrowdyStudioAgentToolResult",
       [&] { (void)api.browserToolResult(input); }},
      {"CrowdyStudioAgentGrantLease",
       [&] { (void)api.grantLease(input); }},
      {"CrowdyStudioAgentRevokeLease",
       [&] { (void)api.revokeLease(input); }},
      {"CrowdyStudioAgentPause", [&] { (void)api.pause(input); }},
      {"CrowdyStudioAgentResume", [&] { (void)api.resume(input); }},
      {"CrowdyStudioAgentCancelRun",
       [&] { (void)api.cancelRun(input); }},
      {"CrowdyStudioAgentCloseSession",
       [&] { (void)api.closeSession(input); }},
  };
  for (const auto& [operation, call] : mutations) {
    std::string root(operation);
    root.front() = static_cast<char>(
        std::tolower(static_cast<unsigned char>(root.front())));
    checkCall(*capture, "https://game.invalid", operation, root, call);
  }

  checkCall(*capture, "https://game.invalid",
            "CrowdyStudioAgentPolicy", "crowdyStudioAgentPolicy",
            [&] { (void)api.policy("42"); });
  checkCall(*capture, "https://game.invalid",
            "CrowdyStudioAgentEffectivePolicy",
            "crowdyStudioAgentEffectivePolicy",
            [&] { (void)api.effectivePolicy("42"); });
  checkCall(*capture, "https://game.invalid",
            "CrowdyStudioAgentUsage", "crowdyStudioAgentUsage",
            [&] { (void)api.usage("42"); });
  checkCall(*capture, "https://game.invalid",
            "CrowdyStudioAgentSetPolicy", "setCrowdyStudioAgentPolicy",
            [&] { (void)api.setPolicy(input); });
  checkCall(*capture, "https://game.invalid",
            "CpCrowdyStudioAgentPlatformPolicy",
            "cpCrowdyStudioAgentPlatformPolicy",
            [&] { (void)api.platformPolicy(); });
  checkCall(*capture, "https://game.invalid",
            "CpSetCrowdyStudioAgentPlatformPolicy",
            "cpSetCrowdyStudioAgentPlatformPolicy",
            [&] { (void)api.setPlatformPolicy(input); });
  checkCall(*capture, "https://game.invalid",
            "CpSetCrowdyStudioAgentAppKill",
            "cpSetCrowdyStudioAgentAppKill",
            [&] { (void)api.setOperatorAppKill(input); });
}

void testAsyncCompletesOnlyFromPoll() {
  auto capture = std::make_shared<CaptureTransport>();
  auto async = std::make_shared<ImmediateAsyncTransport>();
  async->outcome.status = Errc::Ok;
  async->outcome.response = {
      200, R"({"data":{"crowdyStudioAgentBudget":{"dimensions":[],"platformFunded":true,"payer":"PLATFORM"}}})"};
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = capture;
  config.asyncTransport = async;
  CrowdyClient client(std::move(config));

  bool called = false;
  client.crowdyStudioAgent().budgetAsync(
      "session-1", [&](graphql::GraphQLOutcome outcome) {
        called = true;
        CHECK(outcome.ok());
        CHECK(outcome.data["payer"].asString() == "PLATFORM");
      });
  CHECK(!called);
  client.poll();
  CHECK(called);
}

}  // namespace

int main() {
  testEveryNamedOperationUsesTheOneOrigin();
  testAsyncCompletesOnlyFromPoll();
  std::printf("agent_api_test passed\n");
  return 0;
}
