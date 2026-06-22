// Copyright (C) 2023 The Android Open Source Project
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

import {addDebugSliceTrack} from '../../components/tracks/debug_tracks';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import QueryPagePlugin from '../dev.perfetto.QueryPage';

/**
 * Adds the Debug Slice Track for given Jank CUJ name
 *
 * @param {Trace} ctx For properties and methods of trace viewer
 * @param {string} trackName Display Name of the track
 * @param {string | string[]} cujNames List of Jank CUJs to pin
 * @returns Returns true if the track was added, false otherwise
 */
export async function addJankCUJDebugTrack(
  ctx: Trace,
  trackName: string,
  cujNames?: string | string[],
) {
  const jankCujTrackConfig = generateJankCujTrackConfig(cujNames);
  const result = await ctx.engine.query(jankCujTrackConfig.data.sqlSource);

  // Check if query produces any results to prevent pinning an empty track
  if (result.numRows() !== 0) {
    addDebugSliceTrack({trace: ctx, title: trackName, ...jankCujTrackConfig});
    return true;
  }
  return false;
}

const JANK_CUJ_QUERY_PRECONDITIONS = `
  INCLUDE PERFETTO MODULE android.cujs.base;
  INCLUDE PERFETTO MODULE android.critical_blocking_calls;
`;

/**
 * Generate the Track config for a multiple Jank CUJ slices
 *
 * @param {string | string[]} cujNames List of Jank CUJs to pin, default empty
 * @returns Returns the track config for given CUJs
 */
function generateJankCujTrackConfig(cujNames: string | string[] = []) {
  // This method expects the caller to have run JANK_CUJ_QUERY_PRECONDITIONS
  // Not running the precondition query here to save time in case already run
  return generateCujTrackConfig(cujNames, JANK_CUJ_QUERY, JANK_COLUMNS);
}

const JANK_CUJ_QUERY = `
    SELECT
      CASE
        WHEN state = 'canceled' THEN ' ❌ '
        WHEN state = 'completed' THEN ' ✅ '
        ELSE ' ❓ '
        END || cuj_slice_name AS name,
      total_frames,
      missed_app_frames,
      missed_sf_frames,
      sf_callback_missed_frames,
      hwui_callback_missed_frames,
      layer_name,
      ts,
      dur,
      track_id,
      slice_id
    FROM android_jank_cuj_all
    WHERE dur > 0
`;

const JANK_COLUMNS = [
  'name',
  'total_frames',
  'missed_app_frames',
  'missed_sf_frames',
  'sf_callback_missed_frames',
  'hwui_callback_missed_frames',
  'layer_name',
  'ts',
  'dur',
  'track_id',
  'slice_id',
];

/**
 * Adds the Debug Slice Track for given Jank CUJ name
 *
 * @param {Trace} ctx For properties and methods of trace viewer
 * @param {string} trackName Display Name of the track
 * @param {string | string[]} cujNames List of Jank CUJs to pin
 * @returns Returns true if the track was added, false otherwise
 */
export async function addLatencyCUJDebugTrack(
  ctx: Trace,
  trackName: string,
  cujNames?: string | string[],
) {
  const latencyCujTrackConfig = generateLatencyCujTrackConfig(cujNames);
  const result = await ctx.engine.query(latencyCujTrackConfig.data.sqlSource);

  // Check if query produces any results to prevent pinning an empty track
  if (result.numRows() !== 0) {
    addDebugSliceTrack({
      trace: ctx,
      title: trackName,
      ...latencyCujTrackConfig,
    });
    return true;
  }
  return false;
}

/**
 * Generate the Track config for a multiple Latency CUJ slices
 *
 * @param {string | string[]} cujNames List of Latency CUJs to pin, default empty
 * @returns Returns the track config for given CUJs
 */
function generateLatencyCujTrackConfig(cujNames: string | string[] = []) {
  return generateCujTrackConfig(cujNames, LATENCY_CUJ_QUERY, LATENCY_COLUMNS);
}

const LATENCY_CUJ_QUERY = `
    SELECT
      CASE
        WHEN
          EXISTS(
              SELECT 1
              FROM slice AS cuj_state_marker
                     JOIN track marker_track
                          ON marker_track.id = cuj_state_marker.track_id
              WHERE
                cuj_state_marker.ts >= cuj.ts
                AND cuj_state_marker.ts + cuj_state_marker.dur <= cuj.ts + cuj.dur
                AND marker_track.name = cuj.name AND (
                    cuj_state_marker.name GLOB 'cancel'
                    OR cuj_state_marker.name GLOB 'timeout')
            )
          THEN ' ❌ '
        ELSE ' ✅ '
        END || cuj.name AS name,
      cuj.dur / 1e6 as dur_ms,
      cuj.ts,
      cuj.dur,
      cuj.track_id,
      cuj.slice_id
    FROM slice AS cuj
           JOIN process_track AS pt
                ON cuj.track_id = pt.id
    WHERE cuj.name GLOB 'L<*>'
      AND cuj.dur > 0
`;

const LATENCY_COLUMNS = ['name', 'dur_ms', 'ts', 'dur', 'track_id', 'slice_id'];

const BLOCKING_CALLS_DURING_CUJS_QUERY = `
    SELECT
      s.id AS slice_id,
      s.name,
      max(s.ts, cuj.ts) AS ts,
      min(s.ts + s.dur, cuj.ts_end) as ts_end,
      min(s.ts + s.dur, cuj.ts_end) - max(s.ts, cuj.ts) AS dur,
      cuj.cuj_id,
      cuj.cuj_name,
      s.process_name,
      s.upid,
      s.utid,
      'slice' AS table_name
    FROM _android_critical_blocking_calls s
      JOIN  android_jank_cuj cuj
      -- only when there is an overlap
      ON s.ts + s.dur > cuj.ts AND s.ts < cuj.ts_end
          -- and are from the same process
          AND s.upid = cuj.upid
`;

const BLOCKING_CALLS_DURING_CUJS_COLUMNS = [
  'slice_id',
  'name',
  'ts',
  'cuj_ts',
  'dur',
  'cuj_id',
  'cuj_name',
  'process_name',
  'upid',
  'utid',
  'table_name',
];

/**
 * Generate the Track config for a multiple CUJ slices
 *
 * @param {string | string[]} cujNames List of Latency CUJs to pin, default empty
 * @param {string} cujQuery The query of the CUJ track
 * @param {string} cujColumns SQL Columns for the CUJ track
 * @returns Returns the track config for given CUJs
 */
function generateCujTrackConfig(
  cujNames: string | string[] = [],
  cujQuery: string,
  cujColumns: string[],
) {
  // This method expects the caller to have run JANK_CUJ_QUERY_PRECONDITIONS
  // Not running the precondition query here to save time in case already run
  const cujNamesList = typeof cujNames === 'string' ? [cujNames] : cujNames;
  const filterCuj =
    cujNamesList?.length > 0
      ? ` AND cuj.name IN (${cujNamesList
          .map((name) => `'L<${name}>','J<${name}>'`)
          .join(',')})`
      : '';

  return {
    data: {
      sqlSource: `${cujQuery}${filterCuj}`,
      columns: cujColumns,
    },
    rawColumns: cujColumns,
  };
}

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidCujs';
  static readonly dependencies = [QueryPagePlugin];
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'com.android.PinJankCUJs',
      name: 'Add track: Android jank CUJs',
      callback: async () => {
        await this.pinJankCujs(ctx);
      },
    });

    ctx.commands.registerCommand({
      id: 'com.android.ListJankCUJs',
      name: 'Run query: Android jank CUJs',
      callback: async () => {
        await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
        ctx.plugins.getPlugin(QueryPagePlugin).addQueryResultsTab({
          query: JANK_CUJ_QUERY,
          title: 'Android Jank CUJs',
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'com.android.PinLatencyCUJs',
      name: 'Add track: Android latency CUJs',
      callback: async () => {
        await this.pinLatencyCujs(ctx);
      },
    });

    ctx.commands.registerCommand({
      id: 'com.android.ListLatencyCUJs',
      name: 'Run query: Android Latency CUJs',
      callback: () =>
        ctx.plugins.getPlugin(QueryPagePlugin).addQueryResultsTab({
          query: LATENCY_CUJ_QUERY,
          title: 'Android Latency CUJs',
        }),
    });

    ctx.commands.registerCommand({
      id: 'com.android.PinBlockingCalls',
      name: 'Add track: Android Blocking calls during CUJs',
      callback: () => {
        ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS).then(() =>
          addDebugSliceTrack({
            trace: ctx,
            data: {
              sqlSource: BLOCKING_CALLS_DURING_CUJS_QUERY,
              columns: BLOCKING_CALLS_DURING_CUJS_COLUMNS,
            },
            title: 'Blocking calls during CUJs',
            rawColumns: BLOCKING_CALLS_DURING_CUJS_COLUMNS,
          }),
        );
      },
    });
  }

  async pinJankCujs(ctx: Trace) {
    await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
    await addJankCUJDebugTrack(ctx, 'Jank CUJs');
  }

  async pinLatencyCujs(ctx: Trace) {
    addDebugSliceTrack({
      trace: ctx,
      data: {
        sqlSource: LATENCY_CUJ_QUERY,
        columns: LATENCY_COLUMNS,
      },
      title: 'Latency CUJs',
    });
  }
}
