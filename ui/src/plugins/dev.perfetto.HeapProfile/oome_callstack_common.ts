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

import type {time} from '../../base/time';
import type {QueryFlamegraphMetric} from '../../components/query_flamegraph';
import type {Engine} from '../../trace_processor/engine';
import {STR_NULL} from '../../trace_processor/query_result';

export function buildOomeCallstackMetrics(
  ts: time,
): ReadonlyArray<QueryFlamegraphMetric> {
  return [
    {
      name: 'OOME Callstack',
      unit: '',
      statement: `
        WITH RECURSIVE callstack AS (
          SELECT c.id, c.parent_id as parentId, c.depth, c.frame_id
          FROM stack_profile_callsite c
          WHERE c.id = (
            SELECT callsite_id
            FROM heap_graph_thread_callsite hc
            JOIN heap_graph g ON hc.heap_graph_id = g.id
            WHERE g.ts = ${ts}
            LIMIT 1
          )

          UNION ALL

          SELECT c.id, c.parent_id as parentId, c.depth, c.frame_id
          FROM stack_profile_callsite c
          JOIN callstack ON c.id = callstack.parentId
        )
        SELECT
          cs.id,
          cs.parentId,
          __intrinsic_frame_name(
            s.name, f.deobfuscated_name, f.name, s.source_file, f.rel_pc, m.name
          ) as name,
          iif(cs.depth = (select max(depth) from callstack), 1, 0) as value,
          s.source_file || ':' || cast(s.line_number as text) as source_location
        FROM callstack cs
        JOIN stack_profile_frame f ON cs.frame_id = f.id
        JOIN stack_profile_mapping m ON f.mapping = m.id
        LEFT JOIN stack_profile_symbol s ON f.symbol_set_id = s.symbol_set_id
      `,
      unaggregatableProperties: [],
      aggregatableProperties: [
        {
          name: 'source_location',
          displayName: 'Source Location',
          mergeAggregation: 'ONE_OR_SUMMARY',
        },
      ],
    },
  ];
}

export async function loadOomeErrorMsg(
  engine: Engine,
  ts: time,
): Promise<string | undefined> {
  const errorMsgRes = await engine.query(`
      INCLUDE PERFETTO MODULE android.memory.heap_graph.oome;
      SELECT oome.error_msg 
      FROM heap_graph g
      JOIN android_heap_graph_java_oome_details oome ON oome.heap_graph_id = g.id
      WHERE g.ts = ${ts}
      LIMIT 1
    `);
  if (errorMsgRes.numRows() > 0) {
    return errorMsgRes.firstRow({error_msg: STR_NULL}).error_msg ?? undefined;
  }
  return undefined;
}
