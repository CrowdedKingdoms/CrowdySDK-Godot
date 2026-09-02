import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

import { resolveCrowdyJsPath } from '../../tools/parity/crowdyjs-path.mjs';

const root = resolve(import.meta.dirname, '..', '..');
const portalHpp = readFileSync(
  join(root, 'include/crowdy/domains/portal.hpp'),
  'utf8',
);

/**
 * The app-token selection is written out SEVEN times in portal.hpp: once as
 * kAppTokenFields and once inline in each of the six mint/refresh/exchange
 * operations, because the exec helpers take a string_view and the codebase does
 * not use macros in public headers.
 *
 * That duplication is exactly how `discoveryUrl` came to be missing from all
 * seven. Omitting a field there is not a compile error and not a server error:
 * the response simply arrives without it, and every read of it yields an empty
 * string. A client that cannot see discoveryUrl cannot re-discover, and the only
 * symptom is that it fails to recover from something it should have survived.
 */
const FIELD_LIST =
  /token gameTokenId appId expiresAt gameApiUrl gameApiWsUrl(?: \w+)* launchUrl/gu;

function selections(source) {
  return [...source.matchAll(FIELD_LIST)].map((match) =>
    match[0].trim().split(/\s+/u),
  );
}

test('every app-token selection in portal.hpp names the same fields', () => {
  const found = selections(portalHpp);
  // Seven: the constant plus the six inline copies. A drop below that means a
  // selection was reworded into a shape this gate can no longer see, which is
  // worse than a mismatch because it would silently stop being checked.
  assert.equal(
    found.length,
    7,
    `expected 7 app-token selections in portal.hpp, found ${found.length}`,
  );
  for (const fields of found) {
    assert.deepEqual(fields, found[0]);
  }
  assert.ok(
    found[0].includes('discoveryUrl'),
    'discoveryUrl must be selected: without it a client cannot re-discover',
  );
});

test('the selection matches CrowdyJS field for field', () => {
  const path = join(resolveCrowdyJsPath(root), 'src', 'domains', 'portal.ts');
  if (!existsSync(path)) {
    assert.fail(`CrowdyJS portal.ts not found at ${path}`);
  }
  const js = selections(readFileSync(path, 'utf8'));
  assert.ok(js.length > 0, 'found no app-token selection in CrowdyJS portal.ts');
  // Same fields, same order. Order matters only for the diff being readable,
  // but a difference in either direction means one SDK is reading a field the
  // other is not and the two will behave differently on the same server.
  assert.deepEqual(selections(portalHpp)[0], js[0]);
});
