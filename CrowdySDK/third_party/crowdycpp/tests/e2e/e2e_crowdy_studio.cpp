#include <string>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

constexpr const char* kCargoToml = R"toml([package]
name = "crowdy-studio-e2e"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]
)toml";

constexpr const char* kLibRs =
    "fn on_invoke(input: &[u8]) -> Vec<u8> { input.to_vec() }\n";

studio::CrowdyStudioProjectFile projectFile(std::string path,
                                             std::string content) {
  studio::CrowdyStudioProjectFile file;
  file.target = studio::CrowdyStudioTarget::Server;
  file.path = std::move(path);
  file.content = std::move(content);
  return file;
}

}  // namespace

int main() {
  const auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  const std::string gridId = e2e::envOr("CROWDY_E2E_STUDIO_GRID_ID");
  if (gridId.empty()) {
    std::puts("CROWDY_E2E_STUDIO_GRID_ID not configured; skipping");
    return 77;
  }

  auto& game = e2e::ownerGame(cfg);
  auto& studioApi = game.crowdyStudio();
  const std::string suffix = e2e::runSuffix();
  const std::string module = "studio_e2e_" + suffix;

  E2E_SUBTEST("create and read a typed Studio project");
  studio::CreateCrowdyStudioProjectInput create;
  create.appId = cfg.appId;
  create.gridId = gridId;
  create.kind = studio::CrowdyStudioProjectKind::Server;
  create.metadata.name = "CrowdyCPP Studio e2e " + suffix;
  create.metadata.description = "strict-parity CRUD";
  create.metadata.serverModuleName = module;
  create.idempotencyKey = "cpp-studio-create-" + suffix;
  create.files = {projectFile("Cargo.toml", kCargoToml),
                  projectFile("src/lib.rs", kLibRs)};
  auto project = studioApi.createProject(create);
  E2E_CHECK(!project.projectId.empty());
  E2E_CHECK(project.files.size() == 2);
  project = studioApi.getProject({cfg.appId, gridId}, project.projectId);
  E2E_CHECK(project.metadata.serverModuleName == module);

  E2E_SUBTEST("revision-fenced metadata and file patches");
  studio::SaveCrowdyStudioProjectMetadataInput metadata;
  metadata.appId = cfg.appId;
  metadata.projectId = project.projectId;
  metadata.expectedRevisionId = project.revision.id;
  metadata.patch.description =
      studio::CrowdyStudioPatchField<std::string>::value(
          "strict-parity patched");
  metadata.idempotencyKey = "cpp-studio-metadata-" + suffix;
  project = studioApi.saveProjectMetadata(metadata);
  E2E_CHECK(project.metadata.description == "strict-parity patched");

  studio::SaveCrowdyStudioProjectFilesInput files;
  files.appId = cfg.appId;
  files.projectId = project.projectId;
  files.expectedRevisionId = project.revision.id;
  files.idempotencyKey = "cpp-studio-files-" + suffix;
  files.upserts.push_back(projectFile(
      "src/lib.rs",
      "fn on_invoke(input: &[u8]) -> Vec<u8> { let mut out = input.to_vec(); out.push(1); out }\n"));
  project = studioApi.saveProjectFiles(files);
  E2E_CHECK(project.files.size() == 2);

  E2E_SUBTEST("submit the exact saved project as a draft");
  studio::CrowdyStudioPlayerComputeRuntime runtime(game.playerCompute());
  studio::CrowdyStudioDeployTargetInput draft;
  draft.scope = {cfg.appId, gridId};
  draft.target = studio::CrowdyStudioTarget::Server;
  draft.moduleName = module;
  draft.files = project.files;
  draft.sdkVersion = project.sdkVersion;
  draft.abiVersion = project.abiVersion;
  draft.deployment = studio::CrowdyStudioDeployment::Draft;
  const auto submitted = runtime.deploy(draft);
  E2E_CHECK(!submitted.versionId.empty());

  E2E_SUBTEST("archive the e2e project");
  project = studioApi.setProjectArchived(
      {.appId = cfg.appId,
       .projectId = project.projectId,
       .expectedRevisionId = project.revision.id,
       .archived = true,
       .idempotencyKey = "cpp-studio-archive-" + suffix});
  E2E_CHECK(project.archived);
  std::puts("e2e_crowdy_studio passed");
  return 0;
}
