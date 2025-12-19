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
  LONG,
  STR,
  NUM_NULL,
} from '../../trace_processor/query_result';
import {sqlValueToReadableString} from '../../trace_processor/sql_utils';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {threadStateRef} from '../widgets/thread_state';
import {getThreadName} from '../sql_utils/thread';
import {getProcessName} from '../sql_utils/process';
import {sliceRef} from '../widgets/slice';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {SqlRef} from '../../widgets/sql_ref';
import {renderSliceArguments} from '../details/slice_args';
import {ArgsDict, getArgs} from '../sql_utils/args';

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

interface Data {
  readonly name: string;
  readonly ts: time;
  readonly dur: duration;
  readonly rawCols: {[key: string]: SqlValue};
}

export class DebugSliceTrackDetailsPanel implements TrackEventDetailsPanel {
  private data?: Data;

  // These are the actual loaded args from the args table assuming an arg_set_id
  // is supplied.
  private args?: ArgsDict;

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

  private renderThreadStateInfo(threadState: ThreadState): m.Children {
    const thread = threadState.thread;
    const process = thread?.process;
    return m(TreeNode, {left: threadStateRef(this.trace, threadState)}, [
      m(TreeNode, {left: 'Thread', right: getThreadName(thread)}),
      m(TreeNode, {left: 'Process', right: getProcessName(process)}),
      m(TreeNode, {left: 'State', right: threadState.state}),
    ]);
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

  private renderSliceInfo(slice: SliceDetails): m.Children {
    return m(TreeNode, {left: sliceRef(this.trace, slice, 'Slice')}, [
      m(TreeNode, {left: 'Name', right: slice.name}),
      m(TreeNode, {left: 'Thread', right: getThreadName(slice.thread)}),
      m(TreeNode, {left: 'Process', right: getProcessName(slice.process)}),
      hasArgs(slice.args) &&
        m(TreeNode, {left: 'Args'}, [
          renderSliceArguments(this.trace, slice.args),
        ]),
    ]);
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
    const data = this.data;
    const args = this.args;

    if (data === undefined) {
      return m('h2', 'Loading');
    }

    return m(
      DetailsShell,
      {
        title: 'Slice',
      },
      m(GridLayout, [
        this.renderDetailsSection(data),
        args && this.renderArgsSection(args),
      ]),
    );
  }

  private renderDetailsSection(data: Data) {
    const trace = this.trace;
    return m(Section, {title: 'Details'}, [
      m(Tree, [
        m(TreeNode, {left: 'Name', right: data.name}),
        m(TreeNode, {
          left: 'Start time',
          right: m(Timestamp, {trace, ts: data.ts}),
        }),
        m(TreeNode, {
          left: 'Duration',
          right: m(DurationWidget, {trace, dur: data.dur}),
        }),
        m(TreeNode, {
          left: 'SQL ID',
          right: m(SqlRef, {table: this.tableName, id: this.eventId}),
        }),
        this.threadState && this.renderThreadStateInfo(this.threadState),
        this.slice && this.renderSliceInfo(this.slice),
        m(
          TreeNode,
          {left: 'Raw columns'},
          Object.entries(data.rawCols).map(([k, v]) => {
            return m(TreeNode, {
              left: k,
              right: sqlValueToReadableString(v),
            });
          }),
        ),
      ]),
    ]);
  }

  private renderArgsSection(args: ArgsDict) {
    return m(Section, {title: 'Arguments'}, [
      m(Tree, renderArguments(this.trace, args)),
    ]);
  }

  getTitle(): string {
    return `Current Selection`;
  }

  isLoading() {
    return this.data === undefined;
  }
}
