#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/core/bytes.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/core/spsc.hpp"
#include "crowdy/core/uuid.hpp"
#include "test_util.hpp"

using namespace crowdy;

namespace {

void testEndianHelpers() {
  std::uint8_t buf[8];
  le::writeI64(buf, -2);
  CHECK_EQ(buf[0], 0xfe);
  CHECK_EQ(buf[7], 0xff);
  CHECK_EQ(le::readI64(buf), -2);

  le::writeU16(buf, 0x1234);
  CHECK_EQ(buf[0], 0x34);
  CHECK_EQ(buf[1], 0x12);
  CHECK_EQ(le::readU16(buf), 0x1234);

  le::writeF32(buf, 1.5f);
  CHECK_EQ(le::readF32(buf), 1.5f);
  le::writeF64(buf, -3.25);
  CHECK_EQ(le::readF64(buf), -3.25);
}

// RFC 4231 test case 2 for HMAC-SHA256.
void testHmacKnownVector() {
  const char* key = "Jefe";
  const char* msg = "what do ya want for nothing?";
  const std::uint8_t expected[] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
                                   0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
                                   0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
                                   0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
  std::uint8_t out[32];
  CHECK(core::opensslCrypto().hmacSha256(asBytes(key), asBytes(msg), out));
  CHECK(std::memcmp(out, expected, 32) == 0);
  CHECK(core::opensslCrypto().constantTimeEquals(out, expected, 32));
  out[0] ^= 1;
  CHECK(!core::opensslCrypto().constantTimeEquals(out, expected, 32));
}

void testActorUuid() {
  auto a = core::generateActorUuid();
  auto b = core::generateActorUuid();
  CHECK(a != b);
  for (char c : a) CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  core::ActorUuid parsed;
  CHECK(core::actorUuidFromString(core::toString(a), parsed));
  CHECK(parsed == a);
  CHECK(!core::actorUuidFromString("too-short", parsed));
}

void testSpscRing() {
  core::SpscRing<int> ring(8);
  for (int i = 0; i < 8; ++i) {
    int v = i;
    CHECK(ring.tryPush(v));
  }
  int overflow = 99;
  CHECK(!ring.tryPush(overflow));
  for (int i = 0; i < 8; ++i) {
    auto v = ring.tryPop();
    CHECK(v.has_value());
    CHECK_EQ(*v, i);
  }
  CHECK(!ring.tryPop().has_value());

  // Cross-thread ordering smoke test.
  core::SpscRing<int> ring2(1024);
  constexpr int kN = 100000;
  std::thread producer([&] {
    for (int i = 0; i < kN;) {
      int v = i;
      if (ring2.tryPush(v)) ++i;
    }
  });
  int expect = 0;
  while (expect < kN) {
    if (auto v = ring2.tryPop()) {
      CHECK_EQ(*v, expect);
      ++expect;
    }
  }
  producer.join();
}

}  // namespace

int main() {
  testEndianHelpers();
  testHmacKnownVector();
  testActorUuid();
  testSpscRing();
  std::puts("core_test OK");
  return 0;
}
