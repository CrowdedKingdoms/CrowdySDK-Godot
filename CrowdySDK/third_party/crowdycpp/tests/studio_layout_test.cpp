#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "crowdy/graphql/json.hpp"
#include "crowdy/studio/layout.hpp"
#include "crowdy_studio_layout_fixture.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::studio;

namespace {

StudioPaneId paneFromFixture(const graphql::Json& value) {
  const auto pane = studioPaneIdFromString(value.asStringView());
  CHECK(pane.has_value());
  return *pane;
}

StudioLayoutState stateFromFixture(const graphql::Json& value) {
  StudioLayoutState::Visibility visible{};
  StudioLayoutState::Sizes sizes{};
  for (const StudioPaneId pane : STUDIO_PANE_IDS) {
    const auto key = toString(pane);
    const auto index = studioPaneIndex(pane);
    CHECK(value["visible"][key].isBool());
    CHECK(value["sizes"][key].isNumber());
    visible[index] = value["visible"][key].asBool();
    sizes[index] = static_cast<int>(value["sizes"][key].asInt64());
  }
  return {std::move(visible), std::move(sizes)};
}

StudioLayoutControllerOptions usingStorage(
    ICrowdyStudioLayoutStorage& storage) {
  StudioLayoutControllerOptions options;
  options.storage = &storage;
  return options;
}

void testPinnedCrowdyJsFixture() {
  const graphql::Json fixture = graphql::Json::parse(
      generated::kCrowdyJsStudioLayoutFixtureV1);
  CHECK(fixture.isObject());
  CHECK_EQ(fixture["fixtureVersion"].asInt64(), std::int64_t{1});
  CHECK(fixture["crowdyJs"]["version"].asStringView() == "15.0.0");
  CHECK(fixture["crowdyJs"]["commit"].asStringView() ==
        "2b0a5a5aa269c3822a5db8fdde689519bee5fe86");
  CHECK(fixture["storageKey"].asStringView() ==
        STUDIO_LAYOUT_STORAGE_KEY);

  const graphql::Json paneIds = fixture["paneIds"];
  CHECK_EQ(paneIds.size(), STUDIO_PANE_IDS.size());
  for (std::size_t index = 0; index < paneIds.size(); ++index) {
    CHECK(paneFromFixture(paneIds.at(index)) == STUDIO_PANE_IDS[index]);
  }

  const StudioLayoutState expectedDefaults =
      stateFromFixture(fixture["defaults"]);
  StudioLayoutController defaults;
  CHECK(defaults.getState() == expectedDefaults);

  for (const StudioPaneId pane : STUDIO_PANE_IDS) {
    const auto key = toString(pane);
    const auto expected = fixture["ranges"][key];
    const auto actual = studioPaneSizeRange(pane);
    CHECK_EQ(actual.min, static_cast<int>(expected["min"].asInt64()));
    CHECK_EQ(actual.max, static_cast<int>(expected["max"].asInt64()));
  }

  const graphql::Json clamping = fixture["clamping"];
  CHECK_EQ(clamping.size(), std::size_t{28});
  clamping.forEach([](const graphql::Json& entry) {
    const StudioPaneId pane = paneFromFixture(entry["pane"]);
    const graphql::Json input = entry["input"];
    double value = 0;
    if (input.isNumber()) {
      value = input.asDouble();
    } else if (input.asStringView() == "NaN") {
      value = std::numeric_limits<double>::quiet_NaN();
    } else if (input.asStringView() == "-Infinity") {
      value = -std::numeric_limits<double>::infinity();
    } else {
      CHECK(input.asStringView() == "Infinity");
      value = std::numeric_limits<double>::infinity();
    }
    CHECK_EQ(clampStudioPaneSize(pane, value),
             static_cast<int>(entry["output"].asInt64()));
  });

  InMemoryCrowdyStudioLayoutStorage defaultStorage;
  StudioLayoutController persistedDefaults(usingStorage(defaultStorage));
  persistedDefaults.setSize(StudioPaneId::Explorer,
                            persistedDefaults.paneSize(
                                StudioPaneId::Explorer));
  const auto defaultJson =
      defaultStorage.getItem(STUDIO_LAYOUT_STORAGE_KEY);
  CHECK(defaultJson.has_value());
  CHECK(*defaultJson ==
        fixture["defaultPersistedJson"].asStringView());

  InMemoryCrowdyStudioLayoutStorage scenarioStorage;
  StudioLayoutController scenario(usingStorage(scenarioStorage));
  scenario.setVisible(StudioPaneId::Settings, true);
  scenario.toggle(StudioPaneId::Explorer);
  scenario.setSize(StudioPaneId::Agent, 999.6);
  scenario.setSize(StudioPaneId::Bottom, 111.5);
  CHECK(scenario.getState() ==
        stateFromFixture(fixture["scenario"]["state"]));
  const auto scenarioJson =
      scenarioStorage.getItem(STUDIO_LAYOUT_STORAGE_KEY);
  CHECK(scenarioJson.has_value());
  CHECK(*scenarioJson ==
        fixture["scenario"]["persistedJson"].asStringView());

  InMemoryCrowdyStudioLayoutStorage reloadedStorage;
  CHECK(reloadedStorage.setItem(STUDIO_LAYOUT_STORAGE_KEY, *scenarioJson));
  StudioLayoutController reloaded(usingStorage(reloadedStorage));
  CHECK(reloaded.getState() == scenario.getState());
}

class UnavailableStorage final : public ICrowdyStudioLayoutStorage {
 public:
  std::optional<std::string> getItem(std::string_view) override {
    return std::nullopt;
  }
  bool setItem(std::string_view, std::string_view) override {
    writes++;
    return false;
  }
  int writes = 0;
};

#ifndef CROWDY_NO_EXCEPTIONS
class ThrowingStorage final : public ICrowdyStudioLayoutStorage {
 public:
  std::optional<std::string> getItem(std::string_view) override {
    throw std::runtime_error("storage unavailable");
  }
  bool setItem(std::string_view, std::string_view) override {
    throw std::runtime_error("storage unavailable");
  }
};
#endif

void testFailSafeStorageAndDefaults() {
  InMemoryCrowdyStudioLayoutStorage corrupt;
  CHECK(corrupt.setItem(STUDIO_LAYOUT_STORAGE_KEY,
                        R"({"visible":{"explorer":false},"sizes":)"));
  StudioLayoutControllerOptions options;
  options.storage = &corrupt;
  options.defaults.visible[studioPaneIndex(StudioPaneId::Explorer)] = false;
  options.defaults.visible[studioPaneIndex(StudioPaneId::Agent)] = true;
  options.defaults.sizes[studioPaneIndex(StudioPaneId::Bottom)] = 1'000.0;
  StudioLayoutController controller(std::move(options));
  CHECK(!controller.isVisible(StudioPaneId::Explorer));
  CHECK(controller.isVisible(StudioPaneId::Agent));
  CHECK_EQ(controller.paneSize(StudioPaneId::Bottom), 480);

  UnavailableStorage unavailable;
  StudioLayoutController sessionOnly(usingStorage(unavailable));
  sessionOnly.toggle(StudioPaneId::Bottom);
  CHECK(sessionOnly.isVisible(StudioPaneId::Bottom));
  CHECK_EQ(unavailable.writes, 1);

#ifndef CROWDY_NO_EXCEPTIONS
  ThrowingStorage throwing;
  StudioLayoutController throwingController(usingStorage(throwing));
  throwingController.toggle(StudioPaneId::Settings);
  CHECK(throwingController.isVisible(StudioPaneId::Settings));
#endif
}

void testSnapshotsListenersAndDeferredPersistence() {
  InMemoryCrowdyStudioLayoutStorage storage;
  StudioLayoutController controller(usingStorage(storage));
  const StudioLayoutState before = controller.getState();
  int notifications = 0;
  StudioLayoutState latest;
  const auto listener = controller.subscribe(
      [&](const StudioLayoutState& state) {
        notifications++;
        latest = state;
      });
  CHECK_EQ(notifications, 0);

  controller.toggle(StudioPaneId::Agent);
  CHECK_EQ(notifications, 1);
  CHECK(latest.isVisible(StudioPaneId::Agent));
  CHECK(!before.isVisible(StudioPaneId::Agent));

  controller.setVisible(StudioPaneId::Agent, true);
  CHECK_EQ(notifications, 1);
  const auto persisted = storage.getItem(STUDIO_LAYOUT_STORAGE_KEY);
  CHECK(persisted.has_value());

  controller.setSize(StudioPaneId::Agent, 333.5, false);
  CHECK_EQ(notifications, 2);
  CHECK_EQ(controller.paneSize(StudioPaneId::Agent), 334);
  CHECK(storage.getItem(STUDIO_LAYOUT_STORAGE_KEY) == persisted);

  controller.unsubscribe(listener);
  controller.toggle(StudioPaneId::Bottom);
  CHECK_EQ(notifications, 2);
}

}  // namespace

int main() {
  testPinnedCrowdyJsFixture();
  testFailSafeStorageAndDefaults();
  testSnapshotsListenersAndDeferredPersistence();
  std::puts("studio_layout_test OK");
  return 0;
}
