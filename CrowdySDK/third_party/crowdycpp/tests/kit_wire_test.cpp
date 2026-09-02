// Unit tests for the engine wire registry (kit/wire.hpp) — pose codec parity
// with the Rust kit-core::wire fixture (and CrowdyJS kit/wire), the flag
// lanes, and the type-77/type-90 server-event parsers.
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "crowdy/kit/wire.hpp"
#include "test_util.hpp"

using namespace crowdy::kit;

namespace {

// The kit-core::wire pose_roundtrip fixture.
EnginePose parityPose() {
  EnginePose pose;
  pose.x = 1.5f;
  pose.y = -20.25f;
  pose.z = 300.0f;
  pose.yaw = 0.5f;
  pose.pitch = -0.25f;
  pose.velX = 1.0f;
  pose.velY = 2.0f;
  pose.velZ = 3.0f;
  pose.flags = kFlagGrounded | kFlagMob;
  pose.held = 7;
  pose.updatedAtMs = 1784425621617.0;
  return pose;
}

void testPoseRoundtripAndLayout() {
  const EnginePose pose = parityPose();
  std::vector<std::uint8_t> bytes = encodeEnginePose(pose);
  CHECK_EQ(bytes.size(), kPoseBytes);

  // Layout parity: field offsets per the kit-core::wire doc.
  float x, z;
  std::memcpy(&x, &bytes[0], 4);
  std::memcpy(&z, &bytes[8], 4);
  CHECK_EQ(x, 1.5f);
  CHECK_EQ(z, 300.0f);
  CHECK_EQ(bytes[32], kFlagGrounded | kFlagMob);
  CHECK_EQ(bytes[33], 7);
  double updated;
  std::memcpy(&updated, &bytes[36], 8);
  CHECK_EQ(updated, 1784425621617.0);

  auto decoded = decodeEnginePose(bytes);
  CHECK(decoded.has_value());
  CHECK_EQ(decoded->y, -20.25f);
  CHECK_EQ(decoded->yaw, 0.5f);
  CHECK_EQ(decoded->velZ, 3.0f);
  CHECK_EQ(decoded->flags, pose.flags);
  CHECK_EQ(decoded->updatedAtMs, pose.updatedAtMs);
  CHECK(decoded->suffix.empty());
  CHECK(decoded->isMob());
  CHECK(!decoded->isNpc());
  CHECK(!decoded->isPlayer());
}

void testSuffixAndRejects() {
  EnginePose pose;
  pose.x = 1;
  pose.y = 2;
  pose.z = 3;
  pose.suffix = "container-123";
  std::vector<std::uint8_t> bytes = encodeEnginePose(pose);
  auto decoded = decodeEnginePose(bytes);
  CHECK(decoded.has_value());
  CHECK_EQ(decoded->suffix, "container-123");
  CHECK_EQ(poseSuffix(bytes.data(), kPoseBytes), "");

  std::vector<std::uint8_t> tooShort(10, 0);
  CHECK(!decodeEnginePose(tooShort).has_value());

  EnginePose bad;
  bad.x = std::numeric_limits<float>::quiet_NaN();
  CHECK(!decodeEnginePose(encodeEnginePose(bad)).has_value());
}

std::vector<std::uint8_t> frame(std::uint16_t type, const std::string& json) {
  std::vector<std::uint8_t> bytes;
  bytes.push_back(static_cast<std::uint8_t>(type & 0xff));
  bytes.push_back(static_cast<std::uint8_t>(type >> 8));
  bytes.insert(bytes.end(), json.begin(), json.end());
  return bytes;
}

void testEventParsers() {
  auto contactFrame = frame(
      kEventContactDamage,
      R"({"targetUuid":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","damage":3,"mobId":"slime","mobName":"Slime"})");
  auto contact = parseContactDamage(contactFrame.data(), contactFrame.size());
  CHECK(contact.has_value());
  CHECK_EQ(contact->damage, 3);
  CHECK_EQ(contact->mobId, "slime");

  auto weatherFrame = frame(kEventWeather, R"({"weather":"rain","sinceMs":5,"untilMs":99})");
  CHECK(!parseContactDamage(weatherFrame.data(), weatherFrame.size()).has_value());
  auto weather = parseWeatherEvent(weatherFrame.data(), weatherFrame.size());
  CHECK(weather.has_value());
  CHECK_EQ(weather->weather, "rain");
  CHECK_EQ(weather->untilMs, 99);
  CHECK(!parseWeatherEvent(contactFrame.data(), contactFrame.size()).has_value());

  std::uint8_t tiny[1] = {0};
  CHECK(!parseEngineEvent(tiny, 1).has_value());
}

void testRealtimeEventParsers() {
  auto abilityFrame = frame(kEventAbility,
      R"({"kind":"impact","abilityId":"bolt","casterId":"7","victimId":"9","damage":6})");
  auto ability = parseAbilityEvent(abilityFrame.data(), abilityFrame.size());
  CHECK(ability.has_value());
  CHECK_EQ(ability->kind, "impact");
  CHECK_EQ(ability->damage, 6);
  CHECK(!parseMovementViolation(abilityFrame.data(), abilityFrame.size()).has_value());

  auto violationFrame = frame(kEventMovementViolation,
      R"({"kind":"teleport","userId":"9","detail":"80 units"})");
  auto violation = parseMovementViolation(violationFrame.data(), violationFrame.size());
  CHECK(violation.has_value());
  CHECK_EQ(violation->kind, "teleport");
  CHECK_EQ(violation->userId, "9");

  auto pointFrame = frame(kEventControlPoint,
      R"({"pointId":"alpha","owner":"red","previousOwner":""})");
  auto point = parseControlPointEvent(pointFrame.data(), pointFrame.size());
  CHECK(point.has_value());
  CHECK_EQ(point->owner, "red");

  auto raceFrame = frame(kEventRaceTiming,
      R"({"kind":"lap","courseId":"loop","userId":"7","lap":2})");
  auto race = parseRaceTimingEvent(raceFrame.data(), raceFrame.size());
  CHECK(race.has_value());
  CHECK_EQ(race->kind, "lap");
  CHECK_EQ(race->courseId, "loop");

  auto zoneFrame = frame(kEventZoneChange,
      R"({"kind":"shrinking","phase":1,"radiusNow":42.5,"centerX":50,"centerZ":50})");
  auto zone = parseZoneChangeEvent(zoneFrame.data(), zoneFrame.size());
  CHECK(zone.has_value());
  CHECK_EQ(zone->kind, "shrinking");
  CHECK_EQ(zone->phase, 1);
  CHECK(zone->radiusNow > 42.0 && zone->radiusNow < 43.0);
  CHECK(!parseZoneChangeEvent(raceFrame.data(), raceFrame.size()).has_value());
}

void testSessionEventParsers() {
  auto turnFrame = frame(kEventTurn, R"({"actorId":"7","round":2,"turnInRound":3})");
  auto turn = parseTurnEvent(turnFrame.data(), turnFrame.size());
  CHECK(turn.has_value());
  CHECK_EQ(turn->actorId, "7");
  CHECK_EQ(turn->round, 2);
  CHECK_EQ(turn->turnInRound, 3);
  CHECK(!parseScoreEvent(turnFrame.data(), turnFrame.size()).has_value());

  auto scoreFrame = frame(
      kEventScore,
      R"({"winnerId":"9","standings":[{"actorId":"9","score":20,"rank":1}]})");
  auto score = parseScoreEvent(scoreFrame.data(), scoreFrame.size());
  CHECK(score.has_value());
  CHECK_EQ(score->winnerId, "9");
  CHECK_EQ(score->standings.at(0)["rank"].asInt64(), 1);

  auto proposalFrame =
      frame(kEventProposal, R"({"proposalId":"p1","mode":"ranked","players":["1","2"]})");
  auto proposal = parseProposalEvent(proposalFrame.data(), proposalFrame.size());
  CHECK(proposal.has_value());
  CHECK_EQ(proposal->proposalId, "p1");
  CHECK_EQ(proposal->players.size(), 2u);
  CHECK(!parseProposalEvent(turnFrame.data(), turnFrame.size()).has_value());
}

}  // namespace

int main() {
  testPoseRoundtripAndLayout();
  testSuffixAndRejects();
  testEventParsers();
  testSessionEventParsers();
  testRealtimeEventParsers();
  std::puts("kit_wire_test OK");
  return 0;
}
