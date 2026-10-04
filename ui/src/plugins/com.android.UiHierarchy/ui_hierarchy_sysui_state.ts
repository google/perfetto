// Copyright (C) 2026 The Android Open Source Project
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

// One track per SystemUI state field (android_sysui_state), in the
// reporting process's track group: a counter for continuous (float) fields
// such as shade_expansion, else slices, one per value of the field.

import {materialColorScheme} from '../../components/colorizer';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';

export async function registerSysUiStateTracks(
  ctx: Trace,
  getOrCreateGroup: (upid: number) => TrackNode,
): Promise<void> {
  const res = await ctx.engine.query(`
    INCLUDE PERFETTO MODULE android.ui_hierarchy;
    SELECT
      upid,
      field,
      count(value_float) > 0
        AND count(value_float) = count(
          coalesce(value_float, value_bool, value_string, value_key)
        ) AS is_float
    FROM android_sysui_state
    WHERE field IS NOT NULL
    GROUP BY upid, field
    ORDER BY upid, field
  `);
  const it = res.iter({upid: NUM_NULL, field: STR, is_float: NUM});
  for (; it.valid(); it.next()) {
    const upid = it.upid;
    const field = it.field;
    const upidCond = upid === null ? 'upid IS NULL' : `upid = ${upid}`;
    const uri = `/ui_hierarchy_sysui_state/${upid ?? 'none'}/${field}`;
    const fieldCond = `field = '${field.replace(/'/g, "''")}'`;
    const renderer = it.is_float
      ? CounterTrack.create({
          trace: ctx,
          uri,
          sqlSource: `
            SELECT ts, value_float AS value
            FROM android_sysui_state
            WHERE ${upidCond} AND ${fieldCond}
          `,
        })
      : SliceTrack.create({
          trace: ctx,
          uri,
          dataset: new SourceDataset({
            schema: {id: NUM, ts: LONG, dur: LONG, depth: NUM, name: STR},
            src: `
            SELECT
              row_number() OVER (ORDER BY ts) AS id,
              ts,
              dur,
              0 AS depth,
              CASE
                WHEN value_float IS NOT NULL THEN printf('%g', value_float)
                WHEN value_bool IS NOT NULL
                  THEN iif(value_bool, 'true', 'false')
                WHEN value_string IS NOT NULL THEN value_string
                WHEN value_key IS NOT NULL THEN value_key
                ELSE 'changed'
              END AS name
            FROM android_sysui_state
            WHERE ${upidCond} AND ${fieldCond}
          `,
          }),
          colorizer: (row) => materialColorScheme(row.name),
        });
    ctx.tracks.registerTrack({uri, renderer});
    getOrCreateGroup(upid ?? 0).addChildInOrder(
      new TrackNode({uri, name: `SysUI State: ${field}`, sortOrder: -30}),
    );
  }
}
