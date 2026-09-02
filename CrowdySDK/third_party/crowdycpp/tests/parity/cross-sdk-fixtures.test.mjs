import assert from 'node:assert/strict';
import { resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { resolveCrowdyJsPath } from '../../tools/parity/crowdyjs-path.mjs';

const repo = resolve(import.meta.dirname, '..', '..');
const crowdyjs = resolveCrowdyJsPath(repo);

test('shared player control-gate fixture matches pinned CrowdyJS', () => {
  const result = run('tools/parity/control-gate-fixtures.mjs');
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(
    result.stdout,
    /Player control-gate fixture matches CrowdyJS: 14 hooks, 16 reasons/u,
  );
});

test('shared Studio host fixture covers all native tools and statuses', () => {
  const result = run('tools/parity/studio-host-fixtures.mjs');
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(
    result.stdout,
    /Crowdy Studio host fixture matches CrowdyJS: 11 tools, 14 cases/u,
  );
});

function run(script) {
  return spawnSync(
    process.execPath,
    [script, '--crowdyjs', crowdyjs],
    {
      cwd: repo,
      encoding: 'utf8',
    },
  );
}
