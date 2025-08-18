// Copyright (C) 2023 The Android Open Source Project
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
import {duration, Time, time} from '../../base/time';
import {hasArgs, renderArguments} from '../details/args';
import {getSlice, SliceDetails} from '../sql_utils/slice';
import {asArgSetId, asSliceSqlId, Utid} from '../sql_utils/core_types';
import {getThreadState, ThreadState} from '../sql_utils/thread_state';
import {DurationWidget} from '../widgets/duration';
import {Timestamp} from '../widgets/timestamp';
import {
  SqlValue,
  durationFromSql,
  LONG,
  STR,
  timeFromSql,
  NUM_NULL,
} from '../../trace_processor/query_result';
import {sqlValueToReadableString} from '../../trace_processor/sql_utils';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {dictToTreeNodes, Tree, TreeNode} from '../../widgets/tree';
import {threadStateRef} from '../widgets/thread_state';
import {getThreadName} from '../sql_utils/thread';
import {getProcessName} from '../sql_utils/process';
import {sliceRef} from '../widgets/slice';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {SqlRef} from '../../widgets/sql_ref';
import {renderSliceArguments} from '../details/slice_args';
import {Arg, getArgs} from '../sql_utils/args';

export const RAW_PREFIX = 'raw_';

function sqlValueToNumber(value?: SqlValue): number | undefined {
  if (typeof value === 'bigint') return Number(value);
  if (typeof value !== 'number') return undefined;
  return value;
}

function sqlValueToUtid(value?: SqlValue): Utid | undefined {
  if (typeof value === 'bigint') return Number(value) as Utid;
  if (typeof value !== 'number') return undefined;
  return value as Utid;
}

function renderTreeContents(dict: {[key: string]: m.Child}): m.Child[] {
  const children: m.Child[] = [];
  for (const key of Object.keys(dict)) {
    if (dict[key] === null || dict[key] === undefined) continue;
    children.push(
      m(TreeNode, {
        left: key,
        right: dict[key],
      }),
    );
  }
  return children;
}

export class DebugSliceTrackDetailsPanel implements TrackEventDetailsPanel {
  private data?: {
    name: string;
    ts: time;
    dur: duration;
    rawCols: {[key: string]: SqlValue};
  };

  // These are the actual loaded args from the args table assuming an arg_set_id
  // is supplied.
  private args?: Arg[];

  // We will try to interpret the arguments as references into well-known
  // tables. These values will be set if the relevant columns exist and
  // are consistent (e.g. 'ts' and 'dur' for this slice correspond to values
  // in these well-known tables).
  private threadState?: ThreadState;
  private slice?: SliceDetails;

  constructor(
    private readonly trace: Trace,
    private readonly tableName: string,
    private readonly eventId: number,
    private readonly argSetIdCol?: string,
  ) {}

  // If we suspect the slice might be a projection of a row from the
  // thread_state table, we should show some information about the thread and
  // make it clickable in order to go back to the canonical slice.
  // We detect whether it's the case if any of the following are true:
  // - There is a column
  private async maybeLoadThreadState(
    id: number | undefined,
    ts: time,
    dur: duration,
    table: string | undefined,
    utid?: Utid,
  ): Promise<ThreadState | undefined> {
    if (id === undefined) return undefined;
    if (utid === undefined) return undefined;

    const threadState = await getThreadState(this.trace.engine, id);
    if (threadState === undefined) return undefined;
    if (
      table === 'thread_state' ||
      (threadState.ts === ts &&
        threadState.dur === dur &&
        threadState.thread?.utid === utid)
    ) {
      return threadState;
    } else {
      return undefined;
    }
  }

  private renderThreadStateInfo(): m.Child {
    if (this.threadState === undefined) return null;
    return m(
      TreeNode,
      {
        left: threadStateRef(this.threadState),
        right: '',
      },
      renderTreeContents({
        Thread: getThreadName(this.threadState.thread),
        Process: getProcessName(this.threadState.thread?.process),
        State: this.threadState.state,
      }),
    );
  }

  private async maybeLoadSlice(
    id: number | undefined,
    ts: time,
    dur: duration,
    table: string | undefined,
    trackId?: number,
  ): Promise<SliceDetails | undefined> {
    if (id === undefined) return undefined;
    if (table !== 'slice' && trackId === undefined) return undefined;

    const slice = await getSlice(this.trace.engine, asSliceSqlId(id));
    if (slice === undefined) return undefined;
    if (
      table === 'slice' ||
      (slice.ts === ts && slice.dur === dur && slice.trackId === trackId)
    ) {
      return slice;
    } else {
      return undefined;
    }
  }

  private renderSliceInfo() {
    if (this.slice === undefined) return null;
    return m(
      TreeNode,
      {
        left: sliceRef(this.slice, 'Slice'),
        right: '',
      },
      m(TreeNode, {
        left: 'Name',
        right: this.slice.name,
      }),
      m(TreeNode, {
        left: 'Thread',
        right: getThreadName(this.slice.thread),
      }),
      m(TreeNode, {
        left: 'Process',
        right: getProcessName(this.slice.process),
      }),
      hasArgs(this.slice.args) &&
        m(
          TreeNode,
          {
            left: 'Args',
          },
          renderSliceArguments(this.trace, this.slice.args),
        ),
    );
  }

  async load() {
    const queryResult = await this.trace.engine.query(`
      SELECT *
      FROM ${this.tableName}
      WHERE id = ${this.eventId}
    `);

    const row = queryResult.firstRow({
      ts: LONG,
      dur: LONG,
      name: STR,
      ...(this.argSetIdCol && {arg_set_id: NUM_NULL}),
    });

    this.data = {
      name: row.name,
      ts: Time.fromRaw(row.ts),
      dur: row.dur,
      rawCols: {},
    };

    if (row.arg_set_id != null) {
      this.args = await getArgs(this.trace.engine, asArgSetId(row.arg_set_id));
    }

    for (const key of Object.keys(row)) {
      if (key.startsWith(RAW_PREFIX)) {
        this.data.rawCols[key.substr(RAW_PREFIX.length)] = (
          row as {[key: string]: SqlValue}
        )[key];
      }
    }

    this.threadState = await this.maybeLoadThreadState(
      sqlValueToNumber(this.data.rawCols['id']),
      this.data.ts,
      this.data.dur,
      sqlValueToReadableString(this.data.rawCols['table_name']),
      sqlValueToUtid(this.data.rawCols['utid']),
    );

    this.slice = await this.maybeLoadSlice(
      sqlValueToNumber(this.data.rawCols['id']) ??
        sqlValueToNumber(this.data.rawCols['slice_id']),
      this.data.ts,
      this.data.dur,
      sqlValueToReadableString(this.data.rawCols['table_name']),
      sqlValueToNumber(this.data.rawCols['track_id']),
    );
  }

  render() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }
    const details = dictToTreeNodes({
      'Name': this.data['name'] as string,
      'Start time': m(Timestamp, {ts: timeFromSql(this.data['ts'])}),
      'Duration': m(DurationWidget, {dur: durationFromSql(this.data['dur'])}),
      'SQL ID': m(SqlRef, {table: this.tableName, id: this.eventId}),
    });
    details.push(this.renderThreadStateInfo());
    details.push(this.renderSliceInfo());

    const rawCols: {[key: string]: m.Child} = {};
    for (const key of Object.keys(this.data.rawCols)) {
      rawCols[key] = sqlValueToReadableString(this.data.rawCols[key]);
    }

    // Print the raw columns from the source query (previously called 'args')
    details.push(m(TreeNode, {left: 'Raw columns'}, dictToTreeNodes(rawCols)));

    return m(
      DetailsShell,
      {
        title: 'Slice',
      },
      m(
        GridLayout,
        m(Section, {title: 'Details'}, m(Tree, details)),
        this.args &&
          m(
            Section,
            {title: 'Arguments'},
            m(Tree, renderArguments(this.trace, this.args)),
          ),
      ),
    );
  }

  getTitle(): string {
    return `Current Selection`;
  }

  isLoading() {
    return this.data === undefined;
  }
}
