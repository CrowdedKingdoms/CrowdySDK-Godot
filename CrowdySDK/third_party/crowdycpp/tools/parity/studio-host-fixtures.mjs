#!/usr/bin/env node
/**
 * Generate/check exact input, output, and result-status vectors for the 11
 * portable Crowdy Studio host tools from the pinned, built CrowdyJS checkout.
 *
 * Usage:
 *   node tools/parity/studio-host-fixtures.mjs
 *     [--crowdyjs <checkout>] [--write]
 */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import {
  assertCrowdyJsParityTarget,
  resolveCrowdyJsPath,
} from './crowdyjs-path.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const options = parseArgs(process.argv.slice(2));
const crowdyjs = resolveCrowdyJsPath(root, options.crowdyjs);
const target = assertCrowdyJsParityTarget(root, crowdyjs);
const agentModulePath = join(crowdyjs, 'dist', 'crowdy-agent', 'index.js');
const studioModulePath = join(crowdyjs, 'dist', 'crowdy-studio', 'index.js');
for (const path of [agentModulePath, studioModulePath]) {
  if (!existsSync(path)) {
    throw new Error(
      `required CrowdyJS Studio host artifact is missing: ${path}. ` +
        'Run npm ci && npm run build in CrowdyJS first.',
    );
  }
}

const agent = await import(
  `${pathToFileURL(agentModulePath).href}?crowdycpp=${target.commit}`
);
const studio = await import(
  `${pathToFileURL(studioModulePath).href}?crowdycpp=${target.commit}`
);
const {
  CROWDY_AGENT_TOOL_REGISTRY_V1,
  CrowdyAgentBrowserToolDispatcher,
  createCrowdyStudioAgentTools,
} = agent;
const { CrowdyStudioController } = studio;
const fixtureNow = Date.parse('2026-07-24T12:00:00.000Z');
let nextToolCallSequence = 1;

const toolNames = [
  'studio.context.get',
  'studio.state.get',
  'project.select',
  'workspace.tab.open',
  'workspace.tab.close',
  'diagnostics.local.get',
  'runtime.status.get',
  'runtime.test_draft',
  'runtime.deploy_live',
  'runtime.invoke',
  'runtime.stop',
];

const cases = [];
cases.push(await successfulCase('studio-context', 'studio.context.get', {}));
cases.push(await successfulCase('studio-state', 'studio.state.get', {}));
cases.push(
  await successfulCase('project-select', 'project.select', {
    projectRef: 'project-2',
  }),
);
cases.push(
  await successfulCase('workspace-tab-open', 'workspace.tab.open', fileInput()),
);
cases.push(
  await successfulCase(
    'workspace-tab-close',
    'workspace.tab.close',
    fileInput(),
    {
      setup: [{ tool: 'workspace.tab.open', input: fileInput() }],
    },
  ),
);
cases.push(
  await successfulCase(
    'diagnostics-local',
    'diagnostics.local.get',
    {},
    { diagnostics: true },
  ),
);
cases.push(
  await successfulCase('runtime-status', 'runtime.status.get', {}),
);
cases.push(
  await successfulCase(
    'runtime-test-draft',
    'runtime.test_draft',
    draftInput(),
    { leaseId: 'workspace-lease' },
  ),
);

{
  const harness = await createHarness();
  const input = liveInput(harness.controller);
  cases.push(
    await runSuccessfulCase(harness, {
      name: 'runtime-deploy-live-approved',
      tool: 'runtime.deploy_live',
      input,
      leaseId: 'workspace-lease',
      approvalGrant: 'approved',
    }),
  );
}
cases.push(
  await successfulCase(
    'runtime-invoke-draft',
    'runtime.invoke',
    {
      exportName: 'invoke',
      environment: 'DRAFT',
      params: [
        { name: 'enabled', type: 'BOOLEAN', value: 'true' },
        { name: 'label', type: 'STRING', value: 'fixture' },
      ],
    },
    {
      leaseId: 'workspace-lease',
      setup: [
        {
          tool: 'runtime.test_draft',
          input: draftInput(),
          leaseId: 'workspace-lease',
        },
      ],
    },
  ),
);
cases.push(
  await successfulCase('runtime-stop', 'runtime.stop', {}, {
    setup: [
      {
        tool: 'runtime.test_draft',
        input: draftInput(),
        leaseId: 'workspace-lease',
      },
    ],
  }),
);

{
  const harness = await createHarness();
  const input = liveInput(harness.controller);
  const result = await dispatch(harness, {
    tool: 'runtime.deploy_live',
    input,
    leaseId: 'workspace-lease',
  });
  cases.push({
    name: 'runtime-deploy-live-approval-required',
    category: 'approval',
    tool: 'runtime.deploy_live',
    input,
    leaseId: 'workspace-lease',
    expected: resultProjection(result),
  });
  harness.controller.destroy();
}

{
  const harness = await createHarness({
    transformHandlers(handlers) {
      return {
        ...handlers,
        'studio.state.get': async () => new Promise(() => {}),
      };
    },
  });
  const pending = dispatch(harness, {
    tool: 'studio.state.get',
    input: {},
  });
  await Promise.resolve();
  harness.dispatcher.cancelActive();
  const result = await pending;
  cases.push({
    name: 'studio-state-cancelled',
    category: 'cancellation',
    tool: 'studio.state.get',
    input: {},
    expected: resultProjection(result),
  });
  harness.controller.destroy();
}

{
  const harness = await createHarness({
    transformHandlers(handlers) {
      const testDraft = handlers['runtime.test_draft'];
      return {
        ...handlers,
        'runtime.test_draft': async (input, context) => {
          await testDraft(input, context);
          return new Promise(() => {});
        },
      };
    },
  });
  const input = draftInput();
  const result = await dispatch(harness, {
    tool: 'runtime.test_draft',
    input,
    leaseId: 'workspace-lease',
    deadlineMs: 1,
  });
  cases.push({
    name: 'runtime-test-draft-outcome-unknown',
    category: 'outcome-unknown',
    tool: 'runtime.test_draft',
    input,
    leaseId: 'workspace-lease',
    deadlineMs: 1,
    expected: resultProjection(result),
  });
  harness.controller.destroy();
}

const successfulTools = cases
  .filter(({ category }) => category === 'success')
  .map(({ tool }) => tool);
if (
  successfulTools.length !== toolNames.length ||
  toolNames.some((name) => !successfulTools.includes(name))
) {
  throw new Error('Studio host fixture must cover every native tool exactly once');
}

const fixture = {
  fixtureVersion: 1,
  contractVersion: 'crowdy.studio-host-tools/1',
  crowdyJs: target,
  toolNames,
  cases,
};
const text = `${JSON.stringify(fixture, null, 2)}\n`;
const fixturePath = join(
  root,
  'tools',
  'parity',
  'fixtures',
  'crowdyjs-studio-host-tools.v1.json',
);

if (options.write) {
  writeFileSync(fixturePath, text);
  console.log('Crowdy Studio host fixture synchronized from CrowdyJS');
} else if (
  !existsSync(fixturePath) ||
  readFileSync(fixturePath, 'utf8') !== text
) {
  throw new Error(
    'Crowdy Studio host fixture is stale; rerun studio-host-fixtures.mjs --write',
  );
}

console.log(
  `Crowdy Studio host fixture matches CrowdyJS: ` +
    `${toolNames.length} tools, ${cases.length} cases`,
);

async function successfulCase(name, tool, input, setup = {}) {
  const harness = await createHarness({
    diagnostics: setup.diagnostics,
  });
  return runSuccessfulCase(harness, {
    name,
    tool,
    input,
    setup: setup.setup ?? [],
    leaseId: setup.leaseId,
    approvalGrant: setup.approvalGrant,
  });
}

async function runSuccessfulCase(harness, fixtureCase) {
  for (const [index, operation] of (fixtureCase.setup ?? []).entries()) {
    const result = await dispatch(harness, {
      ...operation,
      toolCallId: `setup-${fixtureCase.name}-${index}`,
    });
    if (result.status !== 'SUCCEEDED') {
      throw new Error(
        `${fixtureCase.name} setup failed: ${result.error?.code ?? result.status}`,
      );
    }
  }
  const result = await dispatch(harness, fixtureCase);
  if (result.status !== 'SUCCEEDED') {
    throw new Error(
      `${fixtureCase.name} failed: ${result.error?.code ?? result.status}`,
    );
  }
  const projected = {
    ...fixtureCase,
    category: 'success',
    expected: resultProjection(result),
  };
  harness.controller.destroy();
  return projected;
}

async function createHarness(options = {}) {
  const playerCompute = fakePlayerCompute();
  const controller = new CrowdyStudioController({
    projectProvider: fakeProjectProvider(),
    playerCompute,
    appId: '42',
    gridId: '500',
    initialProjectId: 'project-1',
    sleep: async () => {},
  });
  await controller.initialize();
  if (options.diagnostics) {
    controller.setLocalDiagnostics([
      {
        target: 'SERVER',
        path: 'src/lib.rs',
        line: 3,
        column: 5,
        severity: 'warning',
        code: 'unused',
        message: 'unused value',
        source: 'local-advisory',
      },
    ]);
  }
  const baseHandlers = createCrowdyStudioAgentTools(controller, {
    getClientEpoch: () => '1',
    getContextVersion: () => 'context-1',
    getLeaseKinds: () => ['WORKSPACE', 'PLAY'],
    getHostCapabilityRevision: () => 'capability-1',
    isLeaseActive: (leaseId, kind) =>
      (kind === 'WORKSPACE' && leaseId === 'workspace-lease') ||
      (kind === 'PLAY' && leaseId === 'play-lease'),
  });
  const handlers = options.transformHandlers
    ? options.transformHandlers(baseHandlers)
    : baseHandlers;
  const dispatcher = new CrowdyAgentBrowserToolDispatcher({
    registry: CROWDY_AGENT_TOOL_REGISTRY_V1,
    handlers,
    getSessionId: () => 'session-1',
    getClientEpoch: () => '1',
    getContextVersion: () => 'context-1',
    getMode: () => 'BUILD',
    now: () => fixtureNow,
  });
  return { controller, dispatcher, playerCompute };
}

async function dispatch(harness, operation) {
  const entry = CROWDY_AGENT_TOOL_REGISTRY_V1.require(
    operation.tool,
    '1.0.0',
  );
  return harness.dispatcher.dispatch({
    protocolVersion: 'crowdy.tool-call/1',
    sessionId: 'session-1',
    runId: 'run-1',
    toolCallId:
      operation.toolCallId ??
      `fixture-${operation.tool}-${nextToolCallSequence++}`,
    name: operation.tool,
    version: '1.0.0',
    descriptorDigest: entry.descriptorDigest,
    arguments: clone(operation.input),
    argumentHash: `sha256:${'c'.repeat(64)}`,
    contextVersion: 'context-1',
    clientEpoch: '1',
    ...(operation.leaseId ? { leaseId: operation.leaseId } : {}),
    ...(operation.approvalGrant
      ? { approvalGrant: operation.approvalGrant }
      : {}),
    deadline: new Date(
      fixtureNow + (operation.deadlineMs ?? 120_000),
    ).toISOString(),
  });
}

function resultProjection(result) {
  return {
    status: result.status,
    ...(result.output !== undefined ? { output: clone(result.output) } : {}),
    ...(result.error ? { errorCode: result.error.code } : {}),
  };
}

function fakeProjectProvider() {
  const projects = [project('project-1', '1'), project('project-2', '2')];
  const selected = (projectId) => {
    const value = projects.find((entry) => entry.projectId === projectId);
    if (!value) throw new Error(`unknown fixture project ${projectId}`);
    return clone(value);
  };
  return {
    async listProjects() {
      return projects.map((value) => ({
        projectId: value.projectId,
        gridId: value.gridId,
        name: value.metadata.name,
        kind: value.kind,
        revisionId: value.revision.id,
        serverModuleName: value.metadata.serverModuleName,
        updatedAt: value.updatedAt,
      }));
    },
    async getProject(input) {
      return selected(input.projectId);
    },
    async createProject() {
      return selected('project-1');
    },
    async saveProject(input) {
      return selected(input.projectId);
    },
    async listPersonalLibraryFiles() {
      return [];
    },
    async listCommonFiles() {
      return [];
    },
    async importReferenceFile(input) {
      return selected(input.projectId);
    },
    async savePersonalLibraryFile() {
      throw new Error('not used by Studio host fixtures');
    },
  };
}

function fakePlayerCompute() {
  const calls = [];
  return {
    calls,
    async deploy(input) {
      calls.push({ kind: 'deploy', input: clone(input) });
      return { versionId: input.draft ? 'draft-v1' : 'live-v1' };
    },
    async versions() {
      const lastDeploy = [...calls]
        .reverse()
        .find(({ kind }) => kind === 'deploy');
      return [
        {
          versionId: lastDeploy?.input.draft ? 'draft-v1' : 'live-v1',
          compileStatus: 'succeeded',
          compileLog: null,
        },
      ];
    },
    async setEnabled(input) {
      calls.push({ kind: 'enabled', input: clone(input) });
      return {};
    },
    async setRequires() {
      return true;
    },
    async artifactBytes() {
      throw new Error('not used by server-only Studio host fixtures');
    },
    async usage() {
      return {
        hourUnitsUsed: '1',
        dayUnitsUsed: '2',
        unitsPerHour: '100',
        unitsPerDay: '1000',
        compilesThisHour: 1,
        maxCompilesPerHour: 20,
        gateStatus: 'active',
        gateReason: null,
      };
    },
    async runs() {
      return [];
    },
    async logs() {
      return [];
    },
    async invoke(input) {
      calls.push({ kind: 'invoke', input: clone(input) });
      return {
        resultJson: '{"ok":true}',
        fuelUsed: '4',
        durationUs: 2,
      };
    },
  };
}

function project(projectId, revision) {
  return {
    projectId,
    appId: '42',
    ownerUserId: '7',
    gridId: '500',
    kind: 'SERVER',
    metadata: {
      name: projectId === 'project-1' ? 'Fixture Studio' : 'Second project',
      description:
        projectId === 'project-1' ? 'Cross-SDK projection fixture' : undefined,
      serverModuleName:
        projectId === 'project-1' ? 'fixture-server' : 'second-server',
      pairingPreference: 'NONE',
    },
    files: [
      {
        target: 'SERVER',
        path: 'Cargo.toml',
        content: '[package]\nname = "fixture"\n',
      },
      {
        target: 'SERVER',
        path: 'src/lib.rs',
        content: 'pub fn invoke() {}',
      },
    ],
    sdkVersion: '0.1.5',
    abiVersion: 0,
    revision: {
      id: revision,
      savedAt: '2026-07-24T00:00:00.000Z',
    },
    fileCount: 2,
    totalBytes: '46',
    createdAt: '2026-07-24T00:00:00.000Z',
    updatedAt:
      projectId === 'project-1'
        ? '2026-07-24T00:00:00.000Z'
        : '2026-07-24T00:00:01.000Z',
  };
}

function fileInput() {
  return {
    source: 'PROJECT',
    target: 'SERVER',
    path: 'Cargo.toml',
  };
}

function draftInput() {
  return {
    expectedRevision: '1',
    targets: ['SERVER'],
  };
}

function liveInput(controller) {
  return {
    expectedRevision: '1',
    projectContentHash: controller.getAgentContext().projectContentHash,
    targets: ['SERVER'],
    pairingPreference: 'NONE',
    draft: false,
  };
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
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

