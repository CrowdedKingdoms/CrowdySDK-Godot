#include <cstdint>
#include <memory>
#include <string>

#include "crowdy/client.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/core/uuid.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/wire/codec.hpp"
#include "test_util.hpp"

using namespace crowdy;

namespace {

class NoopTransport final : public graphql::IHttpTransport {
 public:
  graphql::HttpResponse send(const graphql::HttpRequest&) override {
    return {200, R"({"data":{"ok":true}})"};
  }
};

class InjectedCrypto final : public core::ICrypto {
 public:
  bool hmacSha256(Bytes, Bytes, std::uint8_t* out) const override {
    for (std::size_t i = 0; i < kHmacTagSize; ++i) out[i] = 0x5a;
    return true;
  }
  bool sha256(Bytes, std::uint8_t* out) const override {
    for (std::size_t i = 0; i < kHmacTagSize; ++i) {
      out[i] = static_cast<std::uint8_t>(i);
    }
    return true;
  }
  bool constantTimeEquals(const std::uint8_t* a, const std::uint8_t* b,
                          std::size_t len) const override {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < len; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
  }
  bool randomBytes(std::uint8_t* out, std::size_t len) const override {
    for (std::size_t i = 0; i < len; ++i) {
      out[i] = static_cast<std::uint8_t>(i + 1);
    }
    return true;
  }
};

void testUnavailableProviderIsTyped() {
  const auto& crypto = core::defaultCrypto();
  CHECK_EQ(crypto.availability().code, Errc::CryptoUnavailable);
  CHECK_EQ(core::opensslCrypto().availability().code,
           Errc::CryptoUnavailable);

  auto uuid = core::tryGenerateActorUuid(crypto);
  CHECK(!uuid.ok());
  CHECK_EQ(uuid.error(), Errc::CryptoUnavailable);

  wire::LongSpatialParams params;
  params.type = wire::MessageType::ActorUpdateRequest;
  std::uint8_t output[wire::kMaxDatagramSize]{};
  const auto encoded = wire::encodeLongSpatial(
      crypto, params, wire::Token64{},
      MutableBytes(output, sizeof(output)));
  CHECK(!encoded.ok());
  CHECK_EQ(encoded.error(), Errc::CryptoUnavailable);

  std::uint8_t unsignedFrame[wire::kMinLongSpatialNoHmac]{};
  unsignedFrame[wire::offsets::kType] =
      static_cast<std::uint8_t>(wire::MessageType::ActorUpdateNotification);
  unsignedFrame[wire::offsets::kContainsAuth] = 0;
  CHECK(wire::verifyLongSpatial(
            crypto, Bytes(unsignedFrame, sizeof(unsignedFrame)),
            wire::Token64{})
            .ok());

  std::uint8_t signedFrame[wire::kMinLongSpatialWithHmac]{};
  signedFrame[wire::offsets::kType] =
      static_cast<std::uint8_t>(wire::MessageType::ActorUpdateNotification);
  signedFrame[wire::offsets::kContainsAuth] = 1;
  CHECK_EQ(wire::verifyLongSpatial(
               crypto, Bytes(signedFrame, sizeof(signedFrame)),
               wire::Token64{})
               .code,
           Errc::CryptoUnavailable);
}

void testClientReportsUnavailablePortalCrypto() {
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = std::make_shared<NoopTransport>();
  CrowdyClient client(std::move(config));

  const auto entry = client.portal().beginEntry(
      "42", "https://portal.invalid/authorize",
      "https://game.invalid/callback");
  CHECK(!entry.ok());
  CHECK_EQ(entry.status.code, Errc::CryptoUnavailable);
  CHECK(entry.url.empty());
}

void testInjectedCryptoRestoresPortalFlow() {
  InjectedCrypto crypto;
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = std::make_shared<NoopTransport>();
  config.crypto = &crypto;
  CrowdyClient client(std::move(config));

  const auto entry = client.portal().beginEntry(
      "42", "https://portal.invalid/authorize",
      "https://game.invalid/callback");
  CHECK(entry.ok());
  CHECK(!entry.verifier.empty());
  CHECK(!entry.state.empty());
  CHECK(entry.url.find("code_challenge=") != std::string::npos);
}

}  // namespace

int main() {
  testUnavailableProviderIsTyped();
  testClientReportsUnavailablePortalCrypto();
  testInjectedCryptoRestoresPortalFlow();
  std::puts("crypto_off_test OK");
  return 0;
}
