// Game Kit e2e: decks (cards & hidden information).
//
// An admin deploys the decks blueprint (unique typePrefix per run); two
// players deal themselves cards. The suite proves the hidden-information
// contract end to end: card_id carries visibility "owner", so the OWNER's
// containerState read includes it while the OPPONENT's read of the very same
// container must NOT reveal it (server-side read filtering, not client
// discipline). Shuffling runs the manual assign_position automation (admin,
// gameModelRunAutomation) and deals fresh random positions; drawing moves the
// lowest-position deck card into the hand through the zone-guarded function;
// playing a card flips the zone AND copies card_id into the public
// revealed_card_id in one transaction. Negative paths (drawing an opponent's
// card, re-drawing a hand card) resolve success=false. See
// https://docs.crowdedkingdoms.com/game-api/game-models.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() {
  try {
    auto cfg = e2e::requireConfig();
    e2e::requireOwner(cfg);
    const std::string prefix = e2e::kitPrefix("Cd");

    E2E_SUBTEST("deploy decks blueprint");
    auto& admin = e2e::ownerGame(cfg);
    e2e::pruneStaleAutomations(admin, cfg.appId);
    GameKitOptions adminOptions;
    adminOptions.decksTypePrefix = prefix;
    auto adminKit = makeKit(admin, cfg.appId, nullptr, adminOptions);
    DecksBlueprintOptions blueprint;
    blueprint.typePrefix = prefix;
    auto deployed = adminKit.deploy({decksBlueprint(blueprint)});
    E2E_CHECK(deployed.seed.ok());
    for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

    E2E_SUBTEST("two players deal cards into their decks");
    auto a = e2e::provisionPlayer(cfg, "cd-a");
    auto b = e2e::provisionPlayer(cfg, "cd-b");
    GameKitOptions playerOptions;
    playerOptions.decksTypePrefix = prefix;
    auto kitA = makeKit(*a.game, cfg.appId, nullptr, playerOptions);
    auto kitB = makeKit(*b.game, cfg.appId, nullptr, playerOptions);

    const std::vector<std::string> deckA = {"cd_ace", "cd_king", "cd_queen"};
    std::vector<graphql::Json> dealtA = kitA.decks().deal(a.userId, deckA);
    E2E_CHECK(dealtA.size() == 3);
    std::vector<graphql::Json> dealtB = kitB.decks().deal(b.userId, {"cd_two"});
    E2E_CHECK(dealtB.size() == 1);

    E2E_SUBTEST("owner-visibility: opponent reads must not reveal card_id");
    const std::string cardBId = dealtB.front()["containerId"].asString();
    // The owner sees their own card identity.
    graphql::Json ownProps = kitContainerProperties(b.game->gameModel(), cfg.appId, cardBId);
    E2E_CHECK(ownProps["card_id"].asString() == "cd_two");
    // The opponent's read of the SAME container omits (or nulls) card_id.
    graphql::Json foreignProps = kitContainerProperties(a.game->gameModel(), cfg.appId, cardBId);
    graphql::Json hidden = foreignProps["card_id"];
    E2E_CHECK(!hidden.ok() || hidden.isNull() || hidden.asString().empty());
    E2E_CHECK(hidden.asString() != "cd_two");
    // The public half stays empty until the card is played/discarded.
    E2E_CHECK(foreignProps["revealed_card_id"].asString().empty());
    // Same property through the kit's parsed view.
    for (const auto& card : kitA.decks().cards(b.userId)) {
      E2E_CHECK(card.cardId.empty());
    }

    E2E_SUBTEST("shuffle automation deals fresh random positions (runNow)");
    // Deal assigned sequential positions 0..n-1; the manual automation deals
    // rand_int(0, 1000000) per deck card server-side.
    auto positionsChanged = [&] {
      for (const auto& card : kitA.decks().cards(a.userId, "deck")) {
        if (card.position > static_cast<std::int64_t>(deckA.size())) return true;
      }
      return false;
    };
    bool shuffled = false;
    for (int i = 0; i < 5 && !shuffled; ++i) {
      adminKit.decks().shuffle();
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
      shuffled = positionsChanged();
    }
    E2E_CHECK(shuffled);

    E2E_SUBTEST("draw moves the top-of-deck card into the hand");
    // Identify the expected top (lowest position) before drawing.
    std::vector<KitCard> deckBefore = kitA.decks().cards(a.userId, "deck");
    E2E_CHECK(deckBefore.size() == 3);
    const KitCard* top = &deckBefore.front();
    for (const KitCard& card : deckBefore) {
      if (card.position < top->position) top = &card;
    }
    const std::string drawnId = top->containerId;
    auto drawn = kitA.decks().draw(a.userId);
    E2E_CHECK(drawn.success);
    E2E_CHECK(drawn.returnValue.asString() == "hand");
    std::vector<KitCard> hand = kitA.decks().myHand(a.userId);
    E2E_CHECK(hand.size() == 1);
    E2E_CHECK(hand.front().containerId == drawnId);
    E2E_CHECK(!hand.front().cardId.empty());  // owner still sees the identity
    E2E_CHECK(kitA.decks().cards(a.userId, "deck").size() == 2);

    E2E_SUBTEST("negative paths: foreign draw and re-draw are denied");
    // B cannot draw A's deck card (owner_of_self).
    std::vector<KitCard> deckAfter = kitA.decks().cards(a.userId, "deck");
    E2E_CHECK(!deckAfter.empty());
    auto foreignDraw = kitB.decks().drawCard(deckAfter.front().containerId);
    E2E_CHECK(!foreignDraw.success);
    // A cannot draw a card that is already in the hand (zone guard).
    auto redraw = kitA.decks().drawCard(drawnId);
    E2E_CHECK(!redraw.success);

    E2E_SUBTEST("play reveals: zone flip + public reveal commit together");
    const std::string handCardId = hand.front().cardId;
    auto played = kitA.decks().play(drawnId);
    E2E_CHECK(played.success);
    E2E_CHECK(played.returnValue.asString() == handCardId);
    // The opponent now sees the revealed identity on the board.
    graphql::Json revealedProps =
        kitContainerProperties(b.game->gameModel(), cfg.appId, drawnId);
    E2E_CHECK(revealedProps["revealed_card_id"].asString() == handCardId);
    E2E_CHECK(revealedProps["zone"].asString() == "board");

    std::puts("e2e_kit_decks OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_kit_decks FAILED: %s\n", e.what());
    return 1;
  }
}
