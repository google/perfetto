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
import {crystalballToPinRequests} from './handlers/handlerRegistry';
import {executePinRequests} from './handlers/executor';
import {parsePinRequests, PinRequestType} from './handlers/pinRequest';
import AndroidCujsPlugin from '../com.android.AndroidCujs';
import Wattson from '../org.kernel.Wattson';

function getMetricsFromHash(): string[] {
  // TODO(stevegolton): this uses `dev.perfetto.PinAndroidPerfMetrics` for
  // back-compat reasons only. Figure out a way to preserve backwards
  // compatibility of plugin arguments when plugins change id.
  const metricVal = location.hash;
  const regex = new RegExp(`dev.perfetto.PinAndroidPerfMetrics:metrics=(.*)`);
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
    metrics = getMetricsFromHash();
    Wattson.updateWindowsOfInterest(metrics);
  }

  async onTraceLoad(ctx: Trace) {
    ctx.commands.registerCommand({
      id: 'com.android.PinAndroidPerfMetrics',
      name: 'Add and Pin: Jank Metric Slice',
      callback: async () => {
        const metric = await ctx.omnibox.prompt(
          'Metrics names (separated by comma)',
        );
        if (metric === undefined) return;
        await executePinRequests(
          ctx,
          crystalballToPinRequests(metric.split(',')),
        );
      },
    });

    // Generic entry point — the bridge for startup commands / other clients
    // that hand us PinRequests directly (no Crystalball string involved).
    ctx.commands.registerCommand({
      id: 'com.android.PinAndroidPerfMetrics#pinRequests',
      name: 'Pin performance tracks from PinRequest[]',
      callback: (arg: unknown) =>
        executePinRequests(ctx, parsePinRequests(arg)),
    });

    if (metrics.length !== 0) {
      await executePinRequests(ctx, [
        {type: PinRequestType.AllJankCujs},
        {type: PinRequestType.AllLatencyCujs},
        ...crystalballToPinRequests(metrics),
      ]);
    }
  }
}
