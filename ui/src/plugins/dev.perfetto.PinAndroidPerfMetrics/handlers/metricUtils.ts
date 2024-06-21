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

import {PluginContextTrace} from '../../../public';

// TODO: b/337774166 - Perfetto FT handler MetricData
export interface FullTraceMetricData {}

// TODO: b/337774166 - Perfetto CUJ handler MetricData
export interface CujScopedMetricData {}

// TODO: b/337774166 - Blocking Call handler MetricData
export interface BlockingCallMetricData {}

// Common MetricData for all handler. If new needed then add here.
export type MetricData =
  | FullTraceMetricData
  | CujScopedMetricData
  | BlockingCallMetricData;

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
   * @param {PluginContextTrace} ctx The plugin context.
   * @param {string} type 'static' onTraceload to register, 'debug' on command.
   * @returns {void}
   */
  addDebugTrack(
    metricData: MetricData,
    ctx: PluginContextTrace,
    type: 'static' | 'debug',
  ): void;
}
