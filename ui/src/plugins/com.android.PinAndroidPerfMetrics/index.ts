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

import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {METRIC_HANDLERS} from './handlers/handlerRegistry';
import type {MetricData, MetricHandlerMatch} from './handlers/metricUtils';
import AndroidCujsPlugin from '../com.android.AndroidCujs';
import Wattson from '../org.kernel.Wattson';

const JANK_CUJ_QUERY_PRECONDITIONS = `
  SELECT RUN_METRIC('android/android_blocking_calls_cuj_metric.sql');
`;

function getMetricsFromHash(): string[] {
  // TODO(stevegolton): this uses `dev.perfetto.PinAndroidPerfMetrics` for
  // back-compat reasons only. Figure out a way to preserve backwards
  // compatibility of plugin arguments when plugins change id.
  const metricVal = location.hash;
  console.log('PinAndroidPerfMetrics: location.hash =', metricVal);
  const regex = new RegExp(`dev.perfetto.PinAndroidPerfMetrics:metrics=(.*)`);
  const match = metricVal.match(regex);
  if (match === null) {
    console.log('PinAndroidPerfMetrics: No metrics found in hash');
    return [];
  }
  const capturedString = match[1];
  let metricList: string[] = [];
  if (capturedString.includes('--')) {
    metricList = capturedString.split('--');
  } else {
    metricList = [capturedString];
  }
  const decodedMetrics = metricList.map((metric) => decodeURIComponent(metric));
  console.log(
    'PinAndroidPerfMetrics: Decoded metrics from hash =',
    decodedMetrics,
  );
  return decodedMetrics;
}

let metrics: string[];

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
export default class implements PerfettoPlugin {
  static readonly id = 'com.android.PinAndroidPerfMetrics';
  static readonly dependencies = [AndroidCujsPlugin, Wattson];

  static onActivate(): void {
    console.log('PinAndroidPerfMetrics: onActivate()');
    metrics = getMetricsFromHash();
    console.log('PinAndroidPerfMetrics: metrics to pin =', metrics);
    Wattson.updateWindowsOfInterest(metrics);
  }

  async onTraceLoad(ctx: Trace) {
    console.log('PinAndroidPerfMetrics: onTraceLoad()');
    ctx.commands.registerCommand({
      id: 'com.android.PinAndroidPerfMetrics',
      name: 'Add and Pin: Jank Metric Slice',
      callback: async () => {
        console.log('PinAndroidPerfMetrics: Command triggered');
        const metric = await ctx.omnibox.prompt(
          'Metrics names (separated by comma)',
        );
        if (metric === undefined) {
          console.log('PinAndroidPerfMetrics: Command cancelled (no input)');
          return;
        }
        const metricList = metric.split(',');
        console.log(
          'PinAndroidPerfMetrics: Command input metrics =',
          metricList,
        );
        this.callHandlers(metricList, ctx);
      },
    });
    if (metrics.length !== 0) {
      console.log(
        'PinAndroidPerfMetrics: Automatic pinning triggered for metrics =',
        metrics,
      );
      const plugin = ctx.plugins.getPlugin(AndroidCujsPlugin);
      await plugin.pinJankCujs(ctx);
      await plugin.pinLatencyCujs(ctx);
      this.callHandlers(metrics, ctx);
    } else {
      console.log('PinAndroidPerfMetrics: No metrics to automatically pin');
    }
  }

  private async callHandlers(metricsList: string[], ctx: Trace) {
    console.log('PinAndroidPerfMetrics: callHandlers() with =', metricsList);
    // List of metrics that actually match some handler
    const metricsToShow: MetricHandlerMatch[] =
      this.getMetricsToShow(metricsList);

    console.log('PinAndroidPerfMetrics: metricsToShow =', metricsToShow);
    if (metricsToShow.length === 0) {
      return;
    }

    console.log('PinAndroidPerfMetrics: Running precondition query');
    await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
    for (const {metricData, metricHandler} of metricsToShow) {
      console.log(
        'PinAndroidPerfMetrics: Adding track for',
        metricData,
        'using',
        metricHandler,
      );
      metricHandler.addMetricTrack(metricData, ctx);
    }
  }

  private getMetricsToShow(metricList: string[]): MetricHandlerMatch[] {
    console.log(
      'PinAndroidPerfMetrics: getMetricsToShow() matching metrics against handlers',
    );
    const sortedMetricList = [...metricList].sort();
    const validMetrics: MetricHandlerMatch[] = [];
    const alreadyMatchedMetricData: Set<string> = new Set();
    for (const metric of sortedMetricList) {
      for (const metricHandler of METRIC_HANDLERS) {
        const metricData = metricHandler.match(metric);
        if (!metricData) continue;
        const jsonMetricData = this.metricDataToJson(metricData);
        if (!alreadyMatchedMetricData.has(jsonMetricData)) {
          console.log(
            `PinAndroidPerfMetrics: Metric "${metric}" matched handler`,
            metricHandler,
            'with data',
            metricData,
          );
          alreadyMatchedMetricData.add(jsonMetricData);
          validMetrics.push({
            metricData: metricData,
            metricHandler: metricHandler,
          });
        } else {
          console.log(
            `PinAndroidPerfMetrics: Metric "${metric}" already matched (duplicate)`,
          );
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
