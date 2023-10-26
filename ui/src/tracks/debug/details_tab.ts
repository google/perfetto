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
import {
  ColumnType,
  durationFromSql,
  LONG,
  STR,
  timeFromSql,
} from '../../common/query_result';
import {raf} from '../../core/raf_scheduler';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {
  getSlice,
  SliceDetails,
  sliceRef,
} from '../../frontend/sql/slice';
import {
  asSliceSqlId,
  Utid,
} from '../../frontend/sql_types';
import {sqlValueToString} from '../../frontend/sql_utils';
import {
  getProcessName,
  getThreadName,
} from '../../frontend/thread_and_process_info';
import {
  getThreadState,
  ThreadState,
  threadStateRef,
} from '../../frontend/thread_state';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {DetailsShell} from '../../widgets/details_shell';
import {DurationWidget} from '../../widgets/duration';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {
  dictToTree,
  dictToTreeNodes,
  Tree,
  TreeNode,
} from '../../widgets/tree';

import {ARG_PREFIX} from './add_debug_track_menu';

function sqlValueToNumber(value?: ColumnType): number|undefined {
  if (typeof value === 'bigint') return Number(value);
  if (typeof value !== 'number') return undefined;
  return value;
}

function sqlValueToUtid(value?: ColumnType): Utid|undefined {
  if (typeof value === 'bigint') return Number(value) as Utid;
  if (typeof value !== 'number') return undefined;
  return value as Utid;
}

function renderTreeContents(dict: {[key: string]: m.Child}): m.Child[] {
  const children: m.Child[] = [];
  for (const key of Object.keys(dict)) {
    if (dict[key] === null || dict[key] === undefined) continue;
    children.push(m(TreeNode, {
      left: key,
      right: dict[key],
    }));
  }
  return children;
}

export class DebugSliceDetailsTab extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'dev.perfetto.DebugSliceDetailsTab';

  data?: {
    name: string, ts: time, dur: duration, args: {[key: string]: ColumnType};
  };
  // We will try to interpret the arguments as references into well-known
  // tables. These values will be set if the relevant columns exist and
  // are consistent (e.g. 'ts' and 'dur' for this slice correspond to values
  // in these well-known tables).
  threadState?: ThreadState;
  slice?: SliceDetails;

  static create(args: NewBottomTabArgs): DebugSliceDetailsTab {
    return new DebugSliceDetailsTab(args);
  }

  private async maybeLoadThreadState(
      id: number|undefined, ts: time, dur: duration, table: string|undefined,
      utid?: Utid): Promise<ThreadState|undefined> {
    if (id === undefined) return undefined;
    if (utid === undefined) return undefined;

    const threadState = await getThreadState(this.engine, id);
    if (threadState === undefined) return undefined;
    if ((table === 'thread_state') ||
        (threadState.ts === ts && threadState.dur === dur &&
         threadState.thread?.utid === utid)) {
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
          'Thread': getThreadName(this.threadState.thread),
          'Process': getProcessName(this.threadState.thread?.process),
          'State': this.threadState.state,
        }));
  }

  private async maybeLoadSlice(
      id: number|undefined, ts: time, dur: duration, table: string|undefined,
      trackId?: number): Promise<SliceDetails|undefined> {
    if (id === undefined) return undefined;
    if ((table !== 'slice') && trackId === undefined) return undefined;

    const slice = await getSlice(this.engine, asSliceSqlId(id));
    if (slice === undefined) return undefined;
    if ((table === 'slice') ||
        (slice.ts === ts && slice.dur === dur && slice.trackId === trackId)) {
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
        renderTreeContents({
          'Name': this.slice.name,
          'Thread': getThreadName(this.slice.thread),
          'Process': getProcessName(this.slice.process),
        }));
  }


  private async loadData() {
    const queryResult = await this.engine.query(`select * from ${
        this.config.sqlTableName} where id = ${this.config.id}`);
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
        this.data.args[key.substr(ARG_PREFIX.length)] =
            (row as {[key: string]: ColumnType})[key];
      }
    }

    this.threadState = await this.maybeLoadThreadState(
        sqlValueToNumber(this.data.args['id']),
        this.data.ts,
        this.data.dur,
        sqlValueToString(this.data.args['table_name']),
        sqlValueToUtid(this.data.args['utid']));

    this.slice = await this.maybeLoadSlice(
        sqlValueToNumber(this.data.args['id']) ??
            sqlValueToNumber(this.data.args['slice_id']),
        this.data.ts,
        this.data.dur,
        sqlValueToString(this.data.args['table_name']),
        sqlValueToNumber(this.data.args['track_id']));

    raf.scheduleRedraw();
  }

  constructor(args: NewBottomTabArgs) {
    super(args);
    this.loadData();
  }

  viewTab() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }
    const details = dictToTreeNodes({
      'Name': this.data['name'] as string,
      'Start time': m(Timestamp, {ts: timeFromSql(this.data['ts'])}),
      'Duration': m(DurationWidget, {dur: durationFromSql(this.data['dur'])}),
      'Debug slice id': `${this.config.sqlTableName}[${this.config.id}]`,
    });
    details.push(this.renderThreadStateInfo());
    details.push(this.renderSliceInfo());

    const args: {[key: string]: m.Child} = {};
    for (const key of Object.keys(this.data.args)) {
      args[key] = sqlValueToString(this.data.args[key]);
    }

    return m(
        DetailsShell,
        {
          title: 'Debug Slice',
        },
        m(
            GridLayout,
            m(
                Section,
                {title: 'Details'},
                m(Tree, details),
                ),
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

bottomTabRegistry.register(DebugSliceDetailsTab);
