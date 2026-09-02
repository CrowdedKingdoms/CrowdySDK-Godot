#include <cstdio>
#include <functional>
#include <string>

#include "crowdy/agent/registry.hpp"
#include "crowdy/agent/schema.hpp"
#include "test_util.hpp"

using namespace crowdy;

namespace {

void expectAgentError(const std::function<void()>& operation,
                      std::string_view code) {
  bool threw = false;
  try {
    operation();
  } catch (const agent::CrowdyAgentError& error) {
    threw = true;
    CHECK(error.code() == code);
  }
  CHECK(threw);
}

void testCanonicalDigests() {
  CHECK(agent::sha256Digest("abc") ==
        "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const auto& registry = agent::canonicalAgentToolRegistryV1();
  CHECK_EQ(registry.list().size(), std::size_t{28});
  CHECK(registry.registryDigest() ==
        "sha256:20ecf25d55beccc1cb7033095cab3774dc301cb5f3edab818e45921416b48c71");
  CHECK(registry.require("workspace.file.patch", "1.0.0")
            .descriptorDigest ==
        "sha256:76fc6c30aef7d5011171dc2e637244d988c591e35a42c0bd2c97cd797483abde");
  CHECK(registry.require("game.control.move", "1.0.0")
            .descriptorDigest ==
        "sha256:31f7df3ce376fff468d08f2547632514cf60c87ee7c261767cb6cb65646ec156");
  CHECK(&registry.fromWireName("workspace_file_patch_v1") ==
        &registry.require("workspace.file.patch", "1.0.0"));
  expectAgentError(
      [&] { (void)registry.fromWireName("Workspace_file_patch_v1"); },
      "AGENT_TOOL_UNKNOWN");

  const auto canonical =
      graphql::Json::parse(R"({"z":1,"a":[true,"x"]})");
  CHECK(agent::canonicalJson(canonical) == R"({"a":[true,"x"],"z":1})");

  graphql::JArray reasons;
  for (const auto reason : agent::kAgentPreemptionReasons) {
    reasons.emplace_back(reason);
  }
  CHECK(agent::sha256Digest(graphql::JVal(std::move(reasons)).dump()) ==
        "sha256:81dd9cb78b5c171456430fe1b5aefa4a628ab17d7f2a35d1ed5bf78b31834dc1");
}

void testBoundedValidation() {
  const auto& registry = agent::canonicalAgentToolRegistryV1();
  const auto good = graphql::Json::parse(
      R"({"expectedRevision":"7","changes":[{"target":"SERVER","path":"src/lib.rs","content":"pub fn tick() {}","expectedContentHash":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]})");
  registry.validateInput("workspace.file.patch", "1.0.0", good);
  const auto scopes =
      registry.requiredScopes("workspace.file.patch", "1.0.0", good);
  CHECK_EQ(scopes.size(), std::size_t{2});
  CHECK(scopes[0] == "agent.use");
  CHECK(scopes[1] == "studio.project.write.server");

  for (const auto text : {
           R"({"expectedRevision":"7","changes":[{"target":"SERVER","path":"src/lib.rs","content":"ok","expectedContentHash":"ABSENT"}],"userId":"forged"})",
           R"({"expectedRevision":"7","changes":[{"target":"ADMIN","path":"src/lib.rs","content":"ok","expectedContentHash":"ABSENT"}]})",
           R"({"expectedRevision":"7","changes":[{"target":"SERVER","path":"../secret","content":"ok","expectedContentHash":"ABSENT"}]})",
       }) {
    const auto bad = graphql::Json::parse(text);
    expectAgentError(
        [&] {
          registry.validateInput("workspace.file.patch", "1.0.0", bad);
        },
        "AGENT_TOOL_INPUT_INVALID");
  }
  const auto unsafeNumber = graphql::Json::parse(
      R"({"observationId":"obs","capabilityRevision":"cap","controlledEntityId":"entity","direction":"FORWARD","intensity":9007199254740992,"durationMs":100})");
  expectAgentError(
      [&] {
        registry.validateInput("game.control.move", "1.0.0",
                               unsafeNumber);
      },
      "AGENT_TOOL_INPUT_INVALID");
}

void testMalformedDescriptorsFailClosed() {
  const auto& entry =
      agent::canonicalAgentToolRegistryV1().require("studio.context.get",
                                                    "1.0.0");
  std::string authority = entry.descriptor->canonical;
  const auto properties = authority.find(R"("properties":{)");
  CHECK(properties != std::string::npos);
  authority.insert(properties + std::string(R"("properties":{)").size(),
                   R"("userId":{"type":"string","maxLength":8},)");
  const auto parsedAuthority = graphql::Json::parse(authority);
  expectAgentError(
      [&] { (void)agent::parseAgentToolDescriptor(parsedAuthority); },
      "AGENT_TOOL_DESCRIPTOR_INVALID");

  std::string rawSurface = entry.descriptor->canonical;
  const auto name = rawSurface.find("studio.context.get");
  CHECK(name != std::string::npos);
  rawSurface.replace(name, std::string("studio.context.get").size(),
                     "studio.raw_graphql");
  const auto parsedRaw = graphql::Json::parse(rawSurface);
  expectAgentError(
      [&] { (void)agent::parseAgentToolDescriptor(parsedRaw); },
      "AGENT_TOOL_DESCRIPTOR_INVALID");

  std::string drifted(agent::canonicalAgentToolFixtureJsonV1());
  const auto digest = drifted.find(
      "sha256:42c8abffeb9c1425afc7aa4f26fe4a0d76fb19c5da32f4c8cf37c3fa8888b215");
  CHECK(digest != std::string::npos);
  drifted[digest + 8] = drifted[digest + 8] == '0' ? '1' : '0';
  expectAgentError(
      [&] { (void)agent::AgentToolRegistry::fromFixtureJson(drifted); },
      "AGENT_TOOL_DESCRIPTOR_INVALID");
}

}  // namespace

int main() {
  testCanonicalDigests();
  testBoundedValidation();
  testMalformedDescriptorsFailClosed();
  std::printf("agent_registry_test passed\n");
  return 0;
}
