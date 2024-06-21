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
import {METRIC_HANDLERS} from './handlers/handlerRegistry';

const PLUGIN_ID = 'dev.perfetto.PinAndroidPerfMetrics';

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

  private callHandlers(
    metricsList: string[],
    ctx: PluginContextTrace,
    type: 'static' | 'debug',
  ) {
    for (const metric of metricsList) {
      for (const metricHandler of METRIC_HANDLERS) {
        const match = metricHandler.match(metric);
        if (match) {
          metricHandler.addDebugTrack(match, ctx, type);
          break;
        }
      }
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
    if (capturedString.includes('--')) {
      return capturedString.split('--');
    } else {
      return [capturedString];
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: PLUGIN_ID,
  plugin: PinAndroidPerfMetrics,
};
