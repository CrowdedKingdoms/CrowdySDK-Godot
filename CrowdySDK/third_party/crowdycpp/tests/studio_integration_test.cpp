#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/agent/schema.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/studio/integration.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::agent;
using namespace crowdy::player_host;
using namespace crowdy::studio;

namespace {

template <typename T>
concept HasGraphqlClient = requires(T& value) {
  value.graphqlClient();
};

template <typename T>
concept HasFilesystem = requires(T& value) {
  value.filesystem();
};

template <typename T>
concept HasShell = requires(T& value) {
  value.shell();
};

static_assert(!HasGraphqlClient<ICrowdyStudioEditorAdapter>);
static_assert(!HasGraphqlClient<CrowdyStudioControllerHostAdapter>);
static_assert(!HasGraphqlClient<CrowdyStudioIntegration>);
static_assert(!HasFilesystem<ICrowdyStudioEditorAdapter>);
static_assert(!HasFilesystem<CrowdyStudioControllerHostAdapter>);
static_assert(!HasShell<ICrowdyStudioEditorAdapter>);
static_assert(!HasShell<CrowdyStudioControllerHostAdapter>);

class FakeCrypto final : public core::ICrypto {
 public:
  bool hmacSha256(Bytes key, Bytes message,
                  std::uint8_t* out) const override {
    return hash(key, message, out);
  }

  bool sha256(Bytes message, std::uint8_t* out) const override {
    return hash({}, message, out);
  }

  bool constantTimeEquals(const std::uint8_t* left,
                          const std::uint8_t* right,
                          std::size_t size) const override {
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < size; ++index) {
      difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
  }

  bool randomBytes(std::uint8_t* out, std::size_t size) const override {
    for (std::size_t index = 0; index < size; ++index) {
      out[index] = static_cast<std::uint8_t>(index + 1);
    }
    return true;
  }

 private:
  static bool hash(Bytes first, Bytes second, std::uint8_t* out) {
    std::uint32_t value = 2'166'136'261U;
    for (const auto byte : first) {
      value = (value ^ byte) * 16'777'619U;
    }
    for (const auto byte : second) {
      value = (value ^ byte) * 16'777'619U;
    }
    for (std::size_t index = 0; index < 32; ++index) {
      value = value * 1'664'525U + 1'013'904'223U;
      out[index] =
          static_cast<std::uint8_t>((value >> ((index % 4) * 8)) & 0xffU);
    }
    return true;
  }
};

class FixtureCrypto final : public core::ICrypto {
 public:
  bool hmacSha256(Bytes key, Bytes message,
                  std::uint8_t* out) const override {
    std::string input(
        reinterpret_cast<const char*>(key.data()), key.size());
    input.append(
        reinterpret_cast<const char*>(message.data()), message.size());
    return digest(input, out);
  }

  bool sha256(Bytes message, std::uint8_t* out) const override {
    return digest(
        std::string_view(
            reinterpret_cast<const char*>(message.data()),
            message.size()),
        out);
  }

  bool constantTimeEquals(const std::uint8_t* left,
                          const std::uint8_t* right,
                          std::size_t size) const override {
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < size; ++index) {
      difference |=
          static_cast<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
  }

  bool randomBytes(std::uint8_t* out, std::size_t size) const override {
    for (std::size_t index = 0; index < size; ++index) {
      out[index] = static_cast<std::uint8_t>(index + 1);
    }
    return true;
  }

 private:
  static bool digest(std::string_view value, std::uint8_t* out) {
    const std::string encoded = agent::sha256Digest(value);
    for (std::size_t index = 0; index < 32; ++index) {
      const auto hex = encoded.substr(7 + index * 2, 2);
      out[index] = static_cast<std::uint8_t>(
          std::stoul(hex, nullptr, 16));
    }
    return true;
  }
};

class FakeClock final : public core::IClock {
 public:
  std::int64_t epoch = 1'784'894'400'000LL;
  std::int64_t monotonic = 0;
  std::int64_t epochMillis() const override { return epoch; }
  std::int64_t monotonicMillis() const override { return monotonic; }
};

CrowdyStudioProjectFile file(CrowdyStudioTarget target, std::string path,
                             std::string content) {
  CrowdyStudioProjectFile value;
  value.target = target;
  value.path = std::move(path);
  value.content = std::move(content);
  value.revision = "1";
  value.createdAt = "2026-07-24T00:00:00.000Z";
  value.updatedAt = value.createdAt;
  return value;
}

CrowdyStudioProject project(std::string id, std::string revision) {
  CrowdyStudioProject value;
  value.projectId = std::move(id);
  value.appId = "42";
  value.ownerUserId = "7";
  value.gridId = "500";
  value.kind = CrowdyStudioProjectKind::Server;
  value.metadata.name =
      value.projectId == "project-1" ? "Fixture Studio"
                                      : "Second project";
  if (value.projectId == "project-1") {
    value.metadata.description = "Cross-SDK projection fixture";
  }
  value.metadata.serverModuleName =
      value.projectId == "project-1" ? "fixture-server"
                                      : "second-server";
  value.files = {
      file(CrowdyStudioTarget::Server, "Cargo.toml",
           "[package]\nname = \"fixture\"\n"),
      file(CrowdyStudioTarget::Server, "src/lib.rs", "pub fn invoke() {}"),
  };
  value.sdkVersion = "0.1.5";
  value.revision = {std::move(revision), "2026-07-24T00:00:00.000Z"};
  value.fileCount = 2;
  value.totalBytes = "45";
  value.createdAt = "2026-07-24T00:00:00.000Z";
  value.updatedAt =
      value.projectId == "project-1"
          ? "2026-07-24T00:00:00.000Z"
          : "2026-07-24T00:00:01.000Z";
  return value;
}

CrowdyStudioProjectSummary summary(const CrowdyStudioProject& value) {
  return {
      value.projectId,
      value.gridId,
      value.metadata.name,
      value.kind,
      value.revision.id,
      value.metadata.serverModuleName,
      value.metadata.clientModuleName,
      value.archived,
      value.updatedAt,
  };
}

class FakeProjectProvider final : public ICrowdyStudioProjectProvider {
 public:
  explicit FakeProjectProvider(
      std::shared_ptr<std::vector<std::string>> eventLog = {})
      : events(std::move(eventLog)) {
    projects = {project("project-1", "1"), project("project-2", "2")};
  }
  ~FakeProjectProvider() override {
    if (events) events->push_back("provider-destroy");
  }

  std::shared_ptr<std::vector<std::string>> events;
  std::vector<CrowdyStudioProject> projects;
  int saves = 0;

  std::vector<CrowdyStudioProjectSummary> listProjects(
      const CrowdyStudioProjectScope&) override {
    std::vector<CrowdyStudioProjectSummary> values;
    for (const auto& value : projects) values.push_back(summary(value));
    return values;
  }

  CrowdyStudioProject getProject(
      const CrowdyStudioProjectScope&,
      std::string_view projectId) override {
    return get(projectId);
  }

  CrowdyStudioProject createProject(
      const CreateCrowdyStudioProjectInput&) override {
    return projects.front();
  }

  CrowdyStudioProject saveProject(
      const SaveCrowdyStudioProjectInput& input) override {
    auto& value = getMutable(input.projectId);
    CHECK(value.revision.id == input.expectedRevisionId);
    value.metadata = input.metadata;
    value.files = input.files;
    value.revision.id =
        std::to_string(std::stoll(value.revision.id) + 1);
    value.updatedAt = "2026-07-24T00:00:01.000Z";
    ++saves;
    return value;
  }

  std::vector<CrowdyStudioReferenceFile> listPersonalLibraryFiles(
      const CrowdyStudioProjectScope&) override {
    CrowdyStudioReferenceFile value;
    value.id = "library-1";
    value.source = CrowdyStudioReferenceSource::PersonalLibrary;
    value.appId = "42";
    value.title = "Library helper";
    value.target = CrowdyStudioTarget::Server;
    value.path = "src/library.rs";
    value.content = "pub fn library() {}";
    value.revision = "1";
    return {value};
  }

  CrowdyStudioReferenceFile savePersonalLibraryFile(
      const SaveCrowdyStudioLibraryFileInput&) override {
    return listPersonalLibraryFiles({}).front();
  }

  std::vector<CrowdyStudioReferenceFile> listCommonFiles(
      const CrowdyStudioProjectScope&) override {
    CrowdyStudioReferenceFile value;
    value.id = "common-1";
    value.source = CrowdyStudioReferenceSource::Common;
    value.appId = "42";
    value.title = "Common helper";
    value.target = CrowdyStudioTarget::Server;
    value.path = "src/common.rs";
    value.content = "pub fn common() {}";
    value.revision = "1";
    return {value};
  }

  CrowdyStudioProject importReferenceFile(
      const ImportCrowdyStudioReferenceFileInput&) override {
    return projects.front();
  }

 private:
  CrowdyStudioProject get(std::string_view id) const {
    const auto found = std::find_if(
        projects.begin(), projects.end(), [&](const auto& value) {
          return value.projectId == id;
        });
    CHECK(found != projects.end());
    return *found;
  }

  CrowdyStudioProject& getMutable(std::string_view id) {
    const auto found = std::find_if(
        projects.begin(), projects.end(), [&](const auto& value) {
          return value.projectId == id;
        });
    CHECK(found != projects.end());
    return *found;
  }
};

class FakeRuntime final : public ICrowdyStudioRuntime {
 public:
  explicit FakeRuntime(
      std::shared_ptr<std::vector<std::string>> eventLog = {})
      : events(std::move(eventLog)) {}
  ~FakeRuntime() override {
    if (events) events->push_back("runtime-destroy");
  }

  std::shared_ptr<std::vector<std::string>> events;
  std::vector<std::string> calls;
  std::function<void()> onVersions;
  bool failEnable = false;
  bool failInvoke = false;

  CrowdyStudioDeploySubmission deploy(
      const CrowdyStudioDeployTargetInput& input) override {
    calls.push_back(
        input.deployment == CrowdyStudioDeployment::Draft
            ? "deploy-draft"
            : "deploy-live");
    return {input.deployment == CrowdyStudioDeployment::Draft
                ? "draft-v1"
                : "live-v1"};
  }

  std::vector<CrowdyStudioRuntimeVersion> versions(
      const CrowdyStudioProjectScope&,
      std::string_view) override {
    calls.push_back("versions");
    if (onVersions) {
      auto callback = std::move(onVersions);
      onVersions = {};
      callback();
    }
    const bool live =
        std::find(calls.begin(), calls.end(), "deploy-live") != calls.end();
    return {{live ? "live-v1" : "draft-v1", "succeeded", std::nullopt}};
  }

  void setEnabled(const CrowdyStudioProjectScope&,
                  std::string_view, bool enabled) override {
    calls.push_back(enabled ? "enable" : "disable");
    if (failEnable && enabled) {
      throw std::runtime_error(
          "server enable response was lost");
    }
  }

  void setRequires(
      const CrowdyStudioProjectScope&, std::string_view,
      const std::optional<std::string>&) override {
    calls.push_back("requires");
  }

  void startClient(const CrowdyStudioProjectScope&, std::string_view,
                   std::string_view) override {
    calls.push_back("start-client");
  }

  void stopClient() override {
    calls.push_back("stop-client");
    if (events) events->push_back("runtime-stop");
  }

  CrowdyStudioInvokeResult invoke(
      const CrowdyStudioProjectScope&, std::string_view,
      std::string_view, const std::optional<std::string>& params) override {
    calls.push_back("invoke:" + params.value_or(""));
    if (failInvoke) {
      throw std::runtime_error(
          "runtime invoke response was lost");
    }
    return {std::nullopt, R"({"ok":true})", "4", 2};
  }
};

class FakeApproval final : public ICrowdyStudioApprovalGate {
 public:
  int live = 0;
  int restores = 0;
  bool failLive = false;
  bool failRestore = false;
  void requireLiveApproval(
      const CrowdyStudioLiveApprovalRequest& request,
      std::string_view grant) override {
    CHECK(request.projectContentHash.rfind("sha256:", 0) == 0);
    CHECK(grant == "approved");
    if (failLive) {
      throw std::runtime_error(
          "approval provider validation failed");
    }
    ++live;
  }
  void requireRestoreApproval(
      const CrowdyStudioRestoreApprovalRequest& request,
      std::string_view grant) override {
    CHECK(!request.checkpointId.empty());
    CHECK(grant == "approved");
    if (failRestore) {
      throw std::runtime_error(
          "restore approval provider validation failed");
    }
    ++restores;
  }
};

class FakeSynchronization final
    : public ICrowdyStudioSynchronizationProvider {
 public:
  CrowdyStudioAtomicPatchResult applyAtomicPatch(
      const CrowdyStudioProjectScope&, std::string_view,
      const CrowdyStudioAtomicPatchInput&) override {
    return {};
  }
  std::vector<CrowdyStudioCheckpointMetadata> listCheckpoints(
      const CrowdyStudioProjectScope&, std::string_view) override {
    return {};
  }
  CrowdyStudioCheckpointRestoreResult restoreCheckpoint(
      const CrowdyStudioCheckpointRestoreInput&) override {
    ++restores;
    throw std::runtime_error(
        "restore response was lost");
  }

  int restores = 0;
};

class SuccessfulSynchronization final
    : public ICrowdyStudioSynchronizationProvider {
 public:
  explicit SuccessfulSynchronization(FakeProjectProvider& provider)
      : provider_(provider) {}

  CrowdyStudioAtomicPatchResult applyAtomicPatch(
      const CrowdyStudioProjectScope&, std::string_view,
      const CrowdyStudioAtomicPatchInput&) override {
    throw std::runtime_error("atomic patch is outside this restore test");
  }

  std::vector<CrowdyStudioCheckpointMetadata> listCheckpoints(
      const CrowdyStudioProjectScope&, std::string_view) override {
    return {{
        .checkpointId = "checkpoint-1",
        .projectRevisionId = provider_.projects.front().revision.id,
        .contentHash =
            "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .reason = CrowdyStudioCheckpointMetadata::Reason::Manual,
        .files = {},
        .createdAt = "2026-07-24T00:00:00.000Z",
        .restoredAt = std::nullopt,
    }};
  }

  CrowdyStudioCheckpointRestoreResult restoreCheckpoint(
      const CrowdyStudioCheckpointRestoreInput& input) override {
    CHECK(input.checkpointId == "checkpoint-1");
    CHECK(input.approvalGrant == "approved");
    auto restored = provider_.projects.front();
    CHECK(restored.revision.id == input.expectedRevisionId);
    CrowdyStudioCheckpointMetadata preimage;
    preimage.checkpointId = "pre-restore-1";
    preimage.projectRevisionId = restored.revision.id;
    preimage.contentHash =
        "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    preimage.reason =
        CrowdyStudioCheckpointMetadata::Reason::RestorePreimage;
    preimage.createdAt = "2026-07-24T00:00:01.000Z";
    restored.revision.id =
        std::to_string(std::stoll(restored.revision.id) + 1);
    restored.updatedAt = "2026-07-24T00:00:02.000Z";
    provider_.projects.front() = restored;
    ++restores;
    return {std::move(restored), std::move(preimage)};
  }

  int restores = 0;

 private:
  FakeProjectProvider& provider_;
};

class FakeWallet final : public ICrowdyStudioWalletProvider {
 public:
  CrowdyStudioWalletSnapshot balance() override {
    ++reads;
    if (fail) throw std::runtime_error("wallet unavailable");
    return {"1234", "USD"};
  }

  int reads = 0;
  bool fail = false;
};

class FakeEditor final : public ICrowdyStudioEditorAdapter {
 public:
  explicit FakeEditor(
      std::shared_ptr<std::vector<std::string>> eventLog = {})
      : events(std::move(eventLog)) {}
  ~FakeEditor() override {
    if (events) events->push_back("editor-destroy");
  }

  std::shared_ptr<std::vector<std::string>> events;
  CrowdyStudioEditorCallbacks callbacks;
  std::vector<CrowdyStudioEditorSnapshot> snapshots;
  int relayouts = 0;
  bool disposed = false;

  CrowdyStudioEditorMode mode() const noexcept override {
    return CrowdyStudioEditorMode::Native;
  }
  void setCallbacks(CrowdyStudioEditorCallbacks value) override {
    callbacks = std::move(value);
  }
  void synchronize(
      const CrowdyStudioEditorSnapshot& snapshot) override {
    snapshots.push_back(snapshot);
  }
  void relayout() override { ++relayouts; }
  void dispose() noexcept override {
    disposed = true;
    if (events) events->push_back("editor-dispose");
  }
};

class FakeClientRuntime final : public ICrowdyStudioClientRuntime {
 public:
  explicit FakeClientRuntime(
      std::shared_ptr<std::vector<std::string>> eventLog)
      : events(std::move(eventLog)) {}
  ~FakeClientRuntime() override {
    events->push_back("client-runtime-destroy");
  }
  void start(const CrowdyStudioClientArtifact&) override {}
  void stop() override {}
  std::shared_ptr<std::vector<std::string>> events;
};

class FakePlayerHost final : public PlayerHostAdapterV1 {
 public:
  explicit FakePlayerHost(
      std::shared_ptr<std::vector<std::string>> eventLog = {})
      : events(std::move(eventLog)) {}

  void capabilities(CancellationTokenV1,
                    CapabilitiesCallbackV1 callback) override {
    PlayerHostCapabilitiesV1 value;
    value.game_id = "game";
    value.revision = "capability-1";
    value.controlled_entity_id = "player-1";
    value.advertised_at = "2026-07-24T00:00:00.000Z";
    callback(AdapterResultV1<PlayerHostCapabilitiesV1>::success(
        std::move(value)));
  }
  void observe(const ObserveRequestV1&, CancellationTokenV1,
               ObservationCallbackV1 callback) override {
    callback(AdapterResultV1<GameObservationV1>::failure(
        {.code = "AGENT_HOST_UNAVAILABLE",
         .message = "not used",
         .retryable = false,
         .remediation = std::nullopt,
         .field = std::nullopt,
         .required_scope = std::nullopt}));
  }
  void dispatch(const GameCommandV1&, const ValidatedGateV1&,
                CancellationTokenV1,
                CommandCallbackV1 callback) override {
    callback(AdapterResultV1<GameCommandResultV1>::failure(
        {.code = "AGENT_HOST_UNAVAILABLE",
         .message = "not used",
         .retryable = false,
         .remediation = std::nullopt,
         .field = std::nullopt,
         .required_scope = std::nullopt}));
  }
  void clearAgentIntent(PreemptionReasonV1) noexcept override {
    ++clears;
    if (events) events->push_back("host-clear");
  }
  std::shared_ptr<std::vector<std::string>> events;
  int clears = 0;
};

class FakeHttpTransport final : public graphql::IHttpTransport {
 public:
  graphql::HttpResponse send(
      const graphql::HttpRequest&) override {
    return {503, R"({"errors":[{"message":"offline"}]})"};
  }
};

CrowdyStudioControllerOptions controllerOptions(FakeClock& clock) {
  CrowdyStudioControllerOptions options;
  options.appId = "42";
  options.gridId = "500";
  options.initialProjectId = "project-1";
  options.autosaveMs = 5;
  options.compilePollMs = 0;
  options.compilePollLimit = 2;
  options.sleep = [](std::int64_t) {};
  (void)clock;
  return options;
}

std::string iso(std::int64_t epochMs) {
  const std::time_t seconds =
      static_cast<std::time_t>(epochMs / 1'000);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer),
                "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                utc.tm_hour, utc.tm_min, utc.tm_sec,
                static_cast<long long>(epochMs % 1'000));
  return buffer;
}

const NativeLocalToolContractV1& localContract(
    std::string_view name) {
  const auto contracts = nativeLocalToolContractsV1();
  const auto found = std::find_if(
      contracts.begin(), contracts.end(),
      [&](const auto& value) { return value.name == name; });
  CHECK(found != contracts.end());
  return *found;
}

NativeToolInvocationV1 nativeInvocation(
    std::string name, NativeToolArgumentsV1 arguments,
    const FakeClock& clock, std::string id) {
  const auto& descriptor = localContract(name);
  NativeToolInvocationV1 value;
  value.session_id = "session-1";
  value.run_id = "run-1";
  value.tool_call_id = std::move(id);
  value.name = std::move(name);
  value.version = std::string(descriptor.version);
  value.descriptor_digest =
      std::string(descriptor.descriptor_digest);
  value.arguments = std::move(arguments);
  value.argument_hash =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  value.context_version = "context-1";
  value.client_epoch = "1";
  value.deadline = iso(clock.epoch + 10);
  return value;
}

ValidatedStudioGateV1 gate(
    std::string context = "context-1",
    std::optional<std::string> approval = std::nullopt) {
  return {
      .session_id = "session-1",
      .run_id = "run-1",
      .tool_call_id = "tool-1",
      .client_epoch = "1",
      .context_version = std::move(context),
      .lease_id = "workspace-lease",
      .approval_grant = std::move(approval),
      .validated_at = "2026-07-24T00:00:00.000Z",
  };
}

AdapterResultV1<StudioNativeToolOutputV1> dispatch(
    CrowdyStudioControllerHostAdapter& host,
    StudioNativeToolKindV1 kind,
    StudioNativeToolRequestV1 request,
    ValidatedStudioGateV1 validated = gate(),
    CancellationTokenV1 cancellation = {}) {
  CancellationSourceV1 live;
  if (cancellation.cancelled()) cancellation = live.token();
  std::optional<AdapterResultV1<StudioNativeToolOutputV1>> result;
  host.dispatch(
      kind, request, validated, cancellation,
      [&](auto value) { result = std::move(value); });
  CHECK(result.has_value());
  return std::move(*result);
}

CrowdyStudioControllerHostAdapterOptions hostOptions() {
  CrowdyStudioControllerHostAdapterOptions options;
  options.sessionId =
      [] { return std::optional<std::string>("session-1"); };
  options.clientEpoch =
      [] { return std::optional<std::string>("1"); };
  options.contextVersion = [] { return std::string("context-1"); };
  options.leaseKinds = [] {
    return std::vector<LeaseKindV1>{LeaseKindV1::Workspace,
                                    LeaseKindV1::Play};
  };
  options.hostCapabilityRevision =
      [] { return std::optional<std::string>("capability-1"); };
  options.isLeaseActive = [](std::string_view id, LeaseKindV1 kind) {
    return (kind == LeaseKindV1::Workspace &&
            id == "workspace-lease") ||
           (kind == LeaseKindV1::Play && id == "play-lease");
  };
  options.validateApprovalGrant =
      [](StudioNativeToolKindV1,
         const StudioNativeToolRequestV1&,
         const ValidatedStudioGateV1& value) {
        return value.approval_grant == "approved";
      };
  return options;
}

std::string studioFixtureText(std::string_view name) {
  const std::string path =
      std::string(CROWDY_PARITY_FIXTURE_DIR) + "/" + std::string(name);
  std::ifstream input(path, std::ios::binary);
  CHECK(input.good());
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string_view fixtureTargetName(StudioTargetV1 value) {
  return value == StudioTargetV1::Server ? "SERVER" : "CLIENT";
}

std::string_view fixturePairingName(
    StudioPairingPreferenceV1 value) {
  switch (value) {
    case StudioPairingPreferenceV1::None: return "NONE";
    case StudioPairingPreferenceV1::Optional: return "OPTIONAL";
    case StudioPairingPreferenceV1::Required: return "REQUIRED";
  }
  return "";
}

std::string_view fixtureSourceName(StudioFileSourceV1 value) {
  switch (value) {
    case StudioFileSourceV1::Project: return "PROJECT";
    case StudioFileSourceV1::PersonalLibrary:
      return "PERSONAL_LIBRARY";
    case StudioFileSourceV1::Common: return "COMMON";
  }
  return "";
}

std::string_view fixtureSaveStateName(StudioSaveStateV1 value) {
  switch (value) {
    case StudioSaveStateV1::Saving: return "SAVING";
    case StudioSaveStateV1::Saved: return "SAVED";
    case StudioSaveStateV1::Conflict: return "CONFLICT";
    case StudioSaveStateV1::Offline: return "OFFLINE";
  }
  return "";
}

std::string_view fixtureProjectKindName(StudioProjectKindV1 value) {
  switch (value) {
    case StudioProjectKindV1::Server: return "SERVER";
    case StudioProjectKindV1::Client: return "CLIENT";
    case StudioProjectKindV1::FullStack: return "FULL_STACK";
  }
  return "";
}

std::string_view fixtureRuntimePhaseName(
    StudioRuntimePhaseV1 value) {
  switch (value) {
    case StudioRuntimePhaseV1::Idle: return "IDLE";
    case StudioRuntimePhaseV1::TestingDraft: return "TESTING_DRAFT";
    case StudioRuntimePhaseV1::DeployingLive:
      return "DEPLOYING_LIVE";
    case StudioRuntimePhaseV1::Compiling: return "COMPILING";
    case StudioRuntimePhaseV1::Enabling: return "ENABLING";
    case StudioRuntimePhaseV1::Running: return "RUNNING";
    case StudioRuntimePhaseV1::CompileFailed:
      return "COMPILE_FAILED";
    case StudioRuntimePhaseV1::Stopping: return "STOPPING";
    case StudioRuntimePhaseV1::Stopped: return "STOPPED";
    case StudioRuntimePhaseV1::PartialFailure:
      return "PARTIAL_FAILURE";
    case StudioRuntimePhaseV1::Error: return "ERROR";
  }
  return "";
}

std::string_view fixtureRuntimeSyncName(StudioRuntimeSyncV1 value) {
  switch (value) {
    case StudioRuntimeSyncV1::NeverRun: return "NEVER_RUN";
    case StudioRuntimeSyncV1::RunningSaved:
      return "RUNNING_SAVED";
    case StudioRuntimeSyncV1::RunningStale:
      return "RUNNING_STALE";
    case StudioRuntimeSyncV1::Stopped: return "STOPPED";
  }
  return "";
}

graphql::JVal fixtureRuntimeJson(
    const StudioRuntimeStatusV1& value) {
  graphql::JVal result;
  result["phase"] = fixtureRuntimePhaseName(value.phase);
  result["savedRevision"] = value.saved_revision;
  if (value.running_revision) {
    result["runningRevision"] = *value.running_revision;
  }
  result["sync"] = fixtureRuntimeSyncName(value.sync);
  if (value.target) result["target"] = fixtureTargetName(*value.target);
  if (value.draft) result["draft"] = *value.draft;
  if (value.message) result["message"] = *value.message;
  return result;
}

graphql::JVal fixtureProjectJson(
    const StudioProjectProjectionV1& value) {
  graphql::JVal result;
  result["projectId"] = value.project_id;
  result["name"] = value.name;
  if (value.description) result["description"] = *value.description;
  result["kind"] = fixtureProjectKindName(value.kind);
  result["revision"] = value.revision;
  graphql::JArray files;
  for (const auto& fileValue : value.files) {
    graphql::JVal item;
    item["target"] = fixtureTargetName(fileValue.target);
    item["path"] = fileValue.path;
    item["contentHash"] = fileValue.content_hash;
    item["byteLength"] =
        static_cast<std::int64_t>(fileValue.byte_length);
    files.emplace_back(std::move(item));
  }
  result["files"] = std::move(files);
  if (value.server_module_name) {
    result["serverModuleName"] = *value.server_module_name;
  }
  if (value.client_module_name) {
    result["clientModuleName"] = *value.client_module_name;
  }
  result["pairingPreference"] =
      fixturePairingName(value.pairing_preference);
  result["updatedAt"] = value.updated_at;
  return result;
}

graphql::JVal fixtureStudioOutputJson(
    std::string_view name, const NativeToolOutputV1& output) {
  if (name == "studio.context.get") {
    const auto& value = std::get<StudioContextV1>(output);
    graphql::JVal result;
    result["appRef"] = value.app_ref;
    if (value.project_ref) result["projectRef"] = *value.project_ref;
    result["gridRef"] = value.grid_ref;
    result["contextVersion"] = value.context_version;
    result["saveState"] = fixtureSaveStateName(value.save_state);
    result["runtime"] = fixtureRuntimeJson(value.runtime);
    if (value.client_epoch) result["clientEpoch"] = *value.client_epoch;
    graphql::JArray kinds;
    for (const auto kind : value.lease_kinds) {
      kinds.emplace_back(
          kind == LeaseKindV1::Workspace ? "WORKSPACE" : "PLAY");
    }
    result["leaseKinds"] = std::move(kinds);
    if (value.host_capability_revision) {
      result["hostCapabilityRevision"] =
          *value.host_capability_revision;
    }
    return result;
  }
  if (name == "studio.state.get") {
    const auto& value = std::get<StudioStateV1>(output);
    graphql::JVal result;
    if (value.project) {
      result["project"] = fixtureProjectJson(*value.project);
    }
    graphql::JArray files;
    for (const auto& fileValue : value.open_files) {
      graphql::JVal item;
      item["source"] = fixtureSourceName(fileValue.source);
      item["target"] = fixtureTargetName(fileValue.target);
      item["path"] = fileValue.path;
      files.emplace_back(std::move(item));
    }
    result["openFiles"] = std::move(files);
    result["saveState"] = fixtureSaveStateName(value.save_state);
    result["runtime"] = fixtureRuntimeJson(value.runtime);
    return result;
  }
  if (name == "project.select") {
    const auto& value =
        std::get<StudioProjectSelectResultV1>(output);
    return graphql::JVal::object(
        {{"selectedProjectRef", value.selected_project_ref},
         {"revision", value.revision}});
  }
  if (name == "workspace.tab.open" ||
      name == "workspace.tab.close") {
    return graphql::JVal::object(
        {{"ok", std::get<StudioOkV1>(output).ok}});
  }
  if (name == "diagnostics.local.get") {
    const auto& value = std::get<StudioDiagnosticsV1>(output);
    graphql::JVal result;
    graphql::JArray diagnostics;
    for (const auto& diagnostic : value.diagnostics) {
      graphql::JVal item;
      item["source"] =
          diagnostic.source == StudioDiagnosticSourceV1::LocalAdvisory
              ? "LOCAL_ADVISORY"
              : diagnostic.source == StudioDiagnosticSourceV1::Rustc
                    ? "RUSTC"
                    : "RUNTIME";
      item["target"] = fixtureTargetName(diagnostic.target);
      item["path"] = diagnostic.path;
      item["line"] = static_cast<std::int64_t>(diagnostic.line);
      item["column"] = static_cast<std::int64_t>(diagnostic.column);
      switch (diagnostic.severity) {
        case StudioDiagnosticSeverityV1::Error:
          item["severity"] = "ERROR";
          break;
        case StudioDiagnosticSeverityV1::Warning:
          item["severity"] = "WARNING";
          break;
        case StudioDiagnosticSeverityV1::Info:
          item["severity"] = "INFO";
          break;
        case StudioDiagnosticSeverityV1::Hint:
          item["severity"] = "HINT";
          break;
      }
      if (diagnostic.code) item["code"] = *diagnostic.code;
      item["message"] = diagnostic.message;
      diagnostics.emplace_back(std::move(item));
    }
    result["diagnostics"] = std::move(diagnostics);
    return result;
  }
  if (name == "runtime.status.get") {
    return fixtureRuntimeJson(
        std::get<StudioRuntimeStatusV1>(output));
  }
  if (name == "runtime.test_draft" ||
      name == "runtime.deploy_live") {
    const auto& value =
        std::get<StudioRuntimePlanResultV1>(output);
    graphql::JVal result;
    result["runtime"] = fixtureRuntimeJson(value.runtime);
    graphql::JArray targets;
    for (const auto targetValue : value.targets) {
      targets.emplace_back(fixtureTargetName(targetValue));
    }
    result[name == "runtime.test_draft" ? "compiledTargets"
                                         : "deployedTargets"] =
        std::move(targets);
    return result;
  }
  if (name == "runtime.invoke") {
    const auto& value =
        std::get<StudioRuntimeInvokeResultV1>(output);
    graphql::JVal result;
    result["resultType"] =
        value.result_type == StudioRuntimeResultTypeV1::Empty
            ? "EMPTY"
            : value.result_type == StudioRuntimeResultTypeV1::Text
                  ? "TEXT"
                  : "BASE64";
    result["result"] = value.result;
    result["fuelUsed"] = value.fuel_used;
    result["durationUs"] =
        static_cast<std::int64_t>(value.duration_us);
    return result;
  }
  const auto& value =
      std::get<StudioRuntimeStopResultV1>(output);
  graphql::JVal result;
  result["serverStopped"] = value.server_stopped;
  result["clientStopped"] = value.client_stopped;
  graphql::JArray failures;
  for (const auto& failure : value.failures) {
    failures.emplace_back(failure);
  }
  result["failures"] = std::move(failures);
  return result;
}

StudioTargetV1 fixtureTarget(std::string_view value) {
  CHECK(value == "SERVER" || value == "CLIENT");
  return value == "SERVER" ? StudioTargetV1::Server
                           : StudioTargetV1::Client;
}

std::vector<StudioTargetV1> fixtureTargets(
    const graphql::Json& values) {
  std::vector<StudioTargetV1> result;
  values.forEach([&](const graphql::Json& value) {
    result.push_back(fixtureTarget(value.asStringView()));
  });
  return result;
}

NativeToolArgumentsV1 fixtureArguments(
    std::string_view name, const graphql::Json& input) {
  if (name == "studio.context.get" ||
      name == "studio.state.get" ||
      name == "diagnostics.local.get" ||
      name == "runtime.status.get" || name == "runtime.stop") {
    return NoArgumentsV1{};
  }
  if (name == "project.select") {
    return StudioProjectSelectRequestV1{
        .project_ref = input["projectRef"].asString()};
  }
  if (name == "workspace.tab.open" ||
      name == "workspace.tab.close") {
    StudioFileTabRequestV1 request;
    const auto source = input["source"].asStringView();
    request.source =
        source == "PROJECT"
            ? StudioFileSourceV1::Project
            : source == "PERSONAL_LIBRARY"
                  ? StudioFileSourceV1::PersonalLibrary
                  : StudioFileSourceV1::Common;
    request.target = fixtureTarget(input["target"].asStringView());
    request.path = input["path"].asString();
    if (input["referenceRef"].ok()) {
      request.reference_ref = input["referenceRef"].asString();
    }
    return request;
  }
  if (name == "runtime.test_draft") {
    return StudioRuntimeTestDraftRequestV1{
        .expected_revision =
            input["expectedRevision"].asString(),
        .targets = fixtureTargets(input["targets"])};
  }
  if (name == "runtime.deploy_live") {
    const auto pairing = input["pairingPreference"].asStringView();
    return StudioRuntimeDeployLiveRequestV1{
        .expected_revision =
            input["expectedRevision"].asString(),
        .project_content_hash =
            input["projectContentHash"].asString(),
        .targets = fixtureTargets(input["targets"]),
        .pairing_preference =
            pairing == "NONE"
                ? StudioPairingPreferenceV1::None
                : pairing == "OPTIONAL"
                      ? StudioPairingPreferenceV1::Optional
                      : StudioPairingPreferenceV1::Required,
        .draft = input["draft"].asBool()};
  }
  CHECK(name == "runtime.invoke");
  StudioRuntimeInvokeRequestV1 request;
  request.export_name = input["exportName"].asString();
  request.environment =
      input["environment"].asStringView() == "DRAFT"
          ? StudioRuntimeEnvironmentV1::Draft
          : StudioRuntimeEnvironmentV1::Live;
  input["params"].forEach([&](const graphql::Json& parameter) {
    const auto type = parameter["type"].asStringView();
    request.params.push_back({
        .name = parameter["name"].asString(),
        .type = type == "STRING"
                    ? StudioRuntimeParameterTypeV1::String
                    : type == "DECIMAL"
                          ? StudioRuntimeParameterTypeV1::Decimal
                          : StudioRuntimeParameterTypeV1::Boolean,
        .value = parameter["value"].asString(),
    });
  });
  return request;
}

const NativeLocalToolContractV1& fixtureContract(
    std::string_view name) {
  for (const auto& contract : nativeLocalToolContractsV1()) {
    if (contract.name == name) return contract;
  }
  CHECK(false);
  return nativeLocalToolContractsV1().front();
}

NativeToolInvocationV1 fixtureInvocation(
    const graphql::Json& operation, std::size_t sequence) {
  const std::string name = operation["tool"].asString();
  const auto& contract = fixtureContract(name);
  NativeToolInvocationV1 invocation;
  invocation.session_id = "session-1";
  invocation.run_id = "run-1";
  invocation.tool_call_id =
      "fixture-" + std::to_string(sequence);
  invocation.name = name;
  invocation.descriptor_digest =
      std::string(contract.descriptor_digest);
  invocation.arguments =
      fixtureArguments(name, operation["input"]);
  invocation.argument_hash =
      "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  invocation.context_version = "context-1";
  invocation.client_epoch = "1";
  if (operation["leaseId"].ok()) {
    invocation.lease_id = operation["leaseId"].asString();
  }
  if (operation["approvalGrant"].ok()) {
    invocation.approval_grant =
        operation["approvalGrant"].asString();
  }
  invocation.deadline =
      operation["deadlineMs"].asInt64() == 1
          ? "2026-07-24T12:00:00.001Z"
          : "2026-07-24T12:02:00.000Z";
  return invocation;
}

NativeToolDispatcherOptionsV1 fixtureDispatcherOptions(
    const FakeClock& clock) {
  NativeToolDispatcherOptionsV1 options;
  options.clock = &clock;
  options.session_id =
      [] { return std::optional<std::string>("session-1"); };
  options.client_epoch =
      [] { return std::optional<std::string>("1"); };
  options.context_version = [] { return std::string("context-1"); };
  options.mode = [] { return NativeAgentModeV1::Build; };
  options.is_lease_active =
      [](std::string_view id, LeaseKindV1 kind) {
        return (kind == LeaseKindV1::Workspace &&
                id == "workspace-lease") ||
               (kind == LeaseKindV1::Play &&
                id == "play-lease");
      };
  options.validate_approval_grant =
      [](const NativeToolInvocationV1& invocation) {
        return invocation.approval_grant == "approved";
      };
  return options;
}

std::vector<CrowdyStudioEditorDiagnostic>
fixtureLocalDiagnostics(bool enabled) {
  if (!enabled) return {};
  return {{
      .target = CrowdyStudioTarget::Server,
      .path = "src/lib.rs",
      .line = 3,
      .column = 5,
      .endLine = std::nullopt,
      .endColumn = std::nullopt,
      .severity = CrowdyStudioEditorDiagnosticSeverity::Warning,
      .message = "unused value",
      .code = "unused",
      .source = CrowdyStudioEditorDiagnosticSource::LocalAdvisory,
  }};
}

CrowdyStudioControllerHostAdapterOptions fixtureHostOptions() {
  return hostOptions();
}

struct FixtureStudioHarness {
  FakeClock clock;
  FixtureCrypto crypto;
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeApproval approval;
  std::vector<CrowdyStudioEditorDiagnostic> diagnostics;
  FakePlayerHost playerHost;
  AgentControlLeaseManager leases;
  CrowdyStudioController controller;
  CrowdyStudioControllerHostAdapter host;
  NativeToolDispatcherV1 dispatcher;
  std::size_t sequence = 0;

  explicit FixtureStudioHarness(bool withDiagnostics)
      : diagnostics(fixtureLocalDiagnostics(withDiagnostics)),
        leases(playerHost),
        controller(controllerOptions(clock), provider, runtime, crypto,
                   clock, nullptr, &approval),
        host(controller, crypto, fixtureHostOptions()),
        dispatcher(leases, &host,
                   fixtureDispatcherOptions(clock)) {
    controller.initialize();
    controller.setLocalDiagnostics(diagnostics);
  }

  NativeToolResultV1 run(const graphql::Json& operation) {
    std::optional<NativeToolResultV1> result;
    dispatcher.dispatch(
        fixtureInvocation(operation, ++sequence),
        [&](NativeToolResultV1 value) {
          result = std::move(value);
        });
    CHECK(result.has_value());
    return std::move(*result);
  }
};

std::string_view fixtureStatusName(NativeToolResultStatusV1 status) {
  switch (status) {
    case NativeToolResultStatusV1::Succeeded: return "SUCCEEDED";
    case NativeToolResultStatusV1::Failed: return "FAILED";
    case NativeToolResultStatusV1::Cancelled: return "CANCELLED";
    case NativeToolResultStatusV1::TimedOut: return "TIMED_OUT";
    case NativeToolResultStatusV1::OutcomeUnknown:
      return "OUTCOME_UNKNOWN";
  }
  return "";
}

void checkFixtureResult(const NativeToolResultV1& actual,
                        const graphql::Json& fixtureCase) {
  const auto expected = fixtureCase["expected"];
  if (fixtureStatusName(actual.status) != expected["status"].asStringView()) {
    std::fprintf(stderr,
                 "Studio fixture status mismatch for %s: actual=%.*s "
                 "expected=%s error=%s\n",
                 fixtureCase["name"].asString().c_str(),
                 static_cast<int>(fixtureStatusName(actual.status).size()),
                 fixtureStatusName(actual.status).data(),
                 expected["status"].asString().c_str(),
                 actual.error ? actual.error->code.c_str() : "");
  }
  CHECK(fixtureStatusName(actual.status) ==
        expected["status"].asStringView());
  if (expected["errorCode"].ok()) {
    CHECK(actual.error.has_value());
    CHECK(actual.error->code ==
          expected["errorCode"].asString());
  } else {
    CHECK(!actual.error);
  }
  if (expected["output"].ok()) {
    CHECK(actual.output.has_value());
    const auto projected = graphql::Json::parse(
        fixtureStudioOutputJson(
            fixtureCase["tool"].asStringView(), *actual.output)
            .dump());
    CHECK(projected.ok());
    const std::string actualJson = agent::canonicalJson(projected);
    const std::string expectedJson =
        agent::canonicalJson(expected["output"]);
    if (actualJson != expectedJson) {
      std::fprintf(
          stderr,
          "Studio fixture mismatch for %s\nactual: %s\nexpected: %s\n",
          fixtureCase["name"].asString().c_str(), actualJson.c_str(),
          expectedJson.c_str());
    }
    CHECK(actualJson == expectedJson);
  } else {
    CHECK(!actual.output);
  }
}

class HoldingStudioHost final : public CrowdyStudioHostAdapter {
 public:
  explicit HoldingStudioHost(bool ambiguous = false)
      : outcomeUnknown(ambiguous) {}

  void dispatch(StudioNativeToolKindV1,
                const StudioNativeToolRequestV1&,
                const ValidatedStudioGateV1&,
                CancellationTokenV1,
                StudioToolCallbackV1 callback) override {
    if (outcomeUnknown) {
      AgentErrorV1 error;
      error.code = "AGENT_TOOL_OUTCOME_UNKNOWN";
      error.message = "effectful Studio outcome could not be confirmed";
      callback(AdapterResultV1<StudioNativeToolOutputV1>::failure(
          std::move(error), true));
      return;
    }
    pending = std::move(callback);
  }

  void clearAgentOperation(PreemptionReasonV1) noexcept override {
    ++clears;
  }

  std::optional<StudioToolCallbackV1> pending;
  int clears = 0;
  bool outcomeUnknown = false;
};

void testSharedStudioHostFixture() {
  const graphql::Json fixture = graphql::Json::parse(
      studioFixtureText("crowdyjs-studio-host-tools.v1.json"));
  CHECK(fixture.ok());
  CHECK(fixture["fixtureVersion"].asInt64() == 1);
  CHECK(fixture["contractVersion"].asStringView() ==
        "crowdy.studio-host-tools/1");
  CHECK(fixture["crowdyJs"]["version"].asStringView() == "15.0.0");
  CHECK(fixture["crowdyJs"]["commit"].asStringView() ==
        "2b0a5a5aa269c3822a5db8fdde689519bee5fe86");
  CHECK_EQ(fixture["toolNames"].size(), std::size_t{11});

  std::size_t successCount = 0;
  fixture["cases"].forEach([&](const graphql::Json& fixtureCase) {
    const auto category = fixtureCase["category"].asStringView();
    if (category != "success" && category != "approval") return;
    FixtureStudioHarness harness(
        fixtureCase["tool"].asStringView() ==
        "diagnostics.local.get");
    if (fixtureCase["setup"].ok()) {
      fixtureCase["setup"].forEach(
          [&](const graphql::Json& setup) {
            const auto result = harness.run(setup);
            CHECK(result.status ==
                  NativeToolResultStatusV1::Succeeded);
          });
    }
    const auto result = harness.run(fixtureCase);
    checkFixtureResult(result, fixtureCase);
    if (category == "success") ++successCount;
  });
  CHECK_EQ(successCount, std::size_t{11});

  fixture["cases"].forEach([&](const graphql::Json& fixtureCase) {
    const auto category = fixtureCase["category"].asStringView();
    if (category != "cancellation" &&
        category != "outcome-unknown") {
      return;
    }
    FakeClock clock;
    FakePlayerHost playerHost;
    AgentControlLeaseManager leases(playerHost);
    HoldingStudioHost host(category == "outcome-unknown");
    NativeToolDispatcherV1 dispatcher(
        leases, &host, fixtureDispatcherOptions(clock));
    std::optional<NativeToolResultV1> result;
    dispatcher.dispatch(
        fixtureInvocation(fixtureCase, 1),
        [&](NativeToolResultV1 value) {
          result = std::move(value);
        });
    if (category == "outcome-unknown") {
      CHECK(result.has_value());
      checkFixtureResult(*result, fixtureCase);
      return;
    }
    CHECK(!result);
    if (category == "cancellation") {
      dispatcher.cancelActive(PreemptionReasonV1::HUMAN_INPUT);
    }
    CHECK(result.has_value());
    checkFixtureResult(*result, fixtureCase);
    CHECK_EQ(host.clears, 1);
  });
}

void testEveryStudioToolMapping() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeApproval approval;
  auto options = controllerOptions(clock);
  CrowdyStudioController controller(
      std::move(options), provider, runtime, *crypto, clock, nullptr,
      &approval);
  controller.initialize();

  std::vector<CrowdyStudioEditorDiagnostic> diagnostics{{
      .target = CrowdyStudioTarget::Client,
      .path = "src/client.rs",
      .line = 3,
      .column = 5,
      .endLine = 4,
      .endColumn = 9,
      .severity = CrowdyStudioEditorDiagnosticSeverity::Warning,
      .message = "unused value",
      .code = "unused",
      .source = CrowdyStudioEditorDiagnosticSource::Runtime,
  }};
  controller.setLocalDiagnostics(diagnostics);
  CrowdyStudioControllerHostAdapter host(
      controller, *crypto, hostOptions());

  std::size_t mapped = 0;
  auto context = dispatch(
      host, StudioNativeToolKindV1::ContextGet, NoArgumentsV1{});
  CHECK(context.ok());
  CHECK(std::get<StudioContextV1>(*context.value).app_ref == "42");
  ++mapped;

  auto state =
      dispatch(host, StudioNativeToolKindV1::StateGet, NoArgumentsV1{});
  CHECK(state.ok());
  CHECK(std::get<StudioStateV1>(*state.value).project.has_value());
  CHECK_EQ(std::get<StudioStateV1>(*state.value).project->files.size(), 2U);
  ++mapped;

  auto selected = dispatch(
      host, StudioNativeToolKindV1::ProjectSelect,
      StudioProjectSelectRequestV1{.project_ref = "project-2"});
  CHECK(selected.ok());
  CHECK(std::get<StudioProjectSelectResultV1>(*selected.value)
            .selected_project_ref == "project-2");
  ++mapped;

  StudioFileTabRequestV1 tab{
      .source = StudioFileSourceV1::Project,
      .target = StudioTargetV1::Server,
      .path = "Cargo.toml",
      .reference_ref = std::nullopt,
  };
  auto opened = dispatch(
      host, StudioNativeToolKindV1::WorkspaceTabOpen, tab);
  CHECK(opened.ok());
  CHECK(std::get<StudioOkV1>(*opened.value).ok);
  ++mapped;
  auto closed = dispatch(
      host, StudioNativeToolKindV1::WorkspaceTabClose, tab);
  CHECK(closed.ok());
  ++mapped;

  controller.setLocalDiagnostics(diagnostics);
  auto local = dispatch(
      host, StudioNativeToolKindV1::DiagnosticsLocalGet,
      NoArgumentsV1{});
  CHECK(local.ok());
  const auto& projected =
      std::get<StudioDiagnosticsV1>(*local.value).diagnostics;
  CHECK_EQ(projected.size(), 1U);
  CHECK_EQ(controller.getState().localDiagnostics, diagnostics);
  CHECK_EQ(projected.front().source,
           StudioDiagnosticSourceV1::Runtime);
  CHECK_EQ(projected.front().target, StudioTargetV1::Client);
  CHECK_EQ(projected.front().path, "src/client.rs");
  CHECK_EQ(projected.front().line, 3U);
  CHECK_EQ(projected.front().column, 5U);
  CHECK_EQ(projected.front().severity,
           StudioDiagnosticSeverityV1::Warning);
  CHECK(projected.front().code == std::optional<std::string>("unused"));
  ++mapped;

  auto status = dispatch(
      host, StudioNativeToolKindV1::RuntimeStatusGet, NoArgumentsV1{});
  CHECK(status.ok());
  CHECK(std::get<StudioRuntimeStatusV1>(*status.value).saved_revision ==
        "2");
  ++mapped;

  auto draft = dispatch(
      host, StudioNativeToolKindV1::RuntimeTestDraft,
      StudioRuntimeTestDraftRequestV1{
          .expected_revision = "2",
          .targets = {StudioTargetV1::Server}});
  CHECK(draft.ok());
  CHECK(std::get<StudioRuntimePlanResultV1>(*draft.value)
            .runtime.draft == std::optional<bool>(true));
  ++mapped;

  auto invoked = dispatch(
      host, StudioNativeToolKindV1::RuntimeInvoke,
      StudioRuntimeInvokeRequestV1{
          .export_name = "invoke",
          .environment = StudioRuntimeEnvironmentV1::Draft,
          .params = {{.name = "enabled",
                      .type = StudioRuntimeParameterTypeV1::Boolean,
                      .value = "true"}}});
  CHECK(invoked.ok());
  CHECK(std::get<StudioRuntimeInvokeResultV1>(*invoked.value).result ==
        R"({"ok":true})");
  CHECK(runtime.calls.back() == R"(invoke:{"enabled":true})");
  ++mapped;

  auto stopped = dispatch(
      host, StudioNativeToolKindV1::RuntimeStop, NoArgumentsV1{});
  CHECK(stopped.ok());
  CHECK(std::get<StudioRuntimeStopResultV1>(*stopped.value)
            .server_stopped);
  ++mapped;

  const auto exact = controller.makeDeploymentPlan();
  auto liveGate = gate("context-1", "approved");
  auto deployed = dispatch(
      host, StudioNativeToolKindV1::RuntimeDeployLive,
      StudioRuntimeDeployLiveRequestV1{
          .expected_revision = exact.expectedRevisionId,
          .project_content_hash = *exact.projectContentHash,
          .targets = {StudioTargetV1::Server},
          .pairing_preference = StudioPairingPreferenceV1::None,
          .draft = false},
      liveGate);
  CHECK(deployed.ok());
  CHECK_EQ(approval.live, 1);
  ++mapped;

  auto playGate = liveGate;
  playGate.lease_id = "play-lease";
  auto liveInvoke = dispatch(
      host, StudioNativeToolKindV1::RuntimeInvoke,
      StudioRuntimeInvokeRequestV1{
          .export_name = "invoke",
          .environment = StudioRuntimeEnvironmentV1::Live,
          .params = {}},
      playGate);
  CHECK(liveInvoke.ok());

  CHECK_EQ(mapped, 11U);
}

void testHostOutcomeBoundaries() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeApproval approval;
  CrowdyStudioController controller(
      controllerOptions(clock), provider, runtime, *crypto, clock,
      nullptr, &approval);
  controller.initialize();

  auto throwingGateOptions = hostOptions();
  throwingGateOptions.sessionId =
      []() -> std::optional<std::string> {
    throw std::runtime_error("session provider failed");
  };
  CrowdyStudioControllerHostAdapter throwingGateHost(
      controller, *crypto, std::move(throwingGateOptions));
  const auto gateFailure = dispatch(
      throwingGateHost, StudioNativeToolKindV1::StateGet,
      NoArgumentsV1{});
  CHECK(!gateFailure.ok());
  CHECK(!gateFailure.outcome_unknown);
  CHECK_EQ(gateFailure.error->code, "AGENT_TOOL_FAILED");

  CrowdyStudioControllerHostAdapter host(
      controller, *crypto, hostOptions());
  const auto exact = controller.makeDeploymentPlan();
  const StudioRuntimeDeployLiveRequestV1 request{
      .expected_revision = exact.expectedRevisionId,
      .project_content_hash = *exact.projectContentHash,
      .targets = {StudioTargetV1::Server},
      .pairing_preference = StudioPairingPreferenceV1::None,
      .draft = false,
  };

  approval.failLive = true;
  const auto callsBeforeApproval = runtime.calls.size();
  const auto approvalFailure = dispatch(
      host, StudioNativeToolKindV1::RuntimeDeployLive, request,
      gate("context-1", "approved"));
  CHECK(!approvalFailure.ok());
  CHECK(!approvalFailure.outcome_unknown);
  CHECK_EQ(approvalFailure.error->code, "AGENT_TOOL_FAILED");
  CHECK_EQ(runtime.calls.size(), callsBeforeApproval);

  approval.failLive = false;
  runtime.failEnable = true;
  const auto enableFailure = dispatch(
      host, StudioNativeToolKindV1::RuntimeDeployLive, request,
      gate("context-1", "approved"));
  CHECK(!enableFailure.ok());
  CHECK(enableFailure.outcome_unknown);
  CHECK_EQ(enableFailure.error->code,
           "AGENT_TOOL_OUTCOME_UNKNOWN");
  CHECK(std::find(runtime.calls.begin(), runtime.calls.end(), "enable") !=
        runtime.calls.end());

  FakeProjectProvider invokeProvider;
  FakeRuntime invokeRuntime;
  CrowdyStudioController invokeController(
      controllerOptions(clock), invokeProvider, invokeRuntime, *crypto,
      clock);
  invokeController.initialize();
  CHECK(invokeController.testDraft().status ==
        CrowdyStudioDeployResult::Status::Running);
  invokeRuntime.failInvoke = true;
  CrowdyStudioControllerHostAdapter invokeHost(
      invokeController, *crypto, hostOptions());
  const auto invokeFailure = dispatch(
      invokeHost, StudioNativeToolKindV1::RuntimeInvoke,
      StudioRuntimeInvokeRequestV1{
          .export_name = "invoke",
          .environment = StudioRuntimeEnvironmentV1::Draft,
          .params = {}});
  CHECK(!invokeFailure.ok());
  CHECK(invokeFailure.outcome_unknown);
  CHECK_EQ(invokeFailure.error->code,
           "AGENT_TOOL_OUTCOME_UNKNOWN");

  FakeProjectProvider restoreProvider;
  FakeRuntime restoreRuntime;
  FakeApproval restoreApproval;
  FakeSynchronization synchronization;
  CrowdyStudioController restoreController(
      controllerOptions(clock), restoreProvider, restoreRuntime, *crypto,
      clock, &synchronization, &restoreApproval);
  restoreController.initialize();
  bool restoreEffectStarted = false;
  restoreApproval.failRestore = true;
  try {
    (void)restoreController.restoreCheckpoint(
        "checkpoint-1", "approved", std::nullopt,
        [&] { restoreEffectStarted = true; });
    CHECK(false);
  } catch (const std::runtime_error&) {
  }
  CHECK(!restoreEffectStarted);
  restoreApproval.failRestore = false;
  try {
    (void)restoreController.restoreCheckpoint(
        "checkpoint-1", "approved", std::nullopt,
        [&] { restoreEffectStarted = true; });
    CHECK(false);
  } catch (const std::runtime_error&) {
  }
  CHECK(restoreEffectStarted);
  CHECK_EQ(synchronization.restores, 1);
}

void testGateCancellationHashAndPreemptionFencing() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  FakeProjectProvider provider;
  FakeRuntime runtime;
  FakeApproval approval;
  auto options = controllerOptions(clock);
  CrowdyStudioController controller(
      std::move(options), provider, runtime, *crypto, clock, nullptr,
      &approval);
  controller.initialize();
  CrowdyStudioControllerHostAdapter host(
      controller, *crypto, hostOptions());

  CrowdyStudioControllerHostAdapter unboundHost(
      controller, *crypto);
  auto unbound = dispatch(
      unboundHost, StudioNativeToolKindV1::StateGet, NoArgumentsV1{});
  CHECK(!unbound.ok());
  CHECK(unbound.error->code == "AGENT_HOST_UNAVAILABLE");

  auto stale = dispatch(
      host, StudioNativeToolKindV1::StateGet, NoArgumentsV1{},
      gate("stale-context"));
  CHECK(!stale.ok());
  CHECK(stale.error->code == "AGENT_CONTEXT_STALE");

  const auto exact = controller.makeDeploymentPlan();
  const auto calls = runtime.calls.size();
  auto wrongHash = dispatch(
      host, StudioNativeToolKindV1::RuntimeDeployLive,
      StudioRuntimeDeployLiveRequestV1{
          .expected_revision = exact.expectedRevisionId,
          .project_content_hash =
              "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          .targets = {StudioTargetV1::Server},
          .pairing_preference = StudioPairingPreferenceV1::None,
          .draft = false},
      gate("context-1", "approved"));
  CHECK(!wrongHash.ok());
  CHECK(wrongHash.error->code == "AGENT_CONTEXT_STALE");
  CHECK_EQ(runtime.calls.size(), calls);

  auto approvalMismatch = gate("context-1", "wrong");
  auto denied = dispatch(
      host, StudioNativeToolKindV1::RuntimeDeployLive,
      StudioRuntimeDeployLiveRequestV1{
          .expected_revision = exact.expectedRevisionId,
          .project_content_hash = *exact.projectContentHash,
          .targets = {StudioTargetV1::Server},
          .pairing_preference = StudioPairingPreferenceV1::None,
          .draft = false},
      approvalMismatch);
  CHECK(!denied.ok());
  CHECK(denied.error->code == "AGENT_APPROVAL_MISMATCH");
  CHECK_EQ(runtime.calls.size(), calls);

  CancellationSourceV1 cancellation;
  cancellation.cancel();
  std::optional<AdapterResultV1<StudioNativeToolOutputV1>> cancelled;
  host.dispatch(
      StudioNativeToolKindV1::StateGet, NoArgumentsV1{}, gate(),
      cancellation.token(),
      [&](auto value) { cancelled = std::move(value); });
  CHECK(cancelled && !cancelled->ok());
  CHECK(cancelled->error->code == "AGENT_CANCELLED");

  runtime.onVersions = [&] {
    host.clearAgentOperation(PreemptionReasonV1::HUMAN_INPUT);
  };
  auto preempted = dispatch(
      host, StudioNativeToolKindV1::RuntimeTestDraft,
      StudioRuntimeTestDraftRequestV1{
          .expected_revision = "1",
          .targets = {StudioTargetV1::Server}});
  CHECK(!preempted.ok());
  CHECK(preempted.outcome_unknown);
  CHECK(preempted.error->code == "AGENT_TOOL_OUTCOME_UNKNOWN");
  CHECK(controller.getState().agentActivity ==
        CrowdyStudioAgentActivity::Paused);
}

void testNonblockingPollAndScheduledDeadlineProgress() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>();
  auto runtime = std::make_shared<FakeRuntime>();
  FakePlayerHost playerHost;
  int platformPolls = 0;

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.playerHost = &playerHost;
  options.platformPoll = [&] {
    ++platformPolls;
    return std::size_t{1};
  };
  options.nativeTools.clock = &clock;
  options.nativeTools.session_id =
      [] { return std::optional<std::string>("session-1"); };
  options.nativeTools.client_epoch =
      [] { return std::optional<std::string>("1"); };
  options.nativeTools.context_version =
      [] { return std::string("context-1"); };
  options.nativeTools.mode =
      [] { return NativeAgentModeV1::Ask; };
  options.nativeTools.validate_argument_hash =
      [](const NativeToolInvocationV1&) { return true; };
  options.studioHost = hostOptions();

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  integration->initialize();

  std::optional<NativeToolResultV1> result;
  integration->nativeTools().dispatch(
      nativeInvocation(
          "project.select",
          StudioProjectSelectRequestV1{.project_ref = "project-2"},
          clock, "scheduled-project-select"),
      [&](NativeToolResultV1 value) { result = std::move(value); });
  CHECK(!result);
  CHECK_EQ(integration->pendingStudioMaintenance(), 1U);

  CHECK_EQ(integration->poll(), std::size_t{1});
  CHECK(!result);
  clock.epoch += 20;
  clock.monotonic += 20;
  CHECK_EQ(integration->poll(), std::size_t{1});
  CHECK(result.has_value());
  CHECK_EQ(result->status, NativeToolResultStatusV1::TimedOut);
  CHECK_EQ(result->error->code, "AGENT_TOOL_TIMEOUT");
  CHECK_EQ(integration->studio().getState().project->projectId,
           "project-1");
  CHECK_EQ(platformPolls, 2);

  CHECK_EQ(integration->runStudioMaintenance(), 1U);
  CHECK_EQ(integration->pendingStudioMaintenance(), 0U);
  CHECK_EQ(integration->studio().getState().project->projectId,
           "project-1");
  CHECK_EQ(result->status, NativeToolResultStatusV1::TimedOut);
}

void testEditorRoundTripsPollTickAndRelayout() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>();
  auto runtime = std::make_shared<FakeRuntime>();
  auto editor = std::make_shared<FakeEditor>();
  FakePlayerHost playerHost;

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.editor = editor;
  options.playerHost = &playerHost;
  options.nativeTools.clock = &clock;
  options.nativeTools.session_id =
      [] { return std::optional<std::string>("session-1"); };
  options.nativeTools.client_epoch =
      [] { return std::optional<std::string>("1"); };
  options.nativeTools.context_version =
      [] { return std::string("context-1"); };
  options.nativeTools.mode =
      [] { return NativeAgentModeV1::Build; };
  options.nativeTools.is_lease_active =
      [](std::string_view, LeaseKindV1) { return true; };
  options.studioHost = hostOptions();
  int polls = 0;
  options.platformPoll = [&] {
    ++polls;
    return std::size_t{2};
  };

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  CHECK(integration->controlSnapshot().bound);
  CHECK(integration->layoutSnapshot().isVisible(
      StudioPaneId::Explorer));
  integration->initialize();
  CHECK(!editor->snapshots.empty());
  const auto& initial = editor->snapshots.back();
  CHECK(initial.projectId == std::optional<std::string>("project-1"));
  CHECK_EQ(initial.buffers.size(), 4U);
  CHECK_EQ(initial.openFiles.size(), 1U);
  CHECK(initial.selectedFile.has_value());

  editor->callbacks.onProjectFileChange(
      CrowdyStudioTarget::Server, "src/lib.rs",
      "pub fn edited() {}");
  CHECK(integration->studio().fileContent({
            CrowdyStudioFileRef::Source::Project,
            CrowdyStudioTarget::Server,
            "src/lib.rs",
            std::nullopt}) == "pub fn edited() {}");

  editor->callbacks.onOpenFile({
      CrowdyStudioFileRef::Source::Project,
      CrowdyStudioTarget::Server,
      "Cargo.toml",
      std::nullopt});
  CHECK_EQ(integration->studio().getState().openFiles.size(), 2U);
  editor->callbacks.onCloseFile({
      CrowdyStudioFileRef::Source::Project,
      CrowdyStudioTarget::Server,
      "Cargo.toml",
      std::nullopt});
  CHECK_EQ(integration->studio().getState().openFiles.size(), 1U);

  editor->callbacks.onLocalDiagnostics({{
      .target = CrowdyStudioTarget::Client,
      .path = "./src/client.rs",
      .line = 7,
      .column = 11,
      .endLine = 8,
      .endColumn = 13,
      .severity = CrowdyStudioEditorDiagnosticSeverity::Hint,
      .message = "consider a comment",
      .code = "native-hint",
      .source = CrowdyStudioEditorDiagnosticSource::Runtime,
  }});
  CHECK_EQ(integration->editor()->localDiagnostics().size(), 1U);
  CHECK_EQ(integration->studio().getState().localDiagnostics.size(), 1U);
  const auto& diagnostic =
      integration->studio().getState().localDiagnostics.front();
  CHECK_EQ(diagnostic.target, CrowdyStudioTarget::Client);
  CHECK_EQ(diagnostic.path, "src/client.rs");
  CHECK_EQ(diagnostic.line, 7U);
  CHECK_EQ(diagnostic.column, 11U);
  CHECK(diagnostic.endLine == std::optional<std::uint32_t>(8));
  CHECK(diagnostic.endColumn == std::optional<std::uint32_t>(13));
  CHECK_EQ(diagnostic.source, CrowdyStudioDiagnosticSource::Runtime);
  CHECK(diagnostic.code == std::optional<std::string>("native-hint"));

  integration->relayout();
  CHECK_EQ(editor->relayouts, 1);
  CHECK_EQ(integration->poll(), std::size_t{2});
  CHECK_EQ(polls, 1);
  clock.monotonic = 6;
  CHECK_EQ(integration->tick(), std::size_t{2});
  CHECK_EQ(provider->saves, 0);
  CHECK_EQ(polls, 2);
  CHECK_EQ(integration->runStudioMaintenance(), std::size_t{0});
  CHECK_EQ(provider->saves, 1);

  CHECK(!integration->leaseManager().attach("1"));
  std::optional<AdapterResultV1<PlayerHostCapabilitiesV1>> capabilities;
  integration->leaseManager().refreshCapabilities(
      [&](auto result) { capabilities = std::move(result); });
  CHECK(capabilities && capabilities->ok());
  AgentControlLeaseV1 lease;
  lease.lease_id = "play-lease";
  lease.kind = LeaseKindV1::Play;
  lease.status = LeaseStatusV1::Active;
  lease.client_epoch = "1";
  lease.scopes = {LeaseScopeV1::Observe, LeaseScopeV1::Locomotion};
  lease.holder = "test player";
  lease.controlled_entity_id = "player-1";
  lease.host_capability_revision = "capability-1";
  lease.context_version =
      integration->studio().getAgentContext().contextVersion;
  lease.granted_at = iso(clock.epoch);
  lease.expires_at = iso(clock.epoch + 30'000);
  CHECK(!integration->leaseManager().grantLease(lease));
  integration->controlGate().refresh();
  CHECK(integration->controlSnapshot().active_lease.has_value());

  integration->controlGate().onHumanMovementInput();
  CHECK_EQ(playerHost.clears, 1);
  CHECK(!integration->leaseSnapshot().lease.has_value());
  CHECK(!integration->controlSnapshot().active_lease.has_value());
  CHECK(integration->controlSnapshot().last_preemption ==
        std::optional<PreemptionReasonV1>(
            PreemptionReasonV1::HUMAN_INPUT));

  lease.lease_id = "expiring-play-lease";
  lease.granted_at = iso(clock.epoch);
  lease.expires_at = iso(clock.epoch + 1);
  CHECK(!integration->leaseManager().grantLease(lease));
  integration->controlGate().refresh();
  CHECK(integration->controlSnapshot().active_lease.has_value());
  clock.epoch += 2;
  clock.monotonic += 2;
  (void)integration->poll();
  CHECK(!integration->leaseSnapshot().lease.has_value());
  CHECK(!integration->controlSnapshot().active_lease.has_value());
  CHECK(integration->controlSnapshot().last_preemption ==
        std::optional<PreemptionReasonV1>(
            PreemptionReasonV1::LEASE_EXPIRED));

  integration->dispose();
  CHECK(editor->disposed);
  CHECK(!editor->callbacks.onProjectFileChange);
}

void testOwnedWalletProviderIsNonfatal() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>();
  auto runtime = std::make_shared<FakeRuntime>();
  auto wallet = std::make_shared<FakeWallet>();
  std::weak_ptr<FakeWallet> walletWeak = wallet;
  FakePlayerHost playerHost;

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.walletProvider = wallet;
  options.playerHost = &playerHost;
  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  wallet.reset();
  CHECK(!walletWeak.expired());

  integration->initialize();
  integration->studio().setSurfaceVisible(
      CrowdyStudioPolledSurface::Usage, true);
  CHECK(integration->studio().getState().wallet ==
        std::optional<CrowdyStudioWalletSnapshot>(
            CrowdyStudioWalletSnapshot{"1234", "USD"}));
  CHECK_EQ(walletWeak.lock()->reads, 1);

  walletWeak.lock()->fail = true;
  integration->studio().refreshSurface(
      CrowdyStudioPolledSurface::Usage);
  CHECK(!integration->studio().getState().wallet);
  CHECK_EQ(walletWeak.lock()->reads, 2);

  integration.reset();
  CHECK(walletWeak.expired());
}

void testIntegrationApprovedRestoreRequiresInjectedCapabilities() {
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>();
  auto runtime = std::make_shared<FakeRuntime>();
  auto approval = std::make_shared<FakeApproval>();
  auto synchronization =
      std::make_shared<SuccessfulSynchronization>(*provider);
  FakePlayerHost playerHost;

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.synchronization = synchronization;
  options.approval = approval;
  options.playerHost = &playerHost;

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  integration->initialize();
  CHECK_EQ(integration->studio().getState().checkpoints.size(), 1U);
  const std::string previous =
      integration->studio().getState().project->revision.id;
  const auto checkpoint = integration->studio().restoreCheckpoint(
      "checkpoint-1", "approved", previous);
  CHECK_EQ(checkpoint.checkpointId, "pre-restore-1");
  CHECK_EQ(checkpoint.projectRevisionId, previous);
  CHECK(integration->studio().getState().project->revision.id != previous);
  CHECK_EQ(approval->restores, 1);
  CHECK_EQ(synchronization->restores, 1);
}

void testConcreteIntegrationOwnershipAndDestructionOrder() {
  auto events = std::make_shared<std::vector<std::string>>();
  FakeClock clock;
  auto crypto = std::make_shared<FakeCrypto>();
  auto provider = std::make_shared<FakeProjectProvider>(events);
  auto runtime = std::make_shared<FakeRuntime>(events);
  auto editor = std::make_shared<FakeEditor>(events);
  auto clientRuntime = std::make_shared<FakeClientRuntime>(events);
  auto layoutStorage =
      std::make_shared<InMemoryCrowdyStudioLayoutStorage>();
  std::weak_ptr<FakeProjectProvider> providerWeak = provider;
  std::weak_ptr<FakeRuntime> runtimeWeak = runtime;
  std::weak_ptr<FakeEditor> editorWeak = editor;
  std::weak_ptr<FakeClientRuntime> clientRuntimeWeak = clientRuntime;
  std::weak_ptr<InMemoryCrowdyStudioLayoutStorage>
      layoutStorageWeak = layoutStorage;
  FakePlayerHost playerHost(events);

  CrowdyStudioIntegrationOptions options;
  options.studio = controllerOptions(clock);
  options.crypto = crypto;
  options.clock = &clock;
  options.editor = editor;
  options.clientRuntime = clientRuntime;
  options.layoutStorage = layoutStorage;
  options.playerHost = &playerHost;
  options.controlGate.on_preempt = [events](PreemptionReasonV1) {
    events->push_back("control-preempt");
  };
  options.nativeTools.clock = &clock;
  options.nativeTools.session_id =
      [] { return std::optional<std::string>("session-1"); };
  options.nativeTools.client_epoch =
      [] { return std::optional<std::string>("1"); };
  options.nativeTools.context_version =
      [] { return std::string("context-1"); };
  options.studioHost = hostOptions();

  auto integration = CrowdyStudioIntegration::create(
      std::move(options), provider, runtime);
  provider.reset();
  runtime.reset();
  editor.reset();
  clientRuntime.reset();
  layoutStorage.reset();
  crypto.reset();
  CHECK(!providerWeak.expired());
  CHECK(!runtimeWeak.expired());
  CHECK(!editorWeak.expired());
  CHECK(!clientRuntimeWeak.expired());
  CHECK(!layoutStorageWeak.expired());

  integration->initialize();
  CHECK(integration->controlSnapshot().bound);
  integration->layout().setVisible(StudioPaneId::Settings, true);
  CHECK(integration->layoutSnapshot().isVisible(
      StudioPaneId::Settings));
  CHECK(layoutStorageWeak.lock()->getItem(
            STUDIO_LAYOUT_STORAGE_KEY)
            .has_value());
  integration->relayout();
  integration->poll();
  events->clear();
  integration.reset();

  CHECK(providerWeak.expired());
  CHECK(runtimeWeak.expired());
  CHECK(editorWeak.expired());
  CHECK(clientRuntimeWeak.expired());
  CHECK(layoutStorageWeak.expired());
  const auto controlPreempt =
      std::find(events->begin(), events->end(), "control-preempt");
  const auto editorDispose =
      std::find(events->begin(), events->end(), "editor-dispose");
  const auto runtimeStop =
      std::find(events->begin(), events->end(), "runtime-stop");
  CHECK(controlPreempt != events->end());
  CHECK(editorDispose != events->end());
  CHECK(runtimeStop != events->end());
  CHECK(controlPreempt < editorDispose);
  CHECK(editorDispose < runtimeStop);
}

void testCrowdyClientConstructionHelper() {
  auto crypto = std::make_shared<FakeCrypto>();
  auto transport = std::make_shared<FakeHttpTransport>();
  FakeClock clock;
  FakePlayerHost playerHost;
  std::unique_ptr<CrowdyStudioIntegration> integration;
  int platformPolls = 0;
  {
    ClientConfig config;
    config.httpUrl = "https://game.invalid";
    config.transport = transport;
    config.crypto = crypto.get();
    CrowdyClient client(std::move(config));

    CrowdyStudioIntegrationOptions options;
    options.studio = controllerOptions(clock);
    options.crypto = crypto;
    options.clock = &clock;
    options.playerHost = &playerHost;
    options.platformPoll = [&] {
      ++platformPolls;
      return std::size_t{3};
    };
    CrowdyStudioAgentControllerOptions agentOptions;
    agentOptions.sessionId = "session-1";
    options.agent = std::move(agentOptions);
    integration =
        client.createCrowdyStudioIntegration(std::move(options));
    CHECK(integration);
    CHECK(integration->agentController() != nullptr);
    CHECK(integration->controlSnapshot().bound);
    CHECK(dynamic_cast<CrowdyStudioPlayerWalletProvider*>(
              integration->walletProvider()) != nullptr);
    CHECK(integration->poll() >= std::size_t{3});
    CHECK_EQ(platformPolls, 1);
  }
  CHECK(integration->agentController() != nullptr);
  CHECK(integration->walletProvider() != nullptr);
  (void)integration->tick();
  CHECK_EQ(platformPolls, 2);
  integration->dispose();
}

}  // namespace

int main() {
  testSharedStudioHostFixture();
  testEveryStudioToolMapping();
  testHostOutcomeBoundaries();
  testGateCancellationHashAndPreemptionFencing();
  testNonblockingPollAndScheduledDeadlineProgress();
  testEditorRoundTripsPollTickAndRelayout();
  testOwnedWalletProviderIsNonfatal();
  testIntegrationApprovedRestoreRequiresInjectedCapabilities();
  testConcreteIntegrationOwnershipAndDestructionOrder();
  testCrowdyClientConstructionHelper();
  std::printf("studio_integration_test passed\n");
  return 0;
}
