// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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

import {TPTime} from '../common/time';

import {Anchor} from './anchor';
import {BottomTab, bottomTabRegistry, NewBottomTabArgs} from './bottom_tab';
import {globals} from './globals';
import {asTPTimestamp, SchedSqlId, ThreadStateSqlId} from './sql_types';
import {
  getProcessName,
  getThreadName,
  ThreadInfo,
} from './thread_and_process_info';
import {getThreadState, goToSchedSlice, ThreadState} from './thread_state';
import {DetailsShell} from './widgets/details_shell';
import {Duration} from './widgets/duration';
import {GridLayout} from './widgets/grid_layout';
import {Section} from './widgets/section';
import {SqlRef} from './widgets/sql_ref';
import {Timestamp} from './widgets/timestamp';
import {Tree, TreeNode} from './widgets/tree';

interface ThreadStateTabConfig {
  // Id into |thread_state| sql table.
  readonly id: ThreadStateSqlId;
}

export class ThreadStateTab extends BottomTab<ThreadStateTabConfig> {
  static readonly kind = 'org.perfetto.ThreadStateTab';

  state?: ThreadState;
  loaded: boolean = false;

  static create(args: NewBottomTabArgs): ThreadStateTab {
    return new ThreadStateTab(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);

    getThreadState(this.engine, this.config.id).then((state?: ThreadState) => {
      this.loaded = true;
      this.state = state;
      globals.rafScheduler.scheduleFullRedraw();
    });
  }

  getTitle() {
    // TODO(altimin): Support dynamic titles here.
    return 'Current Selection';
  }

  viewTab() {
    // TODO(altimin/stevegolton): Differentiate between "Current Selection" and
    // "Pinned" views in DetailsShell.
    return m(
        DetailsShell,
        {title: 'Thread State', description: this.renderLoadingText()},
        m(GridLayout,
          m(
              Section,
              {title: 'Details'},
              this.state && this.renderTree(this.state),
              )),
    );
  }

  private renderLoadingText() {
    if (!this.loaded) {
      return 'Loading';
    }
    if (!this.state) {
      return `Thread state ${this.config.id} does not exist`;
    }
    // TODO(stevegolton): Return something intelligent here.
    return this.config.id;
  }

  private renderTree(state: ThreadState) {
    const thread = state.thread;
    const process = state.thread?.process;
    return m(
        Tree,
        m(TreeNode, {
          left: 'Start time',
          right: m(Timestamp, {ts: asTPTimestamp(state.ts)}),
        }),
        m(TreeNode, {
          left: 'Duration',
          right: m(Duration, {dur: state.dur}),
        }),
        m(TreeNode, {
          left: 'State',
          right: this.renderState(
              state.state, state.cpu, state.schedSqlId, state.ts),
        }),
        state.blockedFunction && m(TreeNode, {
          left: 'Blocked function',
          right: state.blockedFunction,
        }),
        process && m(TreeNode, {
          left: 'Process',
          right: getProcessName(process),
        }),
        thread && m(TreeNode, {left: 'Thread', right: getThreadName(thread)}),
        state.wakerThread && this.renderWakerThread(state.wakerThread),
        m(TreeNode, {
          left: 'SQL ID',
          right: m(SqlRef, {table: 'thread_state', id: state.threadStateSqlId}),
        }),
    );
  }

  private renderState(
      state: string, cpu: number|undefined, id: SchedSqlId|undefined,
      ts: TPTime): m.Children {
    if (!state) {
      return null;
    }
    if (id === undefined || cpu === undefined) {
      return state;
    }
    return m(
        Anchor,
        {
          title: 'Go to CPU slice',
          icon: 'call_made',
          onclick: () => goToSchedSlice(cpu, id, ts),
        },
        `${state} on CPU ${cpu}`);
  }

  private renderWakerThread(wakerThread: ThreadInfo) {
    return m(
        TreeNode,
        {left: 'Waker'},
        m(TreeNode,
          {left: 'Process', right: getProcessName(wakerThread.process)}),
        m(TreeNode, {left: 'Thread', right: getThreadName(wakerThread)}),
    );
  }

  isLoading() {
    return this.state === undefined;
  }

  renderTabCanvas(): void {}
}

bottomTabRegistry.register(ThreadStateTab);
