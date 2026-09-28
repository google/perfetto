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
import type {ColumnDef} from '../../components/widgets/datagrid/datagrid_schema';
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

/** Track URI for a single process's proc-state timeline. */
export function processTrackUri(upid: number): string {
  return `com.android.ProcessState#process.${upid}`;
}

// Which slice of the interval table the selection refers to. Selecting
// per-process tracks scopes by process; selecting the concurrency counters
// scopes by state, which is what makes "drag over the CACHED_EMPTY counter and
// see who moved" work.
interface Scope {
  readonly upids: ReadonlyArray<number>;
  readonly states: ReadonlyArray<string>;
}

function probeScope(area: AreaSelection): Scope | undefined {
  const upids = new Set<number>();
  const states = new Set<string>();
  for (const track of area.tracks) {
    const tags = track.tags;
    if (tags?.type === TAG_PROCESS_TRACK && tags.upid !== undefined) {
      upids.add(tags.upid);
    } else if (tags?.type === TAG_COUNT_TRACK) {
      if (typeof tags.state === 'string') {
        states.add(tags.state);
      }
    }
  }
  if (upids.size === 0 && states.size === 0) {
    return undefined;
  }
  return {upids: [...upids], states: [...states]};
}

// Restricts the interval table to the selected tracks and the selected time
// range. Intervals are clipped to the selection so partially covered ones are
// attributed proportionally.
function scopedIntervals(scope: Scope, area: AreaSelection): string {
  const terms: string[] = [];
  if (scope.upids.length > 0) {
    terms.push(`upid IN (${scope.upids.join(',')})`);
  }
  if (scope.states.length > 0) {
    terms.push(`state IN (${scope.states.map(sqliteString).join(',')})`);
  }
  return `
    SELECT
      *,
      min(iif(dur < 0, ${area.end}, ts + dur), ${area.end}) - max(ts, ${area.start}) AS clipped_dur
    FROM _android_process_state_intervals
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
function durationColumn(
  trace: Trace,
  title: string,
  helpText?: string,
): ColumnDef {
  return {
    title: helpText !== undefined ? titleWithHelp(title, helpText) : title,
    titleString: helpText !== undefined ? title : undefined,
    columnType: 'quantitative',
    cellRenderer: (value) => {
      // Averaging aggregates come back as floats.
      const dur = typeof value === 'number' ? BigInt(Math.round(value)) : value;
      if (typeof dur !== 'bigint') {
        return renderCell(value);
      }
      return m(DurationWidget, {trace, dur: Duration.fromRaw(dur)});
    },
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

function sliceIdColumn(trace: Trace): ColumnDef {
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
          title: 'Go to process state slice',
          icon: Icons.UpdateSelection,
          onclick: () => {
            trace.selection.selectTrackEvent(processTrackUri(upid), id, {
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

/** How long each process spent in each proc state over the selection. */
export class ProcessStateResidencyAggregator implements Aggregator {
  readonly id = 'android_process_state_residency';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area);
    if (scope === undefined) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const selectionDur = area.end - area.start;
        const table = await createPerfettoTable({
          engine,
          as: `
            WITH scoped AS (${scopedIntervals(scope, area)})
            SELECT
              process_name,
              pid,
              state,
              sum(clipped_dur) AS total_dur,
              sum(clipped_dur) / ${selectionDur}.0 AS occupancy,
              count() AS occurrences
            FROM scoped
            GROUP BY upid, state
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        process_name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        state: {title: 'State', columnType: 'text'},
        total_dur: durationColumn(this.trace, 'Time in state'),
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
        {id: 'state', field: 'state'},
        {id: 'total_dur', field: 'total_dur', aggregate: 'SUM', sort: 'DESC'},
        {id: 'occupancy', field: 'occupancy'},
        {id: 'occurrences', field: 'occurrences', aggregate: 'SUM'},
      ],
    };
  }

  getTabName() {
    return 'Process State Residency';
  }
}

/** Every live state change in the selection, as a table. */
export class ProcessStateTransitionsAggregator implements Aggregator {
  readonly id = 'android_process_state_transitions';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area);
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
              CASE WHEN dur >= 0 THEN dur END AS cur_dur,
              reason
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
        reason: {title: 'State change reason', columnType: 'text'},
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
        {id: 'reason', field: 'reason'},
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
    const scope = probeScope(area);
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
        {
          id: 'bind_application_delay',
          field: 'bind_application_delay',
          aggregate: 'AVG',
        },
        {
          id: 'process_start_delay',
          field: 'process_start_delay',
          aggregate: 'AVG',
        },
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
    const scope = probeScope(area);
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
        {id: 'prev_dur', field: 'prev_dur', aggregate: 'AVG'},
        {id: 'exit_reason', field: 'exit_reason'},
        {id: 'exit_subreason', field: 'exit_subreason'},
      ],
    };
  }

  getTabName() {
    return 'Process Exits';
  }
}
