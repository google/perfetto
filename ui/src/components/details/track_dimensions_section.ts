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
import type {Trace} from '../../public/trace';
import type {TrackEventSelection} from '../../public/selection';
import type {TrackEventDetailsPanelSection} from './thread_slice_details_tab';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {STR, STR_NULL} from '../../trace_processor/query_result';

interface DimensionDisplay {
  readonly name: string;
  readonly displayValue: string | null;
  readonly displayName: string | null;
}

// Unlike timeline chips, details include values uniform across the trace.
// Known dimensions are included only when the selected track has custom args.
export class TrackDimensionsSection implements TrackEventDetailsPanelSection {
  private dimensions: DimensionDisplay[] = [];

  constructor(private readonly trace: Trace) {}

  async load(selection: TrackEventSelection): Promise<void> {
    this.dimensions = [];
    // An optional section must not reject the entire details-panel load.
    try {
      this.dimensions = await queryDimensions(this.trace, selection.eventId);
    } catch {
      this.dimensions = [];
    }
  }

  render(): m.Children {
    if (this.dimensions.length === 0) return null;
    return m(
      Section,
      {title: 'Dimensions'},
      m(
        Tree,
        this.dimensions.map((dimension) =>
          m(TreeNode, {
            left: dimension.name,
            right: renderValue(dimension),
          }),
        ),
      ),
    );
  }
}

async function queryDimensions(
  trace: Trace,
  eventId: number,
): Promise<DimensionDisplay[]> {
  // A UI track can multiplex TP tracks; dimensions follow the selected event.
  const res = await trace.engine.query(`
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
    WHERE s.id = ${eventId} AND a.key GLOB 'custom_dimension.*'
    ORDER BY a.key
  `);
  if (res.numRows() === 0) return [];

  const context = res.firstRow({
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
  const dimensions: DimensionDisplay[] = [
    {name: 'cpu', displayValue: context.cpuValue, displayName: null},
    {name: 'gpu', displayValue: context.gpuValue, displayName: context.gpuName},
    {
      name: 'machine',
      displayValue: context.machineValue,
      displayName: context.machineName,
    },
    {
      name: 'process',
      displayValue: context.processValue,
      displayName: context.processName,
    },
    {
      name: 'thread',
      displayValue: context.threadValue,
      displayName: context.threadName,
    },
  ].filter((dimension) => dimension.displayValue !== null);
  const it = res.iter({name: STR, displayValue: STR, displayName: STR_NULL});
  for (; it.valid(); it.next()) {
    dimensions.push({
      name: it.name,
      displayValue: it.displayValue,
      displayName: it.displayName,
    });
  }
  return dimensions;
}

function renderValue(dimension: DimensionDisplay): string {
  const value = dimension.displayValue ?? '';
  const displayName = dimension.displayName;
  if (displayName !== null && displayName !== '') {
    return `${value} (${displayName})`;
  }
  return value;
}
