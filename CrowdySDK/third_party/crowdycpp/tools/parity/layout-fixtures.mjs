#!/usr/bin/env node
/**
 * Generate/check the portable Crowdy Studio layout contract directly from the
 * pinned, built CrowdyJS checkout.
 *
 * Usage:
 *   node tools/parity/layout-fixtures.mjs [--crowdyjs <checkout>] [--write]
 */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { fileURLToPath } from 'node:url';
import {
  assertCrowdyJsParityTarget,
  resolveCrowdyJsPath,
} from './crowdyjs-path.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const options = parseArgs(process.argv.slice(2));
const crowdyjs = resolveCrowdyJsPath(root, options.crowdyjs);
const target = assertCrowdyJsParityTarget(root, crowdyjs);
const modulePath = join(crowdyjs, 'dist', 'crowdy-studio', 'layout.js');
if (!existsSync(modulePath)) {
  throw new Error(
    `required CrowdyJS layout artifact is missing: ${modulePath}. ` +
      'Run npm ci && npm run build in CrowdyJS first.',
  );
}

const {
  STUDIO_LAYOUT_STORAGE_KEY,
  STUDIO_PANE_IDS,
  StudioLayoutController,
  clampStudioPaneSize,
  studioPaneSizeRange,
} = await import(
  `${pathToFileURL(modulePath).href}?crowdycpp=${encodeURIComponent(target.commit)}`
);

class CaptureStorage {
  key = null;
  value = null;

  getItem() {
    return this.value;
  }

  setItem(key, value) {
    this.key = key;
    this.value = value;
  }
}

const defaults = new StudioLayoutController({ storage: null }).getState();
const ranges = Object.fromEntries(
  STUDIO_PANE_IDS.map((pane) => [pane, studioPaneSizeRange(pane)]),
);
const clamping = [];
for (const pane of STUDIO_PANE_IDS) {
  const range = ranges[pane];
  for (const input of [
    'NaN',
    '-Infinity',
    'Infinity',
    -1_000,
    range.min + 0.49,
    range.min + 0.5,
    range.max + 1_000,
  ]) {
    const numeric =
      input === 'NaN'
        ? Number.NaN
        : input === '-Infinity'
          ? Number.NEGATIVE_INFINITY
          : input === 'Infinity'
            ? Number.POSITIVE_INFINITY
            : input;
    clamping.push({
      pane,
      input,
      output: clampStudioPaneSize(pane, numeric),
    });
  }
}

const defaultStorage = new CaptureStorage();
const defaultController = new StudioLayoutController({
  storage: defaultStorage,
});
defaultController.setSize(
  'explorer',
  defaultController.paneSize('explorer'),
);

const scenarioStorage = new CaptureStorage();
const scenarioController = new StudioLayoutController({
  storage: scenarioStorage,
});
scenarioController.setVisible('settings', true);
scenarioController.toggle('explorer');
scenarioController.setSize('agent', 999.6);
scenarioController.setSize('bottom', 111.5);

const fixture = {
  fixtureVersion: 1,
  crowdyJs: target,
  storageKey: STUDIO_LAYOUT_STORAGE_KEY,
  paneIds: [...STUDIO_PANE_IDS],
  defaults,
  ranges,
  clamping,
  defaultPersistedJson: requiredValue(defaultStorage),
  scenario: {
    state: scenarioController.getState(),
    persistedJson: requiredValue(scenarioStorage),
  },
};
const text = `${JSON.stringify(fixture, null, 2)}\n`;
const fixturePath = join(
  root,
  'tools',
  'parity',
  'fixtures',
  'crowdyjs-studio-layout.v1.json',
);

if (options.write) {
  writeFileSync(fixturePath, text);
  console.log('Crowdy Studio layout fixture synchronized from CrowdyJS');
} else if (!existsSync(fixturePath) || readFileSync(fixturePath, 'utf8') !== text) {
  throw new Error(
    'Crowdy Studio layout fixture is stale; rerun layout-fixtures.mjs --write',
  );
}

console.log(
  `Crowdy Studio layout fixture matches CrowdyJS: ` +
    `${STUDIO_PANE_IDS.length} panes, ${clamping.length} clamp cases`,
);

function requiredValue(storage) {
  if (storage.key !== STUDIO_LAYOUT_STORAGE_KEY) {
    throw new Error(
      `CrowdyJS layout controller persisted unexpected key ${storage.key}`,
    );
  }
  if (typeof storage.value !== 'string') {
    throw new Error('CrowdyJS layout controller did not persist a fixture');
  }
  return storage.value;
}

function parseArgs(args) {
  const parsed = { crowdyjs: null, write: false };
  for (let index = 0; index < args.length; index++) {
    const argument = args[index];
    if (argument === '--write') {
      parsed.write = true;
    } else if (argument === '--crowdyjs') {
      parsed.crowdyjs = args[++index];
      if (!parsed.crowdyjs) throw new Error('missing value for --crowdyjs');
    } else {
      throw new Error(`unknown argument: ${argument}`);
    }
  }
  return parsed;
}
