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
import {LONG, NUM} from '../../../trace_processor/query_result';
import {PluginContextTrace} from '../../../public';
import {SimpleSliceTrackConfig} from '../../../frontend/simple_slice_track';
import {addJankCUJDebugTrack} from '../../dev.perfetto.AndroidCujs';
import {
  addAndPinSliceTrack,
  focusOnSlice,
  SliceIdentifier,
  TrackType,
} from '../../dev.perfetto.AndroidCujs/trackUtils';
import {PLUGIN_ID} from '../pluginId';
import {Time} from '../../../base/time';

const ENABLE_FOCUS_ON_FIRST_JANK = true;

class PinCujScopedJank implements MetricHandler {
  /**
   * Matches metric key & return parsed data if successful.
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
   * Adds the debug tracks for cuj Scoped jank metrics
   * registerStaticTrack used when plugin adds tracks onTraceload()
   * addDebugSliceTrack used for adding tracks using the command
   *
   * @param {CujScopedMetricData} metricData Parsed metric data for the cuj scoped jank
   * @param {PluginContextTrace} ctx PluginContextTrace for trace related properties and methods
   * @param {TrackType} type 'static' for onTraceload and 'debug' for command
   * @returns {void} Adds one track for Jank CUJ slice and one for Janky CUJ frames
   */
  public async addMetricTrack(
    metricData: CujScopedMetricData,
    ctx: PluginContextTrace,
    type: TrackType,
  ) {
    // TODO: b/349502258 - Refactor to single API
    const {config: cujScopedJankSlice, trackName: trackName} =
      await this.cujScopedTrackConfig(metricData, ctx);
    this.pinSingleCuj(ctx, metricData, type);
    const uri = `${PLUGIN_ID}#CUJScopedJankSlice#${metricData}`;

    addAndPinSliceTrack(ctx, cujScopedJankSlice, trackName, type, uri);
    if (ENABLE_FOCUS_ON_FIRST_JANK) {
      await this.focusOnFirstJank(ctx);
    }
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

  private async cujScopedTrackConfig(
    metricData: CujScopedMetricData,
    ctx: PluginContextTrace,
  ): Promise<{
    config: SimpleSliceTrackConfig;
    trackName: string;
  }> {
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

    const createJankyCujFrameTable = `
    CREATE PERFETTO TABLE _janky_frames_during_cuj_from_metric_key AS
    SELECT
      f.vsync as id,
      f.ts AS ts,
      f.dur as dur
    FROM android_jank_cuj_frame f LEFT JOIN android_jank_cuj cuj USING (cuj_id)
    WHERE cuj.process_name = "${processName}" 
    AND cuj_name = "${cuj}" ${jankTypeFilter}
    `;

    await ctx.engine.query(createJankyCujFrameTable);

    const jankyFramesDuringCujQuery = `
    SELECT id, ts, dur 
    FROM _janky_frames_during_cuj_from_metric_key
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

  private async findFirstJank(
    ctx: PluginContextTrace,
  ): Promise<SliceIdentifier> {
    const queryForFirstJankyFrame = `
      SELECT slice_id, track_id, ts, dur FROM slice
        WHERE type = "actual_frame_timeline_slice"
        AND name =
        CAST(
        (SELECT id FROM _janky_frames_during_cuj_from_metric_key LIMIT 1)
        AS VARCHAR(20) );
    `;
    const queryResult = await ctx.engine.query(queryForFirstJankyFrame);
    const row = queryResult.firstRow({
      slice_id: NUM,
      track_id: NUM,
      ts: LONG,
      dur: LONG,
    });
    const slice: SliceIdentifier = {
      sliceId: row.slice_id,
      trackId: row.track_id,
      ts: Time.fromRaw(row.ts),
      dur: row.dur,
    };
    return slice;
  }

  private async focusOnFirstJank(ctx: PluginContextTrace) {
    const slice = await this.findFirstJank(ctx);
    await focusOnSlice(slice);
  }
}

export const pinCujScopedJankInstance = new PinCujScopedJank();
