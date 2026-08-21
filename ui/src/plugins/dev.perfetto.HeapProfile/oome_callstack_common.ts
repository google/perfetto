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

import m from 'mithril';
import type {time} from '../../base/time';
import type {QueryFlamegraphMetric} from '../../components/query_flamegraph';
import type {Engine} from '../../trace_processor/engine';
import {LONG_NULL, STR_NULL} from '../../trace_processor/query_result';
import {formatFileSize} from '../../base/file_utils';
import {removeFalsyValues} from '../../base/array_utils';
import {Grid, GridCell, GridHeaderCell, type GridRow} from '../../widgets/grid';

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
          coalesce(f.deobfuscated_name, f.name) as name,
          iif(cs.depth = (select max(depth) from callstack), 1, 0) as value,
          s.source_file || ':' || cast(s.line_number as text) as source_location
        FROM callstack cs
        JOIN stack_profile_frame f ON cs.frame_id = f.id
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

// Details of the OutOfMemoryError that triggered the heap dump.
export interface OomeDetails {
  readonly allocationSizeBytes?: bigint;
  readonly freeBytesUntilOom?: bigint;
  readonly errorMsg?: string;
}

export async function loadOomeDetails(
  engine: Engine,
  ts: time,
): Promise<OomeDetails | undefined> {
  const res = await engine.query(`
      INCLUDE PERFETTO MODULE android.memory.heap_graph.oome;
      SELECT
        oome.allocation_size_bytes AS allocationSizeBytes,
        oome.free_bytes_until_oom AS freeBytesUntilOom,
        oome.error_msg AS errorMsg
      FROM heap_graph g
      JOIN android_heap_graph_java_oome_details oome
        ON oome.heap_graph_id = g.id
      WHERE g.ts = ${ts}
      LIMIT 1
    `);
  if (res.numRows() === 0) {
    return undefined;
  }
  const row = res.firstRow({
    allocationSizeBytes: LONG_NULL,
    freeBytesUntilOom: LONG_NULL,
    errorMsg: STR_NULL,
  });
  return {
    allocationSizeBytes: row.allocationSizeBytes ?? undefined,
    freeBytesUntilOom: row.freeBytesUntilOom ?? undefined,
    errorMsg: row.errorMsg ?? undefined,
  };
}

export const OOME_DETAILS_TITLE = 'Out of Memory Error';

function oomeDetailRows(details: OomeDetails): GridRow[] {
  const row = (property: string, value: string): GridRow => [
    m(GridCell, property),
    m(GridCell, value),
  ];
  return removeFalsyValues([
    details.allocationSizeBytes !== undefined &&
      row('Allocation size', formatFileSize(details.allocationSizeBytes)),
    details.freeBytesUntilOom !== undefined &&
      row('Free until OOM', formatFileSize(details.freeBytesUntilOom)),
    details.errorMsg !== undefined && row('Error message', details.errorMsg),
  ]);
}

// The OOME details as a Property/Value grid, without a heading.
export function renderOomeDetailsGrid(details?: OomeDetails): m.Children {
  if (details === undefined) {
    return undefined;
  }
  const rowData = oomeDetailRows(details);
  if (rowData.length === 0) {
    return undefined;
  }
  return m(Grid, {
    columns: [
      {key: 'property', header: m(GridHeaderCell, 'Property')},
      {key: 'value', header: m(GridHeaderCell, 'Value')},
    ],
    rowData,
  });
}

// The OOME grid with a heading, for the flamegraph headers.
export function renderOomeDetails(details?: OomeDetails): m.Children {
  const grid = renderOomeDetailsGrid(details);
  if (grid === undefined) {
    return undefined;
  }
  return m('div', [
    m('h3', {class: 'pf-hde-sub-heading'}, OOME_DETAILS_TITLE),
    grid,
  ]);
}
