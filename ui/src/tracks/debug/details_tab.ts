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

import {GridLayout} from '../..//frontend/widgets/grid_layout';
import {Section} from '../..//frontend/widgets/section';
import {ColumnType, LONG, STR} from '../../common/query_result';
import {TPDuration, tpDurationFromSql, tpTimeFromSql} from '../../common/time';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {globals} from '../../frontend/globals';
import {
  getSlice,
  SliceDetails,
  sliceRef,
} from '../../frontend/sql/slice';
import {
  asSliceSqlId,
  asTPTimestamp,
  TPTimestamp,
  Utid,
} from '../../frontend/sql_types';
import {
  getProcessName,
  getThreadName,
} from '../../frontend/thread_and_process_info';
import {
  getThreadState,
  ThreadState,
  threadStateRef,
} from '../../frontend/thread_state';
import {DetailsShell} from '../../frontend/widgets/details_shell';
import {Duration} from '../../frontend/widgets/duration';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {
  dictToTree,
  dictToTreeNodes,
  Tree,
  TreeNode,
} from '../../frontend/widgets/tree';
import {ARG_PREFIX} from './add_debug_track_menu';

interface DebugSliceDetailsTabConfig {
  sqlTableName: string;
  id: number;
}

function sqlValueToString(val: ColumnType): string {
  if (val instanceof Uint8Array) {
    return `<blob length=${val.length}>`;
  }
  if (val === null) {
    return 'NULL';
  }
  return val.toString();
}

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
    BottomTab<DebugSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.DebugSliceDetailsTab';

  data?: {
    name: string,
    ts: TPTimestamp,
    dur: TPDuration,
    args: {[key: string]: ColumnType};
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
      id: number|undefined, ts: TPTimestamp, dur: TPDuration,
      utid?: Utid): Promise<ThreadState|undefined> {
    if (id === undefined) return undefined;
    if (utid === undefined) return undefined;

    const threadState = await getThreadState(this.engine, id);
    if (threadState === undefined) return undefined;
    if (threadState.ts === ts && threadState.dur === dur &&
        threadState.thread?.utid === utid) {
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
      id: number|undefined, ts: TPTimestamp, dur: TPDuration,
      sqlTrackId?: number): Promise<SliceDetails|undefined> {
    if (id === undefined) return undefined;
    if (sqlTrackId === undefined) return undefined;

    const slice = await getSlice(this.engine, asSliceSqlId(id));
    if (slice === undefined) return undefined;
    if (slice.ts === ts && slice.dur === dur &&
        slice.sqlTrackId === sqlTrackId) {
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
      ts: row.ts as TPTimestamp,
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
        sqlValueToUtid(this.data.args['utid']));

    this.slice = await this.maybeLoadSlice(
        sqlValueToNumber(this.data.args['id']) ??
            sqlValueToNumber(this.data.args['slice_id']),
        this.data.ts,
        this.data.dur,
        sqlValueToNumber(this.data.args['track_id']));

    globals.rafScheduler.scheduleRedraw();
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
      'Start time':
          m(Timestamp, {ts: asTPTimestamp(tpTimeFromSql(this.data['ts']))}),
      'Duration': m(Duration, {dur: tpDurationFromSql(this.data['dur'])}),
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

  renderTabCanvas() {
    return;
  }
}

bottomTabRegistry.register(DebugSliceDetailsTab);
