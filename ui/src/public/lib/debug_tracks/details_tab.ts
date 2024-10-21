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
import {duration, Time, time} from '../../../base/time';
import {hasArgs, renderArguments} from '../../../frontend/slice_args';
import {getSlice, SliceDetails} from '../../../trace_processor/sql_utils/slice';
import {
  asSliceSqlId,
  Utid,
} from '../../../trace_processor/sql_utils/core_types';
import {
  getThreadState,
  ThreadState,
} from '../../../trace_processor/sql_utils/thread_state';
import {DurationWidget} from '../../../frontend/widgets/duration';
import {Timestamp} from '../../../frontend/widgets/timestamp';
import {
  ColumnType,
  durationFromSql,
  LONG,
  STR,
  timeFromSql,
} from '../../../trace_processor/query_result';
import {sqlValueToReadableString} from '../../../trace_processor/sql_utils';
import {DetailsShell} from '../../../widgets/details_shell';
import {GridLayout} from '../../../widgets/grid_layout';
import {Section} from '../../../widgets/section';
import {
  dictToTree,
  dictToTreeNodes,
  Tree,
  TreeNode,
} from '../../../widgets/tree';
import {threadStateRef} from '../../../frontend/widgets/thread_state';
import {getThreadName} from '../../../trace_processor/sql_utils/thread';
import {getProcessName} from '../../../trace_processor/sql_utils/process';
import {sliceRef} from '../../../frontend/widgets/slice';
import {TrackEventDetailsPanel} from '../../details_panel';
import {Trace} from '../../trace';

export const ARG_PREFIX = 'arg_';

function sqlValueToNumber(value?: ColumnType): number | undefined {
  if (typeof value === 'bigint') return Number(value);
  if (typeof value !== 'number') return undefined;
  return value;
}

function sqlValueToUtid(value?: ColumnType): Utid | undefined {
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

export class DebugSliceDetailsPanel implements TrackEventDetailsPanel {
  private data?: {
    name: string;
    ts: time;
    dur: duration;
    args: {[key: string]: ColumnType};
  };
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
  ) {}

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

  private renderSliceInfo(): m.Child {
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
          renderArguments(this.trace, this.slice.args),
        ),
    );
  }

  async load() {
    const queryResult = await this.trace.engine.query(
      `select * from ${this.tableName} where id = ${this.eventId}`,
    );
    const row = queryResult.firstRow({
      ts: LONG,
      dur: LONG,
      name: STR,
    });
    this.data = {
      name: row.name,
      ts: Time.fromRaw(row.ts),
      dur: row.dur,
      args: {},
    };

    for (const key of Object.keys(row)) {
      if (key.startsWith(ARG_PREFIX)) {
        this.data.args[key.substr(ARG_PREFIX.length)] = (
          row as {[key: string]: ColumnType}
        )[key];
      }
    }

    this.threadState = await this.maybeLoadThreadState(
      sqlValueToNumber(this.data.args['id']),
      this.data.ts,
      this.data.dur,
      sqlValueToReadableString(this.data.args['table_name']),
      sqlValueToUtid(this.data.args['utid']),
    );

    this.slice = await this.maybeLoadSlice(
      sqlValueToNumber(this.data.args['id']) ??
        sqlValueToNumber(this.data.args['slice_id']),
      this.data.ts,
      this.data.dur,
      sqlValueToReadableString(this.data.args['table_name']),
      sqlValueToNumber(this.data.args['track_id']),
    );

    this.trace.scheduleRedraw();
  }

  render() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }
    const details = dictToTreeNodes({
      'Name': this.data['name'] as string,
      'Start time': m(Timestamp, {ts: timeFromSql(this.data['ts'])}),
      'Duration': m(DurationWidget, {dur: durationFromSql(this.data['dur'])}),
      'Debug slice id': `${this.tableName}[${this.eventId}]`,
    });
    details.push(this.renderThreadStateInfo());
    details.push(this.renderSliceInfo());

    const args: {[key: string]: m.Child} = {};
    for (const key of Object.keys(this.data.args)) {
      args[key] = sqlValueToReadableString(this.data.args[key]);
    }

    return m(
      DetailsShell,
      {
        title: 'Debug Slice',
      },
      m(
        GridLayout,
        m(Section, {title: 'Details'}, m(Tree, details)),
        m(Section, {title: 'Arguments'}, dictToTree(args)),
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
