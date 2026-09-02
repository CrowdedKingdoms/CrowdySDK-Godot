#include <cstring>
#include <memory>
#include <stdexcept>

#include "crowdy/client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/kit/actions.hpp"
#include "crowdy/kit/inventory.hpp"
#include "crowdy/kit/matches.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

namespace {

class KitTransport final : public graphql::IHttpTransport {
 public:
  graphql::HttpResponse response;

  graphql::HttpResponse send(const graphql::HttpRequest&) override {
    return response;
  }
};

void testPolicyJson() {
  CHECK_EQ(kitPolicyJson(ownerOfSelfPolicy()), R"({"type":"owner_of_self"})");
  CHECK_EQ(kitPolicyJson(conditionPolicy("$amount > 0")),
           R"({"expression":"$amount > 0","type":"condition"})");
  CHECK_EQ(kitPolicyJson(featureGate("land_owner")),
           R"({"feature":"land_owner","type":"tier_feature"})");

  // andPolicies composition skips nulls and collapses single rules.
  JVal solo = andPolicies(ownerOfSelfPolicy(), {JVal(), JVal()});
  CHECK_EQ(kitPolicyJson(solo), R"({"type":"owner_of_self"})");
  JVal combined = andPolicies(ownerOfSelfPolicy(), {featureGate("vip")});
  graphql::Json parsed = graphql::Json::parse(kitPolicyJson(combined));
  CHECK_EQ(parsed["type"].asString(), "and");
  CHECK_EQ(parsed["rules"].size(), 2u);
}

void testToSnakeCase() {
  CHECK_EQ(toSnakeCase("Bank"), "bank");
  CHECK_EQ(toSnakeCase("GuildBank"), "guild_bank");
  CHECK_EQ(toSnakeCase("NPCSpawner"), "npcspawner");  // matches the TS regex behavior
  CHECK_EQ(toSnakeCase("wave2Boss"), "wave2_boss");
  CHECK_EQ(toSnakeCase("with space-dash"), "with_space_dash");
}

void testTrustedAuthority() {
  JVal fn;
  applyTrustedAuthority(fn, TrustedAuthority::server());
  graphql::Json parsed = graphql::Json::parse(fn.dump());
  CHECK_EQ(parsed["invokeScope"].asString(), "server");
  CHECK_EQ(graphql::Json::parse(parsed["invokePolicyJson"].asStringView())["type"].asString(),
           "allow");

  JVal fn2;
  applyTrustedAuthority(fn2, TrustedAuthority::automation(), "self.hp > 0");
  graphql::Json parsed2 = graphql::Json::parse(fn2.dump());
  CHECK(parsed2["autonomousInvocable"].asBool());
  graphql::Json policy2 = graphql::Json::parse(parsed2["invokePolicyJson"].asStringView());
  CHECK_EQ(policy2["type"].asString(), "and");
  CHECK_EQ(policy2["rules"].at(0)["type"].asString(), "is_automation");
  CHECK_EQ(policy2["rules"].at(1)["expression"].asString(), "self.hp > 0");
}

void testInventoryBlueprintShape() {
  KitBlueprint bp = inventoryBlueprint(
      {.typePrefix = "Bank", .maxSlots = 10, .slotCount = 8, .recipes = {}, .barters = {}});
  CHECK_EQ(bp.name, "BankInventory");
  CHECK_EQ(bp.containerTypes.size(), 2u);
  CHECK_EQ(bp.propertyDefinitions.size(), 5u);
  CHECK_EQ(bp.functions.size(), 4u);

  const auto names = inventoryNames("Bank");
  CHECK_EQ(names.stackType, "BankItemStack");
  CHECK_EQ(names.grantFn, "bank_grant_stack");
  CHECK_EQ(names.transferFn, "bank_transfer_stack");

  // The consume function's guard must match the TS blueprint exactly.
  graphql::Json consume = graphql::Json::parse(bp.functions[1].dump());
  CHECK_EQ(consume["name"].asString(), "bank_consume_stack");
  graphql::Json consumePolicy =
      graphql::Json::parse(consume["invokePolicyJson"].asStringView());
  CHECK_EQ(consumePolicy["rules"].at(1)["expression"].asString(),
           "$amount > 0 && self.quantity >= $amount");

  // move clamps to slotCount - 1.
  graphql::Json move = graphql::Json::parse(bp.functions[2].dump());
  CHECK_EQ(move["mutations"].at(0)["expression"].asString(), "clamp($to_slot, 0, 7)");

  // max_slots default carries the option.
  graphql::Json maxSlots = graphql::Json::parse(bp.propertyDefinitions[0].dump());
  CHECK_EQ(maxSlots["defaultValueJson"].asString(), "10");
}

void testAtomicInventoryTransactions() {
  InventoryRecipeSpec recipe;
  recipe.recipeId = "wood-planks";
  recipe.inputs = {{"wood", 2}};
  recipe.output = {"plank", 4};
  InventoryBarterSpec barter;
  barter.barterId = "wheat-for-emerald";
  barter.pay = {"wheat", 5};
  barter.receive = {"emerald", 1};
  KitBlueprint bp = inventoryBlueprint(
      {.typePrefix = "", .maxSlots = 24, .slotCount = 64,
       .recipes = {recipe}, .barters = {barter}});
  CHECK_EQ(bp.functions.size(), 6u);
  graphql::Json craft = graphql::Json::parse(bp.functions[4].dump());
  CHECK_EQ(craft["name"].asString(), "craft_wood_planks");
  CHECK(craft["autonomousInvocable"].asBool());
  CHECK_EQ(craft["mutations"].size(), 2u);
  graphql::Json craftPolicy =
      graphql::Json::parse(craft["invokePolicyJson"].asStringView());
  CHECK(std::strstr(craftPolicy["rules"].at(1)["expression"].asString().c_str(),
                    "item_id == \"wood\"") != nullptr);
  graphql::Json trade = graphql::Json::parse(bp.functions[5].dump());
  CHECK_EQ(trade["name"].asString(), "barter_wheat_for_emerald");
  CHECK(trade["autonomousInvocable"].asBool());
}

void testMergeBlueprints() {
  KitBlueprint a = inventoryBlueprint();
  KitBlueprint b = inventoryBlueprint(
      {.typePrefix = "Bank", .maxSlots = 24, .slotCount = 64, .recipes = {}, .barters = {}});
  MergedBlueprints merged = mergeBlueprints("42", {a, b}, "sess-1");

  graphql::Json seed = graphql::Json::parse(merged.seedInput.dump());
  CHECK_EQ(seed["appId"].asString(), "42");
  CHECK_EQ(seed["sessionId"].asString(), "sess-1");
  CHECK_EQ(seed["containerTypes"].size(), 4u);
  CHECK_EQ(seed["functions"].size(), 8u);

  // Duplicate names across blueprints must throw.
  bool threw = false;
  try {
    mergeBlueprints("42", {a, inventoryBlueprint()});
  } catch (const std::invalid_argument& e) {
    threw = true;
    CHECK(std::strstr(e.what(), "container type") != nullptr);
  }
  CHECK(threw);
}

void testComposeBlueprints() {
  KitBlueprint composite =
      composeBlueprints("bundle",
                        {inventoryBlueprint(),
                         inventoryBlueprint({.typePrefix = "X",
                                             .maxSlots = 24,
                                             .slotCount = 64,
                                             .recipes = {},
                                             .barters = {}})});
  CHECK_EQ(composite.name, "bundle");
  CHECK_EQ(composite.containerTypes.size(), 4u);
  CHECK_EQ(composite.functions.size(), 8u);
}

void testOwnerHelpers() {
  CHECK_EQ(ownerEqualsCaller("self.owner_user_id"), "self.owner_user_id == $caller_user_id");
  CHECK_EQ(ownerEqualsCaller("self.owner_user_id", OwnerIdKind::String),
           "self.owner_user_id == to_string($caller_user_id)");
  graphql::Json mirror = graphql::Json::parse(ownerMirrorProperty("Plot").dump());
  CHECK_EQ(mirror["key"].asString(), "owner_user_id");
  CHECK_EQ(mirror["valueType"].asString(), "int");
  CHECK_EQ(mirror["defaultValueJson"].asString(), "0");
}

void testOptimisticAction() {
  // Acceptance keeps the optimistic state and runs confirm.
  int state = 0;
  bool confirmed = false;
  OptimisticActionSpec ok;
  ok.apply = [&] { state = 1; };
  ok.rollback = [&] { state = 0; };
  ok.invoke = [](const std::string& actionId) {
    CHECK(actionId.size() > 8);
    return graphql::Json::parse(R"({"success":true,"itemId":"wood"})");
  };
  ok.confirm = [&](const graphql::Json&) { confirmed = true; };
  auto accepted = run_optimistic_action(ok);
  CHECK(accepted.ok);
  CHECK(confirmed);
  CHECK_EQ(state, 1);
  CHECK_EQ(accepted.result["itemId"].asString(), "wood");

  // Denial rolls back and surfaces the reason.
  OptimisticActionSpec denied = ok;
  denied.confirm = nullptr;
  denied.invoke = [](const std::string&) {
    return graphql::Json::parse(R"({"success":false,"reason":"out of range"})");
  };
  auto deniedOut = run_optimistic_action(denied);
  CHECK(!deniedOut.ok);
  CHECK_EQ(deniedOut.error_message, "out of range");
  CHECK_EQ(state, 0);

  // Transport errors roll back without throwing.
  OptimisticActionSpec thrown = ok;
  thrown.confirm = nullptr;
  thrown.invoke = [](const std::string&) -> graphql::Json {
    throw std::runtime_error("network down");
  };
  auto thrownOut = run_optimistic_action(thrown);
  CHECK(!thrownOut.ok);
  CHECK_EQ(thrownOut.error_message, "network down");
  CHECK_EQ(state, 0);

  // Caller-supplied actionId is honored (deliberate retries).
  OptimisticActionSpec retry;
  retry.action_id = "retry-1";
  std::string seen;
  retry.invoke = [&](const std::string& actionId) {
    seen = actionId;
    return graphql::Json::parse("{}");
  };
  run_optimistic_action(retry);
  CHECK_EQ(seen, "retry-1");
}

void testKitVerdictErrors() {
  const std::string violationMessage =
      "Invoke params violate the 'mine' contract: params.x must be an integer";
  graphql::CrowdyGraphQLError violation(
      {{violationMessage, "BAD_REQUEST", "", "computeInvoke"}});
  CHECK(isKitVerdictError(violation));
  graphql::CrowdyGraphQLError forbidden(
      {{"not authorized", "FORBIDDEN", "", "gameModelInvoke"}});
  CHECK(isKitVerdictError(forbidden));
  graphql::CrowdyGraphQLError unrelated(
      {{"Module is disabled", "BAD_REQUEST", "", "computeInvoke"}});
  CHECK(!isKitVerdictError(unrelated));
  std::runtime_error network("Invoke params violate but this is not GraphQL");
  CHECK(!isKitVerdictError(network));

  auto transport = std::make_shared<KitTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->response = {
      200,
      R"({"data":{"gameModelInvoke":{"eventId":"event-1","functionName":"mine","success":false,"returnValueJson":null,"errorMessage":"payload denial","mutationsApplied":[]}}})"};
  KitInvokeResult payloadVerdict =
      kitInvoke(client.gameModel(), "1", "mine", "container-1");
  CHECK(!payloadVerdict.success);
  CHECK_EQ(payloadVerdict.errorMessage, "payload denial");
  CHECK(payloadVerdict.raw.ok());

#ifndef CROWDY_NO_EXCEPTIONS
  // Legacy GraphQL-error verdict translation is specific to the throwing
  // blocking API. The resolved success=false payload path above is the
  // authoritative non-throwing contract and remains covered in every build.
  transport->response = {
      200,
      R"({"errors":[{"message":"Invoke params violate the 'mine' contract: params.x must be an integer","extensions":{"code":"BAD_REQUEST"}}]})"};
  KitInvokeResult verdict =
      kitInvoke(client.gameModel(), "1", "mine", "container-1");
  CHECK(!verdict.success);
  CHECK_EQ(verdict.errorMessage, violationMessage);
  CHECK(verdict.raw.ok());
  CHECK(!verdict.raw["success"].asBool());
  CHECK_EQ(verdict.raw["functionName"].asString(), "mine");
  CHECK_EQ(verdict.raw["errorMessage"].asString(), violationMessage);
  CHECK(verdict.raw["mutationsApplied"].isArray());
  CHECK_EQ(verdict.raw["mutationsApplied"].size(), 0u);

  transport->response = {
      200,
      R"({"errors":[{"message":"not authorized to invoke 'mine'","extensions":{"code":"FORBIDDEN"}}]})"};
  KitInvokeResult denied =
      kitInvoke(client.gameModel(), "1", "mine", "container-1");
  CHECK(!denied.success);
  CHECK_EQ(denied.errorMessage, "not authorized to invoke 'mine'");
  CHECK(!denied.raw["success"].asBool());

  transport->response = {
      200,
      R"({"errors":[{"message":"Module 'bwf-actions' is disabled","extensions":{"code":"BAD_REQUEST"}}]})"};
  bool rethrew = false;
  try {
    (void)kitInvoke(client.gameModel(), "1", "mine", "container-1");
  } catch (const graphql::CrowdyGraphQLError& error) {
    rethrew = true;
    CHECK_EQ(error.code(), "BAD_REQUEST");
  }
  CHECK(rethrew);
#endif
}

// turnExpired() reads the open turn only: a deadline stranded by an earlier
// turn records a lower sequence and must not register.
void testTurnExpired() {
  KitMatch none;
  CHECK(!turnExpired(none));  // deployed without turnTimer

  KitMatch fresh;
  fresh.turnSeq = 0;
  fresh.turnExpiredSeq = 0;
  CHECK(!turnExpired(fresh));

  KitMatch timedOut;
  timedOut.turnSeq = 3;
  timedOut.turnExpiredSeq = 3;
  CHECK(turnExpired(timedOut));

  // The player beat the clock and turn 4 opened, but turn 3's deadline had
  // already been claimed and fired late.
  KitMatch stranded;
  stranded.turnSeq = 4;
  stranded.turnExpiredSeq = 3;
  CHECK(!turnExpired(stranded));
}

// The turn deadline is deduped per match container and carries the sequence it
// was armed for, which is what makes a late fire detectable.
void testTurnTimerBlueprint() {
  MatchesBlueprintOptions options;
  options.turnTimer = MatchTurnTimer{30000};
  KitBlueprint bp = matchesBlueprint(options);

  bool sawBeginTurn = false;
  bool sawExpireTurn = false;
  for (const JVal& fn : bp.functions) {
    graphql::Json parsed = graphql::Json::parse(fn.dump());
    const std::string name = parsed["name"].asString();
    if (name == "begin_turn") {
      sawBeginTurn = true;
      graphql::Json timer = parsed["timers"].at(0);
      CHECK_EQ(timer["functionName"].asString(), "expire_turn");
      CHECK_EQ(timer["delayMsExpression"].asString(), "30000");
      CHECK_EQ(timer["dedupeKeyExpression"].asString(),
               "concat(\"match_turn:\", $self_container_id)");
      CHECK_EQ(timer["params"].at(0)["expression"].asString(), "self.turn_seq");
    } else if (name == "expire_turn") {
      sawExpireTurn = true;
      CHECK(parsed["autonomousInvocable"].asBool());
      CHECK_EQ(parsed["mutations"].at(0)["expression"].asString(),
               "max(self.turn_expired_seq, $turn_seq)");
    }
  }
  CHECK(sawBeginTurn);
  CHECK(sawExpireTurn);

  // The deadline replaces the polling automation outright.
  CHECK(bp.automations.empty());

  MatchesBlueprintOptions plain;
  KitBlueprint bare = matchesBlueprint(plain);
  for (const JVal& fn : bare.functions) {
    graphql::Json parsed = graphql::Json::parse(fn.dump());
    CHECK(parsed["name"].asString() != "begin_turn");
    CHECK(parsed["name"].asString() != "expire_turn");
  }
}

}  // namespace

int main() {
  testPolicyJson();
  testToSnakeCase();
  testTrustedAuthority();
  testInventoryBlueprintShape();
  testAtomicInventoryTransactions();
  testMergeBlueprints();
  testComposeBlueprints();
  testOwnerHelpers();
  testOptimisticAction();
  testKitVerdictErrors();
  testTurnExpired();
  testTurnTimerBlueprint();
  std::puts("kit_test OK");
  return 0;
}
