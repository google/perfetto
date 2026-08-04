// Copyright (C) 2024 The Android Open Source Project
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
import type {Trace} from '../../../public/trace';

/**
 * Represents data for a Full trace metric
 * Eg.- perfetto_ft_launcher-missed_sf_frames-mean
 * ft here stands for full trace
 */
export interface FullTraceMetricData {
  /** Process name (e.g., com.google.android.apps.nexuslauncher) */
  process: string;

  /** Jank type (e.g., app or sf missed frame) */
  jankType: JankType;

  /** Whether the metric is weighted */
  isWeighted: boolean;
}

/**
 * Represents data for a CUJ scoped metric
 * Eg.- perfetto_cuj_launcher-RECENTS_SCROLLING-counter_metrics-missed_sf_frames-mean
 */
export interface CujScopedMetricData {
  /** Process name (e.g., com.google.android.apps.nexuslauncher) */
  process: string;

  /** Cuj interaction name (e.g., RECENTS_SCROLLING) */
  cujName: string;

  /** Jank type (e.g., app or sf missed frame) */
  jankType: JankType;

  /** Whether the metric is weighted */
  isWeighted: boolean;
}

/**
 * Represents data for a Blocking Call metric
 * Eg.- perfetto_android_blocking_call-cuj-name-com.google.android.apps.nexuslauncher-name-TASKBAR_EXPAND-blocking_calls-name-animation-total_dur_ms-mean
 * Eg.- perfetto_android_blocking_call_per_frame-cuj-name-com.android.systemui-name-NOTIFICATION_SHADE_EXPAND_COLLAPSE::Collapse-blocking_calls-name-input-mean_dur_per_frame_ns-max
 */
export interface BlockingCallMetricData {
  /** Process name (e.g., com.google.android.apps.nexuslauncher) */
  process: string;

  /** Cuj interaction name (e.g., TASKBAR_EXPAND) */
  cujName: string;

  /** Blocking Call name (e.g., animation) */
  blockingCallName: string;

  /** aggregation type (e.g., total_dur_ms-mean) */
  aggregation: string;
}

/**
 * Represents data for a Notifications Blocking Call metric
 * Eg.- perfetto_android_notifications_blocking_call-blocking_calls-name-NotificationStackScrollLayout#onMeasure-cnt
 * Eg.- perfetto_android_notifications_blocking_call-blocking_calls-name-ExpNotRow#onLayout(nostyle)-total_dur_ns
 */
export interface NotificationsBlockingCallMetricData {
  /** Notification name (e.g., NotificationStackScrollLayout) */
  notificationName: string;

  /** aggregation type (e.g., total_dur_ms) */
  aggregation: string;
}

/** Represents a cuj to be pinned. */
export interface CujMetricData {
  cujName: string;
}

export interface ProcessMetricData {
  process: string;
}

export interface GlobalDmaHeapMetricData {}

// Common MetricData for all handler. If new needed then add here.
export type MetricData =
  | FullTraceMetricData
  | CujScopedMetricData
  | BlockingCallMetricData
  | NotificationsBlockingCallMetricData
  | CujMetricData
  | ProcessMetricData
  | GlobalDmaHeapMetricData;

// Common JankType for cujScoped and fullTrace metrics
export type JankType = 'sf_frames' | 'app_frames' | 'frames';

export enum PinIntentKind {
  Cuj = 'cuj',
  CujScopedJank = 'cuj_scoped_jank',
  CujBlockingCall = 'cuj_blocking_call',
  NotificationBlockingCall = 'notification_blocking_call',
  FullTraceJank = 'full_trace_jank',
  ProcessMemory = 'process_memory',
  GlobalDmaHeap = 'global_dma_heap',
}

export enum ProcessMemoryType {
  HeapSize = 'heapSize',
  BitmapMemory = 'bitmapMemory',
  DirtyMemory = 'dirtyMemory',
  GpuMemory = 'gpuMemory',
  ActivityOrBinderLeaks = 'activityOrBinderLeaks',
  HardwareBufferMemory = 'hardwareBufferMemory',
}

export const ALLOWED_PROCESS_MEMORY_TYPES = Object.values(
  ProcessMemoryType,
) as ProcessMemoryType[];

export interface ProcessMemoryMetricData extends ProcessMetricData {
  memoryType: ProcessMemoryType;
}

export type PinIntent =
  | ({kind: PinIntentKind.Cuj} & CujMetricData)
  | ({kind: PinIntentKind.CujScopedJank} & CujScopedMetricData)
  | ({kind: PinIntentKind.CujBlockingCall} & BlockingCallMetricData)
  | ({
      kind: PinIntentKind.NotificationBlockingCall;
    } & NotificationsBlockingCallMetricData)
  | ({kind: PinIntentKind.FullTraceJank} & FullTraceMetricData)
  | ({kind: PinIntentKind.ProcessMemory} & ProcessMemoryMetricData)
  | ({kind: PinIntentKind.GlobalDmaHeap} & GlobalDmaHeapMetricData);

export type PinRequest = PinIntent;
export type PinMetricRequest = PinIntent;

/**
 * Common interface for debug track handlers
 *
 * @template T
 */
export interface MetricHandler<T extends MetricData = MetricData> {
  readonly kind: PinIntentKind;

  /**
   * Match metric key & return parsed data if successful.
   *
   * @param {string} metricKey The metric key to match.
   * @returns {T | undefined} Parsed data or undefined if no match.
   */
  match(metricKey: string): T | undefined;

  /**
   * Parse a raw dictionary request & return parsed data if successful.
   *
   * @param {Record<string, string>} item Dictionary to match and parse.
   * @returns {T | undefined} Parsed data or undefined if no match.
   */
  parseRequest?(item: Record<string, string>): T | undefined;

  /**
   * Add debug track for parsed metric data.
   *
   * @param {T} metricData The parsed metric data.
   * @param {Trace} ctx context for trace methods and properties
   * @returns {void | Promise<void>}
   */
  addMetricTrack(metricData: T, ctx: Trace): void | Promise<void>;
}

// Pair for matching metric and its handler
export type MetricHandlerMatch = {
  metricData: MetricData;
  metricHandler: MetricHandler;
};

export const PROCESS_FIELD_ALIASES = [
  'process',
  'processName',
  'process_name',
  'pkg',
  'package',
];

export const CUJ_FIELD_ALIASES = [
  'cuj',
  'cujName',
  'cuj_name',
  'CUJ',
  'cuj_id',
];

export const JANK_TYPE_FIELD_ALIASES = [
  'jankType',
  'jank_type',
  'frameType',
  'frame_type',
];

export const ALLOWED_JANK_TYPES = [
  'sf_frames',
  'app_frames',
  'frames',
] as const;

export const IS_WEIGHTED_FIELD_ALIASES = [
  'isWeighted',
  'weighted',
  'jps',
  'weighted_missed',
];

export const BLOCKING_CALL_FIELD_ALIASES = [
  'blockingCall',
  'blocking_call',
  'blockingCallName',
  'call',
  'callName',
];

export const NOTIFICATION_FIELD_ALIASES = [
  'notification',
  'notificationName',
  'notification_name',
];

export const AGGREGATION_FIELD_ALIASES = ['aggregation', 'agg'];

export const MEMORY_TYPE_FIELD_ALIASES = [
  'memoryType',
  'memory',
  'mem',
  'mem_type',
  'track',
];

export const FULL_TRACE_FLAG_ALIASES = [
  'fullTrace',
  'ft',
  'missedFrames',
  'full_trace',
];

export const GLOBAL_DMA_HEAP_FLAG_ALIASES = [
  'globalDmaHeap',
  'dmaHeap',
  'dma_heap',
];

export const ALL_JANK_CUJS_FLAG_ALIASES = [
  'allJankCujs',
  'jank_cujs',
  'all_jank_cujs',
];

export const ALL_LATENCY_CUJS_FLAG_ALIASES = [
  'allLatencyCujs',
  'latency_cujs',
  'all_latency_cujs',
];

export function extractProp(
  item: Record<string, string>,
  keys: string[],
): string | undefined {
  for (const k of keys) {
    const val = item[k];
    if (typeof val === 'string' && val !== '') {
      return val;
    }
  }
  return undefined;
}

export function extractCujName(
  item: Record<string, string>,
): string | undefined {
  return extractProp(item, CUJ_FIELD_ALIASES);
}

export function extractProcess(
  item: Record<string, string>,
): string | undefined {
  return extractProp(item, PROCESS_FIELD_ALIASES);
}

export function extractJankType(
  item: Record<string, string>,
): JankType | undefined {
  const val = extractProp(item, JANK_TYPE_FIELD_ALIASES);
  if (val === 'sf_frames' || val === 'app_frames' || val === 'frames') {
    return val;
  }
  return undefined;
}

export function extractIsWeighted(
  item: Record<string, string>,
): boolean | undefined {
  const val = extractProp(item, IS_WEIGHTED_FIELD_ALIASES);
  if (val === 'true' || val === '1') {
    return true;
  }
  if (val === 'false' || val === '0') {
    return false;
  }
  return undefined;
}

export function extractBlockingCallName(
  item: Record<string, string>,
): string | undefined {
  return extractProp(item, BLOCKING_CALL_FIELD_ALIASES);
}

export function extractNotificationName(
  item: Record<string, string>,
): string | undefined {
  return extractProp(item, NOTIFICATION_FIELD_ALIASES);
}

export function extractAggregation(
  item: Record<string, string>,
): string | undefined {
  return extractProp(item, AGGREGATION_FIELD_ALIASES);
}

export function extractBooleanFlag(
  item: Record<string, string>,
  keys: string[],
): boolean {
  const val = extractProp(item, keys);
  return val === 'true' || val === '1';
}

/**
 * Expand process name for specific system processes
 *
 * @param {string} metricProcessName Name of the processes
 * @returns {string} Either the same or expanded name for abbreviated process names
 */
export function expandProcessName(metricProcessName: string): string {
  if (metricProcessName.includes('systemui')) {
    return 'com.android.systemui';
  } else if (metricProcessName.includes('launcher')) {
    return 'com.google.android.apps.nexuslauncher';
  } else if (metricProcessName.includes('surfaceflinger')) {
    return '/system/bin/surfaceflinger';
  } else {
    return metricProcessName;
  }
}
