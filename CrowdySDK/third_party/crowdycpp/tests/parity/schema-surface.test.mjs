import assert from 'node:assert/strict';
import test from 'node:test';
import {
  compareSchemaSurfaces,
  enumValues,
  parseSchemaSurface,
  rootFieldEntries,
} from '../../tools/parity/schema-surface.mjs';

test('parses a Query declared at byte zero', () => {
  const surface = parseSchemaSurface(`type Query {
    first(id: ID!): String!
    second: Int @deprecated(reason: "legacy")
  }
  schema { query: Query }
  `);
  assert.deepEqual(
    rootFieldEntries(surface, 'Query').map(({ name, deprecated }) => ({
      name,
      deprecated,
    })),
    [
      { name: 'first', deprecated: false },
      { name: 'second', deprecated: true },
    ],
  );
});

test('ignores definition and field declaration order', () => {
  const left = parseSchemaSurface(`
    type Query { alpha: Alpha beta: Int }
    type Alpha { z: String a: Boolean }
    enum Mode { B A }
    schema { query: Query }
  `);
  const right = parseSchemaSurface(`
    schema { query: Query }
    enum Mode { A B }
    type Alpha { a: Boolean z: String }
    type Query { beta: Int alpha: Alpha }
  `);
  assert.deepEqual(compareSchemaSurfaces(left, right), []);
});

test('reports additions in both directions and signature changes', () => {
  const cpp = parseSchemaSurface(`
    type Query { shared(limit: Int): String cppOnly: Int }
    type CppOnly { id: ID! }
  `);
  const js = parseSchemaSurface(`
    type Query { shared(limit: Int!): String jsOnly: Boolean }
    input JsOnlyInput { value: String! }
  `);
  const differences = compareSchemaSurfaces(cpp, js, 'CrowdyCPP', 'CrowdyJS');
  assert.deepEqual(
    differences.map(({ id, kind, side }) => ({ id, kind, side })),
    [
      {
        id: 'input:JsOnlyInput',
        kind: 'definition-only',
        side: 'CrowdyJS',
      },
      {
        id: 'type:CppOnly',
        kind: 'definition-only',
        side: 'CrowdyCPP',
      },
      {
        id: 'type:Query.cppOnly',
        kind: 'member-only',
        side: 'CrowdyCPP',
      },
      {
        id: 'type:Query.jsOnly',
        kind: 'member-only',
        side: 'CrowdyJS',
      },
      {
        id: 'type:Query.shared',
        kind: 'member-signature',
        side: 'both',
      },
    ],
  );
});

test('signatures include argument defaults, nullability, and return types', () => {
  const baseline = parseSchemaSurface(`
    type Query {
      value(limit: Int = 10, filter: String): [String!]!
    }
  `);
  const changedDefault = parseSchemaSurface(`
    type Query {
      value(limit: Int = 20, filter: String): [String!]!
    }
  `);
  const changedArgumentNullability = parseSchemaSurface(`
    type Query {
      value(limit: Int! = 10, filter: String): [String!]!
    }
  `);
  const changedReturnType = parseSchemaSurface(`
    type Query {
      value(limit: Int = 10, filter: String): [String]
    }
  `);

  for (const changed of [
    changedDefault,
    changedArgumentNullability,
    changedReturnType,
  ]) {
    assert.deepEqual(
      compareSchemaSurfaces(baseline, changed).map(({ id, kind }) => ({
        id,
        kind,
      })),
      [{ id: 'type:Query.value', kind: 'member-signature' }],
    );
  }
});

test('extracts enum vocabularies deterministically', () => {
  const surface = parseSchemaSurface(`
    enum Reason {
      ZETA
      ALPHA @deprecated(reason: "old")
    }
  `);
  assert.deepEqual(enumValues(surface, 'Reason'), ['ALPHA', 'ZETA']);
});

test('rejects conflicting duplicate members', () => {
  assert.throws(
    () =>
      parseSchemaSurface(`
        type Query { value: String }
        extend type Query { value: Int }
      `),
    /conflicting duplicate GraphQL member value/u,
  );
});
