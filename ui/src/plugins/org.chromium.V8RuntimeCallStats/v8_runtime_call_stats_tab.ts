// Copyright (C) 2025 The Android Open Source Project
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
import {assertExists} from '../../base/logging';
import {PivotTable} from '../../components/widgets/sql/pivot_table/pivot_table';
import {PivotTableState} from '../../components/widgets/sql/pivot_table/pivot_table_state';
import {
  AreaSelection,
  areaSelectionsEqual,
  AreaSelectionTab,
} from '../../public/selection';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {SqlTableDefinition} from '../../components/widgets/sql/table/table_description';
import {PerfettoSqlTypes} from '../../trace_processor/perfetto_sql_type';
import {resolveTableDefinition} from '../../components/widgets/sql/table/columns';
import {Spinner} from '../../widgets/spinner';
import {Aggregation} from '../../components/widgets/sql/pivot_table/aggregations';

const V8_RUNTIME_CALL_STATS_VIEW: SqlTableDefinition = {
  name: 'v8_rcs_view',
  columns: [
    {column: 'ts', type: PerfettoSqlTypes.TIMESTAMP},
    {column: 'dur', type: PerfettoSqlTypes.DURATION},
    {column: 'track_id', type: PerfettoSqlTypes.INT},
    {column: 'v8_rcs_name', type: PerfettoSqlTypes.STRING},
    {column: 'v8_rcs_group', type: PerfettoSqlTypes.STRING},
    {column: 'v8_rcs_count', type: PerfettoSqlTypes.INT},
    {column: 'v8_rcs_dur', type: PerfettoSqlTypes.DURATION},
  ],
};

export class V8RuntimeCallStatsTab implements AreaSelectionTab {
  readonly id = 'v8_runtime_call_stats';
  readonly name = 'V8 Runtime Call Stats';

  private state?: PivotTableState;
  private previousSelection?: AreaSelection;
  private trackIds: number[] = [];

  constructor(private readonly trace: Trace) {}

  render(selection: AreaSelection) {
    const selectionChanged =
      this.previousSelection === undefined ||
      !areaSelectionsEqual(this.previousSelection, selection);
    if (selectionChanged) {
      this.previousSelection = selection;
      this.trackIds = selection.tracks
        .filter((track) => track.tags?.kinds?.includes(SLICE_TRACK_KIND))
        .flatMap((track) => track.tags?.trackIds ?? []);

      this.state = undefined;
      if (this.trackIds.length > 0) {
        this.updateSqlView(selection).then(() => {
          this.state = this.createState();
        });
      }
    }

    if (this.trackIds.length === 0) return undefined;
    const state = this.state;
    if (state?.getData() === undefined) {
      return {
        isLoading: true,
        content: m('div.pf-loading-container', m(Spinner)),
      };
    }

    return {
      isLoading: false,
      content: m(PivotTable, {
        state,
        getSelectableColumns: () => state.table.columns,
      }),
    };
  }

  private async updateSqlView(selection: AreaSelection) {
    await this.trace.engine.query(`
      CREATE OR REPLACE PERFETTO VIEW v8_rcs_view AS
      WITH rcs_entries AS (
        SELECT
          s.ts,
          s.dur,
          s.track_id,
          SUBSTR(a.key, 26, LENGTH(a.key) - 28) AS name,
          SUBSTR(a.key, -3) AS suffix,
          a.int_value,
          CASE
            WHEN s.dur = 0 THEN 1.0
            ELSE
              MAX(0.0, (
                  MIN(s.ts + s.dur, ${selection.end}) -
                  MAX(s.ts, ${selection.start}))
              ) / CAST(s.dur AS DOUBLE)
          END AS ratio
        FROM slice s
        JOIN args a ON s.arg_set_id = a.arg_set_id
        WHERE
          a.key GLOB 'debug.runtime-call-stats.*' AND
          s.track_id IN (${this.trackIds.join(',')}) AND
          s.ts < ${selection.end} AND s.ts + s.dur > ${selection.start}
      )
      SELECT
        ts,
        dur,
        track_id,
        name AS v8_rcs_name,
        ratio,
        CASE
          WHEN name LIKE '%Total%' THEN 'total'
          WHEN name LIKE '%RegExp%' THEN 'regexp'
          WHEN name LIKE '%IC^_%' ESCAPE '^' THEN 'ic'
          WHEN name LIKE '%IC%Miss' THEN 'ic'
          WHEN name LIKE 'IC' THEN 'ic'
          WHEN name LIKE 'Json%' THEN 'json'
          WHEN name LIKE '%Optimize%Background%' THEN 'optimize_bg'
          WHEN name LIKE '%Optimize%Concurrent%' THEN 'optimize_bg'
          WHEN name LIKE 'StackGuard%' THEN 'optimize'
          WHEN name LIKE 'Optimize%' THEN 'optimize'
          WHEN name LIKE 'Deoptimize%' THEN 'optimize'
          WHEN name LIKE 'Recompile%' THEN 'optimize'
          WHEN name LIKE '%TierUp%' THEN 'optimize'
          WHEN name LIKE '%BudgetInterrupt%' THEN 'optimize'
          WHEN name LIKE 'Compile%Optimized%' THEN 'optimize'
          WHEN name LIKE '%Compile%Background%' THEN 'compile_bg'
          WHEN name LIKE 'Compile%' THEN 'compile'
          WHEN name LIKE '%^_Compile%' ESCAPE '^' THEN 'compile'
          WHEN name LIKE '%CompileLazy%' THEN 'compile'
          WHEN name LIKE '%Parse%Background%' THEN 'parse_bg'
          WHEN name LIKE 'Parse%' THEN 'parse'
          WHEN name LIKE 'PreParse%' THEN 'parse'
          WHEN name LIKE '%GetMoreDataCallback%' THEN 'network_data'
          WHEN name LIKE '%Callback%' THEN 'callback'
          WHEN name LIKE '%Blink C\+\+%' THEN 'callback'
          WHEN name LIKE '%API%' THEN 'api'
          WHEN name LIKE 'GC^_Custom^_%'  ESCAPE '^' THEN 'gc_custom'
          WHEN name LIKE 'GC^_%BACKGROUND%' ESCAPE '^' THEN 'gc_bg'
          WHEN name LIKE 'GC^_%Background%' ESCAPE '^' THEN 'gc_bg'
          WHEN name LIKE 'GC^_%AllocateInTargetSpace' ESCAPE '^' THEN 'gc'
          WHEN name LIKE 'GC_%' ESCAPE '^' THEN 'gc'
          WHEN name LIKE 'JS^_Execution' ESCAPE '^' THEN 'javascript'
          WHEN name LIKE 'JavaScript' THEN 'javascript'
          WHEN name LIKE '%Blink^_%' ESCAPE '^' THEN 'blink'
          ELSE 'runtime'
        END AS v8_rcs_group,
        SUM(CASE WHEN suffix = '[0]'
          THEN CAST(int_value * ratio AS INT)
          ELSE 0
          END) AS v8_rcs_count,
        SUM(CASE WHEN suffix = '[1]'
          THEN CAST(int_value * 1000 * ratio AS INT)
          ELSE 0
          END) AS v8_rcs_dur
      FROM rcs_entries
      GROUP BY ts, dur, track_id, name
    `);
  }

  private createState(): PivotTableState {
    const tableDef = resolveTableDefinition(
      this.trace,
      V8_RUNTIME_CALL_STATS_VIEW,
    );

    const findColumn = (name: string) => {
      return assertExists(tableDef.columns.find((c) => c.column === name));
    };

    const v8RcsName = findColumn('v8_rcs_name');
    const v8RcsGroup = findColumn('v8_rcs_group');
    const v8RcsDur = findColumn('v8_rcs_dur');
    const v8RcsCount = findColumn('v8_rcs_count');

    const durAggregation: Aggregation = {
      column: v8RcsDur,
      op: 'sum',
    };

    this.state = new PivotTableState({
      trace: this.trace,
      table: tableDef,
      pivots: [v8RcsGroup, v8RcsName],
      aggregations: [
        durAggregation,
        {
          column: v8RcsCount,
          op: 'sum',
        },
      ],
    });
    // Remove the default 'count' aggregation added by PivotTableState.
    this.state.removeAggregation(this.state.getAggregations().length - 1);
    this.state.sortByAggregation(durAggregation, 'DESC');
    return this.state;
  }
}
