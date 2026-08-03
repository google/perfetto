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

import {expandProcessName} from './metricUtils';
import type {BlockingCallPinRequest, PinRequest} from './pinRequest';
import {PinRequestType} from './pinRequest';
import {execCuj} from './pinCujMetricHandler';
import type {Trace} from '../../../public/trace';
import {
  addDebugSliceTrack,
  type DebugSliceTrackArgs,
} from '../../../components/tracks/debug_tracks';
import {LONG, type QueryResult} from '../../../trace_processor/query_result';

/**
 * Translates a blocking call (or per-frame blocking call) metric key into a
 * request to pin the CUJ and its blocking-call track.
 *
 * @param {string} metricKey The metric key to match.
 * @returns {PinRequest[]} A single BlockingCall request, or [] if the key
 *     doesn't match.
 */
export function translateBlockingCall(metricKey: string): PinRequest[] {
  const matcher =
    /perfetto_android_blocking_call(?:_per_frame)?-cuj-name-(?<process>.*)-name-(?<cujName>.*)-blocking_calls-name-(?<blockingCallName>([^\-]*))-(?<aggregation>.*)/;
  const match = matcher.exec(metricKey);
  if (!match?.groups) {
    return [];
  }
  return [
    {
      type: PinRequestType.BlockingCall,
      process: expandProcessName(match.groups.process),
      cujName: match.groups.cujName,
      blockingCallName: match.groups.blockingCallName,
      aggregation: match.groups.aggregation,
    },
  ];
}

/**
 * Adds the debug tracks for Blocking Call metrics: the CUJ (jank, else latency
 * fallback), the blocking-call track, and — only for the max-duration-per-frame
 * aggregation — the track for the frame with the max duration blocking call.
 *
 * @param {Trace} ctx PluginContextTrace for trace related properties and methods
 * @param {BlockingCallPinRequest} req The blocking call to pin.
 */
export async function execBlockingCall(
  ctx: Trace,
  req: BlockingCallPinRequest,
): Promise<void> {
  // TODO: b/296349525 - Refactor once CUJ tables are migrated to stdlib
  // Currently, we try to pin a Jank CUJ track and if that fails we add
  // a Latency CUJ track. We can instead look up a single CUJ table to
  // better determine what to query and pin.
  await execCuj(ctx, {
    type: PinRequestType.Cuj,
    cujName: req.cujName,
    fallbackToLatency: true,
  });
  const config = blockingCallTrackConfig(req);
  addDebugSliceTrack({trace: ctx, ...config});
  // Only trigger adding track for frame when the aggregation is for max duration per frame.
  const MAX_DUR_PER_FRAME_NS_MEAN = 'max_dur_per_frame_ns-mean';
  if (req.aggregation === MAX_DUR_PER_FRAME_NS_MEAN) {
    const frameConfigArgs = await frameWithMaxDurBlockingCallTrackConfig(
      ctx,
      req,
    );
    if (frameConfigArgs !== undefined) {
      addDebugSliceTrack({trace: ctx, ...frameConfigArgs});
    }
  }
}

function blockingCallTrackConfig(req: BlockingCallPinRequest) {
  const cuj = req.cujName;
  const processName = req.process;
  const blockingCallName = req.blockingCallName;

  const blockingCallDuringCujQuery = `
  INCLUDE PERFETTO MODULE android.frame_blocking_calls.blocking_calls_aggregation;

  SELECT name, ts, dur
  FROM _blocking_calls_frame_cuj
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

async function getFrameIdWithMaxDurationBlockingCall(
  ctx: Trace,
  req: BlockingCallPinRequest,
): Promise<QueryResult> {
  const cuj = req.cujName;
  const processName = req.process;
  const blockingCallName = req.blockingCallName;

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

async function frameWithMaxDurBlockingCallTrackConfig(
  ctx: Trace,
  req: BlockingCallPinRequest,
): Promise<
  | Pick<DebugSliceTrackArgs, 'data' | 'columns' | 'rawColumns' | 'title'>
  | undefined
> {
  const result = await getFrameIdWithMaxDurationBlockingCall(ctx, req);
  if (result.numRows() === 0) {
    console.warn(
      `No frame found for: process=${req.process},` +
        ` CUJ=${req.cujName},` +
        ` blocking_call=${req.blockingCallName}`,
    );
    return undefined;
  }
  const row = result.firstRow({frame_id: LONG});

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
