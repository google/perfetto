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

import {
  expandProcessName,
  CujScopedMetricData,
  MetricHandler,
  JankType,
} from './metricUtils';
import {PluginContextTrace} from '../../../public';
import {SimpleSliceTrackConfig} from '../../../frontend/simple_slice_track';
import {PLUGIN_ID} from '..';
import {addJankCUJDebugTrack} from '../../dev.perfetto.AndroidCujs';
import {
  addAndPinSliceTrack,
  TrackType,
} from '../../dev.perfetto.AndroidCujs/trackUtils';

class PinCujScopedJank implements MetricHandler {
  /**
   * Match metric key & return parsed data if successful.
   *
   * @param {string} metricKey The metric key to match.
   * @returns {CujScopedMetricData | undefined} Parsed data or undefined if no match.
   */
  public match(metricKey: string): CujScopedMetricData | undefined {
    const matcher =
      /perfetto_cuj_(?<process>.*)-(?<cujName>.*)-.*-missed_(?<jankType>frames|sf_frames|app_frames)/;
    const match = matcher.exec(metricKey);
    if (!match?.groups) {
      return undefined;
    }
    const metricData: CujScopedMetricData = {
      process: expandProcessName(match.groups.process),
      cujName: match.groups.cujName,
      jankType: match.groups.jankType as JankType,
    };
    return metricData;
  }

  /**
   * Function to add the debug tracks for cuj Scoped jank metrics
   * registerStaticTrack used when plugin adds tracks onTraceload()
   * addDebugSliceTrack used for adding tracks using the command
   *
   * @param {CujScopedMetricData} metricData Parsed metric data for the cuj scoped jank
   * @param {PluginContextTrace} ctx PluginContextTrace for trace related properties and methods
   * @param {TrackType} type 'static' when called onTraceload and 'debug' when called through command
   * @returns {void} Adds one track for Jank CUJ slice and one for Janky CUJ frames
   */
  public addMetricTrack(
    metricData: CujScopedMetricData,
    ctx: PluginContextTrace,
    type: TrackType,
  ): void {
    // TODO: b/349502258 - Refactor to single API
    const {config: cujScopedJankSlice, trackName: trackName} =
      this.cujScopedTrackConfig(metricData);
    this.pinSingleCuj(ctx, metricData, type);
    const uri = `${PLUGIN_ID}#CUJScopedJankSlice#${metricData}`;

    addAndPinSliceTrack(ctx, cujScopedJankSlice, trackName, type, uri);
  }

  private pinSingleCuj(
    ctx: PluginContextTrace,
    metricData: CujScopedMetricData,
    type: TrackType,
  ) {
    const uri = `${PLUGIN_ID}#CUJScopedBoundaryTimes#${metricData}`;
    const trackName = `Jank CUJ: ${metricData.cujName}`;
    addJankCUJDebugTrack(ctx, trackName, type, metricData.cujName, uri);
  }

  private cujScopedTrackConfig(metricData: CujScopedMetricData): {
    config: SimpleSliceTrackConfig;
    trackName: string;
  } {
    let jankTypeFilter;
    let jankTypeDisplayName = 'all';
    if (metricData.jankType?.includes('app')) {
      jankTypeFilter = ' AND app_missed > 0';
      jankTypeDisplayName = 'app';
    } else if (metricData.jankType?.includes('sf')) {
      jankTypeFilter = ' AND sf_missed > 0';
      jankTypeDisplayName = 'sf';
    }
    const cuj = metricData.cujName;
    const processName = metricData.process;

    const jankyFramesDuringCujQuery = `
    SELECT
      f.vsync as id,
      f.ts AS ts,
      f.dur as dur
    FROM android_jank_cuj_frame f LEFT JOIN android_jank_cuj cuj USING (cuj_id)
    WHERE cuj.process_name = "${processName}" AND cuj_name = "${cuj}" ${jankTypeFilter}
    `;

    const cujScopedJankSlice: SimpleSliceTrackConfig = {
      data: {
        sqlSource: jankyFramesDuringCujQuery,
        columns: ['id', 'ts', 'dur'],
      },
      columns: {ts: 'ts', dur: 'dur', name: 'id'},
      argColumns: ['id', 'ts', 'dur'],
    };

    const trackName = jankTypeDisplayName + ' missed frames in ' + processName;

    return {config: cujScopedJankSlice, trackName: trackName};
  }
}

export const pinCujScopedJankInstance = new PinCujScopedJank();
