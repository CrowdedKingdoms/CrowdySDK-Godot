#!/usr/bin/env node
/**
 * Bidirectional CrowdyJS/CrowdyCPP parity gate.
 *
 * The baseline gate accepts only named, reviewed differences:
 *   - portable gap: real parity work that remains for a later plan phase;
 *   - native equivalent: C++ implements the contract through its native path;
 *   - browser exclusion: the surface is inherently browser/UI specific.
 *
 * Any new, changed, or stale classification fails. `--strict` additionally
 * fails while a portable gap remains, for the final parity/release gate.
 *
 * Usage:
 *   node tools/parity/parity.mjs [--crowdyjs <checkout>]
 *     [--write docs/parity-matrix.md | --check docs/parity-matrix.md]
 *     [--strict]
 */
import {
  existsSync,
  readFileSync,
  readdirSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { execFileSync } from 'node:child_process';
import { basename, dirname, isAbsolute, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildSchema, Kind, parse, validate } from 'graphql';
import {
  compareSchemaSurfaces,
  parseSchemaSurface,
  rootFieldEntries,
} from './schema-surface.mjs';
import {
  assertCrowdyJsParityTarget,
  resolveCrowdyJsPath,
} from './crowdyjs-path.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const options = parseArgs(process.argv.slice(2));
const crowdyjsPath = resolveCrowdyJsPath(root, options.crowdyjs);
assertCrowdyJsParityTarget(root, crowdyjsPath);

const CATEGORY = Object.freeze({
  PORTABLE: 'portable-gap',
  NATIVE: 'native-equivalent',
  BROWSER: 'browser-exclusion',
  COVERED: 'covered-extension',
});

// CrowdyCPP syncs schema.gql from the published SDL, so it currently runs ahead
// of CrowdyJS's committed snapshot rather than matching it.
// Add reviewed extension classifications here only when one SDK intentionally
// advances ahead of the other.
// CrowdyCPP syncs schema.gql from the PUBLISHED SDL, which carries the
// capacity-routing fields; CrowdyJS's committed snapshot predates them. So this
// is CrowdyJS trailing the server, not CrowdyCPP inventing fields — and it
// disappears the next time CrowdyJS regenerates, at which point these two
// entries go stale and the gate says so rather than letting them linger.
//
// The kind and side are part of the key on purpose: a waiver written for "exists
// only in CrowdyCPP" must not silently cover the opposite direction.
const SCHEMA_BASELINE = {
  // The 0.21.0 schema sync advanced the published SDL by several server
  // releases, so the groups below are all the situation described above: server
  // surface CrowdyJS's pinned snapshot has not regenerated against yet. Each
  // group goes stale, and the gate says so, the moment it does.
  // The one difference here that is not a missing field. Both sides declare
  // errorMessage, but the published SDL now deprecates it and redefines its
  // contents as a player-safe sentence carrying no engine detail, with fault as
  // the field a caller branches on. CrowdyJS's snapshot still documents it as
  // the engine's error text, so the signatures disagree in both directions.
};

const ROOT_CLASSIFICATIONS = {
  // The operator-only billing surface, added to the API across cycles eighteen
  // to twenty and picked up here when the pin moved to CrowdyJS 15.0.0.
  //
  // Deliberately unwrapped rather than not-yet-wrapped. Every one of these is
  // @RequiresOperator or org-admin: setting a price, exempting an org from
  // billing, or driving the usage tick. CrowdyCPP is a GAME CLIENT SDK shipped
  // to players' machines, and a wrapper there would be a method no holder of a
  // player's app-scoped token could ever call. Studio and the operator scripts
  // are where this surface belongs.
  ...classifyNames(
    'Mutation',
    ['runSharedUsageBillingTick', 'setBillingRate', 'setOrgBillingExempt'],
    CATEGORY.BROWSER,
    'operator-only billing control; not a game-client surface',
  ),
  ...classifyNames(
    'Query',
    ['billingExemptOrgs', 'billingRateCard'],
    CATEGORY.BROWSER,
    'operator-only billing reads; not a game-client surface',
  ),
  ...classifyNames(
    'Mutation',
    [
      'connectUdpProxy',
      'disconnectUdpProxy',
      'sendActorUpdate',
      'sendVoxelUpdate',
      'sendAudioPacket',
      'sendTextPacket',
      'sendClientEvent',
      'sendSingleActorMessage',
      'sendChannelMessage',
    ],
    CATEGORY.NATIVE,
    'browser GraphQL UDP proxy maps to CrowdyCPP native signed UDP',
  ),
  'Query.udpProxyConnectionStatus': classification(
    CATEGORY.NATIVE,
    'native replication Connection exposes transport state directly',
  ),
  // Subscription.udpNotifications is deliberately NOT classified. It used to be
  // a native waiver — the native Connection receives the gameplay notifications
  // directly — but CrowdyCPP subscribes to it for real since 0.20.0, selecting
  // only RealtimeConnectionEvent, because SERVER_DRAINING is delivered there and
  // nowhere else. A waiver on an implemented root field is a stale
  // classification, and the gate says so.
};


const METHOD_CLASSIFICATIONS = {
  'WorldClient.subscribe': classification(
    CATEGORY.NATIVE,
    'Connection handlers and WorldSession receive native UDP events',
  ),
  'PortalAPI.handleAuthorizeRequest': classification(
    CATEGORY.BROWSER,
    'Overworld browser-page redirect handler; native uses authorization-code APIs',
  ),
  'WorldSessionCore.on': classification(
    CATEGORY.NATIVE,
    'tick-driven WorldSession installs typed Connection handlers',
  ),
  'WorldSessionCore.onDispose': classification(
    CATEGORY.NATIVE,
    'RAII destruction replaces browser disposal callbacks',
  ),
  'WorldSessionCore.setSendTracker': classification(
    CATEGORY.NATIVE,
    'internal native WorldSession/ErrorStore wiring',
  ),
  'WorldSessionCore.trackSend': classification(
    CATEGORY.NATIVE,
    'internal native WorldSession/ErrorStore wiring',
  ),
  'HostTracker.beat': classification(
    CATEGORY.NATIVE,
    'WorldSession::tick performs the configured heartbeat',
  ),
  'LocalActorStore.patchState': classification(
    CATEGORY.NATIVE,
    'mutate the native state struct and call setState',
  ),
  'ActorInbox.decodeFailures': classification(
    CATEGORY.NATIVE,
    'native inbox retains bytes and leaves typed decoding to the caller',
  ),
  'ChannelInbox.decodeFailures': classification(
    CATEGORY.NATIVE,
    'native inbox retains bytes and leaves typed decoding to the caller',
  ),
  'EventRouter.decodeFailures': classification(
    CATEGORY.NATIVE,
    'native event router retains bytes and leaves typed decoding to the caller',
  ),
  'SocialKit.chat.onMessage': classification(
    CATEGORY.NATIVE,
    'WorldSession channel inbox / Connection channel handler',
  ),
  'RemoteActorStore.decodeFailures': classification(
    CATEGORY.NATIVE,
    'native actor stores retain bytes and leave typed decoding to the caller',
  ),
  'PlayerControlGate.start': classification(
    CATEGORY.BROWSER,
    'browser capture listeners translate DOM input, visibility, and offline events into native gate calls',
  ),
};

const CLASS_CLASSIFICATIONS = {
  UdpAPI: classification(
    CATEGORY.NATIVE,
    'browser GraphQL UDP proxy maps to the native replication client',
  ),
  BrowserSessionPkceStore: classification(
    CATEGORY.BROWSER,
    'browser sessionStorage helper; native applications own verifier persistence',
  ),
  CrowdyStudioAgentDomShell: classification(
    CATEGORY.BROWSER,
    'accessible agent dock is a DOM rendering shell over the portable controller',
  ),
  CrowdyStudioDomShell: classification(
    CATEGORY.BROWSER,
    'Studio workspace shell owns DOM elements, browser events, and CSS layout',
  ),
  AgentControlBanner: classification(
    CATEGORY.BROWSER,
    'always-visible control banner is browser DOM chrome over the native lease gate',
  ),
  VirtualFileSystem: classification(
    CATEGORY.BROWSER,
    'bounded VFS is the Monaco/browser-language-worker workspace; native editors own their document model',
  ),
  VfsLimitError: classification(
    CATEGORY.BROWSER,
    'VFS limit errors belong to the Monaco/browser-language-worker workspace',
  ),
  WorkerMessageReader: classification(
    CATEGORY.BROWSER,
    'vscode-jsonrpc adapter transports messages through a browser Web Worker',
  ),
  WorkerMessageWriter: classification(
    CATEGORY.BROWSER,
    'vscode-jsonrpc adapter transports messages through a browser Web Worker',
  ),
  WorkerLanguageClient: classification(
    CATEGORY.BROWSER,
    'browser Rust LSP client owns Web Worker lifecycle and browser timers',
  ),
  CrowdyStudioEmbedDock: classification(
    CATEGORY.BROWSER,
    'embed dock owns window geometry, PointerEvent capture, ARIA, and localStorage',
  ),
  CrowdyStudioTextHud: classification(
    CATEGORY.BROWSER,
    'text HUD renders untrusted presentation output into game-owned DOM',
  ),
  CrowdyStudioEmbed: classification(
    CATEGORY.BROWSER,
    'embed panel owns responsive DOM chrome, focus trapping, and browser input suppression',
  ),
};

const METHOD_ALIASES = {
  // CrowdyJS 15.0.0 calls it `register`; C++ cannot, because `register` was a
  // reserved keyword until C++17 and reads badly beside `registerUser`'s async
  // twin. The pair became load-bearing when the dev-auth bypass was deleted and
  // password sign-in became the path an automated client uses.
  'AuthAPI.register': 'registerUser',
  'ActorsAPI.delete': 'remove',
  'AvatarsAPI.delete': 'remove',
  'PlayerComputeAPI.delete': 'remove',
  'TeamsAPI.remove': 'remove',
  'ChannelsAPI.remove': 'remove',
  'StateAPI.delete': 'remove',
  'GameModelAPI.automations': 'automationsList',
  'GameModelAPI.getFunction': 'function',
  'CrowdyAgentGraphQLTransport.toolResult': 'browserToolResult',
  'AppsAPI.app': 'get',
  'AppsAPI.appBySlug': 'getBySlug',
  'AppsAPI.myApps': 'mine',
  'AppAccessAPI.usersByApp': 'userAccessByApp',
  'AppAccessAPI.usersByAppConnection': 'userAccessConnection',
  'OrganizationsAPI.bySlug': 'getBySlug',
  'OrganizationsAPI.setMemberRoles': 'updateMemberRoles',
  'PaymentsAPI.all': 'checkouts',
  'PaymentsAPI.allConnection': 'checkoutsConnection',
  'PaymentsAPI.capturePaypal': 'capturePaypalCheckout',
  'PaymentsAPI.create': 'createCheckout',
  'PaymentsAPI.events': 'paymentEvents',
  'PaymentsAPI.eventsConnection': 'paymentEventsConnection',
  'PaymentsAPI.mine': 'myCheckouts',
  'PaymentsAPI.mineConnection': 'myCheckoutsConnection',
  'SharedEnvironmentAPI.autoBilling': 'orgAutoBilling',
  'SharedEnvironmentAPI.freeAppQuota': 'orgFreeAppQuota',
  'SharedEnvironmentAPI.paymentMethods': 'orgPaymentMethods',
  'EconomyKit.trades.get': 'trade',
  'EconomyKit.trades.listMine': 'myTrades',
  'EconomyKit.trades.offer': 'tradeOffer',
  'EconomyKit.trades.accept': 'tradeAccept',
  'EconomyKit.trades.cancel': 'tradeCancel',
  'AvatarStateStore.publicState': 'identityState',
  'ChunkStore.get': 'find',
  'HostTracker.isHost': 'amIHost',
  'RemoteActorLane.count': 'size',
  'RemoteActorLane.get': 'find',
  'RemoteActorStore.count': 'size',
  'RemoteActorStore.get': 'find',
  'PlayerControlGate.death': 'onDeath',
  'PlayerControlGate.contextChanged': 'onContextChanged',
  'PlayerControlGate.permissionChanged': 'onPermissionChanged',
  'PlayerControlGate.controlTargetChanged': 'onControlTargetChanged',
  'PlayerControlGate.disconnected': 'onDisconnected',
};

const ASYNC_TWIN_WAIVERS = {
  'AuthAPI.getToken':
    'local AuthState read completes synchronously without transport work',
  'AuthAPI.setToken':
    'local AuthState write completes synchronously without transport work',
  'GameModelAPI.containerChanged':
    'returns an asynchronous subscription handle rather than a one-shot callback',
  'GameModelAPI.activePlayerCountChanged':
    'returns an asynchronous subscription handle rather than a one-shot callback',
  'PortalAPI.beginEntry':
    'native PKCE generation and URL construction are synchronous local work',
};

const CLASS_MAP = {
  AuthAPI: 'AuthAPI',
  DiscoveryDomain: 'DiscoveryAPI',
  UsersAPI: 'UsersAPI',
  PortalAPI: 'PortalAPI',
  ServerStatusAPI: 'ServerStatusAPI',
  ChunksAPI: 'ChunksAPI',
  VoxelsAPI: 'VoxelsAPI',
  ActorsAPI: 'ActorsAPI',
  AvatarsAPI: 'AvatarsAPI',
  StateAPI: 'StateAPI',
  HostAPI: 'HostAPI',
  TeleportAPI: 'TeleportAPI',
  TeamsAPI: 'TeamsAPI',
  ChannelsAPI: 'ChannelsAPI',
  ComputeAPI: 'ComputeAPI',
  GameModelAPI: 'GameModelAPI',
  GameAppsAPI: 'GameAppsAPI',
  MarketplaceAPI: 'MarketplaceAPI',
  PlayerComputeAPI: 'PlayerComputeAPI',
  PlayerModelAPI: 'PlayerModelAPI',
  PlayerWalletAPI: 'PlayerWalletAPI',
  PlatformAPI: 'PlatformAPI',
  OrganizationsAPI: 'OrganizationsAPI',
  AppsAPI: 'AppsAPI',
  AppAccessAPI: 'AppAccessAPI',
  BillingAPI: 'BillingAPI',
  PaymentsAPI: 'PaymentsAPI',
  QuotasAPI: 'QuotasAPI',
  UsageAPI: 'UsageAPI',
  SharedEnvironmentAPI: 'SharedEnvironmentAPI',
  ControlPlaneAPI: 'OperatorAPI',
  WorldClient: 'WorldClient',
  ActorClient: 'ActorClient',
  GameKitClient: 'GameKitClient',
  InventoryKit: 'InventoryKit',
  ObjectsKit: 'ObjectsKit',
  NpcsKit: 'NpcsKit',
  PlotsKit: 'PlotsKit',
  EconomyKit: 'EconomyKit',
  ProgressionKit: 'ProgressionKit',
  LootKit: 'LootKit',
  QuestsKit: 'QuestsKit',
  CombatKit: 'CombatKit',
  MatchesKit: 'MatchesKit',
  DecksKit: 'DecksKit',
  WorldsimKit: 'WorldsimKit',
  SocialKit: 'SocialKit',
  LeaderboardsKit: 'LeaderboardsKit',
  FeaturesKit: 'FeaturesKit',
  LocalActorStore: 'LocalActorStore',
  RemoteActorStore: 'RemoteActorStore',
  ChannelInbox: 'Inbox',
  ActorInbox: 'Inbox',
  EventRouter: 'EventRouter',
  HostTracker: 'WorldSession',
  SaveStateStore: 'SaveStateStore',
  AvatarStateStore: 'AvatarStateStore',
  ContainerMirror: 'ContainerMirror',
  WorldSessionCore: 'WorldSession',
  ChunkStore: 'ChunkStore',
  ErrorStore: 'ErrorStore',
  RemoteActorLane: 'RemoteActorLane',
  CrowdyStudioAPI: 'CrowdyStudioAPI',
  CrowdyStudioController: 'CrowdyStudioController',
  CrowdyStudioAgentController: 'CrowdyStudioAgentController',
  CrowdyAgentGraphQLTransport: 'CrowdyStudioAgentGraphQLTransport',
  CrowdyAgentToolRegistry: 'AgentToolRegistry',
  AgentControlLeaseManager: 'AgentControlLeaseManager',
  CrowdyAgentBrowserToolDispatcher: 'NativeBrowserToolDispatcherAdapter',
  StudioLayoutController: 'StudioLayoutController',
  PlayerControlGate: 'NativePlayerControlGate',
};

const STRICT_CLASS_MAPS = Object.freeze({
  PlayerControlGate: 'NativePlayerControlGate',
});

const CROSS_CUTTING_EXPORT_MODULES = {
  // The connection-machinery modules CrowdyJS 14 added. Listing them is the
  // point: this audit only inspects modules named here, so until they were
  // added, a new export in any of them was invisible to the gate — and these
  // four are exactly where endpoint mobility lives.
  'src/datacenter-redirect.ts': exportModule(
    ['DatacenterMove', 'moveFromError', 'moveFromErrors'],
    CATEGORY.NATIVE,
    'redirect parsing has a header equivalent with the same decision order',
    {
      DatacenterMove: 'DatacenterMove',
      moveFromError: 'moveFromError',
      moveFromErrors: 'moveFromErrors',
    },
    'include/crowdy/graphql/datacenter_redirect.hpp',
  ),
  'src/bootstrap-rediscover.ts': exportModule(
    ['BootstrapRediscoverOptions', 'createBootstrapRediscover'],
    CATEGORY.NATIVE,
    'bootstrap re-discovery is built into the client from ClientConfig::discoveryUrl',
    {
      BootstrapRediscoverOptions: 'RediscoveredEndpoint',
      createBootstrapRediscover: 'RediscoverCoordinator',
    },
    'include/crowdy/graphql/rediscover.hpp',
  ),
  'src/rediscover.ts': exportModule(
    ['MintCapablePortal', 'createMintRediscover'],
    CATEGORY.NATIVE,
    'mint-based re-discovery is any RediscoverFn calling portal().mintAppToken',
    {
      MintCapablePortal: 'PortalAPI',
      createMintRediscover: 'RediscoverFn',
    },
    {
      MintCapablePortal: 'include/crowdy/domains/portal.hpp',
      createMintRediscover: 'include/crowdy/graphql/rediscover.hpp',
    },
  ),
  'src/session.ts': exportModule(
    [
      'SessionListener',
      'SessionStore',
      'TokenStore',
      'BrowserLocalStorageTokenStore',
    ],
    CATEGORY.NATIVE,
    'token persistence maps to ITokenStore/AuthState; the localStorage backing is browser-only but its KEY SPLIT is mirrored by FileTokenStore',
    {
      SessionListener: 'onChange',
      SessionStore: 'AuthState',
      TokenStore: 'ITokenStore',
      BrowserLocalStorageTokenStore: 'FileTokenStore',
    },
    'include/crowdy/graphql/auth_state.hpp',
  ),
  'src/crowdy-studio/layout.ts': exportModule(
    [
      'STUDIO_LAYOUT_STORAGE_KEY',
      'STUDIO_PANE_IDS',
      'StudioLayoutController',
      'StudioLayoutControllerOptions',
      'StudioLayoutListener',
      'StudioLayoutState',
      'StudioLayoutStorage',
      'StudioPaneId',
      'StudioPaneSizeRange',
      'clampStudioPaneSize',
      'studioPaneSizeRange',
    ],
    CATEGORY.NATIVE,
    'headless pane state has an installed crowdy/studio/layout.hpp equivalent',
    {
      STUDIO_LAYOUT_STORAGE_KEY: 'STUDIO_LAYOUT_STORAGE_KEY',
      STUDIO_PANE_IDS: 'STUDIO_PANE_IDS',
      StudioLayoutController: 'StudioLayoutController',
      StudioLayoutControllerOptions: 'StudioLayoutControllerOptions',
      StudioLayoutListener: 'StudioLayoutListener',
      StudioLayoutState: 'StudioLayoutState',
      StudioLayoutStorage: 'ICrowdyStudioLayoutStorage',
      StudioPaneId: 'StudioPaneId',
      StudioPaneSizeRange: 'StudioPaneSizeRange',
      clampStudioPaneSize: 'clampStudioPaneSize',
      studioPaneSizeRange: 'studioPaneSizeRange',
    },
    'include/crowdy/studio/layout.hpp',
  ),
  'src/player-host/control-gate.ts': exportModule(
    [
      'PlayerControlGate',
      'PlayerControlGateAgentControl',
      'PlayerControlGateOptions',
      'PlayerControlGateSnapshot',
    ],
    CATEGORY.NATIVE,
    'portable takeover contract maps to the no-DOM NativePlayerControlGate',
    {
      PlayerControlGate: 'NativePlayerControlGate',
      PlayerControlGateAgentControl: 'INativePlayerControlGateController',
      PlayerControlGateOptions: 'NativePlayerControlGateOptionsV1',
      PlayerControlGateSnapshot: 'NativePlayerControlGateSnapshotV1',
    },
    'include/crowdy/player_host/control_gate.hpp',
  ),
  'src/crowdy-studio/editor.ts': exportModule(
    [
      'CrowdyStudioEditorAdapter',
      'CrowdyStudioEditorCallbacks',
      'CrowdyStudioEditorMode',
    ],
    CATEGORY.NATIVE,
    'portable editor adapter contract maps to the native editor bridge seam',
    {
      CrowdyStudioEditorAdapter: 'ICrowdyStudioEditorAdapter',
      CrowdyStudioEditorCallbacks: 'CrowdyStudioEditorCallbacks',
      CrowdyStudioEditorMode: 'CrowdyStudioEditorMode',
    },
    'include/crowdy/studio/editor.hpp',
  ),
  'src/crowdy-studio/mount.ts': exportModule(
    [
      'CrowdyStudioHandle',
      'MountCrowdyStudioAgentOptions',
      'MountCrowdyStudioOptions',
      'mountCrowdyStudio',
      'observeCrowdyStudioEditorLayout',
    ],
    CATEGORY.BROWSER,
    'mount surface owns DOM hosts, ResizeObserver, Monaco, and textarea fallback',
  ),
  'src/crowdy-studio/monaco-editor.ts': exportModule(
    [
      'MonacoCrowdyStudioEditorOptions',
      'createMonacoCrowdyStudioEditor',
      'isCurrentDiagnosticVersion',
    ],
    CATEGORY.BROWSER,
    'Monaco adapter and its VFS/LSP worker are browser editor infrastructure',
  ),
  'src/crowdy-studio/agent-dom-shell.ts': exportModule(
    ['CrowdyStudioAgentDomShell', 'CrowdyStudioAgentDomShellOptions'],
    CATEGORY.BROWSER,
    'agent dock is an accessible DOM shell over portable agent state',
  ),
  'src/crowdy-studio/dom-shell.ts': exportModule(
    ['CrowdyStudioDomShell'],
    CATEGORY.BROWSER,
    'workspace rendering, menus, panes, and browser events belong to the DOM shell',
  ),
  'src/crowdy-studio/splitter.ts': exportModule(
    [
      'PaneSplitterHandle',
      'PaneSplitterOptions',
      'PaneSplitterRange',
      'createPaneSplitter',
    ],
    CATEGORY.BROWSER,
    'splitter is PointerEvent, keyboard, ARIA, and HTMLElement behavior',
  ),
  'src/crowdy-studio/textarea-editor.ts': exportModule(
    ['createTextareaCrowdyStudioEditor'],
    CATEGORY.BROWSER,
    'fallback editor creates and drives browser textarea elements',
  ),
  'src/crowdy-studio/styles.ts': exportModule(
    ['CROWDY_STUDIO_STYLES'],
    CATEGORY.BROWSER,
    'workspace presentation is CSS text injected into a browser document',
  ),
  'src/live-coding/vfs.ts': exportModule(
    [
      'DEFAULT_VFS_LIMITS',
      'VfsLimitError',
      'VirtualDocument',
      'VirtualFileSystem',
      'VirtualFileSystemLimits',
      'offsetAt',
    ],
    CATEGORY.BROWSER,
    'VFS is the bounded Monaco/browser-language-worker document workspace',
  ),
  'src/live-coding/worker-transport.ts': exportModule(
    [
      'Disposable',
      'LanguageWorkerLike',
      'WorkerLanguageClient',
      'WorkerLanguageClientOptions',
      'WorkerMessageReader',
      'WorkerMessageWriter',
      'createDefaultRustLanguageWorker',
      'isWorkerLspMessage',
    ],
    CATEGORY.BROWSER,
    'worker transport binds vscode-jsonrpc to browser Web Worker messaging',
  ),
  'src/player-host/control-banner.ts': exportModule(
    [
      'AGENT_CONTROL_BANNER_STYLES',
      'AgentControlBanner',
      'AgentControlBannerController',
      'ensureAgentControlBannerStyles',
    ],
    CATEGORY.BROWSER,
    'control banner and styles are browser DOM safety chrome',
  ),
  'src/crowdy-studio/embed/dock.ts': exportModule(
    [
      'CROWDY_STUDIO_EMBED_DEFAULT_DOCK_RATIO',
      'CROWDY_STUDIO_EMBED_DOCK_WIDTH_STORAGE_KEY',
      'CROWDY_STUDIO_EMBED_MIN_DOCK_WIDTH_PX',
      'CROWDY_STUDIO_EMBED_MIN_GAME_WIDTH_PX',
      'CROWDY_STUDIO_EMBED_NARROW_BREAKPOINT_PX',
      'CROWDY_STUDIO_EMBED_SPLITTER_WIDTH_PX',
      'CrowdyStudioEmbedDock',
      'CrowdyStudioEmbedDockStorage',
      'CrowdyStudioEmbedDockWidthRange',
      'addBodyClass',
      'clampCrowdyStudioEmbedDockWidth',
      'crowdyStudioEmbedDockWidthRange',
      'removeBodyClass',
    ],
    CATEGORY.BROWSER,
    'embed dock owns viewport geometry, DOM classes, pointer capture, ARIA, and localStorage',
  ),
  'src/crowdy-studio/embed/embed-styles.ts': exportModule(
    ['CROWDY_STUDIO_EMBED_STYLES', 'ensureCrowdyStudioEmbedStyles'],
    CATEGORY.BROWSER,
    'embed presentation is CSS text injected into a browser document',
  ),
  'src/crowdy-studio/embed/hud-layer.ts': exportModule(
    ['CrowdyStudioHudEntry', 'CrowdyStudioTextHud'],
    CATEGORY.BROWSER,
    'HUD layer renders text-only player presentation into game-owned DOM',
  ),
  'src/crowdy-studio/embed/panel.ts': exportModule(
    [
      'CrowdyStudioEmbed',
      'CrowdyStudioEmbedAgentSessionOptions',
      'CrowdyStudioEmbedContext',
      'CrowdyStudioEmbedDisplayMode',
      'CrowdyStudioEmbedHandle',
      'CrowdyStudioEmbedOptions',
      'CrowdyStudioEmbedServices',
      'CrowdyStudioEmbedTargetPermission',
      'CrowdyStudioEmbedTargetPermissions',
      'createCrowdyStudioEmbed',
    ],
    CATEGORY.BROWSER,
    'embed panel owns responsive DOM docking, modal focus, and gameplay-input suppression',
  ),
};

const STRICT_NATIVE_EXPORT_MODULES = new Set([
  'src/crowdy-studio/editor.ts',
  'src/crowdy-studio/layout.ts',
  'src/player-host/control-gate.ts',
]);

const CROSS_CUTTING_BEHAVIORS = {
  'embed-focus-trap': {
    path: 'src/crowdy-studio/embed/panel.ts',
    markers: ['private trapTab(', 'function focusableElements('],
    classification: classification(
      CATEGORY.BROWSER,
      'modal focus trap enumerates and redirects browser DOM focus',
    ),
  },
  'player-glue-worker-package': {
    path: 'package.json',
    packageExport: './player-glue-worker',
    classification: classification(
      CATEGORY.BROWSER,
      'published subpath is the browser Web Worker entry; native CLIENT runtimes use injected engine sandboxes',
    ),
  },
};

const STRICT_NATIVE_SURFACE_GAPS = [];

if (!existsSync(join(crowdyjsPath, 'schema.gql'))) {
  fail(
    `CrowdyJS checkout is required at ${crowdyjsPath}; pass --crowdyjs <path>.`,
  );
}

const cppSchema = parseSchemaSurface(
  readFileSync(join(root, 'schema.gql'), 'utf8'),
);
const jsSchema = parseSchemaSurface(
  readFileSync(join(crowdyjsPath, 'schema.gql'), 'utf8'),
);
const schemaDifferences = compareSchemaSurfaces(
  cppSchema,
  jsSchema,
  'CrowdyCPP',
  'CrowdyJS',
);
const usedFields = operationRootFields();
for (const field of inlineRootFields()) usedFields.add(field);
const endpointOperationContracts = operationEndpointContracts();
const keyDtoContracts = keyDtoContractResults();
const endpointPlaneParity = endpointPlaneParityResults();
const crossCuttingSurfaces = crossCuttingSurfaceResults();

const state = {
  unclassified: [],
  stale: [],
  portable: [],
  native: [],
  browser: [],
  covered: [],
  deprecated: [],
  usedSchemaClassifications: new Set(),
  usedRootClassifications: new Set(),
  usedMethodClassifications: new Set(),
  usedClassClassifications: new Set(),
  usedMethodAliases: new Set(),
  usedClassMaps: new Set(),
  usedAsyncTwinWaivers: new Set(),
  asyncTwinsChecked: 0,
  dtoFieldsChecked: keyDtoContracts.checked,
  endpointPlanesChecked: endpointPlaneParity.checked,
  crossCuttingExportsChecked: crossCuttingSurfaces.exports.length,
  crossCuttingBehaviorsChecked: crossCuttingSurfaces.behaviors.length,
};
for (const contract of endpointOperationContracts) {
  if (!contract.valid) {
    state.unclassified.push(
      `operation:${contract.domain}/${contract.file}:${contract.name}`,
    );
  }
}
state.unclassified.push(...keyDtoContracts.failures);
state.unclassified.push(...endpointPlaneParity.failures);
state.unclassified.push(...crossCuttingSurfaces.failures);
state.stale.push(...crossCuttingSurfaces.stale);

const targetVersion = JSON.parse(
  readFileSync(join(crowdyjsPath, 'package.json'), 'utf8'),
).version;
const targetCommit = gitRevision(crowdyjsPath);

let report = '# CrowdyCPP parity matrix\n\n';
report +=
  'Generated by `tools/parity/parity.mjs`; do not edit by hand. ' +
  'Portable gaps are real missing work, not waivers. Native equivalents and ' +
  'browser exclusions are the only intentional waivers.\n\n';
report += '## Target\n\n';
report += `- CrowdyJS: \`${targetVersion}\` at \`${targetCommit}\`\n`;
report += '- CrowdyCPP schema: current canonical Management + Game SDL merge\n';
report += options.strict
  ? '- Gate mode: strict portable parity (every portable gap is release-blocking)\n\n'
  : '- Gate mode: reviewed baseline (use `--strict` to reject every portable gap)\n\n';

report += '## Bidirectional schema comparison\n\n';
report +=
  '| Difference | Direction | Classification |\n' +
  '|---|---|---|\n';
if (schemaDifferences.length === 0) {
  report += '| — | — | schemas match semantically |\n';
}
for (const difference of schemaDifferences) {
  const expected = SCHEMA_BASELINE[difference.id];
  let status;
  if (
    expected &&
    expected.expectedKind === difference.kind &&
    expected.expectedSide === difference.side
  ) {
    state.usedSchemaClassifications.add(difference.id);
    recordClassification(state, expected, `schema:${difference.id}`);
    status = renderClassification(expected);
  } else {
    status = '**UNCLASSIFIED SCHEMA DIFFERENCE**';
    state.unclassified.push(`schema:${difference.id} (${difference.message})`);
  }
  report +=
    `| \`${escapeCell(difference.id)}\` | ${escapeCell(difference.side)} | ` +
    `${escapeCell(status)} |\n`;
}
report += '\n';

report += '## Exact operation validation\n\n';
report +=
  `- Isolated generated operations: ${endpointOperationContracts.length}\n` +
  `- Valid against schema.gql: ` +
  `${endpointOperationContracts.filter(({ valid }) => valid).length}\n` +
  `- Invalid: ` +
  `${endpointOperationContracts.filter(({ valid }) => !valid).length}\n\n`;

report += '## GraphQL root-field implementation\n\n';
for (const rootName of ['Query', 'Mutation', 'Subscription']) {
  const cppFields = new Map(
    rootFieldEntries(cppSchema, rootName).map((field) => [field.name, field]),
  );
  const jsFields = new Map(
    rootFieldEntries(jsSchema, rootName).map((field) => [field.name, field]),
  );
  const names = [...new Set([...cppFields.keys(), ...jsFields.keys()])].sort();
  report +=
    `### ${rootName} (${names.length} unique fields)\n\n` +
    '| Field | Schema | Implementation |\n' +
    '|---|---|---|\n';
  for (const name of names) {
    const cppField = cppFields.get(name);
    const jsField = jsFields.get(name);
    const key = `${rootName}.${name}`;
    const schemaDifference = schemaDifferences.find(
      ({ id }) => id === `type:${key}`,
    );
    const schemaStatus =
      cppField && jsField
        ? cppField.signature === jsField.signature
          ? 'both'
          : 'both; signature differs'
        : cppField
          ? 'CrowdyCPP only'
          : 'CrowdyJS only';
    let status;
    if (usedFields.has(name)) {
      const reviewed = SCHEMA_BASELINE[`type:${key}`];
      status = schemaDifference
        ? reviewed
          ? `covered operation; ${renderClassification(reviewed)}`
          : '**UNCLASSIFIED ROOT SIGNATURE DIFFERENCE**'
        : 'covered';
    } else if ((cppField ?? jsField)?.deprecated) {
      status = 'deprecated waiver';
      state.deprecated.push(`root:${key}`);
    } else {
      const expected = ROOT_CLASSIFICATIONS[key];
      if (expected) {
        state.usedRootClassifications.add(key);
        recordClassification(state, expected, `root:${key}`);
        status = renderClassification(expected);
      } else {
        status = '**UNCLASSIFIED GAP**';
        state.unclassified.push(`root:${key}`);
      }
    }
    report +=
      `| \`${escapeCell(name)}\` | ${escapeCell(schemaStatus)} | ` +
      `${escapeCell(status)} |\n`;
  }
  report += '\n';
}

const tsAll = collectTsClasses(crowdyjsPath);
const cppAll = cppClassMethods();
report += '## CrowdyJS class/method implementation\n\n';
for (const [tsClass, methods] of Object.entries(tsAll).sort(([left], [right]) =>
  left.localeCompare(right),
)) {
  const classClassification = CLASS_CLASSIFICATIONS[tsClass];
  const cppName = CLASS_MAP[tsClass] ?? tsClass;
  if (Object.hasOwn(CLASS_MAP, tsClass)) state.usedClassMaps.add(tsClass);
  if (classClassification) {
    state.usedClassClassifications.add(tsClass);
  }
  const cppMethodNames = cppAll[cppName] ?? [];
  const cppMethods = new Set(cppMethodNames.map(normalizeName));
  report += `### ${tsClass} -> ${cppName}\n\n`;
  report += '| CrowdyJS method | Status |\n|---|---|\n';
  for (const method of methods) {
    const key = `${tsClass}.${method}`;
    const alias = METHOD_ALIASES[key];
    if (alias !== undefined) state.usedMethodAliases.add(key);
    const cppMethod = alias ?? method;
    let status;
    if (cppMethods.has(normalizeName(cppMethod))) {
      status = 'covered';
      if (
        tsClass.endsWith('API') &&
        !cppMethods.has(normalizeName(`${cppMethod}Async`))
      ) {
        const waiver = ASYNC_TWIN_WAIVERS[key];
        if (waiver) {
          state.usedAsyncTwinWaivers.add(key);
          status = `covered; async-twin waiver — ${waiver}`;
        } else {
          status = '**MISSING ASYNC TWIN**';
          state.unclassified.push(`method-twin:${key}`);
        }
      } else if (tsClass.endsWith('API')) {
        state.asyncTwinsChecked++;
      }
    } else {
      const expected = METHOD_CLASSIFICATIONS[key] ?? classClassification;
      if (expected) {
        if (METHOD_CLASSIFICATIONS[key]) state.usedMethodClassifications.add(key);
        recordClassification(state, expected, `method:${key}`);
        status = renderClassification(expected);
      } else {
        status = '**UNCLASSIFIED GAP**';
        state.unclassified.push(`method:${key}`);
      }
    }
    report += `| \`${escapeCell(method)}\` | ${escapeCell(status)} |\n`;
  }
  report += '\n';
}

report += '## Crowdy Studio cross-cutting export audit\n\n';
report +=
  '| CrowdyJS export | Classification |\n' +
  '|---|---|\n';
for (const entry of crossCuttingSurfaces.exports) {
  const id = `export:${entry.module}.${entry.symbol}`;
  recordClassification(state, entry.classification, id);
  report +=
    `| \`${escapeCell(`${entry.module}#${entry.symbol}`)}\` | ` +
    `${escapeCell(renderClassification(entry.classification))} |\n`;
}
report += '\n';
for (const entry of crossCuttingSurfaces.behaviors) {
  const id = `surface:${entry.id}`;
  recordClassification(state, entry.classification, id);
  report +=
    `- \`${escapeInlineCode(entry.id)}\`: ` +
    `${renderClassification(entry.classification)}\n`;
}
report += '\n';

report += '## Strict-native surfaces not represented by one method\n\n';
for (const gap of STRICT_NATIVE_SURFACE_GAPS) {
  state.portable.push(`surface:${gap}`);
  report += `- **portable gap:** ${gap}\n`;
}
report += '\n';

checkStaleClassifications(state);

report += '## Summary\n\n';
report += `- Semantic schema differences: ${schemaDifferences.length}\n`;
report +=
  `- Schema signature differences: ${
    schemaDifferences.filter(({ kind }) => kind.includes('signature')).length
  }\n`;
report +=
  `- Schema-invalid generated operations: ${
    endpointOperationContracts.filter(({ valid }) => !valid).length
  }\n`;
report +=
  `- Second-plane assertions checked: ${state.endpointPlanesChecked}\n`;
report += `- C++ sync/async method twins checked: ${state.asyncTwinsChecked}\n`;
report +=
  `- Reviewed async-twin waivers: ${state.usedAsyncTwinWaivers.size}\n`;
report += `- Key DTO fields type-checked: ${state.dtoFieldsChecked}\n`;
report +=
  `- Cross-cutting Studio exports checked: ${state.crossCuttingExportsChecked}\n`;
report +=
  `- Cross-cutting browser behaviors checked: ${state.crossCuttingBehaviorsChecked}\n`;
report += `- Reviewed method aliases checked: ${state.usedMethodAliases.size}\n`;
report += `- Reviewed class maps checked: ${state.usedClassMaps.size}\n`;
report += `- Portable gap entries: ${state.portable.length}\n`;
report += `- Native-equivalent waivers: ${state.native.length}\n`;
report += `- Browser-only waivers: ${state.browser.length}\n`;
report += `- Covered schema extensions: ${state.covered.length}\n`;
report += `- Deprecated waivers: ${state.deprecated.length}\n`;
report += `- Unclassified differences: ${state.unclassified.length}\n`;
report += `- Stale classifications: ${state.stale.length}\n\n`;
const portableByKind = countPrefixes(state.portable);
report +=
  `Portable breakdown: ${portableByKind.schema} schema, ` +
  `${portableByKind.root} roots, ${portableByKind.method} methods, ` +
  `${portableByKind.surface} cross-cutting surfaces.\n\n`;

report += '### Exact portable gaps\n\n';
for (const id of [...new Set(state.portable)].sort()) {
  report += `- \`${escapeInlineCode(id)}\`\n`;
}
report += '\n';

if (state.unclassified.length > 0) {
  report += '### Unclassified failures\n\n';
  for (const id of state.unclassified.sort()) report += `- \`${escapeInlineCode(id)}\`\n`;
  report += '\n';
}
if (state.stale.length > 0) {
  report += '### Stale baseline failures\n\n';
  for (const id of state.stale.sort()) report += `- \`${escapeInlineCode(id)}\`\n`;
  report += '\n';
}
report = `${report.trimEnd()}\n`;

if (options.write) {
  const path = repoPath(options.write);
  writeFileSync(path, report);
  console.log(`wrote ${relativeName(path)}`);
}
if (options.check) {
  const path = repoPath(options.check);
  if (!existsSync(path) || readFileSync(path, 'utf8') !== report) {
    state.unclassified.push(`generated-report:${relativeName(path)}`);
    // The report records its own gate mode, so a matrix written without
    // --strict can never satisfy a --strict check. Echo the flag back rather
    // than let that read as a real parity difference.
    console.error(
      `${relativeName(path)} is stale; regenerate with the same gate mode:\n` +
        `  node tools/parity/parity.mjs --crowdyjs <path>` +
        `${options.strict ? ' --strict' : ''} --write ${relativeName(path)}`,
    );
  } else {
    console.log(`${relativeName(path)} is current`);
  }
}

console.log(
  [
    `schema differences=${schemaDifferences.length}`,
    `portable gaps=${state.portable.length}`,
    `native waivers=${state.native.length}`,
    `browser waivers=${state.browser.length}`,
    `unclassified=${state.unclassified.length}`,
    `stale=${state.stale.length}`,
  ].join(' '),
);

if (
  state.unclassified.length > 0 ||
  state.stale.length > 0 ||
  (options.strict && state.portable.length > 0)
) {
  process.exitCode = 1;
}

function parseArgs(args) {
  const parsed = {
    crowdyjs: null,
    write: null,
    check: null,
    strict: false,
  };
  for (let index = 0; index < args.length; index++) {
    const argument = args[index];
    if (argument === '--strict') {
      parsed.strict = true;
    } else if (['--crowdyjs', '--write', '--check'].includes(argument)) {
      const value = args[++index];
      if (!value) fail(`missing value for ${argument}`);
      parsed[argument.slice(2)] = value;
    } else {
      fail(`unknown argument: ${argument}`);
    }
  }
  if (parsed.write && parsed.check) fail('--write and --check are mutually exclusive');
  return parsed;
}

function classification(category, reason, expectedKind, expectedSide) {
  return { category, reason, expectedKind, expectedSide };
}

function classifyNames(rootName, names, category, reason) {
  return Object.fromEntries(
    names.map((name) => [`${rootName}.${name}`, classification(category, reason)]),
  );
}

function classifySchema(ids, category, reason, expectedKind, expectedSide) {
  return Object.fromEntries(
    ids.map((id) => [
      id,
      classification(category, reason, expectedKind, expectedSide),
    ]),
  );
}

function exportModule(
  symbols,
  category,
  reason,
  nativeSymbols = {},
  nativePaths = null,
) {
  return {
    symbols: [...symbols].sort(),
    classification: classification(category, reason),
    nativeSymbols,
    nativePaths,
  };
}

function crossCuttingSurfaceResults() {
  const exports = [];
  const behaviors = [];
  const failures = [];
  const stale = [];
  const cppSources = new Map();

  for (const [module, baseline] of Object.entries(
    CROSS_CUTTING_EXPORT_MODULES,
  )) {
    if (
      STRICT_NATIVE_EXPORT_MODULES.has(module) &&
      baseline.classification.category !== CATEGORY.NATIVE
    ) {
      failures.push(
        `export-module:${module} (portable module must be a native equivalent)`,
      );
    }
    const path = join(crowdyjsPath, ...module.split('/'));
    if (!existsSync(path)) {
      stale.push(`export-module:${module} (module missing)`);
      continue;
    }
    const actual = typescriptExports(readFileSync(path, 'utf8'));
    const expected = new Set(baseline.symbols);
    for (const symbol of actual) {
      if (!expected.has(symbol)) {
        failures.push(`export:${module}.${symbol} (new cross-cutting export)`);
      }
    }
    for (const symbol of baseline.symbols) {
      const id = `export:${module}.${symbol}`;
      if (!actual.has(symbol)) {
        stale.push(id);
        continue;
      }
      exports.push({
        module,
        symbol,
        classification: baseline.classification,
      });
      if (baseline.classification.category !== CATEGORY.NATIVE) continue;

      const nativeSymbol = baseline.nativeSymbols[symbol];
      const nativePath =
        typeof baseline.nativePaths === 'string'
          ? baseline.nativePaths
          : baseline.nativePaths?.[symbol];
      if (!nativeSymbol || !nativePath) {
        failures.push(`${id} (native equivalent is not specified)`);
        continue;
      }
      if (!cppSources.has(nativePath)) {
        const fullPath = join(root, ...nativePath.split('/'));
        cppSources.set(
          nativePath,
          existsSync(fullPath) ? readFileSync(fullPath, 'utf8') : null,
        );
      }
      const cppSource = cppSources.get(nativePath);
      const identifier = new RegExp(
        `\\b${escapeRegExp(nativeSymbol)}\\b`,
        'u',
      );
      if (cppSource === null || !identifier.test(cppSource)) {
        failures.push(
          `${id} (missing ${nativeSymbol} in ${nativePath})`,
        );
      }
    }
  }

  for (const [id, baseline] of Object.entries(CROSS_CUTTING_BEHAVIORS)) {
    const path = join(crowdyjsPath, ...baseline.path.split('/'));
    if (!existsSync(path)) {
      stale.push(`surface:${id} (source missing)`);
      continue;
    }
    const source = readFileSync(path, 'utf8');
    let found = true;
    if (baseline.packageExport) {
      const manifest = JSON.parse(source);
      found = Object.hasOwn(manifest.exports ?? {}, baseline.packageExport);
    } else {
      found = baseline.markers.every((marker) => source.includes(marker));
    }
    if (!found) {
      stale.push(`surface:${id}`);
      continue;
    }
    behaviors.push({ id, classification: baseline.classification });
  }

  exports.sort(
    (left, right) =>
      left.module.localeCompare(right.module) ||
      left.symbol.localeCompare(right.symbol),
  );
  behaviors.sort((left, right) => left.id.localeCompare(right.id));
  return { exports, behaviors, failures, stale };
}

function typescriptExports(source) {
  const names = new Set();
  const declarationPattern =
    /\bexport\s+(?:(?:declare|abstract|async)\s+)*(?:class|function|const|let|var|interface|type|enum)\s+([A-Za-z_]\w*)/gu;
  for (const match of source.matchAll(declarationPattern)) names.add(match[1]);

  const listPattern =
    /\bexport\s+(?:type\s+)?\{([\s\S]*?)\}\s*(?:from\s+['"][^'"]+['"]\s*)?;/gu;
  for (const match of source.matchAll(listPattern)) {
    for (const raw of match[1].split(',')) {
      const value = raw.replace(/\/\*[\s\S]*?\*\//gu, '').trim();
      if (!value) continue;
      const withoutType = value.replace(/^type\s+/u, '');
      const parts = withoutType.split(/\s+as\s+/u);
      const exported = parts.at(-1)?.trim();
      if (/^[A-Za-z_]\w*$/u.test(exported ?? '')) names.add(exported);
    }
  }
  return names;
}

function escapeRegExp(value) {
  return String(value).replace(/[.*+?^${}()|[\]\\]/gu, '\\$&');
}

function renderClassification(value) {
  if (value.category === CATEGORY.PORTABLE) return `portable gap — ${value.reason}`;
  if (value.category === CATEGORY.NATIVE) return `native equivalent — ${value.reason}`;
  if (value.category === CATEGORY.BROWSER) return `browser exclusion — ${value.reason}`;
  return `covered — ${value.reason}`;
}

function recordClassification(state, value, id) {
  if (value.category === CATEGORY.PORTABLE) state.portable.push(id);
  else if (value.category === CATEGORY.NATIVE) state.native.push(id);
  else if (value.category === CATEGORY.BROWSER) state.browser.push(id);
  else if (value.category === CATEGORY.COVERED) state.covered.push(id);
  else state.unclassified.push(`${id} (unknown classification ${value.category})`);
}

function checkStaleClassifications(state) {
  for (const key of Object.keys(SCHEMA_BASELINE)) {
    if (!state.usedSchemaClassifications.has(key)) {
      state.stale.push(`schema:${key}`);
    }
  }
  for (const key of Object.keys(ROOT_CLASSIFICATIONS)) {
    if (!state.usedRootClassifications.has(key)) {
      state.stale.push(`root:${key}`);
    }
  }
  for (const key of Object.keys(METHOD_CLASSIFICATIONS)) {
    if (!state.usedMethodClassifications.has(key)) {
      state.stale.push(`method:${key}`);
    }
  }
  for (const key of Object.keys(CLASS_CLASSIFICATIONS)) {
    if (!state.usedClassClassifications.has(key)) {
      state.stale.push(`class:${key}`);
    }
  }
  for (const key of Object.keys(METHOD_ALIASES)) {
    if (!state.usedMethodAliases.has(key)) {
      state.stale.push(`method-alias:${key}`);
    }
  }
  for (const key of Object.keys(CLASS_MAP)) {
    if (!state.usedClassMaps.has(key)) {
      state.stale.push(`class-map:${key}`);
    }
  }
  for (const [key, expected] of Object.entries(STRICT_CLASS_MAPS)) {
    if (CLASS_MAP[key] !== expected) {
      state.stale.push(
        `class-map:${key} (expected ${expected}, got ${CLASS_MAP[key] ?? 'missing'})`,
      );
    }
  }
  for (const key of Object.keys(ASYNC_TWIN_WAIVERS)) {
    if (!state.usedAsyncTwinWaivers.has(key)) {
      state.stale.push(`async-twin:${key}`);
    }
  }
}

function operationEndpointContracts() {
  const apiSchema = buildSchema(readFileSync(join(root, 'schema.gql'), 'utf8'));
  const contracts = [];
  const operations = join(root, 'operations');
  for (const domain of readdirSync(operations).sort()) {
    const directory = join(operations, domain);
    if (!statSync(directory).isDirectory()) continue;
    for (const file of readdirSync(directory).sort()) {
      if (!file.endsWith('.graphql')) continue;
      const document = parse(readFileSync(join(directory, file), 'utf8'), {
        noLocation: true,
      });
      const fragments = new Map(
        document.definitions
          .filter(
            (definition) => definition.kind === Kind.FRAGMENT_DEFINITION,
          )
          .map((definition) => [definition.name.value, definition]),
      );
      for (const operation of document.definitions.filter(
        (definition) => definition.kind === Kind.OPERATION_DEFINITION,
      )) {
        if (!operation.name) {
          throw new Error(`${domain}/${file}: anonymous operation`);
        }
        const names = new Set();
        collectOperationSpreads(operation, names);
        const selected = [];
        const pending = [...names];
        while (pending.length > 0) {
          const name = pending.shift();
          if (selected.some((fragment) => fragment.name.value === name)) {
            continue;
          }
          const fragment = fragments.get(name);
          if (!fragment) {
            throw new Error(
              `${domain}/${file}:${operation.name.value} missing fragment ${name}`,
            );
          }
          selected.push(fragment);
          const nested = new Set();
          collectOperationSpreads(fragment, nested);
          pending.push(...nested);
        }
        const isolated = {
          kind: Kind.DOCUMENT,
          definitions: [operation, ...selected],
        };
        contracts.push({
          domain,
          file,
          name: operation.name.value,
          valid: validate(apiSchema, isolated).length === 0,
        });
      }
    }
  }
  return contracts;
}

function collectOperationSpreads(node, names) {
  if (!node || typeof node !== 'object') return;
  if (node.kind === Kind.FRAGMENT_SPREAD) names.add(node.name.value);
  for (const value of Object.values(node)) {
    if (Array.isArray(value)) {
      for (const entry of value) collectOperationSpreads(entry, names);
    } else {
      collectOperationSpreads(value, names);
    }
  }
}

// Both SDKs talk to ONE origin as of CrowdyJS 14 / CrowdyCPP 0.20.0. This used
// to compare, per operation, which of two planes each SDK routed it to — a check
// that becomes vacuous the moment there is one plane, and whose "no comparable
// routes found" failure would have been the only thing it could still say.
//
// Inverted: assert that NEITHER SDK has a second plane. It reads both sources
// because the collapse only holds while both agree; a management client
// reappearing on either side is a real regression, and one that would otherwise
// show up as a silent routing difference rather than as a parity failure.
function endpointPlaneParityResults() {
  const failures = [];
  let checked = 0;
  const sources = [
    {
      label: 'CrowdyCPP',
      path: join(root, 'include', 'crowdy', 'domains', 'marketplace.hpp'),
      patterns: [/\bmanagement_\b/gu, /\bmanagementGql\b/gu],
    },
    {
      label: 'CrowdyCPP',
      path: join(
        root, 'include', 'crowdy', 'domains', 'crowdy_studio_agent.hpp',
      ),
      patterns: [/\bmanagement_\b/gu, /\bmanagementGql\b/gu],
    },
    {
      label: 'CrowdyJS',
      path: join(crowdyjsPath, 'src', 'domains', 'marketplace.ts'),
      patterns: [/\bthis\.management\b/gu],
    },
    {
      label: 'CrowdyJS',
      path: join(crowdyjsPath, 'src', 'crowdy-client.ts'),
      patterns: [/\bthis\.management\s*=/gu],
    },
  ];
  for (const { label, path, patterns } of sources) {
    if (!existsSync(path)) {
      failures.push(`endpoint-plane:${path} (source missing)`);
      continue;
    }
    const source = readFileSync(path, 'utf8');
    for (const pattern of patterns) {
      checked++;
      const hits = [...source.matchAll(pattern)];
      if (hits.length > 0) {
        failures.push(
          `endpoint-plane:${label}.${basename(path)} reintroduces a second ` +
            `plane (${hits.length} x ${pattern.source})`,
        );
      }
    }
  }
  return { checked, failures };
}

function keyDtoContractResults() {
  const failures = [];
  let checked = 0;
  const cppTypes = readFileSync(
    join(root, 'include', 'crowdy', 'domains', 'types.hpp'),
    'utf8',
  );
  const tsAuth = readFileSync(
    join(crowdyjsPath, 'src', 'domains', 'auth.ts'),
    'utf8',
  );
  const cppAuthFields = dataFields(
    declarationBlock(cppTypes, /struct\s+AuthResponse\s*\{/gu),
  );
  const cppArtifactFields = dataFields(
    declarationBlock(cppTypes, /struct\s+ClientArtifactBytes\s*\{/gu),
  );
  const tsAuthFields = dataFields(
    declarationBlock(tsAuth, /export\s+interface\s+AuthResponse\s*\{/gu),
    true,
  );

  const assertField = (label, fields, name, type) => {
    checked++;
    const actual = fields.get(name);
    if (normalizeType(actual) !== normalizeType(type)) {
      failures.push(
        `dto:${label}.${name} (expected ${type}, got ${actual ?? 'missing'})`,
      );
    }
  };
  assertField('CrowdyCPP.AuthResponse', cppAuthFields, 'token', 'std::string');
  assertField(
    'CrowdyCPP.AuthResponse',
    cppAuthFields,
    'gameTokenId',
    'std::string',
  );
  assertField('CrowdyJS.AuthResponse', tsAuthFields, 'token', 'string');
  assertField(
    'CrowdyJS.AuthResponse',
    tsAuthFields,
    'gameTokenId',
    'string',
  );
  assertField(
    'CrowdyCPP.ClientArtifactBytes',
    cppArtifactFields,
    'bytes',
    'std::vector<std::uint8_t>',
  );
  assertField(
    'CrowdyCPP.ClientArtifactBytes',
    cppArtifactFields,
    'artifactHash',
    'std::string',
  );
  assertField(
    'CrowdyCPP.ClientArtifactBytes',
    cppArtifactFields,
    'fuelPerDispatch',
    'std::string',
  );
  assertField(
    'CrowdyCPP.ClientArtifactBytes',
    cppArtifactFields,
    'contractJson',
    'std::optional<std::string>',
  );
  assertField(
    'CrowdyCPP.ClientArtifactBytes',
    cppArtifactFields,
    'versionId',
    'std::string',
  );
  return { checked, failures };
}

function declarationBlock(source, pattern) {
  const match = pattern.exec(source);
  if (!match) return '';
  return balancedBlock(source, pattern.lastIndex - 1);
}

function dataFields(body, typescript = false) {
  const fields = new Map();
  for (const rawLine of body.split('\n')) {
    const line = rawLine.replace(/\/\/.*$/u, '').trim();
    if (!line.endsWith(';')) continue;
    const match = typescript
      ? line.match(/^([A-Za-z_]\w*)\??\s*:\s*(.+);$/u)
      : line.match(/^(.+?)\s+([A-Za-z_]\w*)\s*(?:=[^;]+)?;$/u);
    if (!match) continue;
    const name = typescript ? match[1] : match[2];
    const type = typescript ? match[2] : match[1];
    fields.set(name, type.trim());
  }
  return fields;
}

function normalizeType(value) {
  return String(value ?? '').replace(/\s+/gu, '');
}

function operationRootFields() {
  const used = new Set();
  const operations = join(root, 'operations');
  for (const domain of readdirSync(operations).sort()) {
    const directory = join(operations, domain);
    if (!statSync(directory).isDirectory()) continue;
    for (const file of readdirSync(directory).sort()) {
      if (!file.endsWith('.graphql')) continue;
      collectRootFields(readFileSync(join(directory, file), 'utf8'), used);
    }
  }
  return used;
}

function inlineRootFields() {
  const used = new Set();
  walkFiles(join(root, 'include'), (path) => {
    if (!path.endsWith('.hpp')) return;
    const text = readFileSync(path, 'utf8');
    for (const match of text.matchAll(/R"gql\(([\s\S]*?)\)gql"/gu)) {
      collectRootFields(match[1], used);
    }
    const joined = text.replace(/"\s*\n\s*"/gu, '');
    for (const match of joined.matchAll(
      /"((?:query|mutation|subscription)\s+\w+[\s\S]*?)"/gu,
    )) {
      collectRootFields(match[1], used);
    }
  });
  return used;
}

/** Add each operation's root-level selections to `out`. */
function collectRootFields(document, out) {
  const tokens = documentTokens(document);
  let index = 0;
  while (index < tokens.length) {
    const kind = tokens[index];
    if (!['query', 'mutation', 'subscription'].includes(kind)) {
      index++;
      continue;
    }
    index++;
    if (isName(tokens[index])) index++;
    if (tokens[index] === '(') index = skipGroup(tokens, index, '(', ')');
    index = skipDirectives(tokens, index);
    if (tokens[index] !== '{') continue;
    const close = matchingToken(tokens, index, '{', '}');
    collectSelections(tokens.slice(index + 1, close), out);
    index = close + 1;
  }
}

function collectSelections(tokens, out) {
  let index = 0;
  while (index < tokens.length) {
    if (tokens[index] === '...') {
      index++;
      if (tokens[index] === 'on') index += 2;
      else index++;
      continue;
    }
    if (!isName(tokens[index])) {
      index++;
      continue;
    }
    let field = tokens[index++];
    if (tokens[index] === ':') {
      index++;
      field = tokens[index++];
    }
    if (field && !field.startsWith('__')) out.add(field);
    if (tokens[index] === '(') index = skipGroup(tokens, index, '(', ')');
    index = skipDirectives(tokens, index);
    if (tokens[index] === '{') {
      index = matchingToken(tokens, index, '{', '}') + 1;
    }
  }
}

function documentTokens(document) {
  const stripped = document
    .replace(/#[^\n]*/gu, ' ')
    .replace(/"""[\s\S]*?"""/gu, ' ')
    .replace(/"(?:\\.|[^"\\])*"/gu, '""');
  return stripped.match(/\.{3}|[_A-Za-z][_0-9A-Za-z]*|[$!():=@[\]{|}]/gu) ?? [];
}

function matchingToken(tokens, openIndex, open, close) {
  let depth = 0;
  for (let index = openIndex; index < tokens.length; index++) {
    if (tokens[index] === open) depth++;
    else if (tokens[index] === close) {
      depth--;
      if (depth === 0) return index;
    }
  }
  throw new Error(`unclosed GraphQL document ${open}`);
}

function skipGroup(tokens, index, open, close) {
  return matchingToken(tokens, index, open, close) + 1;
}

function skipDirectives(tokens, start) {
  let index = start;
  while (tokens[index] === '@') {
    index += 2;
    if (tokens[index] === '(') index = skipGroup(tokens, index, '(', ')');
  }
  return index;
}

function isName(value) {
  return typeof value === 'string' && /^[_A-Za-z][_0-9A-Za-z]*$/u.test(value);
}

function collectTsClasses(crowdyjs) {
  const all = {};
  const directories = [
    join(crowdyjs, 'src', 'domains'),
    join(crowdyjs, 'src', 'kit'),
    join(crowdyjs, 'src', 'stores'),
  ];
  for (const directory of directories) {
    mergeClasses(
      all,
      tsClassMethods(
        directory,
        readdirSync(directory).filter((file) => file.endsWith('.ts')),
      ),
    );
  }
  mergeClasses(all, tsClassMethods(join(crowdyjs, 'src'), ['world.ts']));
  mergeClasses(
    all,
    tsClassMethods(join(crowdyjs, 'src', 'crowdy-studio'), [
      'agent-dom-shell.ts',
      'controller.ts',
      'dom-shell.ts',
      'layout.ts',
    ]),
  );
  mergeClasses(
    all,
    tsClassMethods(join(crowdyjs, 'src', 'crowdy-agent'), [
      'browser-dispatcher.ts',
      'controller.ts',
      'graphql-transport.ts',
      'registry.ts',
    ]),
  );
  mergeClasses(
    all,
    tsClassMethods(join(crowdyjs, 'src', 'player-host'), [
      'control-banner.ts',
      'control-gate.ts',
      'lease-manager.ts',
    ]),
  );
  mergeClasses(
    all,
    tsClassMethods(join(crowdyjs, 'src', 'live-coding'), [
      'vfs.ts',
      'worker-transport.ts',
    ]),
  );
  mergeClasses(
    all,
    tsClassMethods(join(crowdyjs, 'src', 'crowdy-studio', 'embed'), [
      'dock.ts',
      'hud-layer.ts',
      'panel.ts',
    ]),
  );
  return all;
}

function tsClassMethods(directory, files) {
  const classes = {};
  for (const file of files) {
    const path = join(directory, file);
    if (!existsSync(path)) continue;
    const text = readFileSync(path, 'utf8');
    const classPattern = /export\s+class\s+(\w+)[^{]*\{/gu;
    let classMatch;
    while ((classMatch = classPattern.exec(text)) !== null) {
      const name = classMatch[1];
      const body = balancedBlock(text, classPattern.lastIndex - 1);
      const methods = new Set();
      const methodPattern =
        /(?:^|\n)  (?!private\b|protected\b)(?:(?:public|static|async|readonly|get|set)\s+)*([A-Za-z_]\w*)\s*(?:<[^;\n{]+>)?\s*[(=<]/gu;
      for (const methodMatch of body.matchAll(methodPattern)) {
        const identifier = methodMatch[1];
        if (
          ![
            'constructor',
            'if',
            'for',
            'return',
            'switch',
            'while',
            'catch',
          ].includes(identifier)
        ) {
          methods.add(identifier);
        }
      }
      for (const groupMatch of body.matchAll(
        /readonly\s+(\w+)\s*=\s*\{([\s\S]*?)\n  \};/gu,
      )) {
        methods.delete(groupMatch[1]);
        for (const methodMatch of groupMatch[2].matchAll(
          /(?:^|\n)    ([A-Za-z_]\w*)\s*:\s*(?:async\s*)?\(/gu,
        )) {
          methods.add(`${groupMatch[1]}.${methodMatch[1]}`);
        }
      }
      if (methods.size > 0) classes[name] = [...methods].sort();
    }
  }
  return classes;
}

function cppClassMethods() {
  const classes = {};
  walkFiles(join(root, 'include', 'crowdy'), (path) => {
    if (!path.endsWith('.hpp')) return;
    const text = readFileSync(path, 'utf8');
    const classPattern = /(?:^|\n)class\s+(\w+)[^;{]*\{/gu;
    let classMatch;
    while ((classMatch = classPattern.exec(text)) !== null) {
      const name = classMatch[1];
      const body = balancedBlock(text, classPattern.lastIndex - 1);
      const methods = new Set();
      let visible = !/\n\s*public:/u.test(body);
      for (const rawLine of body.split('\n')) {
        const line = rawLine.trim();
        if (line === 'public:') {
          visible = true;
          continue;
        }
        if (line === 'private:' || line === 'protected:') {
          visible = false;
          continue;
        }
        if (!visible) continue;
        const methodMatch = rawLine.match(
          /^\s{2}[^=;{}]+?[&*\s](\w+)\s*\(/u,
        );
        if (
          methodMatch &&
          !['if', 'for', 'while', 'switch', 'return', 'operator'].includes(
            methodMatch[1],
          ) &&
          methodMatch[1] !== name
        ) {
          methods.add(methodMatch[1]);
        }
      }
      if (methods.size > 0) {
        classes[name] = [
          ...new Set([...(classes[name] ?? []), ...methods]),
        ].sort();
      }
    }
  });
  return classes;
}

function balancedBlock(text, openBraceIndex) {
  let depth = 0;
  let quote = null;
  let escaped = false;
  let lineComment = false;
  let blockComment = false;
  for (let index = openBraceIndex; index < text.length; index++) {
    const character = text[index];
    const next = text[index + 1];
    if (lineComment) {
      if (character === '\n') lineComment = false;
      continue;
    }
    if (blockComment) {
      if (character === '*' && next === '/') {
        blockComment = false;
        index++;
      }
      continue;
    }
    if (quote) {
      if (!escaped && character === quote) quote = null;
      escaped = !escaped && character === '\\';
      if (character !== '\\') escaped = false;
      continue;
    }
    if (character === '/' && next === '/') {
      lineComment = true;
      index++;
      continue;
    }
    if (character === '/' && next === '*') {
      blockComment = true;
      index++;
      continue;
    }
    if (character === '"' || character === "'" || character === '`') {
      quote = character;
      escaped = false;
      continue;
    }
    if (character === '{') depth++;
    else if (character === '}') {
      depth--;
      if (depth === 0) return text.slice(openBraceIndex + 1, index);
    }
  }
  return text.slice(openBraceIndex + 1);
}

function mergeClasses(target, source) {
  for (const [name, methods] of Object.entries(source)) {
    target[name] = [...new Set([...(target[name] ?? []), ...methods])].sort();
  }
}

function walkFiles(directory, callback) {
  for (const entry of readdirSync(directory).sort()) {
    const path = join(directory, entry);
    if (statSync(path).isDirectory()) walkFiles(path, callback);
    else callback(path);
  }
}

function normalizeName(value) {
  return value.toLowerCase().replace(/[^a-z0-9]/gu, '');
}

function gitRevision(directory) {
  try {
    return execFileSync('git', ['rev-parse', 'HEAD'], {
      cwd: directory,
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    }).trim();
  } catch {
    return 'unknown';
  }
}

function repoPath(path) {
  return isAbsolute(path) ? path : join(root, path);
}

function relativeName(path) {
  return path.startsWith(`${root}/`) ? path.slice(root.length + 1) : path;
}

function escapeCell(value) {
  return String(value).replace(/\|/gu, '\\|').replace(/\n/gu, ' ');
}

function escapeInlineCode(value) {
  return String(value).replace(/`/gu, '\\`');
}

function countPrefixes(values) {
  const counts = { schema: 0, root: 0, method: 0, surface: 0 };
  for (const value of values) {
    const prefix = String(value).split(':', 1)[0];
    if (prefix in counts) counts[prefix]++;
  }
  return counts;
}

function fail(message) {
  console.error(message);
  process.exit(2);
}
