#include <cstring>

#include "crowdy/session/codec.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::session;

namespace {

void testUnrealPoseLayout() {
  // Field offsets must match the published 88-byte layout (version at 0,
  // position/rotation/velocity f64 triples at 8..79, flags at 80/81).
  CHECK_EQ(offsetof(UnrealPose, version), 0u);
  CHECK_EQ(offsetof(UnrealPose, positionX), 8u);
  CHECK_EQ(offsetof(UnrealPose, positionY), 16u);
  CHECK_EQ(offsetof(UnrealPose, positionZ), 24u);
  CHECK_EQ(offsetof(UnrealPose, rotationPitch), 32u);
  CHECK_EQ(offsetof(UnrealPose, rotationYaw), 40u);
  CHECK_EQ(offsetof(UnrealPose, rotationRoll), 48u);
  CHECK_EQ(offsetof(UnrealPose, velocityX), 56u);
  CHECK_EQ(offsetof(UnrealPose, velocityY), 64u);
  CHECK_EQ(offsetof(UnrealPose, velocityZ), 72u);
  CHECK_EQ(offsetof(UnrealPose, crouch), 80u);
  CHECK_EQ(offsetof(UnrealPose, attachments), 81u);
  CHECK_EQ(sizeof(UnrealPose), 88u);
}

void testPodCodecRoundTrip() {
  UnrealPose pose;
  pose.positionX = 12.5;
  pose.positionY = -3.25;
  pose.rotationYaw = 1.57;
  pose.velocityZ = 100.0;
  pose.crouch = 1;
  pose.attachments = 7;

  std::uint8_t buf[UnrealPoseCodec::kSize];
  UnrealPoseCodec::encode(pose, buf);

  // Spot-check wire bytes: version byte and the f64 at offset 8.
  CHECK_EQ(buf[0], 2u);
  double x;
  std::memcpy(&x, buf + 8, 8);
  CHECK_EQ(x, 12.5);

  UnrealPose decoded;
  CHECK(UnrealPoseCodec::decode(Bytes(buf, sizeof(buf)), decoded));
  CHECK_EQ(decoded.positionX, 12.5);
  CHECK_EQ(decoded.rotationYaw, 1.57);
  CHECK_EQ(decoded.crouch, 1u);
  CHECK_EQ(decoded.attachments, 7u);

  // Wrong size rejected.
  CHECK(!UnrealPoseCodec::decode(Bytes(buf, 40), decoded));
}

struct MiniState {
  float x = 0, y = 0, z = 0, yaw = 0;
  std::uint8_t flags = 0;
  std::uint8_t held = 0;
  std::uint8_t pad[2] = {};
};

void testCustomPodState() {
  using Codec = PodCodec<MiniState>;
  static_assert(Codec::kSize == 20);
  MiniState s{1.f, 2.f, 3.f, 0.5f, 9, 4, {}};
  std::uint8_t buf[Codec::kSize];
  Codec::encode(s, buf);
  MiniState d;
  CHECK(Codec::decode(Bytes(buf, sizeof(buf)), d));
  CHECK_EQ(d.x, 1.f);
  CHECK_EQ(d.held, 4u);
}

void testRawCodec() {
  RawCodec<4>::State s;
  s.bytes[0] = 0xaa;
  s.bytes[3] = 0xbb;
  std::uint8_t buf[4];
  RawCodec<4>::encode(s, buf);
  RawCodec<4>::State d;
  CHECK(RawCodec<4>::decode(Bytes(buf, 4), d));
  CHECK_EQ(d.bytes[0], 0xaa);
  CHECK_EQ(d.bytes[3], 0xbb);
}

}  // namespace

int main() {
  testUnrealPoseLayout();
  testPodCodecRoundTrip();
  testCustomPodState();
  testRawCodec();
  std::puts("codec_struct_test OK");
  return 0;
}
