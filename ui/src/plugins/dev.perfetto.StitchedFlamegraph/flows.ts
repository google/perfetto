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

import {
  STITCHED_FLOWS_DOWN_MODULE,
  STITCHED_FLOWS_UP_MODULE,
} from './flows_sql';
import {
  seedsSql,
  StitchedPipeline,
  stitchedMetric,
  type StitchedMetricProvider,
  type StitchedSelection,
} from './metrics';
import type {StitchedSqlModule} from './sql_modules';

export interface FlowsPipeline {
  readonly pipeline: StitchedPipeline;
  // The table of stitched nodes, indexed by id.
  readonly nodes: string;
}

// The pipeline computing the slices stitched by flows from the selection.
// Identical for a given (prefix, selection), so that every provider building
// on it shares a single run.
export function flowsPipeline(
  prefix: string,
  selection: StitchedSelection,
): FlowsPipeline {
  const p = new StitchedPipeline(`${prefix}_flows`);
  return {pipeline: p, nodes: addFlowsSteps(p, selection)};
}

// The stitched SQL modules the flows steps of `selection` need.
export function flowsModules(
  selection: StitchedSelection,
): ReadonlyArray<StitchedSqlModule> {
  return [
    selection.direction === 'DOWN'
      ? STITCHED_FLOWS_DOWN_MODULE
      : STITCHED_FLOWS_UP_MODULE,
  ];
}

function addFlowsSteps(p: StitchedPipeline, selection: StitchedSelection) {
  const {start, end} = selection;
  if (selection.direction === 'DOWN') {
    const seeds = p.table('seeds', seedsSql(selection), 'id');
    const reach = p.table(
      'reach',
      `select * from _stitched_down_reach!(${seeds}, ${start}, ${end})`,
      'id',
    );
    const items = p.table(
      'items',
      `select * from _stitched_down_items!(${reach}, ${start}, ${end})`,
      'id',
    );
    return p.table(
      'nodes',
      `select * from _stitched_down_nodes!(${items})`,
      'id',
    );
  }
  const seedIds = p.table('seed_ids', seedsSql(selection));
  const seeds = p.table(
    'seeds',
    `select * from _stitched_up_seeds!(${seedIds}, ${start}, ${end})`,
    'id',
  );
  const reach = p.table('reach', `select * from _stitched_up_reach!(${seeds})`);
  return p.table(
    'nodes',
    `select * from _stitched_up_nodes!(${reach}, ${seeds})`,
    'id',
  );
}

// Slices stitched across threads and processes by flows (binder, message
// queues, thread pools, coroutines) and async binder AIDL slices, weighted by
// wall time or slice count.
export const FLOWS_PROVIDER: StitchedMetricProvider = {
  modules: flowsModules,
  metrics(trace, selection, prefix) {
    const {pipeline, nodes} = flowsPipeline(prefix, selection);
    return {
      prepare: [pipeline],
      metrics: [
        stitchedMetric(trace, selection, {
          id: 'duration',
          name: 'Duration',
          unit: 'ns',
          valueColumn: 'self_dur',
          nodes,
        }),
        stitchedMetric(trace, selection, {
          id: 'slice_count',
          name: 'Slice Count',
          unit: '',
          valueColumn: 'self_count',
          nodes,
        }),
      ],
      tables: pipeline.tables,
    };
  },
};
