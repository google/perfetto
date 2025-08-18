// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {time} from '../base/time';
import {RecordConfig} from '../controller/record_config_types';
import {
  Aggregation,
  PivotTree,
  TableColumn,
} from '../frontend/pivot_table_types';

import {
  selectionToLegacySelection,
  Selection,
  LegacySelection,
} from '../core/selection_manager';

export {
  Selection,
  SelectionKind,
  NoteSelection,
  SliceSelection,
  HeapProfileSelection,
  PerfSamplesSelection,
  LegacySelection,
  AreaSelection,
  ProfileType,
  ThreadSliceSelection,
  CpuProfileSampleSelection,
} from '../core/selection_manager';

// Tracks within track groups (usually corresponding to processes) are sorted.
// As we want to group all tracks related to a given thread together, we use
// two keys:
// - Primary key corresponds to a priority of a track block (all tracks related
//   to a given thread or a single track if it's not thread-associated).
// - Secondary key corresponds to a priority of a given thread-associated track
//   within its thread track block.
// Each track will have a sort key, which either a primary sort key
// (for non-thread tracks) or a tid and secondary sort key (mapping of tid to
// primary sort key is done independently).
export enum PrimaryTrackSortKey {
  DEBUG_TRACK,
  NULL_TRACK,
  PROCESS_SCHEDULING_TRACK,
  PROCESS_SUMMARY_TRACK,
  EXPECTED_FRAMES_SLICE_TRACK,
  ACTUAL_FRAMES_SLICE_TRACK,
  PERF_SAMPLES_PROFILE_TRACK,
  HEAP_PROFILE_TRACK,
  MAIN_THREAD,
  RENDER_THREAD,
  GPU_COMPLETION_THREAD,
  CHROME_IO_THREAD,
  CHROME_COMPOSITOR_THREAD,
  ORDINARY_THREAD,
  COUNTER_TRACK,
  ASYNC_SLICE_TRACK,
  ORDINARY_TRACK,
}

/**
 * A plain js object, holding objects of type |Class| keyed by string id.
 * We use this instead of using |Map| object since it is simpler and faster to
 * serialize for use in postMessage.
 */
export interface ObjectById<Class extends {id: string}> {
  [id: string]: Class;
}

// Same as ObjectById but the key parameter is called `key` rather than `id`.
export interface ObjectByKey<Class extends {key: string}> {
  [key: string]: Class;
}

export type OmniboxMode = 'SEARCH' | 'COMMAND';

export interface OmniboxState {
  omnibox: string;
  mode: OmniboxMode;
  force?: boolean;
}

export interface Area {
  start: time;
  end: time;
  tracks: string[];
}

export const MAX_TIME = 180;

// 3: TrackKindPriority and related sorting changes.
// 5: Move a large number of items off frontendLocalState and onto state.
// 6: Common PivotTableConfig and pivot table specific PivotTableState.
// 7: Split Chrome categories in two and add 'symbolize ksyms' flag.
// 8: Rename several variables
// "[...]HeapProfileFlamegraph[...]" -> "[...]Flamegraph[...]".
// 9: Add a field to track last loaded recording profile name
// 10: Change last loaded profile tracking type to accommodate auto-save.
// 11: Rename updateChromeCategories to fetchChromeCategories.
// 12: Add a field to cache mapping from UI track ID to trace track ID in order
//     to speed up flow arrows rendering.
// 13: FlamegraphState changed to support area selection.
// 14: Changed the type of uiTrackIdByTraceTrackId from `Map` to an object with
// typed key/value because a `Map` does not preserve type during
// serialisation+deserialisation.
// 15: Added state for Pivot Table V2
// 16: Added boolean tracking if the flamegraph modal was dismissed
// 17:
// - add currentEngineId to track the id of the current engine
// - remove nextNoteId, nextAreaId and use nextId as a unique counter for all
//   indexing except the indexing of the engines
// 18: areaSelection change see b/235869542
// 19: Added visualisedArgs state.
// 20: Refactored thread sorting order.
// 21: Updated perf sample selection to include a ts range instead of single ts
// 22: Add log selection kind.
// 23: Add log filtering criteria for Android log entries.
// 24: Store only a single Engine.
// 25: Move omnibox state off VisibleState.
// 26: Add tags for filtering Android log entries.
// 27. Add a text entry for filtering Android log entries.
// 28. Add a boolean indicating if non matching log entries are hidden.
// 29. Add ftrace state. <-- Borked, state contains a non-serializable object.
// 30. Convert ftraceFilter.excludedNames from Set<string> to string[].
// 31. Convert all timestamps to bigints.
// 32. Add pendingDeeplink.
// 33. Add plugins state.
// 34. Add additional pendingDeeplink fields (query, pid).
// 35. Add force to OmniboxState
// 36. Remove metrics
// 37. Add additional pendingDeeplink fields (visStart, visEnd).
// 38. Add track tags.
// 39. Ported cpu_slice, ftrace, and android_log tracks to plugin tracks. Track
//     state entries now require a URI and old track implementations are no
//     longer registered.
// 40. Ported counter, process summary/sched, & cpu_freq to plugin tracks.
// 41. Ported all remaining tracks.
// 42. Rename trackId -> trackKey.
// 43. Remove visibleTracks.
// 44. Add TabsV2 state.
// 45. Remove v1 tracks.
// 46. Remove trackKeyByTrackId.
// 47. Selection V2
// 48. Rename legacySelection -> selection and introduce new Selection type.
// 49. Remove currentTab, which is only relevant to TabsV1.
// 50. Remove ftrace filter state.
// 51. Changed structure of FlamegraphState.expandedCallsiteByViewingOption.
// 52. Update track group state - don't make the summary track the first track.
// 53. Remove android log state.
// 54. Remove traceTime.
// 55. Rename TrackGroupState.id -> TrackGroupState.key.
// 56. Renamed chrome slice to thread slice everywhere.
// 57. Remove flamegraph related code from state.
// 58. Remove area map.
// 59. Deprecate old area selection type.
// 60. Deprecate old note selection type.
// 61. Remove params/state from TrackState.
export const STATE_VERSION = 61;

export const SCROLLING_TRACK_GROUP = 'ScrollingTracks';

export type EngineMode = 'WASM' | 'HTTP_RPC';

export type NewEngineMode = 'USE_HTTP_RPC_IF_AVAILABLE' | 'FORCE_BUILTIN_WASM';

// Key that is used to sort tracks within a block of tracks associated with a
// given thread.
export enum InThreadTrackSortKey {
  THREAD_COUNTER_TRACK,
  THREAD_SCHEDULING_STATE_TRACK,
  CPU_STACK_SAMPLES_TRACK,
  VISUALISED_ARGS_TRACK,
  ORDINARY,
  DEFAULT_TRACK,
}

// Sort key used for sorting tracks associated with a thread.
export type ThreadTrackSortKey = {
  utid: number;
  priority: InThreadTrackSortKey;
};

// Sort key for all tracks: both thread-associated and non-thread associated.
export type TrackSortKey = PrimaryTrackSortKey | ThreadTrackSortKey;

// Mapping which defines order for threads within a given process.
export type UtidToTrackSortKey = {
  [utid: number]: {
    tid?: number;
    sortKey: PrimaryTrackSortKey;
  };
};

export interface TraceFileSource {
  type: 'FILE';
  file: File;
}

export interface TraceArrayBufferSource {
  type: 'ARRAY_BUFFER';
  buffer: ArrayBuffer;
  title: string;
  url?: string;
  fileName?: string;

  // |uuid| is set only when loading via ?local_cache_key=1234. When set,
  // this matches global.state.traceUuid, with the exception of the following
  // time window: When a trace T1 is loaded and the user loads another trace T2,
  // this |uuid| will be == T2, but the globals.state.traceUuid will be
  // temporarily == T1 until T2 has been loaded (consistently to what happens
  // with all other state fields).
  uuid?: string;
  // if |localOnly| is true then the trace should not be shared or downloaded.
  localOnly?: boolean;

  // The set of extra args, keyed by plugin, that can be passed when opening the
  // trace via postMessge deep-linking. See post_message_handler.ts for details.
  pluginArgs?: {[pluginId: string]: {[key: string]: unknown}};
}

export interface TraceUrlSource {
  type: 'URL';
  url: string;
}

export interface TraceHttpRpcSource {
  type: 'HTTP_RPC';
}

export interface ServerStoredFileSource{
  type: 'STORED_FILE';
  fileName: string;
}

export type TraceSource =
  | TraceFileSource
  | TraceArrayBufferSource
  | TraceUrlSource
  | TraceHttpRpcSource
  | ServerStoredFileSource;

export interface TrackState {
  uri: string;
  key: string;
  name: string;
  trackSortKey: TrackSortKey;
  trackGroup?: string;
  closeable?: boolean;
}

export interface TrackGroupState {
  key: string;
  name: string;
  collapsed: boolean;
  tracks: string[]; // Child track ids.
  fixedOrdering?: boolean; // Render tracks without sorting.
  summaryTrack: string | undefined;
}

export interface EngineConfig {
  id: string;
  mode?: EngineMode; // Is undefined until |ready| is true.
  ready: boolean;
  failed?: string; // If defined the engine has crashed with the given message.
  source: TraceSource;
}

export interface QueryConfig {
  id: string;
  engineId?: string;
  query: string;
}

export interface Status {
  msg: string;
  timestamp: number; // Epoch in seconds (Date.now() / 1000).
}

export interface Note {
  noteType: 'DEFAULT';
  id: string;
  timestamp: time;
  color: string;
  text: string;
}

export interface SpanNote {
  noteType: 'SPAN';
  id: string;
  start: time;
  end: time;
  color: string;
  text: string;
}

export interface Pagination {
  offset: number;
  count: number;
}

export interface RecordingTarget {
  name: string;
  os: TargetOs;
}

export interface AdbRecordingTarget extends RecordingTarget {
  serial: string;
}

export interface Sorting {
  column: string;
  direction: 'DESC' | 'ASC';
}

export interface AggregationState {
  id: string;
  sorting?: Sorting;
}

// Auxiliary metadata needed to parse the query result, as well as to render it
// correctly. Generated together with the text of query and passed without the
// change to the query response.
export interface PivotTableQueryMetadata {
  pivotColumns: TableColumn[];
  aggregationColumns: Aggregation[];
  countIndex: number;
}

// Everything that's necessary to run the query for pivot table
export interface PivotTableQuery {
  text: string;
  metadata: PivotTableQueryMetadata;
}

// Pivot table query result
export interface PivotTableResult {
  // Hierarchical pivot structure on top of rows
  tree: PivotTree;
  // Copy of the query metadata from the request, bundled up with the query
  // result to ensure the correct rendering.
  metadata: PivotTableQueryMetadata;
}

// Input parameters to check whether the pivot table needs to be re-queried.
export interface PivotTableAreaState {
  start: time;
  end: time;
  tracks: string[];
}

export interface PivotTableState {
  // Currently selected area, if null, pivot table is not going to be visible.
  selectionArea?: PivotTableAreaState;

  // Query response
  queryResult: PivotTableResult | null;

  // Selected pivots for tables other than slice.
  // Because of the query generation, pivoting happens first on non-slice
  // pivots; therefore, those can't be put after slice pivots. In order to
  // maintain the separation more clearly, slice and non-slice pivots are
  // located in separate arrays.
  selectedPivots: TableColumn[];

  // Selected aggregation columns. Stored same way as pivots.
  selectedAggregations: Aggregation[];

  // Whether the pivot table results should be constrained to the selected area.
  constrainToArea: boolean;

  // Set to true by frontend to request controller to perform the query to
  // acquire the necessary data from the engine.
  queryRequested: boolean;
}

export interface LoadedConfigNone {
  type: 'NONE';
}

export interface LoadedConfigAutomatic {
  type: 'AUTOMATIC';
}

export interface LoadedConfigNamed {
  type: 'NAMED';
  name: string;
}

export type LoadedConfig =
  | LoadedConfigNone
  | LoadedConfigAutomatic
  | LoadedConfigNamed;

export interface NonSerializableState {
  pivotTable: PivotTableState;
}

export interface PendingDeeplinkState {
  ts?: string;
  dur?: string;
  tid?: string;
  pid?: string;
  query?: string;
  visStart?: string;
  visEnd?: string;
}

export interface TabsV2State {
  openTabs: string[];
  currentTab: string;
}

export interface State {
  version: number;
  nextId: string;

  /**
   * State of the ConfigEditor.
   */
  recordConfig: RecordConfig;
  displayConfigAsPbtxt: boolean;
  lastLoadedConfig: LoadedConfig;

  /**
   * Open traces.
   */
  newEngineMode: NewEngineMode;
  engine?: EngineConfig;
  traceUuid?: string;
  trackGroups: ObjectByKey<TrackGroupState>;
  tracks: ObjectByKey<TrackState>;
  utidToThreadSortKey: UtidToTrackSortKey;
  aggregatePreferences: ObjectById<AggregationState>;
  scrollingTracks: string[];
  pinnedTracks: string[];
  debugTrackId?: string;
  lastTrackReloadRequest?: number;
  queries: ObjectById<QueryConfig>;
  notes: ObjectById<Note | SpanNote>;
  status: Status;
  selection: Selection;
  traceConversionInProgress: boolean;
  flamegraphModalDismissed: boolean;

  // Show track perf debugging overlay
  perfDebug: boolean;

  // Show the sidebar extended
  sidebarVisible: boolean;

  // Hovered and focused events
  hoveredUtid: number;
  hoveredPid: number;
  hoverCursorTimestamp: time;
  hoveredNoteTimestamp: time;
  highlightedSliceId: number;
  focusedFlowIdLeft: number;
  focusedFlowIdRight: number;
  pendingScrollId?: number;

  searchIndex: number;

  tabs: TabsV2State;

  /**
   * Trace recording
   */
  recordingInProgress: boolean;
  recordingCancelled: boolean;
  extensionInstalled: boolean;
  recordingTarget: RecordingTarget;
  availableAdbDevices: AdbRecordingTarget[];
  lastRecordingError?: string;
  recordingStatus?: string;

  fetchChromeCategories: boolean;
  chromeCategories: string[] | undefined;

  // Special key: this part of the state is not going to be serialized when
  // using permalink. Can be used to store those parts of the state that can't
  // be serialized at the moment, such as ES6 Set and Map.
  nonSerializableState: NonSerializableState;

  // Omnibox info.
  omniboxState: OmniboxState;

  // Pending deeplink which will happen when we first finish opening a
  // trace.
  pendingDeeplink?: PendingDeeplinkState;

  // Individual plugin states
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  plugins: {[key: string]: any};

  trackFilterTerm: string | undefined;
}

export declare type RecordMode =
  | 'STOP_WHEN_FULL'
  | 'RING_BUFFER'
  | 'LONG_TRACE';

// 'Q','P','O' for Android, 'L' for Linux, 'C' for Chrome.
export declare type TargetOs =
  | 'S'
  | 'R'
  | 'Q'
  | 'P'
  | 'O'
  | 'C'
  | 'L'
  | 'CrOS'
  | 'Win';

export function isAndroidP(target: RecordingTarget) {
  return target.os === 'P';
}

export function isAndroidTarget(target: RecordingTarget) {
  return ['Q', 'P', 'O'].includes(target.os);
}

export function isChromeTarget(target: RecordingTarget) {
  return ['C', 'CrOS'].includes(target.os);
}

export function isCrOSTarget(target: RecordingTarget) {
  return target.os === 'CrOS';
}

export function isLinuxTarget(target: RecordingTarget) {
  return target.os === 'L';
}

export function isWindowsTarget(target: RecordingTarget) {
  return target.os === 'Win';
}

export function isAdbTarget(
  target: RecordingTarget,
): target is AdbRecordingTarget {
  return !!(target as AdbRecordingTarget).serial;
}

export function hasActiveProbes(config: RecordConfig) {
  const fieldsWithEmptyResult = new Set<string>([
    'hpBlockClient',
    'allAtraceApps',
    'chromePrivacyFiltering',
  ]);
  let key: keyof RecordConfig;
  for (key in config) {
    if (
      typeof config[key] === 'boolean' &&
      config[key] === true &&
      !fieldsWithEmptyResult.has(key)
    ) {
      return true;
    }
  }
  if (config.chromeCategoriesSelected.length > 0) {
    return true;
  }
  return config.chromeHighOverheadCategoriesSelected.length > 0;
}

export function getDefaultRecordingTargets(): RecordingTarget[] {
  return [
    {os: 'Q', name: 'Android Q+ / 10+'},
    {os: 'P', name: 'Android P / 9'},
    {os: 'O', name: 'Android O- / 8-'},
    {os: 'C', name: 'Chrome'},
    {os: 'CrOS', name: 'Chrome OS (system trace)'},
    {os: 'L', name: 'Linux desktop'},
    {os: 'Win', name: 'Windows desktop'},
  ];
}

export function getBuiltinChromeCategoryList(): string[] {
  // List of static Chrome categories, last updated at 2024-05-15 from HEAD of
  // Chromium's //base/trace_event/builtin_categories.h.
  return [
    'accessibility',
    'AccountFetcherService',
    'android.adpf',
    'android.ui.jank',
    'android_webview',
    'android_webview.timeline',
    'aogh',
    'audio',
    'base',
    'benchmark',
    'blink',
    'blink.animations',
    'blink.bindings',
    'blink.console',
    'blink.net',
    'blink.resource',
    'blink.user_timing',
    'blink.worker',
    'blink_style',
    'Blob',
    'browser',
    'browsing_data',
    'CacheStorage',
    'Calculators',
    'CameraStream',
    'cppgc',
    'camera',
    'cast_app',
    'cast_perf_test',
    'cast.mdns',
    'cast.mdns.socket',
    'cast.stream',
    'cc',
    'cc.debug',
    'cdp.perf',
    'chromeos',
    'cma',
    'compositor',
    'content',
    'content_capture',
    'interactions',
    'delegated_ink_trails',
    'device',
    'devtools',
    'devtools.contrast',
    'devtools.timeline',
    'disk_cache',
    'download',
    'download_service',
    'drm',
    'drmcursor',
    'dwrite',
    'DXVA_Decoding',
    'evdev',
    'event',
    'event_latency',
    'exo',
    'extensions',
    'explore_sites',
    'FileSystem',
    'file_system_provider',
    'fledge',
    'fonts',
    'GAMEPAD',
    'gpu',
    'gpu.angle',
    'gpu.angle.texture_metrics',
    'gpu.capture',
    'graphics.pipeline',
    'headless',
    'history',
    'hwoverlays',
    'identity',
    'ime',
    'IndexedDB',
    'input',
    'input.scrolling',
    'io',
    'ipc',
    'Java',
    'jni',
    'jpeg',
    'latency',
    'latencyInfo',
    'leveldb',
    'loading',
    'log',
    'login',
    'media',
    'media_router',
    'memory',
    'midi',
    'mojom',
    'mus',
    'native',
    'navigation',
    'navigation.debug',
    'net',
    'network.scheduler',
    'netlog',
    'offline_pages',
    'omnibox',
    'oobe',
    'openscreen',
    'ozone',
    'partition_alloc',
    'passwords',
    'p2p',
    'page-serialization',
    'paint_preview',
    'pepper',
    'PlatformMalloc',
    'power',
    'ppapi',
    'ppapi_proxy',
    'print',
    'raf_investigation',
    'rail',
    'renderer',
    'renderer_host',
    'renderer.scheduler',
    'resources',
    'RLZ',
    'ServiceWorker',
    'SiteEngagement',
    'safe_browsing',
    'scheduler',
    'scheduler.long_tasks',
    'screenlock_monitor',
    'segmentation_platform',
    'sequence_manager',
    'service_manager',
    'sharing',
    'shell',
    'shortcut_viewer',
    'shutdown',
    'skia',
    'sql',
    'stadia_media',
    'stadia_rtc',
    'startup',
    'sync',
    'system_apps',
    'test_gpu',
    'toplevel',
    'toplevel.flow',
    'ui',
    'v8',
    'v8.execute',
    'v8.wasm',
    'ValueStoreFrontend::Backend',
    'views',
    'views.frame',
    'viz',
    'vk',
    'wakeup.flow',
    'wayland',
    'webaudio',
    'webengine.fidl',
    'weblayer',
    'WebCore',
    'webnn',
    'webrtc',
    'webrtc_stats',
    'xr',
    'disabled-by-default-android_view_hierarchy',
    'disabled-by-default-animation-worklet',
    'disabled-by-default-audio',
    'disabled-by-default-audio.latency',
    'disabled-by-default-audio-worklet',
    'disabled-by-default-base',
    'disabled-by-default-blink.debug',
    'disabled-by-default-blink.debug.display_lock',
    'disabled-by-default-blink.debug.layout',
    'disabled-by-default-blink.debug.layout.trees',
    'disabled-by-default-blink.feature_usage',
    'disabled-by-default-blink.image_decoding',
    'disabled-by-default-blink.invalidation',
    'disabled-by-default-identifiability',
    'disabled-by-default-identifiability.high_entropy_api',
    'disabled-by-default-cc',
    'disabled-by-default-cc.debug',
    'disabled-by-default-cc.debug.cdp-perf',
    'disabled-by-default-cc.debug.display_items',
    'disabled-by-default-cc.debug.lcd_text',
    'disabled-by-default-cc.debug.picture',
    'disabled-by-default-cc.debug.scheduler',
    'disabled-by-default-cc.debug.scheduler.frames',
    'disabled-by-default-cc.debug.scheduler.now',
    'disabled-by-default-content.verbose',
    'disabled-by-default-cpu_profiler',
    'disabled-by-default-cppgc',
    'disabled-by-default-cpu_profiler.debug',
    'disabled-by-default-devtools.screenshot',
    'disabled-by-default-devtools.timeline',
    'disabled-by-default-devtools.timeline.frame',
    'disabled-by-default-devtools.timeline.inputs',
    'disabled-by-default-devtools.timeline.invalidationTracking',
    'disabled-by-default-devtools.timeline.layers',
    'disabled-by-default-devtools.timeline.picture',
    'disabled-by-default-devtools.timeline.stack',
    'disabled-by-default-devtools.target-rundown',
    'disabled-by-default-devtools.v8-source-rundown',
    'disabled-by-default-devtools.v8-source-rundown-sources',
    'disabled-by-default-file',
    'disabled-by-default-fonts',
    'disabled-by-default-gpu_cmd_queue',
    'disabled-by-default-gpu.dawn',
    'disabled-by-default-gpu.debug',
    'disabled-by-default-gpu.decoder',
    'disabled-by-default-gpu.device',
    'disabled-by-default-gpu.graphite.dawn',
    'disabled-by-default-gpu.service',
    'disabled-by-default-gpu.vulkan.vma',
    'disabled-by-default-histogram_samples',
    'disabled-by-default-java-heap-profiler',
    'disabled-by-default-layer-element',
    'disabled-by-default-layout_shift.debug',
    'disabled-by-default-lifecycles',
    'disabled-by-default-loading',
    'disabled-by-default-mediastream',
    'disabled-by-default-memory-infra',
    'disabled-by-default-memory-infra.v8.code_stats',
    'disabled-by-default-mojom',
    'disabled-by-default-net',
    'disabled-by-default-network',
    'disabled-by-default-paint-worklet',
    'disabled-by-default-power',
    'disabled-by-default-renderer.scheduler',
    'disabled-by-default-renderer.scheduler.debug',
    'disabled-by-default-sequence_manager',
    'disabled-by-default-sequence_manager.debug',
    'disabled-by-default-sequence_manager.verbose_snapshots',
    'disabled-by-default-skia',
    'disabled-by-default-skia.gpu',
    'disabled-by-default-skia.gpu.cache',
    'disabled-by-default-skia.shaders',
    'disabled-by-default-skottie',
    'disabled-by-default-SyncFileSystem',
    'disabled-by-default-system_power',
    'disabled-by-default-system_stats',
    'disabled-by-default-thread_pool_diagnostics',
    'disabled-by-default-toplevel.ipc',
    'disabled-by-default-user_action_samples',
    'disabled-by-default-v8.compile',
    'disabled-by-default-v8.cpu_profiler',
    'disabled-by-default-v8.gc',
    'disabled-by-default-v8.gc_stats',
    'disabled-by-default-v8.ic_stats',
    'disabled-by-default-v8.inspector',
    'disabled-by-default-v8.runtime',
    'disabled-by-default-v8.runtime_stats',
    'disabled-by-default-v8.runtime_stats_sampling',
    'disabled-by-default-v8.stack_trace',
    'disabled-by-default-v8.turbofan',
    'disabled-by-default-v8.wasm.detailed',
    'disabled-by-default-v8.wasm.turbofan',
    'disabled-by-default-video_and_image_capture',
    'disabled-by-default-display.framedisplayed',
    'disabled-by-default-viz.gpu_composite_time',
    'disabled-by-default-viz.debug.overlay_planes',
    'disabled-by-default-viz.hit_testing_flow',
    'disabled-by-default-viz.overdraw',
    'disabled-by-default-viz.quads',
    'disabled-by-default-viz.surface_id_flow',
    'disabled-by-default-viz.surface_lifetime',
    'disabled-by-default-viz.triangles',
    'disabled-by-default-viz.visual_debugger',
    'disabled-by-default-webaudio.audionode',
    'disabled-by-default-webgpu',
    'disabled-by-default-webnn',
    'disabled-by-default-webrtc',
    'disabled-by-default-worker.scheduler',
    'disabled-by-default-xr.debug',
  ];
}

export function getContainingGroupKey(
  state: State,
  trackKey: string,
): null | string {
  const track = state.tracks[trackKey];
  if (track === undefined) {
    return null;
  }
  const parentGroupKey = track.trackGroup;
  if (!parentGroupKey) {
    return null;
  }
  return parentGroupKey;
}

export function getLegacySelection(state: State): LegacySelection | null {
  return selectionToLegacySelection(state.selection);
}
