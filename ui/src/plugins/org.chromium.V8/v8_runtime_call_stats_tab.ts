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
  Selection,
  TrackEventSelection,
  TrackSelection,
} from '../../public/selection';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {SqlTableDefinition} from '../../components/widgets/sql/table/table_description';
import {PerfettoSqlTypes} from '../../trace_processor/perfetto_sql_type';
import {resolveTableDefinition} from '../../components/widgets/sql/table/columns';
import {Spinner} from '../../widgets/spinner';
import {Aggregation} from '../../components/widgets/sql/pivot_table/aggregations';
import {Tab} from '../../public/tab';

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

export class V8RuntimeCallStatsTab implements Tab {
  private state?: PivotTableState;
  private previousSelection?: Selection;
  private loading = false;

  constructor(private readonly trace: Trace) {}

  getTitle(): string {
    return 'V8 Runtime Call Stats';
  }

  render(): m.Children {
    const selection = this.trace.selection.selection;
    if (
      selection.kind !== 'area' &&
      selection.kind !== 'track_event' &&
      selection.kind !== 'track' &&
      selection.kind !== 'empty'
    ) {
      return this.renderEmptyState();
    }

    const selectionChanged = this.hasSelectionChanged(selection);

    if (selectionChanged) {
      this.previousSelection = selection;
      this.state = undefined;
      this.loading = true;
      this.loadData(selection);
    }

    if (this.loading) {
      return m('div.pf-loading-container', m(Spinner));
    }

    if (!this.state) {
      return this.renderEmptyState();
    }

    return m(PivotTable, {
      state: this.state,
      getSelectableColumns: () => this.state!.table.columns,
    });
  }

  private renderEmptyState(): m.Children {
    return m(
      'div',
      {style: {padding: '10px'}},
      'Select an area, a slice, or a track to view specific V8 Runtime Call Stats, or clear selection to view all.',
    );
  }

  private hasSelectionChanged(selection: Selection): boolean {
    if (this.previousSelection === undefined) return true;
    if (this.previousSelection.kind !== selection.kind) return true;

    if (selection.kind === 'area') {
      return !areaSelectionsEqual(
        this.previousSelection as AreaSelection,
        selection,
      );
    }

    if (selection.kind === 'track_event') {
      const prev = this.previousSelection as TrackEventSelection;
      return (
        prev.eventId !== selection.eventId ||
        prev.trackUri !== selection.trackUri
      );
    }

    if (selection.kind === 'track') {
      const prev = this.previousSelection as TrackSelection;
      return prev.trackUri !== selection.trackUri;
    }

    return false;
  }

  private async loadData(selection: Selection) {
    let shouldLoad = false;
    let trackIds: number[] = [];

    if (selection.kind === 'area') {
      trackIds = selection.tracks
        .filter((track) => track.tags?.kinds?.includes(SLICE_TRACK_KIND))
        .flatMap((track) => track.tags?.trackIds ?? []);
      shouldLoad = trackIds.length > 0;
    } else if (selection.kind === 'track') {
      const track = this.trace.tracks.getTrack(selection.trackUri);
      trackIds = (track?.tags?.trackIds ?? []) as number[];
      shouldLoad = trackIds.length > 0;
    } else if (selection.kind === 'track_event') {
      const result = await this.trace.engine.query(`
          SELECT 1 FROM args
          JOIN slice ON slice.arg_set_id = args.arg_set_id
          WHERE slice.id = ${selection.eventId}
          AND args.key GLOB 'debug.runtime-call-stats.*'
          LIMIT 1
        `);
      shouldLoad = result.numRows() > 0;
    } else if (selection.kind === 'empty') {
      shouldLoad = true;
    }

    if (shouldLoad && this.previousSelection === selection) {
      await this.updateSqlView(selection, trackIds);
      if (this.previousSelection === selection) {
        this.state = this.createState();
      }
    }

    if (this.previousSelection === selection) {
      this.loading = false;
      this.trace.raf.scheduleFullRedraw();
    }
  }

  private async updateSqlView(selection: Selection, trackIds: number[]) {
    let start: bigint;
    let end: bigint;
    let whereClause: string;

    if (selection.kind === 'area') {
      start = selection.start;
      end = selection.end;
      whereClause = `
          s.track_id IN (${trackIds.join(',')}) AND
          s.ts < ${end} AND s.ts + s.dur > ${start}
      `;
    } else if (selection.kind === 'track') {
      start = this.trace.traceInfo.start;
      end = this.trace.traceInfo.end;
      whereClause = `s.track_id IN (${trackIds.join(',')})`;
    } else if (selection.kind === 'track_event') {
      const prev = selection as TrackEventSelection;
      start = prev.ts;
      end = prev.ts + (prev.dur ?? 0n);
      whereClause = `s.id = ${prev.eventId}`;
    } else {
      // Empty selection - all data
      start = this.trace.traceInfo.start;
      end = this.trace.traceInfo.end;
      whereClause = '1 = 1';
    }

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
                  MIN(s.ts + s.dur, ${end}) -
                  MAX(s.ts, ${start}))
              ) / CAST(s.dur AS DOUBLE)
          END AS ratio
        FROM slice s
        JOIN args a ON s.arg_set_id = a.arg_set_id
        WHERE
          a.key GLOB 'debug.runtime-call-stats.*' AND
          ${whereClause}
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
