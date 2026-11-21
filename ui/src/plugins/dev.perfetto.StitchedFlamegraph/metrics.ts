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
import type {TreeExplorerQueryMetric} from '../../components/tree_explorer_fetcher';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import type {TreeExplorerView} from '../../widgets/tree_explorer';
import {stitchedNodeActions} from './actions';
import type {StitchedSqlModule} from './sql_modules';

// Downstream: what the selection ran and triggered, selection at the top.
// Upstream: what led to the selection, selection at the top too (the tree is
// built with the selection as leaves and shown bottom up).
export type StitchDirection = 'DOWN' | 'UP';

export function viewForDirection(direction: StitchDirection): TreeExplorerView {
  return {kind: direction === 'DOWN' ? 'TOP_DOWN' : 'BOTTOM_UP'};
}

export interface StitchedSelection {
  readonly trackIds: ReadonlyArray<number>;
  readonly start: time;
  readonly end: time;
  readonly direction: StitchDirection;
}

// A sequence of SQL steps, each materialized in its own table so that every
// step runs exactly once with a predictable plan. Table names are prefixed
// so that pipelines of different selections never collide.
export class StitchedPipeline {
  private readonly statements: string[];
  private readonly ownTables: string[] = [];

  // `stdlibModules` are the stdlib modules the steps use beyond the ones the
  // provider's stitched SQL modules include: they are only included when the
  // steps run.
  constructor(
    private readonly prefix: string,
    stdlibModules: ReadonlyArray<string> = [],
  ) {
    this.statements = stdlibModules.map((m) => `include perfetto module ${m}`);
  }

  // Adds a step materializing `as` and returns its table name.
  table(name: string, as: string, indexOn?: string): string {
    const table = `${this.prefix}_${name}`;
    this.statements.push(`create or replace perfetto table ${table} as ${as}`);
    if (indexOn !== undefined) {
      this.statements.push(
        `create or replace perfetto index ${table}_idx on ${table}(${indexOn})`,
      );
    }
    this.ownTables.push(table);
    return table;
  }

  get sql(): string {
    return this.statements.map((s) => `${s};`).join('\n');
  }

  get tables(): ReadonlyArray<string> {
    return this.ownTables;
  }
}

export interface StitchedMetrics {
  // Pipelines the metrics share, run before the metrics are offered.
  readonly prepare: ReadonlyArray<StitchedPipeline>;
  readonly metrics: ReadonlyArray<TreeExplorerQueryMetric>;
  // The tables the pipelines materialize, to drop once they are superseded.
  readonly tables: ReadonlyArray<string>;
}

// A source of stitched flamegraph metrics. Beyond the shared `prepare`
// pipelines, each metric is computed lazily by the tree explorer, only when
// the user picks it. `prefix` is unique to the selection and must prefix
// every table created: pipelines built for the same selection with the same
// prefix are identical, and only run once.
export interface StitchedMetricProvider {
  // The stitched SQL modules the metrics of `selection` need. They run (once
  // per trace) before the metrics are built.
  modules(selection: StitchedSelection): ReadonlyArray<StitchedSqlModule>;

  // Whether the trace has the data the provider needs. Called once per trace.
  isAvailable?(engine: Engine): Promise<boolean>;

  metrics(
    trace: Trace,
    selection: StitchedSelection,
    prefix: string,
  ): StitchedMetrics;
}

// The slices of the selected tracks overlapping the selected window.
export function seedsSql(selection: StitchedSelection): string {
  const {trackIds, start, end} = selection;
  return `
    select id
    from slice
    where track_id in (${trackIds.join(',')})
      and ts < ${end}
      and iif(dur < 0, ${end}, ts + dur) > ${start}
  `;
}

export interface StitchedMetricSpec {
  readonly id: string;
  readonly name: string;
  readonly unit: string;
  // One of the weight columns of `_stitched_finalize!`.
  readonly valueColumn: 'self_dur' | 'self_count';
  // The steps computing `nodes` which only this metric needs, on top of the
  // provider's `prepare` pipelines.
  readonly pipeline?: StitchedPipeline;
  // The table of stitched nodes, as consumed by `_stitched_finalize!`.
  readonly nodes: string;
}

// Builds a tree explorer metric over a pipeline computing stitched nodes.
export function stitchedMetric(
  trace: Trace,
  selection: StitchedSelection,
  spec: StitchedMetricSpec,
): TreeExplorerQueryMetric {
  return {
    id: spec.id,
    name: spec.name,
    unit: spec.unit,
    nameColumnLabel: 'Slice',
    dependencySql: spec.pipeline?.sql,
    statement: `
      select *, ${spec.valueColumn} as value
      from _stitched_finalize!(${spec.nodes})
    `,
    unaggregatableProperties: [
      {name: 'kind', displayName: 'Kind', isVisible: () => false},
      {name: 'thread', displayName: 'Thread'},
    ],
    aggregatableProperties: [
      {
        name: 'slice_ids',
        displayName: 'Slice IDs',
        mergeAggregation: 'CONCAT_WITH_COMMA',
        isVisible: () => false,
      },
    ],
    optionalNodeActions: stitchedNodeActions(trace, selection),
  };
}
