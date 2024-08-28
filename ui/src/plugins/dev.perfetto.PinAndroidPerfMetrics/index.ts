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

import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {TrackType} from '../dev.perfetto.AndroidCujs/trackUtils';
import {METRIC_HANDLERS} from './handlers/handlerRegistry';
import {MetricHandlerMatch} from './handlers/metricUtils';
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
class PinAndroidPerfMetrics implements Plugin {
  private metrics: string[] = [];

  onActivate(): void {
    this.metrics = this.getMetricsFromHash();
  }

  async onTraceLoad(ctx: PluginContextTrace) {
    ctx.registerCommand({
      id: 'dev.perfetto.PinAndroidPerfMetrics#PinAndroidPerfMetrics',
      name: 'Add and Pin: Jank Metric Slice',
      callback: async (metric) => {
        metric = prompt('Metrics names (seperated by comma)', '');
        if (metric === null) return;
        const metricList = metric.split(',');
        this.callHandlers(metricList, ctx, 'debug');
      },
    });
    if (this.metrics.length !== 0) {
      this.callHandlers(this.metrics, ctx, 'static');
    }
  }

  private async callHandlers(
    metricsList: string[],
    ctx: PluginContextTrace,
    type: TrackType,
  ) {
    // List of metrics that actually match some handler
    const metricsToShow: MetricHandlerMatch[] =
      this.getMetricsToShow(metricsList);

    if (metricsToShow.length === 0) {
      return;
    }

    await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
    for (const {metricData, metricHandler} of metricsToShow) {
      metricHandler.addMetricTrack(metricData, ctx, type);
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

  private getMetricsToShow(metricList: string[]) {
    const validMetrics: MetricHandlerMatch[] = [];
    metricList.forEach((metric) => {
      const matchedHandler = this.matchMetricToHandler(metric);
      if (matchedHandler) {
        validMetrics.push(matchedHandler);
      }
    });
    return validMetrics;
  }

  private matchMetricToHandler(metric: string): MetricHandlerMatch | null {
    for (const metricHandler of METRIC_HANDLERS) {
      const match = metricHandler.match(metric);
      if (match) {
        return {
          metricData: match,
          metricHandler: metricHandler,
        };
      }
    }
    return null;
  }
}

export const plugin: PluginDescriptor = {
  pluginId: PLUGIN_ID,
  plugin: PinAndroidPerfMetrics,
};
