#!/usr/bin/env node
/**
 * Dump every CrowdyJS blueprint builder's output as JSON for the blueprint
 * equivalence diff (tools/parity/blueprints-diff.mjs). The variant matrix
 * MUST stay in lockstep with tools/parity/dump_blueprints.cpp.
 *
 * Usage: node tools/parity/dump-blueprints.mjs <path-to-built-CrowdyJS>
 */
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';

const crowdyjs = process.argv[2];
if (!crowdyjs) {
  console.error('usage: dump-blueprints.mjs <path-to-built-CrowdyJS>');
  process.exit(2);
}
const bp = await import(pathToFileURL(resolve(crowdyjs, 'dist/kit/blueprints/index.js')).href);

const npcBehavior = {
  name: 'npc-wander',
  trigger: { intervalMs: 60000 },
  mutations: [
    { target: 'self', property: 'x', expression: 'self.x + rand_int(-2, 2)' },
  ],
};

const lootTable = {
  tableId: 'chest',
  entries: [
    { itemId: 'sword', weight: 2 },
    { itemId: 'gold', weight: 6, minQty: 5, maxQty: 20 },
    { itemId: 'gem', weight: 1 },
  ],
};

const variants = {
  inventory_default: () => bp.inventoryBlueprint(),
  inventory_bank: () => bp.inventoryBlueprint({ typePrefix: 'Bank', maxSlots: 10, slotCount: 8 }),
  lock_key: () => bp.lockBlueprint({ authority: [{ kind: 'key' }] }),
  lock_chest: () =>
    bp.lockBlueprint({
      objectTypeName: 'Chest',
      authority: [{ kind: 'owner' }, { kind: 'chunkPermission', key: 'access', mode: 'smallest' }],
    }),
  npc_wander: () => bp.npcBlueprint({ behaviors: [npcBehavior] }),
  plots_default: () => bp.plotBlueprint(),
  plots_rentable: () => bp.plotBlueprint({ rentable: true }),
  economy_default: () => bp.economyBlueprint(),
  progression_default: () => bp.progressionBlueprint(),
  loot_chest: () => bp.lootBlueprint({ tables: [lootTable] }),
  quests_default: () => bp.questsBlueprint(),
  combat_default: () => bp.combatBlueprint(),
  matches_default: () => bp.matchesBlueprint(),
  matches_turn_timer: () => bp.matchesBlueprint({ turnTimer: { delayMs: 30000 } }),
  matches_turn_tick: () => bp.matchesBlueprint({ turnTick: { intervalMs: 15000 } }),
  decks_default: () => bp.decksBlueprint(),
  worldsim_default: () => bp.worldsimBlueprint(),
  guild_default: () => bp.guildBlueprint({ guildGroupId: '77' }),
  leaderboards_default: () => bp.leaderboardsBlueprint(),
};

const out = {};
for (const [name, build] of Object.entries(variants)) {
  try {
    out[name] = build();
  } catch (e) {
    out[name] = { __error: String(e?.message ?? e) };
  }
}
console.log(JSON.stringify(out, null, 1));
