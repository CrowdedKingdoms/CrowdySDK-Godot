import assert from 'node:assert/strict';
import {
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import {
  assertCrowdyJsParityTarget,
  resolveCrowdyJsPath,
} from '../../tools/parity/crowdyjs-path.mjs';

const repo = resolve(import.meta.dirname, '..', '..');
const crowdyjs = resolveCrowdyJsPath(repo);

test('pinned strict parity target and generated matrix pass', () => {
  assert.deepEqual(assertCrowdyJsParityTarget(repo, crowdyjs), {
    version: '15.0.0',
    commit: '2b0a5a5aa269c3822a5db8fdde689519bee5fe86',
  });
  const result = runParity(
    '--check',
    'docs/parity-matrix.md',
    '--strict',
  );
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(result.stdout, /unclassified=0 stale=0/u);
});

test('strict mode accepts the zero-gap implementation', () => {
  const result = runParity('--strict');
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(result.stdout, /portable gaps=0/u);
});

test('cross-cutting Studio exports stay explicitly audited', () => {
  const matrix = readFileSync(
    join(repo, 'docs', 'parity-matrix.md'),
    'utf8',
  );
  assert.match(
    matrix,
    /layout\.ts#StudioLayoutController` \| native equivalent/u,
  );
  assert.match(
    matrix,
    /control-gate\.ts#PlayerControlGate` \| native equivalent/u,
  );
  assert.match(
    matrix,
    /editor\.ts#CrowdyStudioEditorAdapter` \| native equivalent/u,
  );
  assert.match(matrix, /mount\.ts#mountCrowdyStudio` \| browser exclusion/u);
  assert.match(
    matrix,
    /styles\.ts#CROWDY_STUDIO_STYLES` \| browser exclusion/u,
  );
  assert.match(matrix, /`embed-focus-trap`: browser exclusion/u);
  assert.match(matrix, /`player-glue-worker-package`: browser exclusion/u);
});

test('unused method aliases are stale gate failures', () => {
  const result = runMutatedParity((source) =>
    source.replace(
      'const METHOD_ALIASES = {',
      "const METHOD_ALIASES = {\n  'RemovedParityClass.removed': 'missing',",
    ),
  );
  assert.equal(result.status, 1, result.stderr || result.stdout);
  assert.match(result.stdout, /stale=1/u);
});

test('portable editor surfaces cannot regress to browser exclusions', () => {
  const result = runMutatedParity((source) =>
    source.replace(
      /('src\/crowdy-studio\/editor\.ts': exportModule\([\s\S]*?\n\s+\],\n\s+)CATEGORY\.NATIVE,/u,
      '$1CATEGORY.BROWSER,',
    ),
  );
  assert.equal(result.status, 1, result.stderr || result.stdout);
  assert.match(result.stdout, /unclassified=1/u);
});

test('matrix drift is a gate failure', () => {
  const directory = mkdtempSync(join(tmpdir(), 'crowdycpp-parity-'));
  const stale = join(directory, 'parity-matrix.md');
  try {
    writeFileSync(stale, '# stale\n');
    const result = runParity('--check', stale);
    assert.equal(result.status, 1, result.stderr || result.stdout);
    assert.match(result.stderr, /is stale/u);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test('CrowdyJS resolver fails clearly when no checkout exists', () => {
  const directory = mkdtempSync(join(tmpdir(), 'crowdycpp-no-js-'));
  const configured = process.env.CROWDYJS_PATH;
  try {
    delete process.env.CROWDYJS_PATH;
    assert.throws(
      () => resolveCrowdyJsPath(directory),
      /Set CROWDYJS_PATH.*Checked:/u,
    );
  } finally {
    if (configured === undefined) delete process.env.CROWDYJS_PATH;
    else process.env.CROWDYJS_PATH = configured;
    rmSync(directory, { recursive: true, force: true });
  }
});

function runParity(...extra) {
  return spawnSync(
    process.execPath,
    [
      'tools/parity/parity.mjs',
      '--crowdyjs',
      crowdyjs,
      ...extra,
    ],
    {
      cwd: repo,
      encoding: 'utf8',
    },
  );
}

function runMutatedParity(mutate) {
  const directory = mkdtempSync(join(repo, '.parity-mutation-'));
  try {
    const original = readFileSync(
      join(repo, 'tools', 'parity', 'parity.mjs'),
      'utf8',
    );
    const rooted = mutate(original).replace(
      "const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');",
      `const root = ${JSON.stringify(repo)};`,
    );
    writeFileSync(join(directory, 'parity.mjs'), rooted);
    for (const dependency of ['schema-surface.mjs', 'crowdyjs-path.mjs']) {
      writeFileSync(
        join(directory, dependency),
        readFileSync(join(repo, 'tools', 'parity', dependency), 'utf8'),
      );
    }
    return spawnSync(
      process.execPath,
      [
        join(directory, 'parity.mjs'),
        '--crowdyjs',
        crowdyjs,
        '--strict',
      ],
      {
        cwd: repo,
        encoding: 'utf8',
      },
    );
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
}
