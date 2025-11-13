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
} from '../../com.android.AndroidCujs';
import {
  addDebugSliceTrack,
  DebugSliceTrackArgs,
} from '../../../components/tracks/debug_tracks';
import {LONG, QueryResult, Row} from '../../../trace_processor/query_result';

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
  public async addMetricTrack(metricData: BlockingCallMetricData, ctx: Trace) {
    this.pinSingleCuj(ctx, metricData);
    const config = this.blockingCallTrackConfig(metricData);
    addDebugSliceTrack({trace: ctx, ...config});
    // Only trigger adding track for frame when the aggregation is for max duration per frame.
    const MAX_DUR_PER_FRAME_NS_MEAN = 'max_dur_per_frame_ns-mean';
    if (metricData.aggregation === MAX_DUR_PER_FRAME_NS_MEAN) {
      const frameConfigArgs = await this.frameWithMaxDurBlockingCallTrackConfig(
        ctx,
        metricData,
      );
      addDebugSliceTrack({trace: ctx, ...frameConfigArgs});
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
      rawColumns: ['name', 'ts', 'dur'],
      title: trackName,
    };
  }

  private async getFrameIdWithMaxDurationBlockingCall(
    ctx: Trace,
    metricData: BlockingCallMetricData,
  ): Promise<QueryResult> {
    const cuj = metricData.cujName;
    const processName = metricData.process;
    const blockingCallName = metricData.blockingCallName;

    // Fetch the frame_id of the frame with the max duration blocking call.
    return ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.frame_blocking_calls.blocking_calls_aggregation;

      SELECT
        frame_id
      FROM _blocking_calls_frame_cuj
      WHERE
        process_name = '${processName}'
        AND name = '${blockingCallName}'
        AND cuj_name = '${cuj}'
      -- select frame_id for the metric with the maximum duration.
      ORDER BY dur DESC
      LIMIT 1`);
  }
  private async frameWithMaxDurBlockingCallTrackConfig(
    ctx: Trace,
    metricData: BlockingCallMetricData,
  ): Promise<
    Pick<DebugSliceTrackArgs, 'data' | 'columns' | 'rawColumns' | 'title'>
  > {
    let row: Row = {
      frame_id: null,
    };

    try {
      row = (
        await this.getFrameIdWithMaxDurationBlockingCall(ctx, metricData)
      ).firstRow({frame_id: LONG});
    } catch (e) {
      throw new Error(
        `${e.message} caused by: No frame found for:
          process: ${metricData.process}
          CUJ: ${metricData.cujName}
          blocking call: ${metricData.blockingCallName}`,
      );
    }

    // Fetch the ts and dur for the extended frame boundary corresponding to the above frame_id.
    const frameWithMaxDurBlockingCallQuery = `
      SELECT
        frame_id,
        ts,
        (ts_end - ts) AS dur
      FROM _extended_frame_boundary
      WHERE frame_id = ${row.frame_id}
      `;

    return {
      data: {
        sqlSource: frameWithMaxDurBlockingCallQuery,
        columns: ['frame_id', 'ts', 'dur'],
      },
      columns: {ts: 'ts', dur: 'dur', name: 'frame_id'},
      rawColumns: ['frame_id', 'ts', 'dur'],
      title: 'Frame with max duration blocking call',
    };
  }
}

export const pinBlockingCallHandlerInstance = new BlockingCallMetricHandler();
