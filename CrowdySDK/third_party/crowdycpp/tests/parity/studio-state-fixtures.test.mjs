import assert from 'node:assert/strict';
import { resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { resolveCrowdyJsPath } from '../../tools/parity/crowdyjs-path.mjs';

const repo = resolve(import.meta.dirname, '..', '..');
const crowdyjs = resolveCrowdyJsPath(repo);

test('shared Studio state fixtures match current CrowdyJS', () => {
  const result = spawnSync(
    process.execPath,
    [
      'tools/parity/studio-state-fixtures.mjs',
      '--crowdyjs',
      crowdyjs,
    ],
    {
      cwd: repo,
      encoding: 'utf8',
    },
  );
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(result.stdout, /Studio state fixtures match CrowdyJS/u);
});
