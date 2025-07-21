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
  BlockingCallMetricData,
  MetricHandler,
} from './metricUtils';
import {Trace} from '../../../public/trace';
import {
  addJankCUJDebugTrack,
  addLatencyCUJDebugTrack,
} from '../../dev.perfetto.AndroidCujs';
import {addDebugSliceTrack} from '../../../components/tracks/debug_tracks';
import {LONG} from '../../../trace_processor/query_result';

class BlockingCallMetricHandler implements MetricHandler {
  /**
   * Matches metric key for blocking call and per-frame blocking call metrics & return parsed data
   * if successful.
   *
   * @param {string} metricKey The metric key to match.
   * @returns {BlockingCallMetricData | undefined} Parsed data or undefined if no match.
   */
  public match(metricKey: string): BlockingCallMetricData | undefined {
    const matcher =
      /perfetto_android_blocking_call(?:_per_frame)?-cuj-name-(?<process>.*)-name-(?<cujName>.*)-blocking_calls-name-(?<blockingCallName>([^\-]*))-(?<aggregation>.*)/;
    const match = matcher.exec(metricKey);
    if (!match?.groups) {
      return undefined;
    }
    const metricData: BlockingCallMetricData = {
      process: expandProcessName(match.groups.process),
      cujName: match.groups.cujName,
      blockingCallName: match.groups.blockingCallName,
      aggregation: match.groups.aggregation,
    };
    return metricData;
  }

  /**
   * Adds the debug tracks for Blocking Call metrics
   *
   * @param {BlockingCallMetricData} metricData Parsed metric data for the cuj scoped jank
   * @param {Trace} ctx PluginContextTrace for trace related properties and methods
   * @returns {void} Adds one track for Jank CUJ slice and one for Janky CUJ frames
   */
  public addMetricTrack(metricData: BlockingCallMetricData, ctx: Trace): void {
    this.pinSingleCuj(ctx, metricData);
    const config = this.blockingCallTrackConfig(metricData);
    addDebugSliceTrack({trace: ctx, ...config});
    // Only trigger adding track for frame when the aggregation is for max duration per frame.
    if (metricData.aggregation === 'max_dur_per_frame_ns') {
      this.frameWithMaxDurBlockingCallTrackConfig(ctx, metricData).then(
        (frameConfigArgs) => {
          addDebugSliceTrack({trace: ctx, ...frameConfigArgs});
        },
      );
    }
  }

  private async pinSingleCuj(ctx: Trace, metricData: BlockingCallMetricData) {
    const jankTrackName = `Jank CUJ: ${metricData.cujName}`;
    const latencyTrackName = `Latency CUJ: ${metricData.cujName}`;
    // TODO: b/296349525 - Refactor once CUJ tables are migrated to stdlib
    // Currently, we try to pin a Jank CUJ track and if that fails we add
    // a Latency CUJ track. We can instead look up a single CUJ table to
    // better determine what to query and pin.
    const jankCujPinned = await addJankCUJDebugTrack(
      ctx,
      jankTrackName,
      metricData.cujName,
    );
    if (!jankCujPinned) {
      addLatencyCUJDebugTrack(ctx, latencyTrackName, metricData.cujName);
    }
  }

  private blockingCallTrackConfig(metricData: BlockingCallMetricData) {
    const cuj = metricData.cujName;
    const processName = metricData.process;
    const blockingCallName = metricData.blockingCallName;

    // TODO: b/296349525 - Migrate jank tables from run metrics to stdlib
    const blockingCallDuringCujQuery = `
  SELECT name, ts, dur
  FROM main_thread_slices_scoped_to_cujs
  WHERE process_name = "${processName}"
      AND cuj_name = "${cuj}"
      AND name = "${blockingCallName}"
  `;

    const trackName = 'Blocking calls in ' + processName;
    return {
      data: {
        sqlSource: blockingCallDuringCujQuery,
        columns: ['name', 'ts', 'dur'],
      },
      columns: {ts: 'ts', dur: 'dur', name: 'name'},
      argColumns: ['name', 'ts', 'dur'],
      title: trackName,
    };
  }

  private async frameWithMaxDurBlockingCallTrackConfig(
    ctx: Trace,
    metricData: BlockingCallMetricData,
  ) {
    const cuj = metricData.cujName;
    const processName = metricData.process;
    const blockingCallName = metricData.blockingCallName;

    // Fetch the frame_id of the frame with the max duration blocking call.
    const result = await ctx.engine.query(`
        INCLUDE PERFETTO MODULE android.slices;
        INCLUDE PERFETTO MODULE android.binder;
        INCLUDE PERFETTO MODULE android.critical_blocking_calls;
        INCLUDE PERFETTO MODULE android.frames.timeline;
        INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;

        WITH extended_frame_boundary AS (
          SELECT frame_ts as ts,
          ui_thread_utid,
          frame_id,
          layer_id,
          -- Calculate the end timestamp (ts_end) by taking the start time (frame_ts) of the next frame in the session.
          -- For the last frame, fall back to the default ts_end.
          COALESCE(LEAD(frame_ts) OVER (PARTITION BY cuj_id ORDER BY frame_id ASC), ts_end) AS ts_end,
          frame_id
        FROM _android_frames_in_cuj order by frame_id
        ),
        blocking_calls_per_frame AS (
          SELECT
            MIN(
                bc.dur,
                frame.ts_end - bc.ts,
                bc.ts_end - frame.ts
            ) AS dur,
            MAX(frame.ts, bc.ts) AS ts,
            bc.upid,
            bc.name,
            bc.process_name,
            bc.utid,
            frame.frame_id,
            frame.layer_id
          FROM _android_critical_blocking_calls bc
          JOIN extended_frame_boundary frame
          ON bc.utid = frame.ui_thread_utid
          -- The following condition to accommodate blocking call crossing frame boundary. The blocking
          -- call starts in a frame and ends in a frame. It can either be the same frame or a different
          -- frame.
          WHERE (bc.ts >= frame.ts AND bc.ts <= frame.ts_end) -- Blocking call starts within the frame.
            OR (bc.ts_end >= frame.ts AND bc.ts_end <= frame.ts_end)
        ),
        blocking_calls_frame_cuj AS (
          SELECT
            b.frame_id,
            b.layer_id,
            b.name,
            frame_cuj.cuj_name,
            b.ts,
            b.dur,
            b.process_name
          FROM _android_frames_in_cuj frame_cuj
          JOIN blocking_calls_per_frame b
          USING (upid, frame_id, layer_id)
          )
        SELECT
          frame_id
        FROM blocking_calls_frame_cuj
        WHERE
          process_name = "${processName}"
          AND name = "${blockingCallName}"
          AND cuj_name = "${cuj}"
          -- select frame_id for the metric with the maximum duration.
        ORDER BY dur DESC limit 1`);
    const row = result.firstRow({frame_id: LONG});
    // Fetch the ts and dur of the frame corresponding to the above frame_id.
    const frameWithMaxDurBlockingCallQuery = `
        SELECT
          cast_string!(frame_id) as frame_id,
          ts,
          dur
        FROM android_frames_layers
        WHERE frame_id = ${row.frame_id}
      `;

    const trackName = 'Frame with max duration blocking call';
    const config = {
      data: {
        sqlSource: frameWithMaxDurBlockingCallQuery,
        columns: ['frame_id', 'ts', 'dur'],
      },
      columns: {ts: 'ts', dur: 'dur', name: 'frame_id'},
      argColumns: ['frame_id', 'ts', 'dur'],
      title: trackName,
    };
    return Promise.resolve(config);
  }
}

export const pinBlockingCallHandlerInstance = new BlockingCallMetricHandler();
