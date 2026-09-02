#!/usr/bin/env node
/**
 * Sync/check the descriptor digest and closed preemption vocabularies that
 * later native-agent phases must implement. This gate deliberately validates
 * CrowdyJS's built registry as well as its committed Game API fixture.
 *
 * Usage:
 *   node tools/parity/agent-fixtures.mjs [--crowdyjs <checkout>] [--write]
 */
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { fileURLToPath } from 'node:url';
import { enumValues, parseSchemaSurface } from './schema-surface.mjs';
import {
  assertCrowdyJsParityTarget,
  resolveCrowdyJsPath,
} from './crowdyjs-path.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const args = parseArgs(process.argv.slice(2));
const crowdyjs = resolveCrowdyJsPath(root, args.crowdyjs);
const target = assertCrowdyJsParityTarget(root, crowdyjs);
const fixtureDirectory = join(root, 'tools', 'parity', 'fixtures');
const cppDescriptorPath = join(
  fixtureDirectory,
  'crowdyjs-descriptor-digests.v1.json',
);
const cppPreemptionPath = join(
  fixtureDirectory,
  'crowdyjs-preemption-reasons.v1.json',
);
const cppToolsPath = join(
  fixtureDirectory,
  'crowdyjs-agent-tools.v1.json',
);
const jsDescriptorPath = join(
  crowdyjs,
  'src',
  'crowdy-agent',
  'fixtures',
  'crowdyjs-descriptor-digests.v1.json',
);
const jsTypesPath = join(crowdyjs, 'src', 'crowdy-agent', 'types.ts');
const jsSchemaPath = join(crowdyjs, 'schema.gql');
const jsRegistryPath = join(crowdyjs, 'dist', 'crowdy-agent', 'index.js');

for (const path of [jsDescriptorPath, jsTypesPath, jsSchemaPath, jsRegistryPath]) {
  if (!existsSync(path)) {
    throw new Error(
      `required CrowdyJS parity artifact is missing: ${path}. ` +
        'Run npm ci && npm run build in CrowdyJS first.',
    );
  }
}

const jsDescriptorText = readFileSync(jsDescriptorPath, 'utf8');
const jsDescriptorFixture = JSON.parse(jsDescriptorText);
validateDescriptorFixture(jsDescriptorFixture);
const descriptorFixture = {
  ...jsDescriptorFixture,
  crowdyJs: target,
};

const preemptionReasons = parsePreemptionReasons(
  readFileSync(jsTypesPath, 'utf8'),
);
const preemptionFixture = {
  contractVersion: 'crowdy.studio-agent/1',
  crowdyJs: target,
  digest: sha256(JSON.stringify(preemptionReasons)),
  reasons: preemptionReasons,
};

if (args.write) {
  writeFileSync(
    cppDescriptorPath,
    `${JSON.stringify(descriptorFixture, null, 2)}\n`,
  );
  writeFileSync(
    cppPreemptionPath,
    `${JSON.stringify(preemptionFixture, null, 2)}\n`,
  );
}

const cppDescriptorFixture = JSON.parse(
  readFileSync(cppDescriptorPath, 'utf8'),
);
assertDeepEqual(
  'descriptor fixture',
  cppDescriptorFixture,
  descriptorFixture,
);
const cppPreemptionFixture = JSON.parse(
  readFileSync(cppPreemptionPath, 'utf8'),
);
assertDeepEqual(
  'preemption fixture',
  cppPreemptionFixture,
  preemptionFixture,
);

const jsSchemaReasons = enumValues(
  parseSchemaSurface(readFileSync(jsSchemaPath, 'utf8')),
  'CrowdyStudioAgentPreemptionReason',
);
const cppSchemaReasons = enumValues(
  parseSchemaSurface(readFileSync(join(root, 'schema.gql'), 'utf8')),
  'CrowdyStudioAgentPreemptionReason',
);
const expectedSorted = [...preemptionReasons].sort();
assertDeepEqual('CrowdyJS schema preemption enum', jsSchemaReasons, expectedSorted);
assertDeepEqual('CrowdyCPP schema preemption enum', cppSchemaReasons, expectedSorted);
assertDeepEqual(
  'CrowdyCPP generated preemption enum',
  generatedCppPreemptionReasons(
    readFileSync(join(root, 'include', 'crowdy', 'generated', 'enums.hpp'), 'utf8'),
  ).sort(),
  expectedSorted,
);

const {
  CROWDY_AGENT_TOOL_REGISTRY_V1,
  CrowdyAgentToolRegistry,
} = await import(pathToFileURL(jsRegistryPath).href);
assertEqual(
  'full CrowdyJS registry digest',
  CROWDY_AGENT_TOOL_REGISTRY_V1.registryDigest,
  jsDescriptorFixture.crowdyJsFullRegistryDigest,
);

const subset = [];
for (const [key, expectedDigest] of Object.entries(
  jsDescriptorFixture.descriptorDigests,
).sort(([left], [right]) => left.localeCompare(right))) {
  const separator = key.lastIndexOf('@');
  const name = key.slice(0, separator);
  const version = key.slice(separator + 1);
  const entry = CROWDY_AGENT_TOOL_REGISTRY_V1.require(name, version);
  assertEqual(`${key} descriptor digest`, entry.descriptorDigest, expectedDigest);
  subset.push(entry.descriptor);
}
const subsetRegistry = new CrowdyAgentToolRegistry(subset);
assertEqual(
  'Game API subset registry digest',
  subsetRegistry.registryDigest,
  jsDescriptorFixture.gameApiSubsetRegistryDigest,
);
const toolsFixture = {
  contractVersion: 'crowdy.agent-tools/1',
  crowdyJs: target,
  registryDigest: subsetRegistry.registryDigest,
  tools: subsetRegistry.list().map(({ descriptor, descriptorDigest }) => ({
    descriptor,
    descriptorDigest,
  })),
};
if (args.write) {
  writeFileSync(cppToolsPath, `${JSON.stringify(toolsFixture, null, 2)}\n`);
  console.log('agent fixtures synchronized from CrowdyJS');
}
if (!existsSync(cppToolsPath)) {
  throw new Error(`required CrowdyCPP agent tools fixture is missing: ${cppToolsPath}`);
}
const cppToolsFixture = JSON.parse(readFileSync(cppToolsPath, 'utf8'));
assertDeepEqual('canonical Game API agent tools', cppToolsFixture, toolsFixture);

console.log(
  `agent fixtures match CrowdyJS: ${subset.length} descriptors, ` +
    `${preemptionReasons.length} preemption reasons`,
);

function parseArgs(raw) {
  const parsed = { crowdyjs: null, write: false };
  for (let index = 0; index < raw.length; index++) {
    if (raw[index] === '--write') {
      parsed.write = true;
    } else if (raw[index] === '--crowdyjs') {
      parsed.crowdyjs = raw[++index];
      if (!parsed.crowdyjs) throw new Error('missing value for --crowdyjs');
    } else {
      throw new Error(`unknown argument: ${raw[index]}`);
    }
  }
  return parsed;
}

function validateDescriptorFixture(fixture) {
  if (fixture.contractVersion !== 'crowdy.agent-tools/1') {
    throw new Error('unexpected agent descriptor contract version');
  }
  for (const field of [
    'crowdyJsFullRegistryDigest',
    'gameApiSubsetRegistryDigest',
  ]) {
    assertDigest(field, fixture[field]);
  }
  const entries = Object.entries(fixture.descriptorDigests ?? {});
  if (entries.length !== 28) {
    throw new Error(`expected 28 Game API descriptor digests, got ${entries.length}`);
  }
  for (const [key, digest] of entries) {
    if (!/^[a-z0-9_.]+@[0-9]+\.[0-9]+\.[0-9]+$/u.test(key)) {
      throw new Error(`invalid descriptor fixture key: ${key}`);
    }
    assertDigest(key, digest);
  }
}

function parsePreemptionReasons(source) {
  const match = source.match(
    /export\s+type\s+CrowdyAgentPreemptionReason\s*=([\s\S]*?);/u,
  );
  if (!match) throw new Error('CrowdyJS preemption reason type not found');
  const reasons = [...match[1].matchAll(/'([A-Z][A-Z0-9_]*)'/gu)].map(
    (entry) => entry[1],
  );
  if (reasons.length === 0 || new Set(reasons).size !== reasons.length) {
    throw new Error('CrowdyJS preemption reason type is empty or duplicated');
  }
  return reasons;
}

function generatedCppPreemptionReasons(source) {
  const match = source.match(
    /enum\s+class\s+CrowdyStudioAgentPreemptionReason\s*\{([\s\S]*?)\};/u,
  );
  if (!match) throw new Error('generated C++ preemption enum not found');
  return match[1]
    .split('\n')
    .map((line) => line.trim().replace(/,$/u, ''))
    .filter(Boolean);
}

function assertDigest(field, value) {
  if (!/^sha256:[0-9a-f]{64}$/u.test(value ?? '')) {
    throw new Error(`${field} is not a sha256 digest`);
  }
}

function sha256(value) {
  return `sha256:${createHash('sha256').update(value).digest('hex')}`;
}

function assertEqual(field, actual, expected) {
  if (actual !== expected) {
    throw new Error(`${field} drift: expected ${expected}, got ${actual}`);
  }
}

function assertDeepEqual(field, actual, expected) {
  const left = canonicalJson(actual);
  const right = canonicalJson(expected);
  if (left !== right) {
    throw new Error(`${field} drifted from CrowdyJS`);
  }
}

function canonicalJson(value) {
  if (Array.isArray(value)) return `[${value.map(canonicalJson).join(',')}]`;
  if (value && typeof value === 'object') {
    return `{${Object.keys(value)
      .sort()
      .map((key) => `${JSON.stringify(key)}:${canonicalJson(value[key])}`)
      .join(',')}}`;
  }
  return JSON.stringify(value);
}
