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

import {CujMetricData, MetricHandler} from './metricUtils';
import {PluginContextTrace} from '../../../public';
import {addJankCUJDebugTrack} from '../../dev.perfetto.AndroidCujs';
import {TrackType} from '../../dev.perfetto.AndroidCujs/trackUtils';
import {PLUGIN_ID} from '../pluginId';

/** Pins a single CUJ from CUJ scoped metrics. */
class PinCujMetricHandler implements MetricHandler {
  /**
   * Matches metric key & return parsed data if successful.
   *
   * @param {string} metricKey The metric key to match.
   * @returns {CujMetricData | undefined} Parsed data or undefined if no match.
   */
  public match(metricKey: string): CujMetricData | undefined {
    const matcher = /perfetto_cuj_(?<process>.*)-(?<cujName>.*)-.*-missed_.*/;
    const match = matcher.exec(metricKey);
    if (!match?.groups) {
      return undefined;
    }
    return {
      cujName: match.groups.cujName,
    };
  }

  /**
   * Adds the debug tracks for cuj Scoped jank metrics
   * registerStaticTrack used when plugin adds tracks onTraceload()
   * addDebugSliceTrack used for adding tracks using the command
   *
   * @param {CujMetricData} metricData Parsed metric data for the cuj scoped jank
   * @param {PluginContextTrace} ctx PluginContextTrace for trace related properties and methods
   * @param {TrackType} type 'static' for onTraceload and 'debug' for command
   * @returns {void} Adds one track for Jank CUJ slice and one for Janky CUJ frames
   */
  public async addMetricTrack(
    metricData: CujMetricData,
    ctx: PluginContextTrace,
    type: TrackType,
  ) {
    this.pinSingleCuj(ctx, metricData.cujName, type);
  }

  private pinSingleCuj(
    ctx: PluginContextTrace,
    cujName: string,
    type: TrackType,
  ) {
    const uri = `${PLUGIN_ID}#PinCuj#${cujName}`;
    const trackName = `Jank CUJ: ${cujName}`;
    addJankCUJDebugTrack(ctx, trackName, type, cujName, uri);
  }
}

export const pinCujInstance = new PinCujMetricHandler();
