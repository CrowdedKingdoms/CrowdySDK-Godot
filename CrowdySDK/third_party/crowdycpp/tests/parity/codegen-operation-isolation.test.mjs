import test from 'node:test';
import assert from 'node:assert/strict';
import {
  readFileSync,
  readdirSync,
  statSync,
} from 'node:fs';
import { basename, join, resolve } from 'node:path';
import {
  buildSchema,
  Kind,
  parse,
  validate,
} from 'graphql';

const root = resolve(import.meta.dirname, '..', '..');
const generated = readFileSync(
  join(root, 'include/crowdy/generated/operations.hpp'),
  'utf8',
);
const schema = buildSchema(readFileSync(join(root, 'schema.gql'), 'utf8'));

function document(name) {
  const matches = [...generated.matchAll(
    new RegExp(
      `k${name}IsolatedDocument = R"gql\\(([\\s\\S]*?)\\)gql";`,
      'gu',
    ),
  )];
  assert.equal(
    matches.length,
    1,
    `expected one generated document constant for ${name}`,
  );
  return matches[0][1];
}

test('every generated operation is isolated and valid against the schema', () => {
  const contracts = operationContracts();
  assert.ok(contracts.length > 0);
  assert.equal(
    new Set(contracts.map(({ name }) => name)).size,
    contracts.length,
    'operation names must be globally unique so generated constants cannot collide',
  );

  for (const contract of contracts) {
    const text = document(contract.name);
    const parsed = parse(text, { noLocation: true });
    const operations = parsed.definitions.filter(
      (definition) => definition.kind === Kind.OPERATION_DEFINITION,
    );
    assert.equal(
      operations.length,
      1,
      `${contract.name} generated a non-isolated document`,
    );
    assert.equal(operations[0].name?.value, contract.name);

    assert.deepEqual(
      validate(schema, parsed).map((error) => error.message),
      [],
      `${contract.domain}/${contract.file}:${contract.name} is invalid against schema.gql`,
    );

    const dispatchMatches = [...generated.matchAll(
      new RegExp(
        `operationName == "${contract.name}"\\) return ` +
          `k${contract.name}IsolatedDocument;`,
        'gu',
      ),
    )];
    assert.equal(
      dispatchMatches.length,
      1,
      `${contract.name} documentFor dispatch is missing or duplicated`,
    );
  }
});

test('C++ callers cannot send multi-operation file documents', () => {
  const multiOperationConstants = new Set(
    operationFiles()
      .filter(({ operations }) => operations.length > 1)
      .map(
        ({ domain, file }) =>
          `gen::${domain}::k${basename(file, '.graphql')}Document`,
      ),
  );
  const violations = [];
  for (const path of [
    ...filesBelow(join(root, 'include')),
    ...filesBelow(join(root, 'src')),
    ...filesBelow(join(root, 'tests')),
  ]) {
    if (!/\.(?:cpp|hpp)$/u.test(path) || path.includes('/generated/')) continue;
    const source = readFileSync(path, 'utf8');
    for (const constant of multiOperationConstants) {
      if (source.includes(constant)) {
        violations.push(`${path.slice(root.length + 1)} uses ${constant}`);
      }
    }
  }
  assert.deepEqual(violations, []);
});

// This gate used to check that each wrapper routed an operation to a plane the
// operation actually validated against. There is one plane now, so that check
// could no longer fail for the reason it existed. What is worth refusing today
// is the RETURN of a second plane: a management client, endpoint, or helper
// reappearing inside a domain wrapper means some caller has been handed back
// the question of which origin to talk to, which is the thing 0.20.0 removed.
test('no domain wrapper reintroduces a second endpoint plane', () => {
  const offenders = [];
  const banned = [
    /\bmanagement_\b/u,
    /\bmanagementGql\b/u,
    /\bmanagementClient\b/u,
    /\bmanagementUrl\b/u,
    /\bmanagementSubscriptions\b/u,
    /\bmanagementInput(?:Async)?\s*\(/u,
    /\bmanagement(?:Async)?\s*\(\s*"/u,
  ];
  const wrappers = filesBelow(join(root, 'include', 'crowdy', 'domains'))
    .filter((path) => path.endsWith('.hpp'));
  assert.ok(wrappers.length > 0, 'found no domain wrappers to inspect');
  for (const path of wrappers) {
    const source = readFileSync(path, 'utf8');
    for (const pattern of banned) {
      if (pattern.test(source)) {
        offenders.push(`${path.slice(root.length + 1)} matches ${pattern}`);
      }
    }
  }
  assert.deepEqual(offenders, []);
});

function operationContracts() {
  const contracts = [];
  for (const { domain, file, document: source } of operationFiles()) {
    const fragments = new Map(
      source.definitions
        .filter((definition) => definition.kind === Kind.FRAGMENT_DEFINITION)
        .map((definition) => [definition.name.value, definition]),
    );
    for (const operation of source.definitions.filter(
      (definition) => definition.kind === Kind.OPERATION_DEFINITION,
    )) {
      const names = new Set();
      collectSpreads(operation, names);
      const selected = [];
      const pending = [...names];
      while (pending.length > 0) {
        const name = pending.shift();
        if (selected.some((fragment) => fragment.name.value === name)) continue;
        const fragment = fragments.get(name);
        assert.ok(fragment, `${domain}/${file} is missing fragment ${name}`);
        selected.push(fragment);
        const nested = new Set();
        collectSpreads(fragment, nested);
        pending.push(...nested);
      }
      contracts.push({
        domain,
        file,
        name: operation.name.value,
      });
    }
  }
  return contracts;
}

function operationFiles() {
  const files = [];
  const operations = join(root, 'operations');
  for (const domain of readdirSync(operations).sort()) {
    const directory = join(operations, domain);
    if (!statSync(directory).isDirectory()) continue;
    for (const file of readdirSync(directory).sort()) {
      if (!file.endsWith('.graphql')) continue;
      const document = parse(readFileSync(join(directory, file), 'utf8'), {
        noLocation: true,
      });
      files.push({
        domain,
        file,
        document,
        operations: document.definitions.filter(
          (definition) => definition.kind === Kind.OPERATION_DEFINITION,
        ),
      });
    }
  }
  return files;
}

function collectSpreads(node, names) {
  if (!node || typeof node !== 'object') return;
  if (node.kind === Kind.FRAGMENT_SPREAD) names.add(node.name.value);
  for (const value of Object.values(node)) {
    if (Array.isArray(value)) {
      for (const entry of value) collectSpreads(entry, names);
    } else {
      collectSpreads(value, names);
    }
  }
}

function filesBelow(directory, out = []) {
  for (const entry of readdirSync(directory).sort()) {
    const path = join(directory, entry);
    if (statSync(path).isDirectory()) filesBelow(path, out);
    else out.push(path);
  }
  return out;
}
