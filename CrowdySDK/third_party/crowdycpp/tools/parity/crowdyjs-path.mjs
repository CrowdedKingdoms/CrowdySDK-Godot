import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';

function isCrowdyJsCheckout(path) {
  return existsSync(join(path, 'schema.gql')) &&
    existsSync(join(path, 'package.json'));
}

/**
 * Resolve the pinned CrowdyJS checkout without weakening parity.
 *
 * An explicit CLI path or CROWDYJS_PATH is authoritative and must be valid.
 * Otherwise normal sibling, CI child-checkout, and git-worktree sibling
 * layouts are checked in deterministic order.
 */
export function resolveCrowdyJsPath(repoRoot, explicitPath = null) {
  const configured = explicitPath ?? process.env.CROWDYJS_PATH;
  if (configured) {
    const path = resolve(repoRoot, configured);
    if (isCrowdyJsCheckout(path)) return path;
    throw new Error(
      `Configured CrowdyJS checkout is invalid: ${path}. ` +
        'Expected schema.gql and package.json.',
    );
  }

  const candidates = [
    resolve(repoRoot, '..', 'CrowdyJS'),
    resolve(repoRoot, 'CrowdyJS'),
  ];
  try {
    const commonDirectory = execFileSync(
      'git',
      ['rev-parse', '--path-format=absolute', '--git-common-dir'],
      { cwd: repoRoot, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] },
    ).trim();
    candidates.push(join(dirname(dirname(commonDirectory)), 'CrowdyJS'));
  } catch {
    // Non-git source archives still use the normal sibling/child layouts.
  }

  const checked = [...new Set(candidates.map((path) => resolve(path)))];
  const found = checked.find(isCrowdyJsCheckout);
  if (found) return found;
  throw new Error(
    'CrowdyJS checkout not found. Set CROWDYJS_PATH or place CrowdyJS ' +
      `beside this checkout. Checked: ${checked.join(', ')}`,
  );
}

/**
 * Require parity tools to run against the version and exact commit declared
 * by CrowdyCPP's package metadata. This prevents a same-version moving branch
 * from silently becoming the release target.
 */
export function assertCrowdyJsParityTarget(repoRoot, crowdyJsPath) {
  const manifest = JSON.parse(
    readFileSync(join(repoRoot, 'package.json'), 'utf8'),
  );
  const target = manifest.crowdyjsParityTarget;
  if (
    !target ||
    typeof target.version !== 'string' ||
    typeof target.commit !== 'string' ||
    !/^[0-9a-f]{40}$/u.test(target.commit)
  ) {
    throw new Error(
      'package.json must declare crowdyjsParityTarget.version and its full commit SHA',
    );
  }

  const candidate = JSON.parse(
    readFileSync(join(crowdyJsPath, 'package.json'), 'utf8'),
  );
  let commit;
  let trackedChanges;
  try {
    commit = execFileSync('git', ['rev-parse', 'HEAD'], {
      cwd: crowdyJsPath,
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    }).trim();
    trackedChanges = execFileSync(
      'git',
      ['status', '--porcelain', '--untracked-files=no'],
      {
        cwd: crowdyJsPath,
        encoding: 'utf8',
        stdio: ['ignore', 'pipe', 'ignore'],
      },
    ).trim();
  } catch {
    throw new Error(
      `CrowdyJS parity checkout is not a readable git checkout: ${crowdyJsPath}`,
    );
  }

  if (candidate.version !== target.version || commit !== target.commit) {
    throw new Error(
      `CrowdyJS parity target mismatch: expected ${target.version} at ` +
        `${target.commit}, received ${candidate.version ?? 'unknown'} at ${commit}`,
    );
  }
  if (trackedChanges) {
    throw new Error(
      `CrowdyJS parity target has tracked changes at ${crowdyJsPath}; ` +
        'fixture and parity evidence must come from the exact committed tree',
    );
  }
  return target;
}
