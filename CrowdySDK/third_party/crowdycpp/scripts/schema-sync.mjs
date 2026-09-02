#!/usr/bin/env node
/**
 * Refresh the committed schema.gql from the published production SDL:
 *   https://docs.crowdedkingdoms.com/schema/game-api.graphql
 *
 * Maintainers only; external builds use the committed snapshot. Override the
 * source with --game <path|url>.
 *
 * There is ONE schema because there is one API at one origin. This script used
 * to keep a second snapshot, schema.management.gql, so codegen could label each
 * operation Management/Game/Both. That plane distinction is gone as of 0.20.0,
 * and keeping the snapshot would have been worse than useless: the published
 * management SDL is now DERIVED from the unified schema by filtering it to a
 * root-field allowlist, so it is a strict subset. Validating against a subset
 * answers "is this operation in the management docs tab", not "will the server
 * accept it" — and every operation outside the allowlist would have been
 * labelled Game by a check that had stopped describing a real endpoint.
 *
 * Requires the maintainer-only Node dependencies (`npm ci`). Normal CMake
 * builds never invoke this script.
 *
 * Usage: node scripts/schema-sync.mjs [--game <src>] [--check]
 */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mergeTypeDefs } from '@graphql-tools/merge';
import { print } from 'graphql';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const DEFAULTS = {
  game: 'https://docs.crowdedkingdoms.com/schema/game-api.graphql',
};

function parseArgs(argv) {
  const args = { ...DEFAULTS, check: false };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--game') args.game = argv[++i];
    else if (argv[i] === '--check') args.check = true;
    else throw new Error(`unknown argument: ${argv[i]}`);
  }
  return args;
}

async function load(src) {
  if (/^https?:\/\//.test(src)) {
    const res = await fetch(src);
    if (!res.ok) throw new Error(`fetch ${src} failed: HTTP ${res.status}`);
    return await res.text();
  }
  return readFileSync(src, 'utf8');
}

const args = parseArgs(process.argv);
const game = await load(args.game);

// Normalised rather than verbatim, so codegen's input is stable against the
// SDL's field ordering and description churn.
const normalised = print(mergeTypeDefs([game])) + '\n';
const snapshots = [
  {
    destination: resolve(root, 'schema.gql'),
    label: 'schema.gql',
    source: args.game,
    text: normalised,
  },
];
if (args.check) {
  const drifted = snapshots.filter(
    ({ destination, text }) =>
      !existsSync(destination) ||
      readFileSync(destination, 'utf8') !== text,
  );
  if (drifted.length > 0) {
    for (const { label, source } of drifted) {
      console.error(`${label} drifted from ${source}`);
    }
    console.error(
      'Run the same command without --check, then regenerate codegen.',
    );
    process.exitCode = 1;
  } else {
    console.log('schema.gql matches the supplied SDL');
  }
} else {
  for (const { destination, text } of snapshots) {
    writeFileSync(destination, text);
  }
  console.log(`schema.gql written from:\n  ${args.game}`);
}
