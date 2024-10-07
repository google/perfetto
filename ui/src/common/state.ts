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
import {TraceSource} from '../public/trace_source';

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

export interface EngineConfig {
  id: string;
  source: TraceSource;
}

export interface QueryConfig {
  id: string;
  engineId?: string;
  query: string;
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

export interface PendingDeeplinkState {
  ts?: string;
  dur?: string;
  tid?: string;
  pid?: string;
  query?: string;
  visStart?: string;
  visEnd?: string;
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
  engine?: EngineConfig;

  debugTrackId?: string;
  lastTrackReloadRequest?: number;
  flamegraphModalDismissed: boolean;

  // Show track perf debugging overlay
  perfDebug: boolean;

  // Show the sidebar extended
  sidebarVisible: boolean;

  // Hovered and focused events
  hoveredUtid: number;
  hoveredPid: number;
  hoveredNoteTimestamp: time;
  highlightedSliceId: number;

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

  trackFilterTerm: string | undefined;

  // TODO(primiano): this is a hack to force-re-run controllers required for the
  // controller->managers migration. Remove once controllers are gone.
  forceRunControllers: number;
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
