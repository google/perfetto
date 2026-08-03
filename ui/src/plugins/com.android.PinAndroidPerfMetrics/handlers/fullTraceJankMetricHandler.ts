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

import {expandProcessName, type JankType} from './metricUtils';
import type {FullTraceMissedFramesPinRequest, PinRequest} from './pinRequest';
import {PinRequestType} from './pinRequest';
import type {Trace} from '../../../public/trace';
import {addDebugSliceTrack} from '../../../components/tracks/debug_tracks';

/**
 * Translates a full-trace jank metric key into a request to pin the missed
 * frames of a process across the whole trace.
 *
 * @param {string} metricKey The metric key to match.
 * @returns {PinRequest[]} A single FullTraceMissedFrames request, or [] if the
 *     key doesn't match.
 */
export function translateFullTraceJank(metricKey: string): PinRequest[] {
  const matcher =
    /perfetto_ft_(?<process>.*)-(?<jps>weighted_)?missed_(?<jankType>frames|sf_frames|app_frames)/;
  const match = matcher.exec(metricKey);
  if (!match?.groups) {
    return [];
  }
  return [
    {
      type: PinRequestType.FullTraceMissedFrames,
      process: expandProcessName(match.groups.process),
      jankType: match.groups.jankType as JankType,
      isWeighted: !!match.groups.jps,
    },
  ];
}

/**
 * Adds the debug track for full trace jank metrics.
 *
 * @param {Trace} ctx PluginContextTrace for trace related properties and methods
 * @param {FullTraceMissedFramesPinRequest} req The missed frames to pin.
 * @returns {void} Adds one track for Jank slice
 */
export async function execFullTraceMissedFrames(
  ctx: Trace,
  req: FullTraceMissedFramesPinRequest,
): Promise<void> {
  const INCLUDE_PREQUERY = `
    INCLUDE PERFETTO MODULE android.frames.jank_type;
    INCLUDE PERFETTO MODULE slices.with_context;
    `;
  const config = fullTraceJankConfig(req);
  await ctx.engine.query(INCLUDE_PREQUERY);
  addDebugSliceTrack({trace: ctx, ...config});
}

function fullTraceJankConfig(req: FullTraceMissedFramesPinRequest) {
  let jankTypeFilter;
  let jankTypeDisplayName;
  if (req.jankType?.includes('app')) {
    jankTypeFilter = ' android_is_app_jank_type(display_value)';
    jankTypeDisplayName = 'app';
  } else if (req.jankType?.includes('sf')) {
    jankTypeFilter = ' android_is_sf_jank_type(display_value)';
    jankTypeDisplayName = 'sf';
  } else {
    jankTypeFilter = ' android_is_missed_frame_type(display_value)';
    jankTypeDisplayName = 'all';
  }
  const processName = req.process;

  // TODO: b/324245198 - Refactor when jank_type added to android_frame_stats
  const fullTraceJankQuery = `
      WITH filtered_args AS (
        SELECT DISTINCT arg_set_id
        FROM args
        WHERE key = 'Jank type'
        ${jankTypeFilter ? 'AND ' + jankTypeFilter : ''}
      )
      SELECT
        name,
        ts as ts,
        dur as dur,
        track_id as track_id,
        id as slice_id,
        category,
        thread_name,
        tid as tid,
        process_name,
        pid as pid
      FROM thread_or_process_slice
      JOIN filtered_args ON filtered_args.arg_set_id = thread_or_process_slice.arg_set_id
      WHERE process_name = '${processName}'`;
  const fullTraceJankColumns = [
    'name',
    'ts',
    'dur',
    'track_id',
    'slice_id',
    'category',
    'thread_name',
    'tid',
    'process_name',
    'pid',
  ];

  const trackName = jankTypeDisplayName + ' missed frames in ' + processName;

  return {
    data: {
      sqlSource: fullTraceJankQuery,
      columns: fullTraceJankColumns,
    },
    columns: {ts: 'ts', dur: 'dur', name: 'name'},
    rawColumns: fullTraceJankColumns,
    title: trackName,
  };
}
