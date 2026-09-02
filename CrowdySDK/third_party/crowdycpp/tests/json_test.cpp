#include <limits>

#include "crowdy/graphql/json.hpp"
#include "test_util.hpp"

using namespace crowdy::graphql;

namespace {

void testJValBuildAndDump() {
  JVal v;
  v["appId"] = JVal("42");
  v["chunk"]["x"] = JVal(std::int64_t{1});
  v["chunk"]["y"] = JVal(std::int64_t{-2});
  v["flag"] = JVal(true);
  v["name"] = JVal("he\"llo\n");
  v["list"] = JVal::array({JVal(1), JVal(2)});
  v["nothing"] = JVal(nullptr);

  const std::string dumped = v.dump();
  // Round-trip through the parser to validate structure.
  Json parsed = Json::parse(dumped);
  CHECK(parsed.ok());
  CHECK_EQ(parsed["appId"].asString(), "42");
  CHECK_EQ(parsed["chunk"]["x"].asInt64(), 1);
  CHECK_EQ(parsed["chunk"]["y"].asInt64(), -2);
  CHECK(parsed["flag"].asBool());
  CHECK_EQ(parsed["name"].asString(), "he\"llo\n");
  CHECK_EQ(parsed["list"].size(), 2u);
  CHECK_EQ(parsed["list"].at(1).asInt64(), 2);
  CHECK(parsed["nothing"].isNull());
}

void testJsonParseAccessors() {
  Json j = Json::parse(R"({
    "s": "text", "i": 7, "f": 1.5, "b": false,
    "big": "9007199254740993",
    "max": "9223372036854775807",
    "min": "-9223372036854775808",
    "positiveOverflow": "9223372036854775808",
    "negativeOverflow": "-9223372036854775809",
    "numericOverflow": 9223372036854775808,
    "arr": [10, 20],
    "obj": {"nested": true},
    "nullv": null
  })");
  CHECK(j.ok());
  CHECK(j.isObject());
  CHECK_EQ(j["s"].asString(), "text");
  CHECK_EQ(j["i"].asInt64(), 7);
  CHECK_EQ(j["f"].asDouble(), 1.5);
  CHECK(!j["b"].asBool(true));
  CHECK_EQ(j["big"].asBigInt(), 9007199254740993LL);
  CHECK_EQ(j["i"].asBigInt(), 7);  // numbers accepted too
  CHECK_EQ(j["max"].asBigInt(), std::numeric_limits<std::int64_t>::max());
  CHECK_EQ(j["min"].asBigInt(), std::numeric_limits<std::int64_t>::min());
  CHECK_EQ(j["positiveOverflow"].asBigInt(73), 73);
  CHECK_EQ(j["negativeOverflow"].asBigInt(74), 74);
  CHECK_EQ(j["numericOverflow"].asBigInt(75), 75);
  CHECK(!j["positiveOverflow"].tryAsBigInt().has_value());
  CHECK(!j["numericOverflow"].tryAsBigInt().has_value());
  CHECK_EQ(j["positiveOverflow"].asBigIntString(),
           "9223372036854775808");
  CHECK_EQ(j["numericOverflow"].asBigIntString(),
           "9223372036854775808");
  CHECK_EQ(j["arr"].size(), 2u);
  CHECK_EQ(j["arr"].at(0).asInt64(), 10);
  CHECK(j["arr"].at(5).isNull());
  CHECK(j["obj"]["nested"].asBool());
  CHECK(j["missing"].isNull());
  CHECK(j["nullv"].isNull());
  CHECK_EQ(j["missing"]["deep"]["deeper"].asString("fb"), "fb");

  int count = 0;
  j["arr"].forEach([&](Json e) { count += static_cast<int>(e.asInt64()); });
  CHECK_EQ(count, 30);

  int members = 0;
  j.forEachMember([&](std::string_view, Json) { ++members; });
  CHECK_EQ(members, 13);

  CHECK(!Json::parse("{invalid").ok());
  CHECK(!Json::parse("").ok());
  CHECK_EQ(parseBigInt("9223372036854775807").value_or(0),
           std::numeric_limits<std::int64_t>::max());
  CHECK_EQ(parseBigInt("-9223372036854775808").value_or(0),
           std::numeric_limits<std::int64_t>::min());
  CHECK(!parseBigInt("9223372036854775808").has_value());
  CHECK(!parseBigInt("-9223372036854775809").has_value());
  CHECK(!parseBigInt("12x").has_value());
}

}  // namespace

int main() {
  testJValBuildAndDump();
  testJsonParseAccessors();
  std::puts("json_test OK");
  return 0;
}
