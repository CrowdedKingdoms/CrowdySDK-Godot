const DEFINITION_KINDS = new Set([
  'schema',
  'scalar',
  'type',
  'interface',
  'union',
  'enum',
  'input',
  'directive',
]);

const ROOT_TYPES = ['Query', 'Mutation', 'Subscription'];

/**
 * Parse the contract-bearing parts of a GraphQL SDL without third-party
 * packages. Definitions and fields are keyed by name, so harmless printer and
 * declaration-order differences do not look like schema drift.
 */
export function parseSchemaSurface(source) {
  const tokens = lexGraphql(source);
  const starts = definitionStarts(tokens);
  const definitions = new Map();

  for (let index = 0; index < starts.length; index++) {
    const current = starts[index];
    const next = starts[index + 1];
    const slice = tokens.slice(
      current.keywordIndex,
      next ? next.descriptionIndex : tokens.length,
    );
    const parsed = parseDefinition(slice);
    if (!parsed) continue;
    mergeDefinition(definitions, parsed);
  }

  const roots = Object.fromEntries(
    ROOT_TYPES.map((name) => [
      name,
      definitions.get(`type:${name}`)?.members ?? new Map(),
    ]),
  );
  return { definitions, roots };
}

/**
 * Return every semantic difference in both directions. `leftLabel` and
 * `rightLabel` are included in the records so callers can render useful gate
 * failures without knowing parser internals.
 */
export function compareSchemaSurfaces(
  left,
  right,
  leftLabel = 'left',
  rightLabel = 'right',
) {
  const differences = [];
  const keys = [...new Set([
    ...left.definitions.keys(),
    ...right.definitions.keys(),
  ])].sort();

  for (const key of keys) {
    const leftDefinition = left.definitions.get(key);
    const rightDefinition = right.definitions.get(key);
    if (!leftDefinition) {
      differences.push({
        id: key,
        kind: 'definition-only',
        side: rightLabel,
        message: `${key} exists only in ${rightLabel}`,
      });
      continue;
    }
    if (!rightDefinition) {
      differences.push({
        id: key,
        kind: 'definition-only',
        side: leftLabel,
        message: `${key} exists only in ${leftLabel}`,
      });
      continue;
    }
    if (leftDefinition.header !== rightDefinition.header) {
      differences.push({
        id: key,
        kind: 'definition-signature',
        side: 'both',
        message: `${key} has a different definition signature`,
        left: leftDefinition.header,
        right: rightDefinition.header,
      });
    }

    const memberNames = [...new Set([
      ...leftDefinition.members.keys(),
      ...rightDefinition.members.keys(),
    ])].sort();
    for (const memberName of memberNames) {
      const leftMember = leftDefinition.members.get(memberName);
      const rightMember = rightDefinition.members.get(memberName);
      const id = `${key}.${memberName}`;
      if (!leftMember) {
        differences.push({
          id,
          kind: 'member-only',
          side: rightLabel,
          message: `${id} exists only in ${rightLabel}`,
        });
      } else if (!rightMember) {
        differences.push({
          id,
          kind: 'member-only',
          side: leftLabel,
          message: `${id} exists only in ${leftLabel}`,
        });
      } else if (leftMember.signature !== rightMember.signature) {
        differences.push({
          id,
          kind: 'member-signature',
          side: 'both',
          message: `${id} has a different signature`,
          left: leftMember.signature,
          right: rightMember.signature,
        });
      }
    }
  }

  return differences;
}

export function rootFieldEntries(surface, rootName) {
  return [...(surface.roots[rootName] ?? new Map()).values()]
    .map((member) => ({
      name: member.name,
      signature: member.signature,
      deprecated: member.signature.includes('@ deprecated'),
    }))
    .sort((left, right) => left.name.localeCompare(right.name));
}

export function enumValues(surface, enumName) {
  const definition = surface.definitions.get(`enum:${enumName}`);
  if (!definition) return [];
  return [...definition.members.keys()].sort();
}

export function lexGraphql(source) {
  const tokens = [];
  let index = source.charCodeAt(0) === 0xfeff ? 1 : 0;

  while (index < source.length) {
    const character = source[index];
    if (/[\s,]/u.test(character)) {
      index++;
      continue;
    }
    if (character === '#') {
      while (index < source.length && source[index] !== '\n') index++;
      continue;
    }
    if (source.startsWith('"""', index)) {
      const start = index;
      index += 3;
      while (index < source.length && !source.startsWith('"""', index)) {
        if (source[index] === '\\' && source.startsWith('\\"""', index)) {
          index += 4;
        } else {
          index++;
        }
      }
      if (index >= source.length) throw new Error('unterminated GraphQL block string');
      index += 3;
      tokens.push({ kind: 'STRING', value: source.slice(start, index) });
      continue;
    }
    if (character === '"') {
      const start = index++;
      let escaped = false;
      while (index < source.length) {
        const current = source[index++];
        if (!escaped && current === '"') break;
        if (!escaped && current === '\\') escaped = true;
        else escaped = false;
      }
      if (source[index - 1] !== '"') throw new Error('unterminated GraphQL string');
      tokens.push({ kind: 'STRING', value: source.slice(start, index) });
      continue;
    }
    if (/[_A-Za-z]/u.test(character)) {
      const start = index++;
      while (index < source.length && /[_0-9A-Za-z]/u.test(source[index])) index++;
      tokens.push({ kind: 'NAME', value: source.slice(start, index) });
      continue;
    }
    if (character === '-' || /[0-9]/u.test(character)) {
      const start = index++;
      while (index < source.length && /[0-9.eE+-]/u.test(source[index])) index++;
      tokens.push({ kind: 'NUMBER', value: source.slice(start, index) });
      continue;
    }
    if (source.startsWith('...', index)) {
      tokens.push({ kind: 'PUNCT', value: '...' });
      index += 3;
      continue;
    }
    if ('!$():=@[]{|}&'.includes(character)) {
      tokens.push({ kind: 'PUNCT', value: character });
      index++;
      continue;
    }
    throw new Error(`unexpected GraphQL character ${JSON.stringify(character)} at ${index}`);
  }

  return tokens;
}

function definitionStarts(tokens) {
  const starts = [];
  const stack = [];
  const opening = new Map([['{', '}'], ['(', ')'], ['[', ']']]);
  const closing = new Set(['}', ')', ']']);

  for (let index = 0; index < tokens.length; index++) {
    const token = tokens[index].value;
    if (stack.length === 0) {
      const kindIndex = token === 'extend' ? index + 1 : index;
      if (DEFINITION_KINDS.has(tokens[kindIndex]?.value)) {
        let descriptionIndex = index;
        while (
          descriptionIndex > 0 &&
          tokens[descriptionIndex - 1].kind === 'STRING'
        ) {
          descriptionIndex--;
        }
        starts.push({ descriptionIndex, keywordIndex: index });
      }
    }
    if (opening.has(token)) stack.push(opening.get(token));
    else if (closing.has(token)) {
      const expected = stack.pop();
      if (expected !== token) {
        const context = tokens
          .slice(Math.max(0, index - 8), index + 9)
          .map((entry) => entry.value)
          .join(' ');
        throw new Error(
          `unbalanced GraphQL token ${token} at token ${index} ` +
            `(expected ${expected ?? 'nothing'}): ${context}`,
        );
      }
    }
  }
  if (stack.length > 0) throw new Error('unbalanced GraphQL schema');
  return starts;
}

function parseDefinition(tokens) {
  if (tokens.length === 0) return null;
  let index = 0;
  let extended = false;
  if (tokens[index]?.value === 'extend') {
    extended = true;
    index++;
  }
  const kind = tokens[index++]?.value;
  if (!DEFINITION_KINDS.has(kind)) return null;

  let name;
  if (kind === 'schema') {
    name = '$schema';
  } else if (kind === 'directive') {
    if (tokens[index]?.value !== '@') throw new Error('malformed directive definition');
    name = tokens[index + 1]?.value;
  } else {
    name = tokens[index]?.value;
  }
  if (!name) throw new Error(`missing name for GraphQL ${kind} definition`);

  const openBrace = tokens.findIndex((token) => token.value === '{');
  const headerEnd = openBrace >= 0 ? openBrace : tokens.length;
  const header = canonicalTokens(
    tokens.slice(0, headerEnd).filter((token) => token.kind !== 'STRING'),
  );
  const members =
    openBrace >= 0
      ? parseMembers(tokens.slice(openBrace + 1, matchingBrace(tokens, openBrace)), kind)
      : parseNonBlockMembers(tokens, kind);

  return {
    key: `${kind}:${name}`,
    kind,
    name,
    header: extended ? header.replace(/^extend /u, '') : header,
    members,
  };
}

function parseMembers(tokens, kind) {
  const members = new Map();
  let index = 0;
  while (index < tokens.length) {
    while (tokens[index]?.kind === 'STRING') index++;
    if (index >= tokens.length) break;
    if (tokens[index]?.kind !== 'NAME') {
      throw new Error(`expected ${kind} member, got ${tokens[index]?.value}`);
    }
    const start = index;
    const name = tokens[index++].value;

    if (kind === 'enum') {
      index = consumeDirectives(tokens, index);
    } else {
      if (tokens[index]?.value === '(') index = consumeGroup(tokens, index, '(', ')');
      if (tokens[index]?.value !== ':') {
        throw new Error(`expected ':' after ${kind} member ${name}`);
      }
      index++;
      index = consumeTypeReference(tokens, index);
      if (tokens[index]?.value === '=') {
        index++;
        index = consumeValue(tokens, index);
      }
      index = consumeDirectives(tokens, index);
    }

    const signature = canonicalTokens(tokens.slice(start, index));
    addMember(members, { name, signature });
  }
  return members;
}

function parseNonBlockMembers(tokens, kind) {
  const members = new Map();
  if (kind !== 'union') return members;
  const equals = tokens.findIndex((token) => token.value === '=');
  if (equals < 0) return members;
  for (let index = equals + 1; index < tokens.length; index++) {
    if (tokens[index].kind !== 'NAME') continue;
    const name = tokens[index].value;
    addMember(members, { name, signature: name });
  }
  return members;
}

function mergeDefinition(definitions, incoming) {
  const existing = definitions.get(incoming.key);
  if (!existing) {
    definitions.set(incoming.key, incoming);
    return;
  }
  if (existing.header !== incoming.header) {
    // Root/schema extensions and duplicate printed roots legitimately have
    // different headers only by the `extend` keyword, removed above.
    existing.header = [existing.header, incoming.header].sort().join(' + ');
  }
  for (const member of incoming.members.values()) addMember(existing.members, member);
}

function addMember(members, member) {
  const existing = members.get(member.name);
  if (existing && existing.signature !== member.signature) {
    throw new Error(
      `conflicting duplicate GraphQL member ${member.name}: ` +
        `${existing.signature} != ${member.signature}`,
    );
  }
  members.set(member.name, member);
}

function matchingBrace(tokens, openIndex) {
  let depth = 0;
  for (let index = openIndex; index < tokens.length; index++) {
    if (tokens[index].value === '{') depth++;
    else if (tokens[index].value === '}') {
      depth--;
      if (depth === 0) return index;
    }
  }
  throw new Error('unclosed GraphQL definition block');
}

function consumeTypeReference(tokens, index) {
  if (tokens[index]?.value === '[') {
    index = consumeTypeReference(tokens, index + 1);
    if (tokens[index]?.value !== ']') throw new Error('malformed GraphQL list type');
    index++;
  } else if (tokens[index]?.kind === 'NAME') {
    index++;
  } else {
    throw new Error(`malformed GraphQL type reference near ${tokens[index]?.value}`);
  }
  if (tokens[index]?.value === '!') index++;
  return index;
}

function consumeValue(tokens, index) {
  const token = tokens[index];
  if (!token) throw new Error('missing GraphQL default value');
  if (token.value === '[') return consumeGroup(tokens, index, '[', ']');
  if (token.value === '{') return consumeGroup(tokens, index, '{', '}');
  return index + 1;
}

function consumeDirectives(tokens, index) {
  while (tokens[index]?.value === '@') {
    if (tokens[index + 1]?.kind !== 'NAME') throw new Error('malformed GraphQL directive');
    index += 2;
    if (tokens[index]?.value === '(') index = consumeGroup(tokens, index, '(', ')');
  }
  return index;
}

function consumeGroup(tokens, index, open, close) {
  if (tokens[index]?.value !== open) throw new Error(`expected ${open}`);
  const stack = [close];
  const pairs = new Map([['{', '}'], ['(', ')'], ['[', ']']]);
  index++;
  while (index < tokens.length && stack.length > 0) {
    const value = tokens[index++].value;
    if (pairs.has(value)) stack.push(pairs.get(value));
    else if (value === stack.at(-1)) stack.pop();
  }
  if (stack.length > 0) throw new Error(`unclosed GraphQL ${open}`);
  return index;
}

function canonicalTokens(tokens) {
  return tokens.map((token) => token.value).join(' ');
}
