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
import type {Engine} from '../../trace_processor/engine';
import {STR, STR_NULL} from '../../trace_processor/query_result';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import type {SliceSqlId} from '../sql_utils/core_types';

export interface TrackDimension {
  readonly name: string;
  readonly value: string;
  readonly displayName?: string;
}

// Returns the dimensions of the track of the given slice. A UI track can
// multiplex several trace processor tracks, so this follows the slice rather
// than the UI track. Unlike timeline chips, this includes values uniform
// across the trace. Known dimensions (cpu, gpu, machine, process, thread) are
// only included when the track has custom dimensions.
export async function getTrackDimensions(
  engine: Engine,
  sliceId: SliceSqlId,
): Promise<TrackDimension[]> {
  const res = await engine.query(`
    SELECT
      substr(a.key, length('custom_dimension.') + 1) AS name,
      a.display_value AS displayValue,
      label.string_value AS displayName,
      CAST(extract_arg(t.dimension_arg_set_id, 'cpu') AS TEXT) AS cpuValue,
      CAST(g.gpu AS TEXT) AS gpuValue,
      g.name AS gpuName,
      CAST(m.label_index AS TEXT) AS machineValue,
      m.name AS machineName,
      CAST(p.pid AS TEXT) AS processValue,
      p.name AS processName,
      CAST(th.tid AS TEXT) AS threadValue,
      th.name AS threadName
    FROM slice AS s
    JOIN track AS t ON t.id = s.track_id
    JOIN args AS a ON a.arg_set_id = t.dimension_arg_set_id
    LEFT JOIN args AS label
      ON label.arg_set_id = t.source_arg_set_id
      AND label.key = 'custom_dimension_display_name.' ||
        substr(a.key, length('custom_dimension.') + 1)
    LEFT JOIN machine AS m ON m.id = t.machine_id
    LEFT JOIN process AS p
      ON p.upid = extract_arg(t.dimension_arg_set_id, 'upid')
    LEFT JOIN thread AS th
      ON th.utid = extract_arg(t.dimension_arg_set_id, 'utid')
    LEFT JOIN gpu AS g ON g.id = extract_arg(t.dimension_arg_set_id, 'ugpu')
    WHERE s.id = ${sliceId} AND a.key GLOB 'custom_dimension.*'
    ORDER BY a.key
  `);
  if (res.numRows() === 0) return [];

  const known = res.firstRow({
    cpuValue: STR_NULL,
    gpuValue: STR_NULL,
    gpuName: STR_NULL,
    machineValue: STR_NULL,
    machineName: STR_NULL,
    processValue: STR_NULL,
    processName: STR_NULL,
    threadValue: STR_NULL,
    threadName: STR_NULL,
  });
  const dimensions: TrackDimension[] = [];
  const addKnown = (
    name: string,
    value: string | null,
    displayName: string | null = null,
  ) => {
    if (value === null) return;
    dimensions.push({name, value, displayName: displayName ?? undefined});
  };
  addKnown('cpu', known.cpuValue);
  addKnown('gpu', known.gpuValue, known.gpuName);
  addKnown('machine', known.machineValue, known.machineName);
  addKnown('process', known.processValue, known.processName);
  addKnown('thread', known.threadValue, known.threadName);

  const it = res.iter({name: STR, displayValue: STR, displayName: STR_NULL});
  for (; it.valid(); it.next()) {
    dimensions.push({
      name: it.name,
      value: it.displayValue,
      displayName: it.displayName ?? undefined,
    });
  }
  return dimensions;
}

export function renderTrackDimensions(
  dimensions: ReadonlyArray<TrackDimension>,
): m.Children {
  if (dimensions.length === 0) return undefined;
  return m(
    Section,
    {title: 'Dimensions'},
    m(
      Tree,
      dimensions.map((dimension) =>
        m(TreeNode, {
          left: dimension.name,
          right: formatValue(dimension),
        }),
      ),
    ),
  );
}

function formatValue({value, displayName}: TrackDimension): string {
  if (displayName === undefined || displayName === '') return value;
  return `${value} (${displayName})`;
}
