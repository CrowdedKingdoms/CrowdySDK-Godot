#!/usr/bin/env node
/**
 * Blueprint equivalence diff: compare the JSON dumps of every blueprint
 * builder from CrowdyJS (dump-blueprints.mjs) and CrowdyCPP
 * (dump_blueprints.cpp), normalized:
 *   - object keys sorted
 *   - embedded *Json strings parsed into structures
 *   - undefined/omitted vs empty-array differences ignored
 *
 * Usage: node tools/parity/blueprints-diff.mjs <js-dump.json> <cpp-dump.json>
 */
import { readFileSync } from 'node:fs';

const [jsPath, cppPath] = process.argv.slice(2);
if (!jsPath || !cppPath) {
  console.error('usage: blueprints-diff.mjs <js-dump.json> <cpp-dump.json>');
  process.exit(2);
}

const JSON_STRING_KEYS = new Set([
  'invokePolicyJson', 'selectorJson', 'paramsJson', 'defaultValueJson', 'valueJson',
  'propertiesJson', 'permissionEffectsJson', 'metadataJson', 'filterJson',
]);

function normalize(value, keyHint = '') {
  if (typeof value === 'string' && JSON_STRING_KEYS.has(keyHint)) {
    try {
      return { __json: normalize(JSON.parse(value)) };
    } catch {
      return value;
    }
  }
  if (Array.isArray(value)) return value.map((v) => normalize(v));
  if (value && typeof value === 'object') {
    const out = {};
    for (const key of Object.keys(value).sort()) {
      const v = value[key];
      if (v === undefined || v === null) continue;
      if (Array.isArray(v) && v.length === 0) continue;
      out[key] = normalize(v, key);
    }
    return out;
  }
  return value;
}

function diff(a, b, path, out) {
  const na = JSON.stringify(a);
  const nb = JSON.stringify(b);
  if (na === nb) return;
  if (a && b && typeof a === 'object' && typeof b === 'object' &&
      Array.isArray(a) === Array.isArray(b)) {
    const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
    for (const key of keys) diff(a[key], b[key], `${path}.${key}`, out);
    return;
  }
  out.push(`${path}: js=${na ?? 'undefined'} cpp=${nb ?? 'undefined'}`);
}

const js = normalize(JSON.parse(readFileSync(jsPath, 'utf8')));
const cpp = normalize(JSON.parse(readFileSync(cppPath, 'utf8')));

let failures = 0;
for (const variant of new Set([...Object.keys(js), ...Object.keys(cpp)])) {
  const problems = [];
  diff(js[variant], cpp[variant], variant, problems);
  if (problems.length === 0) {
    console.log(`OK   ${variant}`);
  } else {
    failures++;
    console.log(`DIFF ${variant} (${problems.length}):`);
    for (const p of problems.slice(0, 12)) console.log(`  ${p}`);
    if (problems.length > 12) console.log(`  ... ${problems.length - 12} more`);
  }
}
process.exit(failures === 0 ? 0 : 1);
