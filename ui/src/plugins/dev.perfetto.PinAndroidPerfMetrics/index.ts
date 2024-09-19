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

import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {METRIC_HANDLERS} from './handlers/handlerRegistry';
import {MetricData, MetricHandlerMatch} from './handlers/metricUtils';
import {PLUGIN_ID} from './pluginId';

const JANK_CUJ_QUERY_PRECONDITIONS = `
  SELECT RUN_METRIC('android/android_blocking_calls_cuj_metric.sql');
`;

/**
 * Plugin that adds and pins the debug track for the metric passed
 * For more context -
 * This plugin reads the names of regressed metrics from the url upon loading
 * It then checks the metric names against some handlers and if they
 * match it accordingly adds the debug tracks for them
 * This way when comparing two different perfetto traces before and after
 * the regression, the user will not have to manually search for the
 * slices related to the regressed metric
 */
class PinAndroidPerfMetrics implements PerfettoPlugin {
  private metrics: string[] = [];

  onActivate(): void {
    this.metrics = this.getMetricsFromHash();
  }

  async onTraceReady(ctx: Trace) {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.PinAndroidPerfMetrics#PinAndroidPerfMetrics',
      name: 'Add and Pin: Jank Metric Slice',
      callback: async (metric) => {
        metric = prompt('Metrics names (separated by comma)', '');
        if (metric === null) return;
        const metricList = metric.split(',');
        this.callHandlers(metricList, ctx);
      },
    });
    if (this.metrics.length !== 0) {
      this.callHandlers(this.metrics, ctx);
    }
  }

  private async callHandlers(metricsList: string[], ctx: Trace) {
    // List of metrics that actually match some handler
    const metricsToShow: MetricHandlerMatch[] =
      this.getMetricsToShow(metricsList);

    if (metricsToShow.length === 0) {
      return;
    }

    await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
    for (const {metricData, metricHandler} of metricsToShow) {
      metricHandler.addMetricTrack(metricData, ctx);
    }
  }

  private getMetricsFromHash(): string[] {
    const metricVal = location.hash;
    const regex = new RegExp(`${PLUGIN_ID}:metrics=(.*)`);
    const match = metricVal.match(regex);
    if (match === null) {
      return [];
    }
    const capturedString = match[1];
    let metricList: string[] = [];
    if (capturedString.includes('--')) {
      metricList = capturedString.split('--');
    } else {
      metricList = [capturedString];
    }
    return metricList.map((metric) => decodeURIComponent(metric));
  }

  private getMetricsToShow(metricList: string[]): MetricHandlerMatch[] {
    const sortedMetricList = [...metricList].sort();
    const validMetrics: MetricHandlerMatch[] = [];
    const alreadyMatchedMetricData: Set<string> = new Set();
    for (const metric of sortedMetricList) {
      for (const metricHandler of METRIC_HANDLERS) {
        const metricData = metricHandler.match(metric);
        if (!metricData) continue;
        const jsonMetricData = this.metricDataToJson(metricData);
        if (!alreadyMatchedMetricData.has(jsonMetricData)) {
          alreadyMatchedMetricData.add(jsonMetricData);
          validMetrics.push({
            metricData: metricData,
            metricHandler: metricHandler,
          });
        }
      }
    }
    return validMetrics;
  }

  private metricDataToJson(metricData: MetricData): string {
    // Used to have a deterministic keys order.
    return JSON.stringify(metricData, Object.keys(metricData).sort());
  }
}

export const plugin: PluginDescriptor = {
  pluginId: PLUGIN_ID,
  plugin: PinAndroidPerfMetrics,
};
