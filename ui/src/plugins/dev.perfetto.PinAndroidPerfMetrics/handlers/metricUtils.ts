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
import {Trace} from '../../../public/trace';

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
}

/**
 * Represents data for a Blocking Call metric
 * Eg.- perfetto_android_blocking_call-cuj-name-com.google.android.apps.nexuslauncher-name-TASKBAR_EXPAND-blocking_calls-name-animation-total_dur_ms-mean
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

/** Represents a cuj to be pinned. */
export interface CujMetricData {
  cujName: string;
}

// Common MetricData for all handler. If new needed then add here.
export type MetricData =
  | FullTraceMetricData
  | CujScopedMetricData
  | BlockingCallMetricData
  | CujMetricData;

// Common JankType for cujScoped and fullTrace metrics
export type JankType = 'sf_frames' | 'app_frames' | 'frames';

/**
 * Common interface for debug track handlers
 */
export interface MetricHandler {
  /**
   * Match metric key & return parsed data if successful.
   *
   * @param {string} metricKey The metric key to match.
   * @returns {MetricData | undefined} Parsed data or undefined if no match.
   */
  match(metricKey: string): MetricData | undefined;

  /**
   * Add debug track for parsed metric data.
   *
   * @param {MetricData} metricData The parsed metric data.
   * @param {Trace} ctx context for trace methods and properties
   * @returns {void}
   */
  addMetricTrack(metricData: MetricData, ctx: Trace): void;
}

// Pair for matching metric and its handler
export type MetricHandlerMatch = {
  metricData: MetricData;
  metricHandler: MetricHandler;
};

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
