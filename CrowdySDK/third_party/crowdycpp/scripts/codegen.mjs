#!/usr/bin/env node
/**
 * Generate the committed C++ GraphQL surface from:
 *   - operations/<domain>/*.graphql  ->  src/generated/operations.hpp
 *   - schema.gql (enums)             ->  src/generated/enums.hpp
 *
 * Output is committed so external builds never run this script.
 * Usage: node scripts/codegen.mjs
 */
import {
  existsSync,
  readFileSync,
  writeFileSync,
  readdirSync,
  statSync,
  mkdirSync,
} from 'node:fs';
import { createHash } from 'node:crypto';
import { resolve, dirname, join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildSchema, Kind, parse, print, validate } from 'graphql';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const opsDir = join(root, 'operations');
const outDir = join(root, 'include', 'crowdy', 'generated');
mkdirSync(outDir, { recursive: true });
const check = process.argv.slice(2).includes('--check');
const unknownArgs = process.argv.slice(2).filter((argument) => argument !== '--check');
if (unknownArgs.length > 0) {
  throw new Error(`unknown argument: ${unknownArgs[0]}`);
}

const schema = readFileSync(join(root, 'schema.gql'), 'utf8');
const apiSchema = buildSchema(schema);
const operationInputs = [];
for (const domain of listDomains()) {
  for (const file of readdirSync(join(opsDir, domain)).filter((entry) =>
    entry.endsWith('.graphql')).sort()) {
    operationInputs.push({
      path: `${domain}/${file}`,
      text: readFileSync(join(opsDir, domain, file), 'utf8'),
    });
  }
}
const digest = (value) => createHash('sha256').update(value).digest('hex');
const schemaDigest = digest(schema);
const operationsDigest = digest(
  operationInputs.map(({ path, text }) => `${path}\0${text}\0`).join(''),
);

const HEADER = `// GENERATED FILE — do not edit by hand.
// Regenerate with: node scripts/codegen.mjs
// Inputs: operations/**/*.graphql and schema.gql (synced from the published
// SDL at https://docs.crowdedkingdoms.com/schema/game-api.graphql).
// schema.gql sha256: ${schemaDigest}
// operations sha256: ${operationsDigest}
`;

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

function listDomains() {
  return readdirSync(opsDir)
    .filter((d) => statSync(join(opsDir, d)).isDirectory())
    .sort();
}

function operationsInFile(text, sourcePath) {
  const document = parse(text, { noLocation: true });
  const fragments = new Map(
    document.definitions
      .filter((definition) => definition.kind === Kind.FRAGMENT_DEFINITION)
      .map((definition) => [definition.name.value, definition]),
  );
  const collectSpreads = (node, names) => {
    if (!node || typeof node !== 'object') return;
    if (node.kind === Kind.FRAGMENT_SPREAD) names.add(node.name.value);
    for (const value of Object.values(node)) {
      if (Array.isArray(value)) {
        for (const entry of value) collectSpreads(entry, names);
      } else {
        collectSpreads(value, names);
      }
    }
  };
  return document.definitions
    .filter((definition) => definition.kind === Kind.OPERATION_DEFINITION)
    .map((operation) => {
      if (!operation.name) throw new Error('anonymous operations are not supported');
      const names = new Set();
      collectSpreads(operation, names);
      const selected = [];
      const pending = [...names];
      while (pending.length > 0) {
        const name = pending.shift();
        if (selected.some((fragment) => fragment.name.value === name)) continue;
        const fragment = fragments.get(name);
        if (!fragment) throw new Error(`operation ${operation.name.value}: missing fragment ${name}`);
        selected.push(fragment);
        const nested = new Set();
        collectSpreads(fragment, nested);
        pending.push(...nested);
      }
      const isolatedDocument = {
        kind: Kind.DOCUMENT,
        definitions: [operation, ...selected],
      };
      const errors = validate(apiSchema, isolatedDocument);
      if (errors.length > 0) {
        throw new Error(
          `${sourcePath}:${operation.name.value} is invalid against ` +
            `schema.gql:\n  ${errors.map((error) => error.message).join('\n  ')}`,
        );
      }
      return {
        kind: operation.operation,
        name: operation.name.value,
        document: print(isolatedDocument),
      };
    });
}

let opsHpp = `${HEADER}
#pragma once

#include <string_view>

/// GraphQL operation documents, one namespace per domain. File constants are
/// retained for compatibility; operation constants contain only that operation
/// and its transitive fragments so unrelated roots cannot invalidate a request.
namespace crowdy::gen {

`;

const manifest = [];
const globalOperationNames = new Map();
for (const domain of listDomains()) {
  opsHpp += `namespace ${domain} {\n\n`;
  const domainOps = [];
  const seenOperationNames = new Set();
  const generatedSymbols = new Map();
  const reserveSymbol = (symbol, origin) => {
    const previous = generatedSymbols.get(symbol);
    if (previous) {
      throw new Error(
        `generated C++ symbol collision ${domain}.${symbol}: ` +
          `${previous} and ${origin}`,
      );
    }
    generatedSymbols.set(symbol, origin);
  };
  const files = readdirSync(join(opsDir, domain))
    .filter((f) => f.endsWith('.graphql'))
    .sort();
  for (const file of files) {
    const text = readFileSync(join(opsDir, domain, file), 'utf8').trim();
    const sourcePath = `${domain}/${file}`;
    const ops = operationsInFile(text, sourcePath);
    if (ops.length === 0) continue;
    const base = basename(file, '.graphql');
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(base)) {
      throw new Error(`${sourcePath}: file base is not a C++ identifier`);
    }
    // Guard against raw-string delimiter collisions.
    if (text.includes(')gql"')) throw new Error(`raw-string delimiter clash in ${file}`);
    reserveSymbol(`k${base}Document`, sourcePath);
    opsHpp += `/// ${domain}/${file}\n`;
    opsHpp += `inline constexpr std::string_view k${base}Document = R"gql(${text})gql";\n`;
    for (const op of ops) {
      const previousGlobal = globalOperationNames.get(op.name);
      if (previousGlobal) {
        throw new Error(
          `duplicate generated operation name ${op.name}: ` +
            `${previousGlobal} and ${sourcePath}`,
        );
      }
      globalOperationNames.set(op.name, sourcePath);
      if (seenOperationNames.has(op.name)) {
        throw new Error(`duplicate operation ${domain}.${op.name}`);
      }
      seenOperationNames.add(op.name);
      if (op.document.includes(')gql"')) {
        throw new Error(`raw-string delimiter clash in ${file}:${op.name}`);
      }
      reserveSymbol(
        `k${op.name}IsolatedDocument`,
        `${sourcePath}:${op.name}`,
      );
      reserveSymbol(
        `k${op.name}OperationName`,
        `${sourcePath}:${op.name}`,
      );
      opsHpp += `inline constexpr std::string_view k${op.name}IsolatedDocument = R"gql(${op.document})gql";\n`;
      opsHpp += `inline constexpr std::string_view k${op.name}OperationName = "${op.name}";\n`;
      manifest.push({ domain, file, ...op });
      domainOps.push(op);
    }
    opsHpp += '\n';
  }
  opsHpp += `inline constexpr std::string_view documentFor(std::string_view operationName) {\n`;
  for (const op of domainOps) {
    opsHpp += `  if (operationName == "${op.name}") return k${op.name}IsolatedDocument;\n`;
  }
  opsHpp += `  return {};\n}\n\n`;
  opsHpp += `}  // namespace ${domain}\n\n`;
}
opsHpp += '}  // namespace crowdy::gen\n';
emit(join(outDir, 'operations.hpp'), opsHpp);

// ---------------------------------------------------------------------------
// Enums from schema.gql
// ---------------------------------------------------------------------------

const enums = [];
{
  const re = /(?:^|\n)enum\s+([A-Za-z0-9_]+)\s*\{([\s\S]*?)\n\}/g;
  let m;
  while ((m = re.exec(schema)) !== null) {
    const name = m[1];
    // Strip block docstrings and single-line strings before extracting values.
    const body = m[2].replace(/"""[\s\S]*?"""/g, ' ').replace(/"[^"\n]*"/g, ' ');
    const values = [];
    for (const rawLine of body.split('\n')) {
      const line = rawLine.trim();
      if (!line) continue;
      const vm = line.match(/^([A-Za-z_][A-Za-z0-9_]*)\s*$/);
      if (vm) values.push(vm[1]);
    }
    if (values.length > 0 && new Set(values).size === values.length) {
      enums.push({ name, values });
    } else if (values.length > 0) {
      throw new Error(`enum ${name}: duplicate or malformed values parsed: ${values}`);
    }
  }
}

// C++ keyword-safe identifier for an enum value.
const CPP_KEYWORDS = new Set(['default', 'delete', 'new', 'private', 'public', 'protected',
  'register', 'template', 'this', 'true', 'false', 'nullptr', 'operator', 'export', 'import']);
const cppId = (v) => (CPP_KEYWORDS.has(v) || /^\d/.test(v) ? `${v}_` : v);

let enumsHpp = `${HEADER}
#pragma once

#include <optional>
#include <string_view>

/// GraphQL enums from the published schema. Values keep their wire spelling;
/// toString/fromString convert between the enum and the GraphQL string.
namespace crowdy::gen {

`;
for (const e of enums.sort((a, b) => a.name.localeCompare(b.name))) {
  enumsHpp += `enum class ${e.name} {\n`;
  for (const v of e.values) enumsHpp += `  ${cppId(v)},\n`;
  enumsHpp += `};\n\n`;
  enumsHpp += `inline constexpr std::string_view toString(${e.name} v) {\n  switch (v) {\n`;
  for (const v of e.values) enumsHpp += `    case ${e.name}::${cppId(v)}: return "${v}";\n`;
  enumsHpp += `  }\n  return "";\n}\n\n`;
  const fn = `${e.name[0].toLowerCase()}${e.name.slice(1)}FromString`;
  enumsHpp += `inline std::optional<${e.name}> ${fn}(std::string_view s) {\n`;
  for (const v of e.values)
    enumsHpp += `  if (s == "${v}") return ${e.name}::${cppId(v)};\n`;
  enumsHpp += `  return std::nullopt;\n}\n\n`;
}
enumsHpp += '}  // namespace crowdy::gen\n';
emit(join(outDir, 'enums.hpp'), enumsHpp);

console.log(
  `${check ? 'checked' : 'generated'} ${manifest.length} operations across ` +
  `${listDomains().length} domains, ` +
  `${enums.length} enums -> include/crowdy/generated/`,
);

function emit(path, content) {
  if (!check) {
    writeFileSync(path, content);
    return;
  }
  if (!existsSync(path) || readFileSync(path, 'utf8') !== content) {
    console.error(
      `${path.slice(root.length + 1)} has codegen drift; run node scripts/codegen.mjs`,
    );
    process.exitCode = 1;
  }
}
