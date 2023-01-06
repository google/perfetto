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

import {RecordConfig} from '../controller/record_config_types';
import {
  Aggregation,
  PivotTree,
  RegularColumn,
  TableColumn,
} from '../frontend/pivot_table_redux_types';

/**
 * A plain js object, holding objects of type |Class| keyed by string id.
 * We use this instead of using |Map| object since it is simpler and faster to
 * serialize for use in postMessage.
 */
export interface ObjectById<Class extends{id: string}> { [id: string]: Class; }

export interface Timestamped {
  lastUpdate: number;
}

export type OmniboxMode = 'SEARCH'|'COMMAND';

export interface OmniboxState {
  omnibox: string;
  mode: OmniboxMode;
}

export interface VisibleState extends Timestamped {
  startSec: number;
  endSec: number;
  resolution: number;
}

export interface AreaSelection {
  kind: 'AREA';
  areaId: string;
  // When an area is marked it will be assigned a unique note id and saved as
  // an AreaNote for the user to return to later. id = 0 is the special id that
  // is overwritten when a new area is marked. Any other id is a persistent
  // marking that will not be overwritten.
  // When not set, the area selection will be replaced with any
  // new area selection (i.e. not saved anywhere).
  noteId?: string;
}

export type AreaById = Area&{id: string};

export interface Area {
  startSec: number;
  endSec: number;
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
export const STATE_VERSION = 28;

export const SCROLLING_TRACK_GROUP = 'ScrollingTracks';

export type EngineMode = 'WASM'|'HTTP_RPC';

export type NewEngineMode = 'USE_HTTP_RPC_IF_AVAILABLE'|'FORCE_BUILTIN_WASM';

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
  DEBUG_SLICE_TRACK,
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
  utid: number,
  priority: InThreadTrackSortKey,
}

// Sort key for all tracks: both thread-associated and non-thread associated.
export type TrackSortKey = PrimaryTrackSortKey|ThreadTrackSortKey;

// Mapping which defines order for threads within a given process.
export type UtidToTrackSortKey = {
  [utid: number]: {
    tid?: number, sortKey: PrimaryTrackSortKey,
  }
}

export enum ProfileType {
  HEAP_PROFILE = 'heap_profile',
  NATIVE_HEAP_PROFILE = 'heap_profile:libc.malloc',
  JAVA_HEAP_SAMPLES = 'heap_profile:com.android.art',
  JAVA_HEAP_GRAPH = 'graph',
  PERF_SAMPLE = 'perf',
}

export type FlamegraphStateViewingOption =
    'SPACE'|'ALLOC_SPACE'|'OBJECTS'|'ALLOC_OBJECTS'|'PERF_SAMPLES';

export interface CallsiteInfo {
  id: number;
  parentId: number;
  depth: number;
  name?: string;
  totalSize: number;
  selfSize: number;
  mapping: string;
  merged: boolean;
  highlighted: boolean;
  location?: string;
}

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
}

export interface TraceUrlSource {
  type: 'URL';
  url: string;
}

export interface TraceHttpRpcSource {
  type: 'HTTP_RPC';
}

export type TraceSource =
    TraceFileSource|TraceArrayBufferSource|TraceUrlSource|TraceHttpRpcSource;

export interface TrackState {
  id: string;
  engineId: string;
  kind: string;
  name: string;
  labels?: string[];
  trackSortKey: TrackSortKey;
  trackGroup?: string;
  config: {
    trackId?: number;
    trackIds?: number[];
  };
}

export interface TrackGroupState {
  id: string;
  engineId: string;
  name: string;
  collapsed: boolean;
  tracks: string[];  // Child track ids.
}

export interface EngineConfig {
  id: string;
  mode?: EngineMode;  // Is undefined until |ready| is true.
  ready: boolean;
  failed?: string;  // If defined the engine has crashed with the given message.
  source: TraceSource;
}

export interface QueryConfig {
  id: string;
  engineId?: string;
  query: string;
}

export interface PermalinkConfig {
  requestId?: string;  // Set by the frontend to request a new permalink.
  hash?: string;       // Set by the controller when the link has been created.
  isRecordingConfig?:
      boolean;  // this permalink request is for a recording config only
}

export interface TraceTime {
  startSec: number;
  endSec: number;
}

export interface FrontendLocalState {
  visibleState: VisibleState;
}

export interface Status {
  msg: string;
  timestamp: number;  // Epoch in seconds (Date.now() / 1000).
}

export interface Note {
  noteType: 'DEFAULT';
  id: string;
  timestamp: number;
  color: string;
  text: string;
}

export interface AreaNote {
  noteType: 'AREA';
  id: string;
  areaId: string;
  color: string;
  text: string;
}

export interface NoteSelection {
  kind: 'NOTE';
  id: string;
}

export interface SliceSelection {
  kind: 'SLICE';
  id: number;
}

export interface CounterSelection {
  kind: 'COUNTER';
  leftTs: number;
  rightTs: number;
  id: number;
}

export interface HeapProfileSelection {
  kind: 'HEAP_PROFILE';
  id: number;
  upid: number;
  ts: number;
  type: ProfileType;
}

export interface PerfSamplesSelection {
  kind: 'PERF_SAMPLES';
  id: number;
  upid: number;
  leftTs: number;
  rightTs: number;
  type: ProfileType;
}

export interface FlamegraphState {
  kind: 'FLAMEGRAPH_STATE';
  upids: number[];
  startNs: number;
  endNs: number;
  type: ProfileType;
  viewingOption: FlamegraphStateViewingOption;
  focusRegex: string;
  expandedCallsite?: CallsiteInfo;
}

export interface CpuProfileSampleSelection {
  kind: 'CPU_PROFILE_SAMPLE';
  id: number;
  utid: number;
  ts: number;
}

export interface ChromeSliceSelection {
  kind: 'CHROME_SLICE';
  id: number;
  table: string;
}

export interface ThreadStateSelection {
  kind: 'THREAD_STATE';
  id: number;
}

export interface LogSelection {
  kind: 'LOG';
  id: number;
  trackId: string;
}

export type Selection =
    (NoteSelection|SliceSelection|CounterSelection|HeapProfileSelection|
     CpuProfileSampleSelection|ChromeSliceSelection|ThreadStateSelection|
     AreaSelection|PerfSamplesSelection|LogSelection)&{trackId?: string};
export type SelectionKind = Selection['kind'];  // 'THREAD_STATE' | 'SLICE' ...

export interface LogsPagination {
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
  direction: 'DESC'|'ASC';
}

export interface AggregationState {
  id: string;
  sorting?: Sorting;
}

export interface MetricsState {
  availableMetrics?: string[];  // Undefined until list is loaded.
  selectedIndex?: number;
  requestedMetric?: string;  // Unset after metric request is handled.
}

// Auxiliary metadata needed to parse the query result, as well as to render it
// correctly. Generated together with the text of query and passed without the
// change to the query response.
export interface PivotTableReduxQueryMetadata {
  pivotColumns: TableColumn[];
  aggregationColumns: Aggregation[];
}

// Everything that's necessary to run the query for pivot table
export interface PivotTableReduxQuery {
  text: string;
  metadata: PivotTableReduxQueryMetadata;
}

// Pivot table query result
export interface PivotTableReduxResult {
  // Hierarchical pivot structure on top of rows
  tree: PivotTree;
  // Copy of the query metadata from the request, bundled up with the query
  // result to ensure the correct rendering.
  metadata: PivotTableReduxQueryMetadata;
}

// Input parameters to check whether the pivot table needs to be re-queried.
export interface PivotTableReduxAreaState {
  areaId: string;
  tracks: string[];
}

export type SortDirection = 'DESC'|'ASC';

export interface PivotTableReduxState {
  // Currently selected area, if null, pivot table is not going to be visible.
  selectionArea?: PivotTableReduxAreaState;

  // Query response
  queryResult: PivotTableReduxResult|null;

  // Selected pivots for tables other than slice.
  // Because of the query generation, pivoting happens first on non-slice
  // pivots; therefore, those can't be put after slice pivots. In order to
  // maintain the separation more clearly, slice and non-slice pivots are
  // located in separate arrays.
  selectedPivots: RegularColumn[];

  // Selected aggregation columns. Stored same way as pivots.
  selectedAggregations: Aggregation[];

  // Whether the pivot table results should be constrained to the selected area.
  constrainToArea: boolean;

  // Set to true by frontend to request controller to perform the query to
  // acquire the necessary data from the engine.
  queryRequested: boolean;

  // Argument names in the current trace, used for autocompletion purposes.
  argumentNames: string[];
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
    LoadedConfigNone|LoadedConfigAutomatic|LoadedConfigNamed;

export interface NonSerializableState {
  pivotTableRedux: PivotTableReduxState;
}

export interface LogFilteringCriteria {
  minimumLevel: number;
  tags: string[];
  textEntry: string;
  hideNonMatching: boolean;
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
  traceTime: TraceTime;
  traceUuid?: string;
  trackGroups: ObjectById<TrackGroupState>;
  tracks: ObjectById<TrackState>;
  uiTrackIdByTraceTrackId: {[key: number]: string;};
  utidToThreadSortKey: UtidToTrackSortKey;
  areas: ObjectById<AreaById>;
  aggregatePreferences: ObjectById<AggregationState>;
  visibleTracks: string[];
  scrollingTracks: string[];
  pinnedTracks: string[];
  debugTrackId?: string;
  lastTrackReloadRequest?: number;
  queries: ObjectById<QueryConfig>;
  metrics: MetricsState;
  permalink: PermalinkConfig;
  notes: ObjectById<Note|AreaNote>;
  status: Status;
  currentSelection: Selection|null;
  currentFlamegraphState: FlamegraphState|null;
  logsPagination: LogsPagination;
  traceConversionInProgress: boolean;
  visualisedArgs: string[];

  /**
   * This state is updated on the frontend at 60Hz and eventually syncronised to
   * the controller at 10Hz. When the controller sends state updates to the
   * frontend the frontend has special logic to pick whichever version of this
   * key is most up to date.
   */
  frontendLocalState: FrontendLocalState;

  // Show track perf debugging overlay
  perfDebug: boolean;

  // Show the sidebar extended
  sidebarVisible: boolean;

  // Hovered and focused events
  hoveredUtid: number;
  hoveredPid: number;
  hoveredLogsTimestamp: number;
  hoveredNoteTimestamp: number;
  highlightedSliceId: number;
  focusedFlowIdLeft: number;
  focusedFlowIdRight: number;
  pendingScrollId?: number;

  searchIndex: number;
  currentTab?: string;

  /**
   * Trace recording
   */
  recordingInProgress: boolean;
  recordingCancelled: boolean;
  extensionInstalled: boolean;
  flamegraphModalDismissed: boolean;
  recordingTarget: RecordingTarget;
  availableAdbDevices: AdbRecordingTarget[];
  lastRecordingError?: string;
  recordingStatus?: string;

  fetchChromeCategories: boolean;
  chromeCategories: string[]|undefined;
  analyzePageQuery?: string;

  // Special key: this part of the state is not going to be serialized when
  // using permalink. Can be used to store those parts of the state that can't
  // be serialized at the moment, such as ES6 Set and Map.
  nonSerializableState: NonSerializableState;

  // Android logs filtering state.
  logFilteringCriteria: LogFilteringCriteria;

  // Omnibox info.
  omniboxState: OmniboxState;
}

export const defaultTraceTime = {
  startSec: 0,
  endSec: 10,
};

export declare type RecordMode =
    'STOP_WHEN_FULL' | 'RING_BUFFER' | 'LONG_TRACE';

// 'Q','P','O' for Android, 'L' for Linux, 'C' for Chrome.
export declare type TargetOs = 'S' | 'R' | 'Q' | 'P' | 'O' | 'C' | 'L' | 'CrOS';

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

export function isAdbTarget(target: RecordingTarget):
    target is AdbRecordingTarget {
  return !!(target as AdbRecordingTarget).serial;
}

export function hasActiveProbes(config: RecordConfig) {
  const fieldsWithEmptyResult = new Set<string>(
      ['hpBlockClient', 'allAtraceApps', 'chromePrivacyFiltering']);
  let key: keyof RecordConfig;
  for (key in config) {
    if (typeof (config[key]) === 'boolean' && config[key] === true &&
        !fieldsWithEmptyResult.has(key)) {
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
    {os: 'Q', name: 'Android Q+'},
    {os: 'P', name: 'Android P'},
    {os: 'O', name: 'Android O-'},
    {os: 'C', name: 'Chrome'},
    {os: 'CrOS', name: 'Chrome OS (system trace)'},
    {os: 'L', name: 'Linux desktop'},
  ];
}

export function getBuiltinChromeCategoryList(): string[] {
  // List of static Chrome categories, last updated at 2022-12-05 from HEAD of
  // Chromium's //base/trace_event/builtin_categories.h.
  return [
    'accessibility',
    'AccountFetcherService',
    'android_webview',
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
    'blink_gc',
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
    'catan_investigation',
    'cc',
    'cc.debug',
    'cdp.perf',
    'chromeos',
    'cma',
    'compositor',
    'content',
    'content_capture',
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
    'gpu.capture',
    'headless',
    'history',
    'hwoverlays',
    'identity',
    'ime',
    'IndexedDB',
    'input',
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
    'net',
    'netlog',
    'offline_pages',
    'omnibox',
    'oobe',
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
    'thread_pool',
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
    'weblayer',
    'WebCore',
    'webrtc',
    'webrtc_stats',
    'xr',
    'disabled-by-default-android_view_hierarchy',
    'disabled-by-default-animation-worklet',
    'disabled-by-default-audio',
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
    'disabled-by-default-cc',
    'disabled-by-default-cc.debug',
    'disabled-by-default-cc.debug.cdp-perf',
    'disabled-by-default-cc.debug.display_items',
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
    'disabled-by-default-file',
    'disabled-by-default-fonts',
    'disabled-by-default-gpu_cmd_queue',
    'disabled-by-default-gpu.dawn',
    'disabled-by-default-gpu.debug',
    'disabled-by-default-gpu.decoder',
    'disabled-by-default-gpu.device',
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
    'disabled-by-default-SyncFileSystem',
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
    'disabled-by-default-viz.gpu_composite_time',
    'disabled-by-default-viz.debug.overlay_planes',
    'disabled-by-default-viz.hit_testing_flow',
    'disabled-by-default-viz.overdraw',
    'disabled-by-default-viz.quads',
    'disabled-by-default-viz.surface_id_flow',
    'disabled-by-default-viz.surface_lifetime',
    'disabled-by-default-viz.triangles',
    'disabled-by-default-webaudio.audionode',
    'disabled-by-default-webrtc',
    'disabled-by-default-worker.scheduler',
    'disabled-by-default-xr.debug',
  ];
}

export function getContainingTrackId(state: State, trackId: string): null|
    string {
  const track = state.tracks[trackId];
  if (!track) {
    return null;
  }
  const parentId = track.trackGroup;
  if (!parentId) {
    return null;
  }
  return parentId;
}
