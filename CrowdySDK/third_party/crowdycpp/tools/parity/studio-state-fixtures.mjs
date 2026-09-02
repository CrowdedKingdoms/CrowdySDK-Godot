#!/usr/bin/env node
/**
 * Verify the shared typed-diagnostic and runtime-sync projection fixtures
 * against the reviewed CrowdyJS checkout.
 *
 * Usage:
 *   node tools/parity/studio-state-fixtures.mjs [--crowdyjs <checkout>] [--write]
 */
import assert from 'node:assert/strict';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';
import {
  assertCrowdyJsParityTarget,
  resolveCrowdyJsPath,
} from './crowdyjs-path.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const options = parseArgs(process.argv.slice(2));
const crowdyjs = resolveCrowdyJsPath(root, options.crowdyjs);
const target = assertCrowdyJsParityTarget(root, crowdyjs);
const fixtureDirectory = join(root, 'tools', 'parity', 'fixtures');
const diagnosticsFixture = readFixture(
  join(fixtureDirectory, 'crowdy-studio-diagnostics.v1.json'),
  'crowdy.studio-diagnostics/1',
  target,
  options.write,
);
const runtimeFixture = readFixture(
  join(fixtureDirectory, 'crowdy-studio-runtime-sync.v1.json'),
  'crowdy.studio-runtime-sync-projection/1',
  target,
  options.write,
);

const diagnosticsModulePath = join(
  crowdyjs,
  'dist',
  'crowdy-studio',
  'diagnostics.js',
);
const studioToolsModulePath = join(
  crowdyjs,
  'dist',
  'crowdy-agent',
  'studio-tools.js',
);
for (const path of [diagnosticsModulePath, studioToolsModulePath]) {
  if (!existsSync(path)) {
    throw new Error(
      `required CrowdyJS Studio artifact is missing: ${path}. ` +
        'Run npm ci && npm run build in CrowdyJS first.',
    );
  }
}

const { parseRustcDiagnostics } = await import(
  pathToFileURL(diagnosticsModulePath).href
);
const { createCrowdyStudioAgentTools } = await import(
  pathToFileURL(studioToolsModulePath).href
);

for (const fixtureCase of diagnosticsFixture.cases) {
  assert.deepEqual(
    parseRustcDiagnostics(fixtureCase.output, fixtureCase.defaultTarget),
    fixtureCase.expected,
    `CrowdyJS diagnostic fixture drift: ${fixtureCase.name}`,
  );
}

for (const fixtureCase of runtimeFixture.cases) {
  const state = {
    project: {
      revision: { id: fixtureCase.projectRevisionId },
    },
    runtime: fixtureCase.runtime,
    runtimeSync: fixtureCase.runtimeSync,
  };
  const controller = {
    getState() {
      return state;
    },
  };
  const tools = createCrowdyStudioAgentTools(controller);
  assert.deepEqual(
    tools['runtime.status.get'](),
    fixtureCase.expectedAgentRuntime,
    `CrowdyJS runtime-sync fixture drift: ${fixtureCase.name}`,
  );
}

console.log(
  `Studio state fixtures match CrowdyJS: ` +
    `${diagnosticsFixture.cases.length} diagnostic cases, ` +
    `${runtimeFixture.cases.length} runtime cases`,
);

function parseArgs(raw) {
  const parsed = { crowdyjs: null, write: false };
  for (let index = 0; index < raw.length; index++) {
    if (raw[index] === '--write') {
      parsed.write = true;
      continue;
    }
    if (raw[index] !== '--crowdyjs') {
      throw new Error(`unknown argument: ${raw[index]}`);
    }
    parsed.crowdyjs = raw[++index];
    if (!parsed.crowdyjs) throw new Error('missing value for --crowdyjs');
  }
  return parsed;
}

function readFixture(path, contractVersion, expectedTarget, write = false) {
  const raw = readFileSync(path, 'utf8');
  const fixture = JSON.parse(raw);
  if (
    fixture.contractVersion !== contractVersion ||
    !Array.isArray(fixture.cases) ||
    fixture.cases.length === 0
  ) {
    throw new Error(`invalid Studio state fixture: ${path}`);
  }
  // --write restamps provenance only. The recorded cases are still replayed
  // against the CrowdyJS build below, so moving the pin cannot quietly accept
  // a projection change — it only saves hand-editing the stamp.
  if (write) {
    fixture.crowdyJs = expectedTarget;
    const text = `${JSON.stringify(fixture, null, 2)}${raw.endsWith('\n') ? '\n' : ''}`;
    if (text !== raw) {
      writeFileSync(path, text);
      console.log(`restamped ${path}`);
    }
  }
  assert.deepEqual(
    fixture.crowdyJs,
    expectedTarget,
    `Studio state fixture has stale CrowdyJS target metadata: ${path}. ` +
      'Rerun with --write to restamp it after a pin bump.',
  );
  return fixture;
}
