// Game model: an admin seeds a tiny model (container type, property defs, an
// owner-gated guarded function and an open function); a player instantiates
// and mutates it; invoke-policy denials resolve success:false with a failure
// event; sessions gate is_current_turn functions; traverse follows edges.
// Mirrors Game API SDK e2e: game model.
// Reference: https://docs.crowdedkingdoms.com/game-api/game-models
#include "e2e_util.hpp"

using namespace crowdy;

namespace {

struct InvokeOutcome {
  bool success = false;
  std::string errorMessage;
  graphql::Json raw;
};

// Invoke never throws for POLICY denials on this server: denials resolve
// success:false in the payload.
InvokeOutcome invoke(e2e::Player& p, const std::string& appId, const std::string& fn,
                     const std::string& selfId, const std::string& paramsJson = "{}",
                     const std::string& sessionId = {}) {
  graphql::JVal input;
  input["appId"] = appId;
  input["functionName"] = fn;
  input["selfContainerId"] = selfId;
  input["paramsJson"] = paramsJson;
  if (!sessionId.empty()) input["sessionId"] = sessionId;
  InvokeOutcome out;
  out.raw = p.game->gameModel().invoke(input);
  out.success = out.raw["success"].asBool();
  out.errorMessage = out.raw["errorMessage"].asString();
  return out;
}

std::int64_t returnValue(const InvokeOutcome& out) {
  return graphql::Json::parse(out.raw["returnValueJson"].asStringView()).asInt64();
}

int run() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto& og = e2e::ownerGame(cfg);
  auto a = e2e::provisionPlayer(cfg, "gm-a");
  auto b = e2e::provisionPlayer(cfg, "gm-b");

  const std::string typeName = e2e::kitPrefix("E2eGmCrate");
  const std::string fnInc = "e2e_gm_inc_" + e2e::runSuffix();
  const std::string fnPing = "e2e_gm_ping_" + e2e::runSuffix();
  const std::string fnTurn = "e2e_gm_turn_" + e2e::runSuffix();

  E2E_SUBTEST("owner seeds the model");
  {
    graphql::JVal type;
    type["typeName"] = typeName;
    type["displayName"] = "E2E Crate";
    type["instantiableBy"] = "member";

    graphql::JVal countProp;
    countProp["containerTypeName"] = typeName;
    countProp["key"] = "count";
    countProp["valueType"] = "int";
    countProp["defaultValueJson"] = "0";
    graphql::JVal labelProp;
    labelProp["containerTypeName"] = typeName;
    labelProp["key"] = "label";
    labelProp["valueType"] = "string";
    labelProp["defaultValueJson"] = "\"\"";
    labelProp["writable"] = "owner";

    graphql::JVal amountParam;
    amountParam["name"] = "amount";
    amountParam["valueType"] = "int";
    amountParam["required"] = true;

    graphql::JVal incMutation;
    incMutation["target"] = "self";
    incMutation["property"] = "count";
    incMutation["expression"] = "self.count + $amount";

    // Owner-gated increment with a condition guard (only 1..10 at a time).
    graphql::JVal inc;
    inc["name"] = fnInc;
    inc["containerTypeName"] = typeName;
    inc["returnType"] = "int";
    inc["parameters"] = graphql::JVal::array({amountParam});
    inc["mutations"] = graphql::JVal::array({incMutation});
    inc["returnExpression"] = "self.count";
    inc["invokePolicyJson"] = kit::kitPolicyJson(kit::andPolicy(
        {kit::ownerOfSelfPolicy(), kit::conditionPolicy("$amount > 0 && $amount <= 10")}));

    // Open allow-policy function anyone may call.
    graphql::JVal ping;
    ping["name"] = fnPing;
    ping["containerTypeName"] = typeName;
    ping["returnType"] = "int";
    ping["returnExpression"] = "42";
    ping["invokePolicyJson"] = kit::kitPolicyJson(kit::allowPolicy());

    // Turn-gated function for the session subtest.
    graphql::JVal turnMutation;
    turnMutation["target"] = "self";
    turnMutation["property"] = "count";
    turnMutation["expression"] = "self.count + 1";
    graphql::JVal turn;
    turn["name"] = fnTurn;
    turn["containerTypeName"] = typeName;
    turn["returnType"] = "int";
    turn["mutations"] = graphql::JVal::array({turnMutation});
    turn["returnExpression"] = "self.count";
    turn["invokePolicyJson"] = kit::kitPolicyJson(kit::isCurrentTurnPolicy());

    graphql::JVal seed;
    seed["appId"] = cfg.appId;
    seed["containerTypes"] = graphql::JVal::array({type});
    seed["propertyDefinitions"] = graphql::JVal::array({countProp, labelProp});
    seed["functions"] = graphql::JVal::array({inc, ping, turn});
    graphql::Json seeded = og.gameModel().seed(seed);
    E2E_CHECK(seeded["containerTypesCreated"].asInt64() >= 1);
    E2E_CHECK(seeded["functionsCreated"].asInt64() >= 3);
  }

  E2E_SUBTEST("player creates a container and sets an owner-writable property");
  graphql::JVal containerInput;
  containerInput["appId"] = cfg.appId;
  containerInput["typeName"] = typeName;
  containerInput["displayName"] = "A's crate";
  graphql::Json container = a.game->gameModel().createContainer(containerInput);
  const std::string containerId = container["containerId"].asString();
  E2E_CHECK(!containerId.empty());
  E2E_CHECK(container["ownerUserId"].asString() == a.userId);

  {
    graphql::JVal setInput;
    setInput["appId"] = cfg.appId;
    setInput["containerId"] = containerId;
    setInput["key"] = "label";
    setInput["valueType"] = "string";
    setInput["valueJson"] = "\"hello\"";
    a.game->gameModel().setProperty(setInput);
    graphql::Json state = a.game->gameModel().containerState(cfg.appId, containerId);
    graphql::Json props = graphql::Json::parse(state["propertiesJson"].asStringView());
    E2E_CHECK(props["label"].asString() == "hello");
  }

  E2E_SUBTEST("allowed invoke mutates, returns the value, and logs an event");
  {
    auto first = invoke(a, cfg.appId, fnInc, containerId, "{\"amount\":5}");
    E2E_CHECK(first.success);
    E2E_CHECK(returnValue(first) == 5);
    auto second = invoke(a, cfg.appId, fnInc, containerId, "{\"amount\":3}");
    E2E_CHECK(second.success);
    E2E_CHECK(returnValue(second) == 8);
    // The open allow-policy function works for the non-owner too.
    auto ping = invoke(b, cfg.appId, fnPing, containerId);
    E2E_CHECK(ping.success);
    E2E_CHECK(returnValue(ping) == 42);

    graphql::JVal eventVars;
    eventVars["appId"] = cfg.appId;
    eventVars["selfContainerId"] = containerId;
    eventVars["functionName"] = fnInc;
    graphql::Json events = a.game->gameModel().events(eventVars);
    E2E_CHECK(events.size() >= 2);
    bool sawInvocation = false;
    events.forEach([&](graphql::Json event) {
      if (event["success"].asBool() && event["callerUserId"].asString() == a.userId)
        sawInvocation = true;
    });
    E2E_CHECK(sawInvocation);
  }

  E2E_SUBTEST("POLICY denial resolves success:false and writes a failure event");
  {
    auto denied = invoke(b, cfg.appId, fnInc, containerId, "{\"amount\":5}");
    E2E_CHECK(!denied.success);
    E2E_CHECK(!denied.errorMessage.empty());
    // Condition guard denial too (owner, but amount out of range).
    auto guarded = invoke(a, cfg.appId, fnInc, containerId, "{\"amount\":100}");
    E2E_CHECK(!guarded.success);
    // State unchanged by either denial.
    auto after = invoke(a, cfg.appId, fnInc, containerId, "{\"amount\":1}");
    E2E_CHECK(after.success);
    E2E_CHECK(returnValue(after) == 9);

    graphql::JVal failVars;
    failVars["appId"] = cfg.appId;
    failVars["selfContainerId"] = containerId;
    failVars["functionName"] = fnInc;
    failVars["success"] = false;
    graphql::Json failures = a.game->gameModel().events(failVars);
    E2E_CHECK(failures.size() >= 1);
    bool sawDenial = false;
    failures.forEach([&](graphql::Json event) {
      if (event["callerUserId"].asString() == b.userId &&
          !event["errorMessage"].asString().empty()) {
        sawDenial = true;
      }
    });
    E2E_CHECK(sawDenial);
  }

  E2E_SUBTEST("sessions: join + setSessionTurn + is_current_turn gating");
  {
    graphql::JVal sessionInput;
    sessionInput["appId"] = cfg.appId;
    sessionInput["name"] = "e2e-gm-session-" + e2e::runSuffix();
    graphql::Json session = a.game->gameModel().createSession(sessionInput);
    const std::string sessionId = session["sessionId"].asString();
    E2E_CHECK(!sessionId.empty());

    graphql::JVal joinInput;
    joinInput["appId"] = cfg.appId;
    joinInput["sessionId"] = sessionId;
    graphql::Json joined = b.game->gameModel().joinSession(joinInput);
    E2E_CHECK(joined["userId"].asString() == b.userId);

    // Only an app admin, the elected host, or the current turn holder may set
    // the turn — a fresh session has no holder, so the ADMIN seeds the first
    // turn; after that the holder can pass it along.
    graphql::JVal turnInput;
    turnInput["appId"] = cfg.appId;
    turnInput["sessionId"] = sessionId;
    turnInput["userId"] = a.userId;
    graphql::Json turned = og.gameModel().setSessionTurn(turnInput);
    E2E_CHECK(turned["currentTurnUserId"].asString() == a.userId);

    auto aOnTurn = invoke(a, cfg.appId, fnTurn, containerId, "{}", sessionId);
    E2E_CHECK(aOnTurn.success);
    auto bOffTurn = invoke(b, cfg.appId, fnTurn, containerId, "{}", sessionId);
    E2E_CHECK(!bOffTurn.success);
    E2E_CHECK(!bOffTurn.errorMessage.empty());

    turnInput["userId"] = b.userId;
    a.game->gameModel().setSessionTurn(turnInput);
    auto bOnTurn = invoke(b, cfg.appId, fnTurn, containerId, "{}", sessionId);
    E2E_CHECK(bOnTurn.success);
  }

  E2E_SUBTEST("traverse over an edge");
  {
    graphql::JVal otherInput;
    otherInput["appId"] = cfg.appId;
    otherInput["typeName"] = typeName;
    otherInput["displayName"] = "A's other crate";
    graphql::Json other = a.game->gameModel().createContainer(otherInput);
    const std::string otherId = other["containerId"].asString();
    E2E_CHECK(!otherId.empty());

    graphql::JVal edgeInput;
    edgeInput["appId"] = cfg.appId;
    edgeInput["fromContainerId"] = containerId;
    edgeInput["toContainerId"] = otherId;
    edgeInput["relationshipType"] = "e2e_linked";
    graphql::Json edge = a.game->gameModel().addEdge(edgeInput);
    E2E_CHECK(!edge["edgeId"].asString().empty());

    graphql::JVal traverseVars;
    traverseVars["appId"] = cfg.appId;
    traverseVars["rootId"] = containerId;
    traverseVars["relationshipType"] = "e2e_linked";
    traverseVars["depth"] = std::int64_t{1};
    graphql::Json traversal = a.game->gameModel().traverse(traverseVars);
    bool reachedOther = false;
    traversal["nodes"].forEach([&](graphql::Json node) {
      if (node["containerId"].asString() == otherId) reachedOther = true;
    });
    E2E_CHECK(reachedOther);
    E2E_CHECK(traversal["edges"].size() >= 1);
  }

  std::puts("e2e_game_model OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
