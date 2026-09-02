// Durable chunk storage: dense 4096-byte voxel grid round-trip, spatial bulk
// load (getChunksByDistance), chunk-level state round-trip, and LOD set
// round-trip. Mirrors Game API SDK e2e: chunks.
// Reference: https://docs.crowdedkingdoms.com/game-api/chunks
//
// Reads go through the raw GraphQL escape hatch because the live schema's
// read inputs use `coordinates` / `centerCoordinate` + `maxDistance` field
// names (the typed ChunksAPI read helpers currently send `chunk`/`distance`).
#include <array>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

constexpr std::size_t kChunkBytes = 4096;

std::array<std::uint8_t, kChunkBytes> patternVoxels(std::uint8_t seed) {
  std::array<std::uint8_t, kChunkBytes> v{};
  for (std::size_t i = 0; i < v.size(); ++i)
    v[i] = static_cast<std::uint8_t>((i * 13 + seed) & 0xff);
  return v;
}

graphql::Json raw(e2e::Player& p, std::string_view doc, const graphql::JVal& vars,
                  std::string_view opName, std::string_view root) {
  return p.game->graphqlClient().request(doc, vars, opName)[root];
}

int run() {
  auto cfg = e2e::requireConfig();
  auto p = e2e::provisionPlayer(cfg, "chunks-a");

  const domains::ChunkRef c{200200, 0, 200200};

  E2E_SUBTEST("updateChunk stores a 4096-byte voxel grid");
  const auto stored = patternVoxels(3);
  const std::string storedB64 = core::base64Encode(Bytes(stored.data(), stored.size()));
  {
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["coordinates"] = c.toInput();
    input["voxels"] = storedB64;
    graphql::Json result = p.game->chunks().update(input);
    E2E_CHECK(result["coordinates"]["x"].asBigInt() == c.x);
  }

  E2E_SUBTEST("getChunk returns the grid byte-identical");
  {
    graphql::JVal vars;
    vars["input"]["appId"] = cfg.appId;
    vars["input"]["coordinates"] = c.toInput();
    graphql::Json chunk = raw(p, gen::chunks::kGetChunkDocument, vars, "GetChunk", "getChunk");
    auto decoded = core::base64Decode(chunk["voxels"].asStringView());
    E2E_CHECK(decoded.has_value());
    E2E_CHECK(decoded->size() == kChunkBytes);
    E2E_CHECK(std::memcmp(decoded->data(), stored.data(), kChunkBytes) == 0);
  }

  E2E_SUBTEST("getChunksByDistance contains the chunk");
  {
    graphql::JVal vars;
    vars["input"]["appId"] = cfg.appId;
    vars["input"]["centerCoordinate"] = c.toInput();
    vars["input"]["maxDistance"] = std::int64_t{1};
    graphql::Json page = raw(p, gen::chunks::kGetChunksByDistanceDocument, vars,
                             "GetChunksByDistance", "getChunksByDistance");
    bool found = false;
    page["chunks"].forEach([&](graphql::Json chunk) {
      if (chunk["coordinates"]["x"].asBigInt() == c.x &&
          chunk["coordinates"]["y"].asBigInt() == c.y &&
          chunk["coordinates"]["z"].asBigInt() == c.z) {
        found = true;
        auto decoded = core::base64Decode(chunk["voxels"].asStringView());
        E2E_CHECK(decoded.has_value() && decoded->size() == kChunkBytes);
        E2E_CHECK(std::memcmp(decoded->data(), stored.data(), kChunkBytes) == 0);
      }
    });
    E2E_CHECK(found);
  }

  E2E_SUBTEST("updateChunkState round-trip");
  const std::string chunkState = core::base64Encode(asBytes("chunk-state-" + e2e::runSuffix()));
  {
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["coordinates"] = c.toInput();
    input["chunkState"] = chunkState;
    // Writing chunk-level state is a studio-admin operation (requires
    // manage_apps); the player still reads it back below.
    graphql::Json result = e2e::ownerGame(cfg).chunks().updateState(input);
    E2E_CHECK(result["coordinates"]["x"].asBigInt() == c.x);

    graphql::JVal vars;
    vars["input"]["appId"] = cfg.appId;
    vars["input"]["coordinates"] = c.toInput();
    graphql::Json chunk = raw(p, gen::chunks::kGetChunkDocument, vars, "GetChunk", "getChunk");
    std::string roundTripped = chunk["chunkState"].asString();
    if (roundTripped != chunkState) {
      auto decoded = core::base64Decode(roundTripped);
      E2E_CHECK(decoded.has_value());
      roundTripped = std::string(decoded->begin(), decoded->end());
    }
    E2E_CHECK(roundTripped == chunkState);
  }

  E2E_SUBTEST("updateChunkLods + getChunkLods round-trip");
  const std::string lod0 = core::base64Encode(asBytes("lod0-data-" + e2e::runSuffix()));
  const std::string lod1 = core::base64Encode(asBytes("lod1-data-" + e2e::runSuffix()));
  {
    graphql::JVal entry0;
    entry0["level"] = std::int64_t{0};
    entry0["data"] = lod0;
    graphql::JVal entry1;
    entry1["level"] = std::int64_t{1};
    entry1["data"] = lod1;
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["coordinates"] = c.toInput();
    input["lods"] = graphql::JVal::array({entry0, entry1});
    // Writing LODs is a studio-admin operation (requires manage_apps); the
    // player still reads them back below.
    graphql::Json result = e2e::ownerGame(cfg).chunks().updateLods(input);
    E2E_CHECK(result["lods"].size() == 2);

    graphql::JVal vars;
    vars["input"]["appId"] = cfg.appId;
    vars["input"]["coordinates"] = c.toInput();
    vars["input"]["lodLevels"] = graphql::JVal::array(
        {graphql::JVal(std::int64_t{0}), graphql::JVal(std::int64_t{1})});
    graphql::Json lods = raw(p, gen::chunks::kGetChunkLodsDocument, vars, "GetChunkLods",
                             "getChunkLods")["lods"];
    E2E_CHECK(lods.size() == 2);
    bool saw0 = false, saw1 = false;
    lods.forEach([&](graphql::Json lod) {
      if (lod["level"].asInt64() == 0 && lod["data"].asString() == lod0) saw0 = true;
      if (lod["level"].asInt64() == 1 && lod["data"].asString() == lod1) saw1 = true;
    });
    E2E_CHECK(saw0 && saw1);
  }

  std::puts("e2e_chunks OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
