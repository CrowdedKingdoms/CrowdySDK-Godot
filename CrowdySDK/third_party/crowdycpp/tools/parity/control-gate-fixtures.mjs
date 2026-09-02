#!/usr/bin/env node
/**
 * Generate/check exact PlayerControlGate transition vectors from the pinned,
 * built CrowdyJS checkout.
 *
 * Usage:
 *   node tools/parity/control-gate-fixtures.mjs
 *     [--crowdyjs <checkout>] [--write]
 */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import {
  assertCrowdyJsParityTarget,
  resolveCrowdyJsPath,
} from './crowdyjs-path.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const options = parseArgs(process.argv.slice(2));
const crowdyjs = resolveCrowdyJsPath(root, options.crowdyjs);
const target = assertCrowdyJsParityTarget(root, crowdyjs);
const modulePath = join(crowdyjs, 'dist', 'player-host', 'control-gate.js');
const sourcePath = join(crowdyjs, 'src', 'player-host', 'control-gate.ts');
const typesPath = join(crowdyjs, 'src', 'crowdy-agent', 'types.ts');
for (const path of [modulePath, sourcePath, typesPath]) {
  if (!existsSync(path)) {
    throw new Error(
      `required CrowdyJS control-gate artifact is missing: ${path}. ` +
        'Run npm ci && npm run build in CrowdyJS first.',
    );
  }
}

const source = readFileSync(sourcePath, 'utf8');
if (
  !/constructor\(\s*private\s+readonly\s+clearAgentIntent\s*:/u.test(source)
) {
  throw new Error(
    'CrowdyJS PlayerControlGate no longer requires clearAgentIntent construction authority',
  );
}
const reasonVocabulary = parsePreemptionReasons(
  readFileSync(typesPath, 'utf8'),
);
const { PlayerControlGate } = await import(
  `${pathToFileURL(modulePath).href}?crowdycpp=${target.commit}`
);

class FakeEventTarget {
  listeners = new Map();

  addEventListener(type, listener) {
    const listeners = this.listeners.get(type) ?? new Set();
    listeners.add(listener);
    this.listeners.set(type, listeners);
  }

  removeEventListener(type, listener) {
    this.listeners.get(type)?.delete(listener);
  }

  dispatch(type, event) {
    for (const listener of [...(this.listeners.get(type) ?? [])]) {
      listener(event);
    }
  }
}

class FakeDocument extends FakeEventTarget {
  visibilityState = 'visible';
}

const defaults = defaultAndHumanWindowFixture(PlayerControlGate);
const rebindOfflineStop = rebindAndOfflineStopFixture(PlayerControlGate);
const imperativeReasons = reasonVocabulary.map((reason, index) =>
  imperativeReasonFixture(PlayerControlGate, reason, index),
);
const imperativeHooks = [
  hookFixture(PlayerControlGate, 'keyboard', 'HUMAN_INPUT'),
  hookFixture(PlayerControlGate, 'escape', 'ESCAPE'),
  hookFixture(PlayerControlGate, 'pointer', 'HUMAN_INPUT'),
  hookFixture(PlayerControlGate, 'movement', 'HUMAN_INPUT'),
  hookFixture(PlayerControlGate, 'pause', 'HUMAN_STOP'),
  hookFixture(PlayerControlGate, 'stop', 'HUMAN_STOP'),
  hookFixture(PlayerControlGate, 'death', 'DEATH'),
  hookFixture(PlayerControlGate, 'contextChanged', 'CONTEXT_CHANGED'),
  hookFixture(PlayerControlGate, 'permissionChanged', 'PERMISSION_CHANGED'),
  hookFixture(
    PlayerControlGate,
    'controlTargetChanged',
    'CONTROL_TARGET_CHANGED',
  ),
  hookFixture(PlayerControlGate, 'disconnected', 'DISCONNECTED'),
  hookFixture(PlayerControlGate, 'pagehide', 'DISCONNECTED'),
  hookFixture(PlayerControlGate, 'offline', 'DISCONNECTED'),
  hookFixture(PlayerControlGate, 'backgrounded', 'DISCONNECTED'),
];

const fixture = {
  fixtureVersion: 1,
  contractVersion: 'crowdy.player-control-gate/1',
  crowdyJs: target,
  construction: {
    clearAgentIntentRequired: true,
  },
  reasonVocabulary,
  defaults,
  rebindOfflineStop,
  imperativeHooks,
  imperativeReasons,
};
const text = `${JSON.stringify(fixture, null, 2)}\n`;
const fixturePath = join(
  root,
  'tools',
  'parity',
  'fixtures',
  'crowdyjs-player-control-gate.v1.json',
);

if (options.write) {
  writeFileSync(fixturePath, text);
  console.log('Player control-gate fixture synchronized from CrowdyJS');
} else if (
  !existsSync(fixturePath) ||
  readFileSync(fixturePath, 'utf8') !== text
) {
  throw new Error(
    'Player control-gate fixture is stale; rerun control-gate-fixtures.mjs --write',
  );
}

console.log(
  `Player control-gate fixture matches CrowdyJS: ` +
    `${imperativeHooks.length} hooks, ${imperativeReasons.length} reasons`,
);

function defaultAndHumanWindowFixture(Gate) {
  let now = 10_000;
  const windowValue = new FakeEventTarget();
  const documentValue = new FakeDocument();
  const events = [];
  const manager = fakeLeaseManager(
    playLease('lease-human'),
    'human',
    events,
  );
  const controller = fakeController(
    playLease('lease-human'),
    'human',
    events,
  );
  const gate = new Gate(
    (reason) => events.push(`fallback:${reason}`),
    {
      window: windowValue,
      document: documentValue,
      now: () => now,
      onPreempt: (reason) => events.push(`onPreempt:${reason}`),
    },
  );
  gate.start();
  const initialSnapshot = snapshot(gate);
  const emissions = [];
  let recordEmissions = false;
  const unsubscribe = gate.subscribe((value) => {
    emissions.push(clone(value));
    if (recordEmissions) events.push(`snapshot:${snapshotToken(value)}`);
  });
  const subscribedSnapshot = clone(emissions.at(-1));
  const unbind = gate.bind(manager, controller);
  const boundSnapshot = snapshot(gate);

  events.length = 0;
  emissions.length = 0;
  recordEmissions = true;
  windowValue.dispatch('keydown', { code: 'KeyW' });
  events.push('human-handler');
  const activeAtZero = gate.humanInputActive();
  const afterHumanInput = snapshot(gate);
  const humanInputOrder = [...events];
  now += 149;
  const activeAt149Ms = gate.humanInputActive();
  now += 1;
  const activeAt150Ms = gate.humanInputActive();

  unsubscribe();
  unbind();
  gate.destroy();
  return {
    humanInputActiveMs: 150,
    initialSnapshot,
    subscribedSnapshot,
    boundSnapshot,
    humanInputOrder,
    humanInputEmissions: emissions,
    afterHumanInput,
    window: {
      activeAtZero,
      activeAt149Ms,
      activeAt150Ms,
    },
  };
}

function rebindAndOfflineStopFixture(Gate) {
  let now = 20_000;
  const events = [];
  const firstLease = playLease('lease-first');
  const secondLease = playLease('lease-second');
  const firstManager = fakeLeaseManager(firstLease, 'first', events);
  const firstController = fakeController(firstLease, 'first', events);
  const secondManager = fakeLeaseManager(secondLease, 'second', events);
  const secondController = fakeController(secondLease, 'second', events, {
    stopRejects: true,
  });
  const gate = new Gate(
    (reason) => events.push(`fallback:${reason}`),
    {
      now: () => now,
      onPreempt: (reason) => events.push(`onPreempt:${reason}`),
    },
  );
  let recordEmissions = false;
  const emissions = [];
  const unsubscribe = gate.subscribe((value) => {
    emissions.push(clone(value));
    if (recordEmissions) events.push(`snapshot:${snapshotToken(value)}`);
  });
  const firstUnbind = gate.bind(firstManager, firstController);

  events.length = 0;
  emissions.length = 0;
  recordEmissions = true;
  const secondUnbind = gate.bind(secondManager, secondController);
  const rebindOrder = [...events];
  const rebindEmissions = clone(emissions);
  const reboundSnapshot = snapshot(gate);

  events.length = 0;
  emissions.length = 0;
  gate.stop();
  const stopOrder = [...events];
  const stopEmissions = clone(emissions);
  const stoppedSnapshot = snapshot(gate);

  secondManager.setLease(playLease('lease-reset'));
  secondController.setLease(playLease('lease-reset'));
  events.length = 0;
  emissions.length = 0;
  const resetUnbind = gate.bind(secondManager, secondController);
  const reboundAfterStopSnapshot = snapshot(gate);

  events.length = 0;
  emissions.length = 0;
  resetUnbind();
  const unbindOrder = [...events];
  const unbindEmissions = clone(emissions);
  const unboundSnapshot = snapshot(gate);

  firstUnbind();
  secondUnbind();
  unsubscribe();
  gate.destroy();
  now += 1;
  return {
    rebindOrder,
    rebindEmissions,
    reboundSnapshot,
    stopOrder,
    stopEmissions,
    stoppedSnapshot,
    reboundAfterStopSnapshot,
    unbindOrder,
    unbindEmissions,
    unboundSnapshot,
  };
}

function imperativeReasonFixture(Gate, reason, index) {
  const events = [];
  const value = playLease(`lease-reason-${index}`);
  const manager = fakeLeaseManager(value, 'reason', events);
  const controller = fakeController(value, 'reason', events);
  const gate = new Gate(
    (next) => events.push(`fallback:${next}`),
    { onPreempt: (next) => events.push(`onPreempt:${next}`) },
  );
  const unbind = gate.bind(manager, controller);
  events.length = 0;
  gate.preempt(reason);
  const result = {
    reason,
    order: [...events],
    snapshot: snapshot(gate),
  };
  unbind();
  gate.destroy();
  return result;
}

function hookFixture(Gate, action, expectedReason) {
  const events = [];
  const value = playLease(`lease-${action}`);
  const manager = fakeLeaseManager(value, action, events);
  const controller = fakeController(value, action, events);
  const windowValue = new FakeEventTarget();
  const documentValue = new FakeDocument();
  const gate = new Gate(
    (reason) => events.push(`fallback:${reason}`),
    {
      window: windowValue,
      document: documentValue,
      now: () => 30_000,
      onPreempt: (reason) => events.push(`onPreempt:${reason}`),
    },
  );
  gate.start();
  const unbind = gate.bind(manager, controller);
  events.length = 0;
  switch (action) {
    case 'keyboard':
      windowValue.dispatch('keydown', { code: 'KeyW' });
      break;
    case 'escape':
      windowValue.dispatch('keydown', { code: 'Escape' });
      break;
    case 'pointer':
      windowValue.dispatch('pointerdown', {});
      break;
    case 'movement':
      windowValue.dispatch('mousemove', {
        movementX: 1,
        movementY: 0,
        buttons: 0,
      });
      break;
    case 'pause':
      gate.pause();
      break;
    case 'stop':
      gate.stop();
      break;
    case 'death':
      gate.death();
      break;
    case 'contextChanged':
      gate.contextChanged();
      break;
    case 'permissionChanged':
      gate.permissionChanged();
      break;
    case 'controlTargetChanged':
      gate.controlTargetChanged();
      break;
    case 'disconnected':
      gate.disconnected();
      break;
    case 'pagehide':
      windowValue.dispatch('pagehide', {});
      break;
    case 'offline':
      windowValue.dispatch('offline', {});
      break;
    case 'backgrounded':
      documentValue.visibilityState = 'hidden';
      documentValue.dispatch('visibilitychange', {});
      break;
    default:
      throw new Error(`unknown control-gate hook ${action}`);
  }
  const result = {
    action,
    expectedReason,
    order: [...events],
    snapshot: snapshot(gate),
  };
  unbind();
  gate.destroy();
  return result;
}

function fakeLeaseManager(initialLease, label, events) {
  let lease = initialLease;
  return {
    snapshot: () => ({
      connected: true,
      clientEpoch: '1',
      lease,
      capabilities: null,
    }),
    preempt(reason) {
      events.push(`clear:${label}:${reason}`);
      lease = null;
    },
    setLease(value) {
      lease = value;
    },
  };
}

function fakeController(initialLease, label, events, options = {}) {
  let lease = initialLease;
  return {
    getState: () => ({ leases: lease ? [lease] : [] }),
    async revokeLease(leaseId, reason) {
      events.push(`revoke:${label}:${leaseId}:${reason}`);
    },
    async pause() {
      events.push(`pause:${label}`);
    },
    async stop() {
      events.push(`stop:${label}`);
      if (options.stopRejects) throw new Error('offline');
    },
    setLease(value) {
      lease = value;
    },
  };
}

function playLease(leaseId) {
  return {
    leaseId,
    kind: 'PLAY',
    status: 'ACTIVE',
    clientEpoch: '1',
    scopes: ['observe', 'locomotion', 'interact'],
    holder: 'Current player',
    controlledEntityId: 'player-1',
    hostCapabilityRevision: 'capability-1',
    contextVersion: 'context-1',
    grantedAt: '2026-07-24T00:00:00.000Z',
    expiresAt: '2026-07-24T00:01:00.000Z',
  };
}

function snapshot(gate) {
  return clone(gate.snapshot());
}

function snapshotToken(value) {
  return [
    value.bound ? 'bound' : 'unbound',
    value.activeLease?.leaseId ?? 'none',
    value.lastPreemption ?? 'none',
    value.humanInputActive ? 'human' : 'idle',
    value.offlineStop ? 'offline-stop' : 'online',
  ].join(':');
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function parsePreemptionReasons(typesSource) {
  const match = typesSource.match(
    /export\s+type\s+CrowdyAgentPreemptionReason\s*=([\s\S]*?);/u,
  );
  if (!match) throw new Error('CrowdyJS preemption reason type not found');
  const reasons = [...match[1].matchAll(/'([A-Z][A-Z0-9_]*)'/gu)].map(
    (entry) => entry[1],
  );
  if (reasons.length === 0 || new Set(reasons).size !== reasons.length) {
    throw new Error('CrowdyJS preemption reason type is empty or duplicated');
  }
  return reasons;
}

function parseArgs(args) {
  const parsed = { crowdyjs: null, write: false };
  for (let index = 0; index < args.length; index++) {
    const argument = args[index];
    if (argument === '--write') {
      parsed.write = true;
    } else if (argument === '--crowdyjs') {
      parsed.crowdyjs = args[++index];
      if (!parsed.crowdyjs) throw new Error('missing value for --crowdyjs');
    } else {
      throw new Error(`unknown argument: ${argument}`);
    }
  }
  return parsed;
}
