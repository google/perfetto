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
import {
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
} from '../../components/aggregation_adapter';
import {formatPercentValue} from '../../components/aggregation_panel';
import {titleWithHelp} from '../../components/distribution_panel';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import {renderCell} from '../../components/widgets/datagrid/datagrid';
import type {
  CellRenderer,
  ColumnDef,
} from '../../components/widgets/datagrid/datagrid_schema';
import {Icons} from '../../base/semantic_icons';
import {Duration, Time} from '../../base/time';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {sqliteString} from '../../base/string_utils';
import {Anchor} from '../../widgets/anchor';

/** Track tag `type` of the per-process proc-state timelines. */
export const TAG_PROCESS_TRACK = 'android_process_state';

/** Track tag `type` of the per-state concurrency counters. */
export const TAG_COUNT_TRACK = 'android_process_state_count';

/** Track tag `type` of the per-process freezer timelines. */
export const TAG_FREEZER_TRACK = 'android_freezer';

/** Track tag `type` of the FROZEN and UNFROZEN concurrency counters. */
export const TAG_FREEZER_COUNT_TRACK = 'android_freezer_count';

/** Track tag `type` of the per-process capability tracks. */
export const TAG_CAPABILITY_TRACK = 'android_process_capability';

/** Track tag `type` of the per-process OOM score tracks. */
export const TAG_OOM_SCORE_TRACK = 'android_oom_score';

/** Track URI for a single process's proc-state timeline. */
export function processTrackUri(upid: number): string {
  return `com.android.ProcessState#process.${upid}`;
}

/** Track URI for a single process's freezer timeline. */
export function freezerTrackUri(upid: number): string {
  return `${processTrackUri(upid)}.freezer`;
}

// Which slice of an interval table the selection refers to. Selecting
// per-process tracks scopes by process; selecting the concurrency counters
// scopes by state, which is what makes "drag over the CACHED_EMPTY counter and
// see who moved" work.
interface Scope {
  readonly upids: ReadonlyArray<number>;
  readonly states: ReadonlyArray<string>;
  // Per-process tracks that show a single value, e.g. one capability.
  readonly values: ReadonlyArray<{upid: number; value: string}>;
}

function probeScope(
  area: AreaSelection,
  processTag: string,
  countTag?: string,
  // Per-process tracks with this tag show only its value.
  valueTag?: string,
): Scope | undefined {
  const upids = new Set<number>();
  const states = new Set<string>();
  const values: Array<{upid: number; value: string}> = [];
  for (const track of area.tracks) {
    const tags = track.tags;
    if (tags?.type === processTag && tags.upid !== undefined) {
      const value = valueTag === undefined ? undefined : tags[valueTag];
      if (typeof value === 'string') {
        values.push({upid: tags.upid, value});
      } else {
        upids.add(tags.upid);
      }
    } else if (countTag !== undefined && tags?.type === countTag) {
      if (typeof tags.state === 'string') {
        states.add(tags.state);
      }
    }
  }
  if (upids.size === 0 && states.size === 0 && values.length === 0) {
    return undefined;
  }
  return {upids: [...upids], states: [...states], values};
}

// Restricts the interval table to the selected tracks and the selected time
// range. Intervals are clipped to the selection so partially covered ones are
// attributed proportionally.
function scopedIntervals(
  scope: Scope,
  area: AreaSelection,
  table: string,
  column: string,
): string {
  const terms: string[] = [];
  if (scope.upids.length > 0) {
    terms.push(`upid IN (${scope.upids.join(',')})`);
  }
  if (scope.states.length > 0) {
    terms.push(`state IN (${scope.states.map(sqliteString).join(',')})`);
  }
  for (const {upid, value} of scope.values) {
    terms.push(`(upid = ${upid} AND ${column} = ${sqliteString(value)})`);
  }
  return `
    SELECT
      *,
      min(iif(dur < 0, ${area.end}, ts + dur), ${area.end}) - max(ts, ${area.start}) AS clipped_dur
    FROM ${table}
    WHERE (${terms.join(' OR ')})
      AND ts < ${area.end}
      AND (dur < 0 OR ts + dur > ${area.start})
  `;
}

// Restricts the interval table to transitions occurring within the selected
// time range on the selected tracks. For state-counter tracks, includes both
// transitions into and out of the selected states.
function scopedTransitions(scope: Scope, area: AreaSelection): string {
  const terms: string[] = [];
  if (scope.upids.length > 0) {
    terms.push(`upid IN (${scope.upids.join(',')})`);
  }
  if (scope.states.length > 0) {
    const stateList = scope.states.map(sqliteString).join(',');
    terms.push(`(state IN (${stateList}) OR prev_state IN (${stateList}))`);
  }
  return `
    SELECT *
    FROM _android_process_state_intervals
    WHERE (${terms.join(' OR ')})
      AND ts >= ${area.start}
      AND ts < ${area.end}
  `;
}

// Durations follow the user's time format and precision settings.
export function durationCellRenderer(trace: Trace): CellRenderer {
  return (value) => {
    // Averaging aggregates come back as floats.
    const dur = typeof value === 'number' ? BigInt(Math.round(value)) : value;
    if (typeof dur !== 'bigint') {
      return renderCell(value);
    }
    return m(DurationWidget, {trace, dur: Duration.fromRaw(dur)});
  };
}

function durationColumn(
  trace: Trace,
  title: string,
  helpText?: string,
): ColumnDef {
  return {
    title: helpText !== undefined ? titleWithHelp(title, helpText) : title,
    titleString: helpText !== undefined ? title : undefined,
    columnType: 'quantitative',
    cellRenderer: durationCellRenderer(trace),
  };
}

function timestampColumn(trace: Trace, title: string): ColumnDef {
  return {
    title,
    columnType: 'quantitative',
    cellRenderer: (value) => {
      if (typeof value === 'bigint') {
        return m(Timestamp, {trace, ts: Time.fromRaw(value)});
      }
      return renderCell(value);
    },
  };
}

function sliceIdColumn(trace: Trace, trackUri = processTrackUri): ColumnDef {
  return {
    title: 'Slice ID',
    columnType: 'identifier',
    cellRenderer: (value) => {
      if (typeof value !== 'string') {
        return renderCell(value);
      }
      const {id, upid} = JSON.parse(value) as {id: number; upid: number};
      return m(
        Anchor,
        {
          title: 'Go to slice',
          icon: Icons.UpdateSelection,
          onclick: () => {
            trace.selection.selectTrackEvent(trackUri(upid), id, {
              scrollToSelection: true,
              switchToCurrentSelectionTab: false,
            });
          },
        },
        String(id),
      );
    },
    cellFormatter: (value) => {
      if (typeof value === 'string') {
        return String((JSON.parse(value) as {id: number}).id);
      }
      return String(value ?? '');
    },
  };
}

/** An interval table that a residency tab breaks down per process. */
export interface ResidencySource {
  readonly id: string;
  readonly name: string;
  readonly table: string;
  // The column the time is broken down by, and the grid titles.
  readonly column: string;
  readonly columnTitle: string;
  readonly durationTitle: string;
  // Tracks tagged processTag scope the tab by process, countTag by state.
  readonly processTag: string;
  readonly countTag?: string;
  // Per-process tracks with this tag show a single value of column.
  readonly valueTag?: string;
}

export const PROCESS_STATE_RESIDENCY: ResidencySource = {
  id: 'android_process_state_residency',
  name: 'Process State Residency',
  table: '_android_process_state_intervals',
  column: 'state',
  columnTitle: 'State',
  durationTitle: 'Time in state',
  processTag: TAG_PROCESS_TRACK,
  countTag: TAG_COUNT_TRACK,
};

export const FREEZER_RESIDENCY: ResidencySource = {
  id: 'android_freezer_residency',
  name: 'Freezer Residency',
  table: '_android_freezer_intervals',
  column: 'state',
  columnTitle: 'State',
  durationTitle: 'Time in state',
  processTag: TAG_FREEZER_TRACK,
  countTag: TAG_FREEZER_COUNT_TRACK,
};

export const CAPABILITY_RESIDENCY: ResidencySource = {
  id: 'android_process_capability_residency',
  name: 'Capability Residency',
  table: '_android_process_capability_intervals',
  column: 'capability',
  columnTitle: 'Capability',
  durationTitle: 'Time enabled',
  processTag: TAG_CAPABILITY_TRACK,
  valueTag: 'capability',
};

/** How long each process spent in each state of a source over the selection. */
export class ResidencyAggregator implements Aggregator {
  readonly id: string;

  constructor(
    private readonly trace: Trace,
    private readonly source: ResidencySource,
  ) {
    this.id = source.id;
  }

  probe(area: AreaSelection) {
    const {table, column, processTag, countTag, valueTag} = this.source;
    const scope = probeScope(area, processTag, countTag, valueTag);
    if (scope === undefined) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const selectionDur = area.end - area.start;
        const residency = await createPerfettoTable({
          engine,
          as: `
            WITH scoped AS (${scopedIntervals(scope, area, table, column)})
            SELECT
              process_name,
              pid,
              ${column},
              sum(clipped_dur) AS total_dur,
              sum(clipped_dur) / ${selectionDur}.0 AS occupancy,
              count() AS occurrences
            FROM scoped
            GROUP BY upid, ${column}
          `,
        });
        return createAggregationData(residency);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    const {column, columnTitle, durationTitle} = this.source;
    return {
      schema: {
        process_name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        [column]: {title: columnTitle, columnType: 'text'},
        total_dur: durationColumn(this.trace, durationTitle),
        occupancy: {
          title: '% of selection',
          columnType: 'quantitative',
          cellRenderer: formatPercentValue,
        },
        occurrences: {title: 'Occurrences', columnType: 'quantitative'},
      },
      initialColumns: [
        {id: 'process_name', field: 'process_name'},
        {id: 'pid', field: 'pid'},
        {id: column, field: column},
        {id: 'total_dur', field: 'total_dur', aggregate: 'SUM', sort: 'DESC'},
        {id: 'occupancy', field: 'occupancy'},
        {id: 'occurrences', field: 'occurrences', aggregate: 'SUM'},
      ],
    };
  }

  getTabName() {
    return this.source.name;
  }
}

/** Every live state change in the selection, as a table. */
export class ProcessStateTransitionsAggregator implements Aggregator {
  readonly id = 'android_process_state_transitions';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area, TAG_PROCESS_TRACK, TAG_COUNT_TRACK);
    if (scope === undefined) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const table = await createPerfettoTable({
          engine,
          as: `
            WITH scoped AS (${scopedTransitions(scope, area)})
            SELECT
              ts,
              json_object('id', id, 'upid', upid) AS slice_id,
              process_name,
              pid,
              prev_state,
              prev_state_duration AS prev_dur,
              state AS cur_state,
              iif(dur < 0, trace_end() - ts, dur) AS cur_dur,
              state_change_reason
            FROM scoped
            WHERE state != 'NONEXISTENT' AND state != 'EXITED'
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        ts: timestampColumn(this.trace, 'Transition ts'),
        slice_id: sliceIdColumn(this.trace),
        process_name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        prev_state: {title: 'Previous state', columnType: 'text'},
        prev_dur: durationColumn(
          this.trace,
          'Time in previous state',
          'Total elapsed duration of the previous state interval, unclipped by the selection window.',
        ),
        cur_state: {title: 'Current state', columnType: 'text'},
        cur_dur: durationColumn(
          this.trace,
          'Time in current state',
          'Total elapsed duration of the current state interval, unclipped by the selection window.',
        ),
        state_change_reason: {title: 'State change reason', columnType: 'text'},
      },
      initialColumns: [
        {id: 'ts', field: 'ts', sort: 'ASC'},
        {id: 'slice_id', field: 'slice_id'},
        {id: 'process_name', field: 'process_name'},
        {id: 'pid', field: 'pid'},
        {id: 'prev_state', field: 'prev_state'},
        {id: 'prev_dur', field: 'prev_dur'},
        {id: 'cur_state', field: 'cur_state'},
        {id: 'cur_dur', field: 'cur_dur'},
        {id: 'state_change_reason', field: 'state_change_reason'},
      ],
    };
  }

  getTabName() {
    return 'Process State Transitions';
  }
}

/** Process starts in the selection, with hosting and delay metadata. */
export class ProcessStartsAggregator implements Aggregator {
  readonly id = 'android_process_starts';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area, TAG_PROCESS_TRACK, TAG_COUNT_TRACK);
    if (scope === undefined) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const table = await createPerfettoTable({
          engine,
          as: `
            WITH scoped AS (${scopedTransitions(scope, area)})
            SELECT
              ts,
              json_object('id', id, 'upid', upid) AS slice_id,
              process_name,
              pid,
              prev_state,
              hosting_type,
              hosting_name,
              trigger_type,
              bind_application_delay_ms * 1000000 AS bind_application_delay,
              process_start_delay_ms * 1000000 AS process_start_delay
            FROM scoped
            WHERE state = 'NONEXISTENT'
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        ts: timestampColumn(this.trace, 'Start ts'),
        slice_id: sliceIdColumn(this.trace),
        process_name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        prev_state: {title: 'Previous state', columnType: 'text'},
        hosting_type: {title: 'Hosting type', columnType: 'text'},
        hosting_name: {title: 'Hosting name', columnType: 'text'},
        trigger_type: {title: 'Trigger type', columnType: 'text'},
        bind_application_delay: durationColumn(
          this.trace,
          'Bind application delay',
        ),
        process_start_delay: durationColumn(this.trace, 'Process start delay'),
      },
      initialColumns: [
        {id: 'ts', field: 'ts', sort: 'ASC'},
        {id: 'slice_id', field: 'slice_id'},
        {id: 'process_name', field: 'process_name'},
        {id: 'pid', field: 'pid'},
        {id: 'prev_state', field: 'prev_state'},
        {id: 'hosting_type', field: 'hosting_type'},
        {id: 'hosting_name', field: 'hosting_name'},
        {id: 'trigger_type', field: 'trigger_type'},
        {id: 'bind_application_delay', field: 'bind_application_delay'},
        {id: 'process_start_delay', field: 'process_start_delay'},
      ],
    };
  }

  getTabName() {
    return 'Process Starts';
  }
}

/** Process exits in the selection, with previous state and exit reasons. */
export class ProcessExitsAggregator implements Aggregator {
  readonly id = 'android_process_exits';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area, TAG_PROCESS_TRACK, TAG_COUNT_TRACK);
    if (scope === undefined) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const table = await createPerfettoTable({
          engine,
          as: `
            WITH scoped AS (${scopedTransitions(scope, area)})
            SELECT
              ts,
              json_object('id', id, 'upid', upid) AS slice_id,
              process_name,
              pid,
              prev_state,
              prev_state_duration AS prev_dur,
              exit_reason,
              exit_subreason
            FROM scoped
            WHERE state = 'EXITED'
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        ts: timestampColumn(this.trace, 'Exit ts'),
        slice_id: sliceIdColumn(this.trace),
        process_name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        prev_state: {title: 'Previous state', columnType: 'text'},
        prev_dur: durationColumn(
          this.trace,
          'Time in previous state',
          'Total elapsed duration of the previous state interval, unclipped by the selection window.',
        ),
        exit_reason: {title: 'Exit reason', columnType: 'text'},
        exit_subreason: {title: 'Exit subreason', columnType: 'text'},
      },
      initialColumns: [
        {id: 'ts', field: 'ts', sort: 'ASC'},
        {id: 'slice_id', field: 'slice_id'},
        {id: 'process_name', field: 'process_name'},
        {id: 'pid', field: 'pid'},
        {id: 'prev_state', field: 'prev_state'},
        {id: 'prev_dur', field: 'prev_dur'},
        {id: 'exit_reason', field: 'exit_reason'},
        {id: 'exit_subreason', field: 'exit_subreason'},
      ],
    };
  }

  getTabName() {
    return 'Process Exits';
  }
}

/** Every freeze and unfreeze in the selection, with the unfreeze reasons. */
export class FreezerTransitionsAggregator implements Aggregator {
  readonly id = 'android_freezer_transitions';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area, TAG_FREEZER_TRACK, TAG_FREEZER_COUNT_TRACK);
    if (scope === undefined) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        // Every freeze or unfreeze changes both counters, so a counter selects
        // them all.
        const processes =
          scope.states.length > 0
            ? 'TRUE'
            : `upid IN (${scope.upids.join(',')})`;
        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT
              ts,
              json_object('id', id, 'upid', upid) AS slice_id,
              process_name,
              pid,
              state,
              prev_state_duration AS prev_dur,
              iif(dur < 0, trace_end() - ts, dur) AS cur_dur,
              unfreeze_reason
            FROM _android_freezer_intervals
            -- Only spans started by a freeze or an unfreeze have one.
            WHERE prev_state_duration IS NOT NULL
              AND ${processes}
              AND ts >= ${area.start}
              AND ts < ${area.end}
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        ts: timestampColumn(this.trace, 'Transition ts'),
        slice_id: sliceIdColumn(this.trace, freezerTrackUri),
        process_name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        state: {title: 'Current state', columnType: 'text'},
        prev_dur: durationColumn(
          this.trace,
          'Time in previous state',
          'Total elapsed duration of the previous state interval, unclipped by the selection window.',
        ),
        cur_dur: durationColumn(
          this.trace,
          'Time in current state',
          'Total elapsed duration of the current state interval, unclipped by the selection window.',
        ),
        unfreeze_reason: {title: 'Unfreeze reason', columnType: 'text'},
      },
      initialColumns: [
        {id: 'ts', field: 'ts', sort: 'ASC'},
        {id: 'slice_id', field: 'slice_id'},
        {id: 'process_name', field: 'process_name'},
        {id: 'pid', field: 'pid'},
        {id: 'state', field: 'state'},
        {id: 'prev_dur', field: 'prev_dur'},
        {id: 'cur_dur', field: 'cur_dur'},
        {id: 'unfreeze_reason', field: 'unfreeze_reason'},
      ],
    };
  }

  getTabName() {
    return 'Freezer Transitions';
  }
}

/**
 * The OOM score changes of the given processes: one row per new score, with
 * how long it lasted and the score before it.
 */
export function oomScoreChanges(upids: ReadonlyArray<number>): string {
  return `
    SELECT
      c.id,
      c.ts,
      coalesce(lead(c.ts) OVER w, p.end_ts, trace_end()) - c.ts AS dur,
      c.upid,
      c.oom_score,
      c.reason,
      lag(c.oom_score) OVER w AS prev_score,
      c.ts - lag(c.ts) OVER w AS prev_dur
    FROM (
      SELECT id, ts, upid, oom_score, reason
      FROM (
        SELECT
          *,
          lag(oom_score) OVER (PARTITION BY upid ORDER BY ts) AS prev
        FROM _android_process_oom_scores
        WHERE upid IN (${upids.join(',')})
      )
      -- A score repeats when only the proc state, the capabilities or the
      -- process group changed.
      WHERE prev IS NULL OR prev != oom_score
    ) AS c
    JOIN process AS p USING (upid)
    WINDOW w AS (PARTITION BY c.upid ORDER BY c.ts)
  `;
}

/** Every OOM score change in the selection, with the OomAdjuster reason. */
export class OomScoreTransitionsAggregator implements Aggregator {
  readonly id = 'android_oom_score_transitions';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area, TAG_OOM_SCORE_TRACK);
    if (scope === undefined) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT
              t.ts,
              m.process_name,
              p.pid,
              t.prev_score,
              t.prev_dur,
              t.oom_score AS cur_score,
              t.dur AS cur_dur,
              t.reason
            FROM (${oomScoreChanges(scope.upids)}) AS t
            JOIN process AS p USING (upid)
            LEFT JOIN android_process_metadata AS m USING (upid)
            WHERE t.prev_score IS NOT NULL
              AND t.ts >= ${area.start}
              AND t.ts < ${area.end}
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        ts: timestampColumn(this.trace, 'Transition ts'),
        process_name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        prev_score: {title: 'Previous score', columnType: 'quantitative'},
        prev_dur: durationColumn(
          this.trace,
          'Time at previous score',
          'Total elapsed duration at the previous score, unclipped by the selection window.',
        ),
        cur_score: {title: 'Current score', columnType: 'quantitative'},
        cur_dur: durationColumn(
          this.trace,
          'Time at current score',
          'Total elapsed duration at the current score, unclipped by the selection window.',
        ),
        reason: {
          title: titleWithHelp(
            'OomAdjuster reason',
            'What triggered the OomAdjuster update that set this score. One update can change the scores of many processes.',
          ),
          titleString: 'OomAdjuster reason',
          columnType: 'text',
        },
      },
      initialColumns: [
        {id: 'ts', field: 'ts', sort: 'ASC'},
        {id: 'process_name', field: 'process_name'},
        {id: 'pid', field: 'pid'},
        {id: 'prev_score', field: 'prev_score'},
        {id: 'prev_dur', field: 'prev_dur'},
        {id: 'cur_score', field: 'cur_score'},
        {id: 'cur_dur', field: 'cur_dur'},
        {id: 'reason', field: 'reason'},
      ],
    };
  }

  getTabName() {
    return 'OOM Score Transitions';
  }
}
