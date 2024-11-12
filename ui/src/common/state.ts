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

export const MAX_TIME = 180;

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

export interface RecordCommand {
  commandline: string;
  pbtxt: string;
  pbBase64: string;
}

export interface RecordingState {
  /**
   * State of the ConfigEditor.
   */
  recordConfig: RecordConfig;
  lastLoadedConfig: LoadedConfig;

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

  bufferUsage: number;
  recordingLog: string;
  recordCmd?: RecordCommand;
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
  return ['Q', 'P', 'O', 'S'].includes(target.os);
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
