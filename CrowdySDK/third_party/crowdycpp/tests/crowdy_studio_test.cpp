#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/studio/controller.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::studio;

namespace {

class CaptureTransport final : public graphql::IHttpTransport {
 public:
  std::deque<graphql::HttpResponse> responses;
  std::vector<graphql::HttpRequest> requests;

  graphql::HttpResponse send(
      const graphql::HttpRequest& request) override {
    requests.push_back(request);
    if (responses.empty()) {
      throw std::runtime_error("CaptureTransport has no response");
    }
    auto response = responses.front();
    responses.pop_front();
    return response;
  }
};

std::string projectJson(std::string_view revision = "1",
                        std::string_view serverContent = "fn server() {}",
                        bool includeClient = true,
                        std::string_view grid = "\"500\"") {
  std::string files =
      R"({"target":"SERVER","path":"src/lib.rs","content":")" +
      std::string(serverContent) +
      R"(","revision":"9007199254740993","provenance":"LIBRARY","provenanceLibraryFileId":"lib-1","provenanceLibraryRevision":"9007199254740995","provenanceCommonVersionId":null,"createdAt":"2026-07-23T00:00:00Z","updatedAt":"2026-07-23T00:00:00Z"})";
  if (includeClient) {
    files +=
        R"(,{"target":"CLIENT","path":"src/lib.rs","content":"fn client() {}","revision":"1","provenance":"COMMON","provenanceLibraryFileId":null,"provenanceLibraryRevision":null,"provenanceCommonVersionId":"common-v1","createdAt":"2026-07-23T00:00:00Z","updatedAt":"2026-07-23T00:00:00Z"})";
  }
  return R"({"projectId":"project-1","appId":"9007199254740997","ownerUserId":"9007199254740999","gridId":)" +
         std::string(grid) +
         R"(,"name":"Tools","description":null,"serverModuleName":"tools-server","clientModuleName":)" +
         (includeClient ? R"("tools-client")" : "null") +
         R"(,"pairingPreference":")" +
         (includeClient ? "PAIRED" : "SERVER_ONLY") +
         R"(","sdkVersion":"0.1.5","abiVersion":0,"revision":")" +
         std::string(revision) +
         R"(","archived":false,"archivedAt":null,"fileCount":)" +
         (includeClient ? "2" : "1") +
         R"(,"totalBytes":"9007199254741001","createdAt":"2026-07-23T00:00:00Z","updatedAt":"2026-07-23T00:00:01Z","files":[)" +
         files + "]}";
}

graphql::HttpResponse dataResponse(std::string_view root,
                                   const std::string& value) {
  return {200, R"({"data":{")" + std::string(root) + "\":" + value +
                   "}}"};
}

void testApiMappingAndInputSemantics() {
  auto transport = std::make_shared<CaptureTransport>();
  ClientConfig config;
  config.httpUrl = "https://game.invalid";
  config.transport = transport;
  CrowdyClient client(std::move(config));

  transport->responses.push_back(dataResponse(
      "crowdyStudioProjects",
      R"([{"projectId":"project-1","gridId":null,"name":"Tools","serverModuleName":"tools-server","clientModuleName":"tools-client","pairingPreference":"PAIRED","revision":"9007199254740993","archived":false,"updatedAt":"2026-07-23T00:00:00Z"}])"));
  const auto projects = client.crowdyStudio().listProjects(
      {"9007199254740997", "500"});
  CHECK_EQ(projects.size(), std::size_t{1});
  CHECK(projects[0].revisionId == "9007199254740993");
  CHECK(!projects[0].gridId);
  CHECK(projects[0].kind == CrowdyStudioProjectKind::FullStack);

  transport->responses.push_back(
      dataResponse("crowdyStudioProject", projectJson()));
  CrowdyStudioProject project = client.crowdyStudio().getProject(
      {"9007199254740997", "500"}, "project-1");
  CHECK(project.appId == "9007199254740997");
  CHECK(project.ownerUserId == "9007199254740999");
  CHECK(project.totalBytes == "9007199254741001");
  CHECK(project.files[0].revision == "9007199254740993");
  CHECK(project.files[0].provenance ==
        CrowdyStudioFileProvenance::Library);
  CHECK(project.files[0].provenanceLibraryRevision ==
        std::optional<std::string>{"9007199254740995"});
  CHECK(project.files[1].provenance ==
        CrowdyStudioFileProvenance::Common);

  project.files[0].content = "fn changed() {}";
  project.files.erase(project.files.begin() + 1);
  transport->responses.push_back(dataResponse(
      "crowdyStudioProjectSave", projectJson("2", "fn changed() {}", false)));
  const CrowdyStudioProject saved = client.crowdyStudio().saveProject({
      project.appId,
      "500",
      project.projectId,
      project.revision.id,
      project.metadata,
      project.files,
      project.sdkVersion,
      project.abiVersion,
      "save-key",
  });
  CHECK(saved.revision.id == "2");
  const std::string& saveBody = transport->requests.back().body;
  CHECK(saveBody.find(R"("expectedRevision":"1")") != std::string::npos);
  CHECK(saveBody.find(R"("idempotencyKey":"save-key")") !=
        std::string::npos);
  CHECK(saveBody.find(R"("content":"fn changed() {}")") !=
        std::string::npos);
  CHECK(saveBody.find(R"("target":"CLIENT")") != std::string::npos);
  CHECK(saveBody.find(R"("description":null)") != std::string::npos);
  CHECK(saveBody.find("sourceFilesJson") == std::string::npos);

  SaveCrowdyStudioProjectMetadataInput metadata;
  metadata.appId = project.appId;
  metadata.projectId = project.projectId;
  metadata.expectedRevisionId = "2";
  metadata.patch.name = "Renamed";
  metadata.patch.description =
      CrowdyStudioPatchField<std::string>::null();
  metadata.patch.clientModuleName =
      CrowdyStudioPatchField<std::string>::value("renamed-client");
  transport->responses.push_back(dataResponse(
      "crowdyStudioProjectSaveMetadata", projectJson("3")));
  (void)client.crowdyStudio().saveProjectMetadata(metadata);
  const std::string& metadataBody = transport->requests.back().body;
  CHECK(metadataBody.find(R"("description":null)") != std::string::npos);
  CHECK(metadataBody.find(R"("clientModuleName":"renamed-client")") !=
        std::string::npos);
  CHECK(metadataBody.find(R"("gridId":)") == std::string::npos);
  CHECK(metadataBody.find(R"("serverModuleName":)") == std::string::npos);

  transport->responses.push_back(dataResponse(
      "crowdyStudioCommonFiles",
      R"([{"commonFileId":"common-1","appId":"9007199254740997","slug":"helpers","title":"Helpers","description":null,"path":"src/helpers.rs","target":"SERVER","tags":["helper"],"status":"PUBLISHED","versionId":"common-v1","versionNo":"9007199254741011","content":"pub fn helper() {}","contentSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","publishedByUserId":"9007199254740999","publishedAt":"2026-07-23T00:00:00Z","createdAt":"2026-07-23T00:00:00Z","updatedAt":"2026-07-23T00:00:00Z"}])"));
  const auto common = client.crowdyStudio().listCommonFiles(
      {"9007199254740997", "500"});
  CHECK(common[0].id == "common-v1");
  CHECK(common[0].revision == "9007199254741011");
  CHECK(common[0].source == CrowdyStudioReferenceSource::Common);

  bool callbackCalled = false;
  transport->responses.push_back(dataResponse(
      "crowdyStudioProjects",
      R"([{"projectId":"project-1","gridId":"500","name":"Tools","serverModuleName":"tools-server","clientModuleName":null,"pairingPreference":"SERVER_ONLY","revision":"4","archived":false,"updatedAt":"2026-07-23T00:00:00Z"}])"));
  client.crowdyStudio().listProjectsAsync(
      {project.appId, "500"}, {},
      [&](graphql::GraphQLOutcome outcome,
          std::vector<CrowdyStudioProjectSummary> value) {
        CHECK(outcome.ok());
        CHECK_EQ(value.size(), std::size_t{1});
        CHECK(value[0].kind == CrowdyStudioProjectKind::Server);
        callbackCalled = true;
      });
  CHECK(!callbackCalled);
  client.poll();
  CHECK(callbackCalled);

#ifndef CROWDY_NO_EXCEPTIONS
  // These two assertions cover typed translation of exceptions from the
  // blocking GraphQL API. Non-throwing builds retain all mapping/controller
  // coverage above and exercise GraphQLOutcome failures in async API tests.
  transport->responses.push_back(
      {200,
       R"({"errors":[{"message":"CROWDY_STUDIO_REVISION_CONFLICT: stale","extensions":{"code":"CROWDY_STUDIO_REVISION_CONFLICT"}}]})"});
  bool revisionConflict = false;
  try {
    (void)client.crowdyStudio().saveProjectMetadata(metadata);
  } catch (const CrowdyStudioRevisionConflictError& error) {
    revisionConflict =
        error.code() == "CROWDY_STUDIO_REVISION_CONFLICT";
  }
  CHECK(revisionConflict);

  transport->responses.push_back(
      {200,
       R"({"errors":[{"message":"retry key changed input","extensions":{"code":"IDEMPOTENCY_CONFLICT"}}]})"});
  bool idempotencyConflict = false;
  try {
    (void)client.crowdyStudio().saveProjectMetadata(metadata);
  } catch (const CrowdyStudioIdempotencyConflictError& error) {
    idempotencyConflict = error.code() == "IDEMPOTENCY_CONFLICT";
  }
  CHECK(idempotencyConflict);
#else
  bool revisionOutcome = false;
  transport->responses.push_back(
      {200,
       R"({"errors":[{"message":"CROWDY_STUDIO_REVISION_CONFLICT: stale","extensions":{"code":"CROWDY_STUDIO_REVISION_CONFLICT"}}]})"});
  client.crowdyStudio().saveProjectMetadataAsync(
      metadata, [&](graphql::GraphQLOutcome outcome,
                    CrowdyStudioProject) {
        revisionOutcome =
            !outcome.ok() && !outcome.errors.empty() &&
            outcome.errors.front().code ==
                "CROWDY_STUDIO_REVISION_CONFLICT";
      });
  client.poll();
  CHECK(revisionOutcome);

  bool idempotencyOutcome = false;
  transport->responses.push_back(
      {200,
       R"({"errors":[{"message":"retry key changed input","extensions":{"code":"IDEMPOTENCY_CONFLICT"}}]})"});
  client.crowdyStudio().saveProjectMetadataAsync(
      metadata, [&](graphql::GraphQLOutcome outcome,
                    CrowdyStudioProject) {
        idempotencyOutcome =
            !outcome.ok() && !outcome.errors.empty() &&
            outcome.errors.front().code == "IDEMPOTENCY_CONFLICT";
      });
  client.poll();
  CHECK(idempotencyOutcome);
#endif
}

CrowdyStudioProjectFile sourceFile(
    CrowdyStudioTarget target, std::string path, std::string content,
    CrowdyStudioFileProvenance provenance =
        CrowdyStudioFileProvenance::Authored) {
  CrowdyStudioProjectFile file;
  file.target = target;
  file.path = std::move(path);
  file.content = std::move(content);
  file.revision = "1";
  file.provenance = provenance;
  return file;
}

CrowdyStudioProject makeProject(
    std::string id = "project-1",
    CrowdyStudioProjectKind kind = CrowdyStudioProjectKind::FullStack,
    std::string revision = "1") {
  CrowdyStudioProject project;
  project.projectId = std::move(id);
  project.appId = "42";
  project.ownerUserId = "7";
  project.gridId = "500";
  project.kind = kind;
  project.metadata.name = "Weather tools";
  if (kind != CrowdyStudioProjectKind::Client) {
    project.metadata.serverModuleName = "weather-server";
    project.files.push_back(sourceFile(
        CrowdyStudioTarget::Server, "Cargo.toml", "[package]"));
    project.files.push_back(sourceFile(
        CrowdyStudioTarget::Server, "src/lib.rs", "fn server() {}"));
  }
  if (kind != CrowdyStudioProjectKind::Server) {
    project.metadata.clientModuleName = "weather-client";
    project.files.push_back(sourceFile(
        CrowdyStudioTarget::Client, "Cargo.toml", "[package]"));
    project.files.push_back(sourceFile(
        CrowdyStudioTarget::Client, "src/lib.rs", "fn client() {}"));
  }
  project.metadata.pairingPreference =
      kind == CrowdyStudioProjectKind::FullStack
          ? CrowdyStudioPairingPreference::Required
          : CrowdyStudioPairingPreference::None;
  project.sdkVersion = "0.1.5";
  project.abiVersion = 0;
  project.revision = {std::move(revision), "2026-07-23T00:00:00Z"};
  project.fileCount = static_cast<int>(project.files.size());
  project.totalBytes = "40";
  project.createdAt = "2026-07-23T00:00:00Z";
  project.updatedAt = "2026-07-23T00:00:00Z";
  return project;
}

CrowdyStudioProjectSummary summary(const CrowdyStudioProject& project) {
  return {project.projectId,
          project.gridId,
          project.metadata.name,
          project.kind,
          project.revision.id,
          project.metadata.serverModuleName,
          project.metadata.clientModuleName,
          project.archived,
          project.updatedAt};
}

class FakeProjectProvider final : public ICrowdyStudioProjectProvider {
 public:
  std::vector<CrowdyStudioProject> projects{
      makeProject(), makeProject("project-2", CrowdyStudioProjectKind::Server)};
  std::vector<SaveCrowdyStudioProjectInput> saves;
  bool offline = false;
  bool conflictOnce = false;

  std::vector<CrowdyStudioProjectSummary> listProjects(
      const CrowdyStudioProjectScope&) override {
    std::vector<CrowdyStudioProjectSummary> result;
    for (const auto& project : projects) result.push_back(summary(project));
    return result;
  }

  CrowdyStudioProject getProject(
      const CrowdyStudioProjectScope&, std::string_view projectId) override {
    return get(projectId);
  }

  CrowdyStudioProject createProject(
      const CreateCrowdyStudioProjectInput& input) override {
    CrowdyStudioProject created = makeProject(
        "project-created", input.kind, "1");
    created.metadata = input.metadata;
    created.files = input.files;
    projects.push_back(created);
    return created;
  }

  CrowdyStudioProject saveProject(
      const SaveCrowdyStudioProjectInput& input) override {
    if (offline) throw CrowdyStudioOfflineError("offline");
    saves.push_back(input);
    CrowdyStudioProject& current = getMutable(input.projectId);
    if (conflictOnce) {
      conflictOnce = false;
      current.revision.id =
          std::to_string(std::stoll(current.revision.id) + 1);
      current.files[1].content = "fn remote() {}";
      throw CrowdyStudioRevisionConflictError("stale revision", current);
    }
    if (current.revision.id != input.expectedRevisionId) {
      throw CrowdyStudioRevisionConflictError("stale revision", current);
    }
    current.metadata = input.metadata;
    current.files = input.files;
    current.gridId = input.gridId;
    current.revision.id =
        std::to_string(std::stoll(current.revision.id) + 1);
    current.revision.savedAt = "2026-07-23T00:00:01Z";
    current.updatedAt = current.revision.savedAt;
    return current;
  }

  std::vector<CrowdyStudioReferenceFile> listPersonalLibraryFiles(
      const CrowdyStudioProjectScope&) override {
    return {};
  }

  CrowdyStudioReferenceFile savePersonalLibraryFile(
      const SaveCrowdyStudioLibraryFileInput& input) override {
    CrowdyStudioReferenceFile file;
    file.id = "library-1";
    file.source = CrowdyStudioReferenceSource::PersonalLibrary;
    file.appId = input.appId;
    file.title = input.title;
    file.target = input.target;
    file.path = input.path;
    file.content = input.content;
    file.revision = "1";
    return file;
  }

  std::vector<CrowdyStudioReferenceFile> listCommonFiles(
      const CrowdyStudioProjectScope&) override {
    return {};
  }

  CrowdyStudioProject importReferenceFile(
      const ImportCrowdyStudioReferenceFileInput& input) override {
    CrowdyStudioProject& project = getMutable(input.projectId);
    project.files.push_back(sourceFile(
        CrowdyStudioTarget::Server,
        input.destinationPath.value_or("src/imported.rs"),
        "pub fn imported() {}", CrowdyStudioFileProvenance::Common));
    project.revision.id =
        std::to_string(std::stoll(project.revision.id) + 1);
    return project;
  }

  CrowdyStudioProject get(std::string_view id) const {
    const auto found = std::find_if(
        projects.begin(), projects.end(),
        [&](const CrowdyStudioProject& project) {
          return project.projectId == id;
        });
    if (found == projects.end()) throw std::runtime_error("not found");
    return *found;
  }

  CrowdyStudioProject& getMutable(std::string_view id) {
    const auto found = std::find_if(
        projects.begin(), projects.end(),
        [&](const CrowdyStudioProject& project) {
          return project.projectId == id;
        });
    if (found == projects.end()) throw std::runtime_error("not found");
    return *found;
  }
};

class FakeRuntime final : public ICrowdyStudioRuntime {
 public:
  std::vector<std::string> calls;

  CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput& input) override {
    calls.push_back("deploy:" + std::string(toString(input.target)) + ":" +
                    (input.deployment == CrowdyStudioDeployment::Draft
                         ? "DRAFT"
                         : "LIVE"));
    return {input.target == CrowdyStudioTarget::Client ? "client-v1"
                                                       : "server-v1"};
  }

  std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope&,
      std::string_view moduleName) override {
    calls.push_back("poll:" + std::string(moduleName));
    return {{moduleName.find("client") != std::string_view::npos
                 ? "client-v1"
                 : "server-v1",
             "succeeded", std::nullopt}};
  }

  void setEnabled(const CrowdyStudioProjectScope&,
                  std::string_view moduleName, bool enabled) override {
    calls.push_back("enabled:" + std::string(moduleName) + ":" +
                    (enabled ? "true" : "false"));
  }

  void setRequires(
      const CrowdyStudioProjectScope&, std::string_view serverName,
      const std::optional<std::string>& clientName) override {
    calls.push_back("requires:" + std::string(serverName) + ":" +
                    clientName.value_or("<none>"));
  }

  void startClient(const CrowdyStudioProjectScope&,
                   std::string_view moduleName,
                   std::string_view versionId) override {
    calls.push_back("start:" + std::string(moduleName) + ":" +
                    std::string(versionId));
  }

  void stopClient() override { calls.push_back("stop-client"); }

  CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope&, std::string_view moduleName,
      std::string_view exportName,
      const std::optional<std::string>&) override {
    calls.push_back("invoke:" + std::string(moduleName) + ":" +
                    std::string(exportName));
    return {std::nullopt, R"({"ok":true})", "4", 2};
  }
};

class FakeClock final : public core::IClock {
 public:
  std::int64_t epoch = 1'000;
  std::int64_t monotonic = 0;
  std::int64_t epochMillis() const override { return epoch; }
  std::int64_t monotonicMillis() const override { return monotonic; }
};

class FakeApprovalGate final : public ICrowdyStudioApprovalGate {
 public:
  int liveApprovals = 0;
  int restoreApprovals = 0;

  void requireLiveApproval(
      const CrowdyStudioLiveApprovalRequest& request,
      std::string_view grant) override {
    CHECK(request.projectId == "project-1");
    CHECK(request.projectContentHash.rfind("sha256:", 0) == 0);
    CHECK(grant == "live-grant");
    ++liveApprovals;
  }

  void requireRestoreApproval(
      const CrowdyStudioRestoreApprovalRequest& request,
      std::string_view grant) override {
    CHECK(request.projectId == "project-1");
    CHECK(grant == "restore-grant");
    ++restoreApprovals;
  }
};

class FakeSynchronization final
    : public ICrowdyStudioSynchronizationProvider {
 public:
  explicit FakeSynchronization(FakeProjectProvider& projectProvider)
      : provider(projectProvider) {}

  FakeProjectProvider& provider;
  int patchCalls = 0;
  int restoreCalls = 0;

  CrowdyStudioAtomicPatchResult applyAtomicPatch(
      const CrowdyStudioProjectScope&, std::string_view projectId,
      const CrowdyStudioAtomicPatchInput& input) override {
    ++patchCalls;
    CrowdyStudioProject& project = provider.getMutable(projectId);
    const std::string previousRevision = project.revision.id;
    std::vector<CrowdyStudioCheckpointFile> changed;
    for (const auto& change : input.changes) {
      auto file = std::find_if(
          project.files.begin(), project.files.end(),
          [&](const CrowdyStudioProjectFile& current) {
            return current.target == change.target &&
                   current.path == change.path;
          });
      if (file == project.files.end()) {
        project.files.push_back(
            sourceFile(change.target, change.path, change.content));
      } else {
        file->content = change.content;
      }
      changed.push_back(
          {change.target, change.path, "sha256:changed",
           change.content.size()});
    }
    project.revision.id =
        std::to_string(std::stoll(project.revision.id) + 1);
    CrowdyStudioCheckpointMetadata checkpoint;
    checkpoint.checkpointId = "checkpoint-1";
    checkpoint.projectRevisionId = previousRevision;
    checkpoint.contentHash = "sha256:checkpoint";
    checkpoint.reason =
        CrowdyStudioCheckpointMetadata::Reason::AgentWrite;
    checkpoint.files = changed;
    checkpoint.createdAt = "2026-07-23T00:00:00Z";
    return {project, checkpoint, changed};
  }

  std::vector<CrowdyStudioCheckpointMetadata> listCheckpoints(
      const CrowdyStudioProjectScope&, std::string_view) override {
    return {};
  }

  CrowdyStudioCheckpointRestoreResult restoreCheckpoint(
      const CrowdyStudioCheckpointRestoreInput& input) override {
    ++restoreCalls;
    CrowdyStudioProject& project =
        provider.getMutable(input.projectId);
    const std::string previousRevision = project.revision.id;
    project.files[1].content = "fn restored() {}";
    project.revision.id =
        std::to_string(std::stoll(project.revision.id) + 1);
    CrowdyStudioCheckpointMetadata preimage;
    preimage.checkpointId = "pre-restore";
    preimage.projectRevisionId = previousRevision;
    preimage.contentHash = "sha256:preimage";
    preimage.reason =
        CrowdyStudioCheckpointMetadata::Reason::RestorePreimage;
    return {project, preimage};
  }
};

std::string contentHash(std::string_view value) {
  std::uint8_t digest[32]{};
  CHECK(core::opensslCrypto().sha256(asBytes(value), digest));
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output = "sha256:";
  for (const std::uint8_t byte : digest) {
    output.push_back(kHex[byte >> 4]);
    output.push_back(kHex[byte & 0x0f]);
  }
  return output;
}

CrowdyStudioController makeController(
    FakeProjectProvider& provider, FakeRuntime& runtime, FakeClock& clock,
    FakeSynchronization* synchronization = nullptr,
    FakeApprovalGate* approval = nullptr) {
  CrowdyStudioControllerOptions options;
  options.appId = "42";
  options.gridId = "500";
  options.autosaveMs = 10;
  options.retryMs = 10;
  options.compilePollMs = 0;
  options.sleep = [](std::int64_t) {};
  return CrowdyStudioController(
      std::move(options), provider, runtime, core::opensslCrypto(), clock,
      synchronization, approval);
}

void testControllerSaveConflictOfflineAndSwitch() {
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeClock clock;
  auto controller = makeController(provider, runtime, clock);
  controller.initialize();
  CHECK(controller.getState().project->projectId == "project-1");

  controller.updateFile(CrowdyStudioTarget::Server, "src/lib.rs",
                        "fn autosaved() {}");
  CHECK(controller.getState().saveState ==
        CrowdyStudioSaveState::Saving);
  clock.monotonic = 11;
  controller.tick();
  CHECK(controller.getState().saveState ==
        CrowdyStudioSaveState::Saved);
  CHECK_EQ(provider.saves.size(), std::size_t{1});

  provider.offline = true;
  controller.updateFile(CrowdyStudioTarget::Server, "src/lib.rs",
                        "fn offline() {}");
  CHECK(!controller.saveNow());
  CHECK(controller.getState().saveState ==
        CrowdyStudioSaveState::Offline);
  CHECK(controller.fileContent(
            {CrowdyStudioFileRef::Source::Project,
             CrowdyStudioTarget::Server, "src/lib.rs", std::nullopt}) ==
        "fn offline() {}");
  provider.offline = false;
  clock.monotonic += 11;
  controller.tick();
  CHECK(controller.getState().saveState ==
        CrowdyStudioSaveState::Saved);

  provider.conflictOnce = true;
  controller.updateFile(CrowdyStudioTarget::Server, "src/lib.rs",
                        "fn local() {}");
  CHECK(!controller.saveNow());
  CHECK(controller.getState().saveState ==
        CrowdyStudioSaveState::Conflict);
  CHECK(controller.fileContent(
            {CrowdyStudioFileRef::Source::Project,
             CrowdyStudioTarget::Server, "src/lib.rs", std::nullopt}) ==
        "fn local() {}");
  CHECK(controller.overwriteConflict());
  CHECK(controller.getState().saveState ==
        CrowdyStudioSaveState::Saved);

  controller.updateFile(CrowdyStudioTarget::Server, "src/lib.rs",
                        "fn before_switch() {}");
  const std::size_t savesBeforeSwitch = provider.saves.size();
  controller.switchProject("project-2");
  CHECK(provider.saves.size() == savesBeforeSwitch + 1);
  CHECK(controller.getState().project->projectId == "project-2");
  CHECK(controller.getState().runtimeSync.state ==
        CrowdyStudioRuntimeSyncState::NeverRun);
}

void testAtomicPatchAndRestoreApproval() {
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeClock clock;
  FakeSynchronization synchronization(provider);
  FakeApprovalGate approval;
  auto controller =
      makeController(provider, runtime, clock, &synchronization, &approval);
  controller.initialize();
  const std::string original =
      controller.fileContent({CrowdyStudioFileRef::Source::Project,
                              CrowdyStudioTarget::Server, "src/lib.rs",
                              std::nullopt});

  CrowdyStudioAtomicPatchInput invalid;
  invalid.expectedRevisionId =
      controller.getState().project->revision.id;
  invalid.changes = {
      {CrowdyStudioTarget::Server, "src/lib.rs",
       CrowdyStudioPatchOperation::Replace, "fn changed() {}",
       contentHash(original)},
      {CrowdyStudioTarget::Server, "src/lib.rs",
       CrowdyStudioPatchOperation::Replace, "fn second() {}",
       contentHash(original)},
  };
  bool rejected = false;
  try {
    (void)controller.applyAtomicPatch(invalid);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);
  CHECK_EQ(synchronization.patchCalls, 0);
  CHECK(controller.fileContent(
            {CrowdyStudioFileRef::Source::Project,
             CrowdyStudioTarget::Server, "src/lib.rs", std::nullopt}) ==
        original);

  CrowdyStudioAtomicPatchInput valid;
  valid.expectedRevisionId =
      controller.getState().project->revision.id;
  valid.changes = {
      {CrowdyStudioTarget::Server, "src/lib.rs",
       CrowdyStudioPatchOperation::Replace, "fn patched() {}",
       contentHash(original)},
      {CrowdyStudioTarget::Server, "src/new.rs",
       CrowdyStudioPatchOperation::Create, "pub fn new_file() {}",
       "ABSENT"},
  };
  const auto result = controller.applyAtomicPatch(valid);
  CHECK_EQ(synchronization.patchCalls, 1);
  CHECK(result.checkpoint.projectRevisionId ==
        valid.expectedRevisionId);
  CHECK(controller.getState().saveState ==
        CrowdyStudioSaveState::Saved);
  CHECK(controller.fileContent(
            {CrowdyStudioFileRef::Source::Project,
             CrowdyStudioTarget::Server, "src/new.rs", std::nullopt}) ==
        "pub fn new_file() {}");

  const std::string beforeRestore =
      controller.getState().project->revision.id;
  const auto checkpoint = controller.restoreCheckpoint(
      "checkpoint-1", "restore-grant", beforeRestore);
  CHECK_EQ(approval.restoreApprovals, 1);
  CHECK_EQ(synchronization.restoreCalls, 1);
  CHECK(checkpoint.projectRevisionId == beforeRestore);
  CHECK(controller.fileContent(
            {CrowdyStudioFileRef::Source::Project,
             CrowdyStudioTarget::Server, "src/lib.rs", std::nullopt}) ==
        "fn restored() {}");
}

void testDeploymentBindingsAndRuntimeState() {
  {
    FakeProjectProvider unapprovedProvider;
    FakeRuntime unapprovedRuntime;
    FakeClock unapprovedClock;
    auto unapproved = makeController(
        unapprovedProvider, unapprovedRuntime, unapprovedClock);
    unapproved.initialize();
    unapprovedRuntime.calls.clear();
    bool approvalRequired = false;
    try {
      (void)unapproved.deployLivePlan(
          unapproved.makeDeploymentPlan(), "caller-supplied-text");
    } catch (const std::runtime_error&) {
      approvalRequired = true;
    }
    CHECK(approvalRequired);
    CHECK(unapprovedRuntime.calls.empty());
  }

  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeClock clock;
  FakeApprovalGate approval;
  auto controller = makeController(provider, runtime, clock, nullptr, &approval);
  controller.initialize();
  runtime.calls.clear();

  const CrowdyStudioDeploymentPlan exact =
      controller.makeDeploymentPlan();
  CrowdyStudioDeploymentPlan wrongTargets = exact;
  wrongTargets.targets = {CrowdyStudioTarget::Server};
  bool targetMismatch = false;
  try {
    (void)controller.deployLivePlan(wrongTargets, "live-grant");
  } catch (const std::runtime_error&) {
    targetMismatch = true;
  }
  CHECK(targetMismatch);
  CHECK(runtime.calls.empty());

  CrowdyStudioDeploymentPlan wrongPairing = exact;
  wrongPairing.pairingPreference =
      CrowdyStudioPairingPreference::Optional;
  bool pairingMismatch = false;
  try {
    (void)controller.deployLivePlan(wrongPairing, "live-grant");
  } catch (const std::runtime_error&) {
    pairingMismatch = true;
  }
  CHECK(pairingMismatch);
  CHECK(runtime.calls.empty());

  CrowdyStudioDeploymentPlan wrongContent = exact;
  wrongContent.projectContentHash = "sha256:wrong";
  bool contentMismatch = false;
  try {
    (void)controller.deployLivePlan(wrongContent, "live-grant");
  } catch (const std::runtime_error&) {
    contentMismatch = true;
  }
  CHECK(contentMismatch);
  CHECK(runtime.calls.empty());

  const CrowdyStudioDeployResult deployed =
      controller.deployLive(exact, "live-grant");
  CHECK(deployed.status == CrowdyStudioDeployResult::Status::Running);
  CHECK_EQ(approval.liveApprovals, 1);
  CHECK(runtime.calls ==
        std::vector<std::string>({
            "deploy:CLIENT:LIVE",
            "poll:weather-client",
            "deploy:SERVER:LIVE",
            "poll:weather-server",
            "requires:weather-server:weather-client",
            "enabled:weather-server:true",
            "start:weather-client:client-v1",
        }));
  CHECK(controller.getState().runtimeSync.state ==
        CrowdyStudioRuntimeSyncState::RunningSaved);
  CHECK(controller.getState().runtimeSync.deployment ==
        std::optional<CrowdyStudioDeployment>{
            CrowdyStudioDeployment::Live});

  controller.updateFile(CrowdyStudioTarget::Server, "src/lib.rs",
                        "fn newer_saved_source() {}");
  CrowdyStudioSettingsPatch renamed;
  renamed.serverModuleName =
      CrowdyStudioPatchField<std::string>::value("new-server-name");
  controller.updateSettings(renamed);
  CHECK(controller.getState().runtimeSync.state ==
        CrowdyStudioRuntimeSyncState::RunningStale);
  const auto invoked = controller.invoke(
      "status", std::nullopt, CrowdyStudioDeployment::Live);
  CHECK(invoked.resultJson == std::optional<std::string>{R"({"ok":true})"});
  CHECK(runtime.calls.back() == "invoke:weather-server:status");

  bool environmentMismatch = false;
  try {
    (void)controller.invoke("status", std::nullopt,
                            CrowdyStudioDeployment::Draft);
  } catch (const std::runtime_error&) {
    environmentMismatch = true;
  }
  CHECK(environmentMismatch);

  const auto stopped = controller.stopProject();
  CHECK(stopped.serverStopped == std::optional<bool>{true});
  CHECK(stopped.clientStopped == std::optional<bool>{true});
  CHECK(runtime.calls.back() == "enabled:weather-server:false");
  CHECK(controller.getState().runtimeSync.state ==
        CrowdyStudioRuntimeSyncState::Stopped);
}

void testProjectScopeMismatchFailsClosed() {
  FakeProjectProvider provider;
  provider.projects[1].appId = "43";
  FakeRuntime runtime;
  FakeClock clock;
  auto controller = makeController(provider, runtime, clock);
  controller.initialize();
  bool mismatch = false;
  try {
    controller.switchProject("project-2");
  } catch (const std::runtime_error&) {
    mismatch = true;
  }
  CHECK(mismatch);
  CHECK(controller.getState().project->projectId == "project-1");
}

}  // namespace

int main() {
  testApiMappingAndInputSemantics();
  testControllerSaveConflictOfflineAndSwitch();
  testAtomicPatchAndRestoreApproval();
  testDeploymentBindingsAndRuntimeState();
  testProjectScopeMismatchFailsClosed();
  std::printf("crowdy_studio_test passed\n");
  return 0;
}
