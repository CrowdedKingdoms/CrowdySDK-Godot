// Game Kit e2e: economy — wallets, shop, escrow trades, and the player
// market. The owner deploys an economyBlueprint (TrustedAuthority::server
// mints, so the OWNER's game client performs earns) alongside the inventory
// blueprint whose ItemStack containers the economy guards verify; two funded
// players then buy from an admin shop listing (wallet debit + stock
// decrement + item grant in one transaction), swap items through an escrow
// trade with a cancel path, and trade on the market. Insufficient funds and
// replays resolve success:false with nothing changed. See
// docs.crowdedkingdoms.com/game-api/game-models; mirrors the CrowdyJS
// economy kit conventions.
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  const std::string ecoPrefix = e2e::kitPrefix("Eco");
  const std::string invPrefix = e2e::kitPrefix("EcoBag");

  GameKitOptions options;
  options.economy.typePrefix = ecoPrefix;
  options.inventoryTypePrefix = invPrefix;

  auto admin = e2e::signIn(cfg, cfg.ownerEmail);
  auto adminKit = makeKit(*admin.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("deploy economy + inventory blueprints");
  EconomyBlueprintOptions ecoOptions;
  ecoOptions.typePrefix = ecoPrefix;
  InventoryBlueprintOptions invOptions;
  invOptions.typePrefix = invPrefix;
  auto deployed = adminKit.deploy({economyBlueprint(ecoOptions), inventoryBlueprint(invOptions)});
  E2E_CHECK(deployed.seed.ok());
  for (const auto& w : deployed.warnings) std::printf("deploy warning: %s\n", w.c_str());

  auto alice = e2e::provisionPlayer(cfg, "kiteco-a");
  auto bob = e2e::provisionPlayer(cfg, "kiteco-b");
  auto poor = e2e::provisionPlayer(cfg, "kiteco-poor");
  auto aliceKit = makeKit(*alice.game, cfg.appId, nullptr, options);
  auto bobKit = makeKit(*bob.game, cfg.appId, nullptr, options);
  auto poorKit = makeKit(*poor.game, cfg.appId, nullptr, options);

  E2E_SUBTEST("trusted mints fund the wallets (owner client only)");
  const std::string aliceWallet =
      aliceKit.economy().ensureWallet(alice.userId)["containerId"].asString();
  const std::string bobWallet =
      bobKit.economy().ensureWallet(bob.userId)["containerId"].asString();
  const std::string poorWallet =
      poorKit.economy().ensureWallet(poor.userId)["containerId"].asString();
  E2E_CHECK(!aliceWallet.empty() && !bobWallet.empty() && !poorWallet.empty());
  // A plain player must NOT be able to mint (anti-cheat convention).
  auto selfMint = aliceKit.economy().earn(aliceWallet, 1000);
  E2E_CHECK(!selfMint.success);
  auto minted = adminKit.economy().earn(aliceWallet, 20);
  E2E_CHECK(minted.success);
  E2E_CHECK(minted.returnValue.asInt64() == 20);
  E2E_CHECK(adminKit.economy().earn(bobWallet, 10).success);
  E2E_CHECK(adminKit.economy().earn(poorWallet, 3).success);

  E2E_SUBTEST("shop: admin listing, atomic buy");
  const std::string listingId =
      adminKit.economy()
          .shopCreate("E2E Potion", "e2e_potion", 5, 2)["containerId"]
          .asString();
  E2E_CHECK(!listingId.empty());
  E2E_CHECK(aliceKit.economy().shopList().size() >= 1);

  aliceKit.inventory().ensure(alice.userId);
  const std::string alicePotions =
      aliceKit.inventory().createStack("e2e_potion", 0, 0, alice.userId)["containerId"].asString();
  auto bought = aliceKit.economy().shopBuy(listingId, aliceWallet, alicePotions);
  E2E_CHECK(bought.success);
  E2E_CHECK(bought.returnValue.asInt64() == 15);  // 20 - 5
  E2E_CHECK(aliceKit.economy().balance(aliceWallet) == 15);
  {
    bool found = false;
    for (const auto& l : aliceKit.economy().shopList()) {
      if (l.containerId != listingId) continue;
      found = true;
      E2E_CHECK(l.stock == 1);
    }
    E2E_CHECK(found);
  }

  E2E_SUBTEST("shop: insufficient funds denied, nothing changed");
  poorKit.inventory().ensure(poor.userId);
  const std::string poorPotions =
      poorKit.inventory().createStack("e2e_potion", 0, 0, poor.userId)["containerId"].asString();
  auto broke = poorKit.economy().shopBuy(listingId, poorWallet, poorPotions);  // 3 < 5
  E2E_CHECK(!broke.success);
  E2E_CHECK(poorKit.economy().balance(poorWallet) == 3);
  {
    bool found = false;
    for (const auto& s : poorKit.inventory().stacks(poor.userId)) {
      if (s.containerId != poorPotions) continue;
      found = true;
      E2E_CHECK(s.quantity == 0);
    }
    E2E_CHECK(found);
    for (const auto& l : poorKit.economy().shopList()) {
      if (l.containerId == listingId) E2E_CHECK(l.stock == 1);
    }
  }

  E2E_SUBTEST("escrow trade: offer, accept, atomic four-stack swap");
  // Alice gives 3 gems for 2 of Bob's potions.
  const std::string aliceGems =
      aliceKit.inventory().createStack("e2e_gem", 5, 1, alice.userId)["containerId"].asString();
  bobKit.inventory().ensure(bob.userId);
  const std::string bobPotions =
      bobKit.inventory().createStack("e2e_potion", 4, 0, bob.userId)["containerId"].asString();
  const std::string bobGems =
      bobKit.inventory().createStack("e2e_gem", 0, 1, bob.userId)["containerId"].asString();

  KitTradeOfferCreate offerInput;
  offerInput.toUserId = bob.userId;
  offerInput.giveStackId = aliceGems;
  offerInput.giveItemId = "e2e_gem";
  offerInput.giveQty = 3;
  offerInput.wantItemId = "e2e_potion";
  offerInput.wantQty = 2;
  offerInput.receiveStackId = alicePotions;
  const std::string offerId =
      aliceKit.economy().tradeOffer(offerInput)["containerId"].asString();
  E2E_CHECK(!offerId.empty());
  E2E_CHECK(bobKit.economy().myTrades(bob.userId).size() >= 1);

  // A third party can't accept an offer that wasn't addressed to them.
  auto hijack = poorKit.economy().tradeAccept(offerId, poorPotions, poorPotions);
  E2E_CHECK(!hijack.success);

  auto accepted = bobKit.economy().tradeAccept(offerId, bobPotions, bobGems);
  E2E_CHECK(accepted.success);
  E2E_CHECK(accepted.returnValue.asString() == "accepted");
  {
    std::int64_t aliceGemQty = -1, alicePotionQty = -1, bobGemQty = -1, bobPotionQty = -1;
    for (const auto& s : aliceKit.inventory().stacks(alice.userId)) {
      if (s.containerId == aliceGems) aliceGemQty = s.quantity;
      if (s.containerId == alicePotions) alicePotionQty = s.quantity;
    }
    for (const auto& s : bobKit.inventory().stacks(bob.userId)) {
      if (s.containerId == bobGems) bobGemQty = s.quantity;
      if (s.containerId == bobPotions) bobPotionQty = s.quantity;
    }
    E2E_CHECK(aliceGemQty == 2);    // 5 - 3
    E2E_CHECK(alicePotionQty == 3); // 1 (shop buy) + 2
    E2E_CHECK(bobGemQty == 3);      // 0 + 3
    E2E_CHECK(bobPotionQty == 2);   // 4 - 2
  }
  // Replaying the accept is denied — the offer is no longer open.
  auto replay = bobKit.economy().tradeAccept(offerId, bobPotions, bobGems);
  E2E_CHECK(!replay.success);

  E2E_SUBTEST("escrow trade: cancel path");
  offerInput.giveQty = 1;
  offerInput.wantQty = 1;
  const std::string cancelOfferId =
      aliceKit.economy().tradeOffer(offerInput)["containerId"].asString();
  auto cancelled = aliceKit.economy().tradeCancel(cancelOfferId);
  E2E_CHECK(cancelled.success);
  E2E_CHECK(cancelled.returnValue.asString() == "cancelled");
  auto acceptCancelled = bobKit.economy().tradeAccept(cancelOfferId, bobPotions, bobGems);
  E2E_CHECK(!acceptCancelled.success);

  E2E_SUBTEST("market: list + buy pays the seller atomically");
  const std::string marketId =
      bobKit.economy().marketList(bobPotions, "e2e_potion", 2, 7)["containerId"].asString();
  E2E_CHECK(!marketId.empty());
  E2E_CHECK(aliceKit.economy().marketBrowse().size() >= 1);
  const std::int64_t bobBefore = bobKit.economy().balance(bobWallet);
  auto marketBought = aliceKit.economy().marketBuy(marketId, aliceWallet, alicePotions);
  E2E_CHECK(marketBought.success);
  E2E_CHECK(marketBought.returnValue.asInt64() == 8);  // 15 - 7
  E2E_CHECK(bobKit.economy().balance(bobWallet) == bobBefore + 7);
  {
    std::int64_t alicePotionQty = -1;
    for (const auto& s : aliceKit.inventory().stacks(alice.userId)) {
      if (s.containerId == alicePotions) alicePotionQty = s.quantity;
    }
    E2E_CHECK(alicePotionQty == 5);  // 3 + 2
  }
  // The listing deactivated in the same transaction; a re-buy is denied.
  auto rebuy = aliceKit.economy().marketBuy(marketId, aliceWallet, alicePotions);
  E2E_CHECK(!rebuy.success);

  E2E_SUBTEST("market: seller cancel path");
  const std::string cancelMarketId =
      bobKit.economy().marketList(bobGems, "e2e_gem", 1, 4)["containerId"].asString();
  auto notSeller = aliceKit.economy().marketCancel(cancelMarketId);
  E2E_CHECK(!notSeller.success);
  auto takedown = bobKit.economy().marketCancel(cancelMarketId);
  E2E_CHECK(takedown.success);
  auto buyCancelled = aliceKit.economy().marketBuy(cancelMarketId, aliceWallet, aliceGems);
  E2E_CHECK(!buyCancelled.success);

  std::puts("e2e_kit_economy OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "e2e_kit_economy failed: %s\n", e.what());
  return 1;
}
