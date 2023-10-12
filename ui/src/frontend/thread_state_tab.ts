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

import {Duration, time} from '../base/time';
import {runQuery} from '../common/queries';
import {raf} from '../core/raf_scheduler';
import {addDebugSliceTrack} from '../tracks/debug/slice_track';
import {Anchor} from '../widgets/anchor';
import {Button} from '../widgets/button';
import {DetailsShell} from '../widgets/details_shell';
import {DurationWidget} from '../widgets/duration';
import {GridLayout} from '../widgets/grid_layout';
import {Section} from '../widgets/section';
import {SqlRef} from '../widgets/sql_ref';
import {Tree, TreeNode} from '../widgets/tree';

import {BottomTab, bottomTabRegistry, NewBottomTabArgs} from './bottom_tab';
import {SchedSqlId, ThreadStateSqlId} from './sql_types';
import {
  getFullThreadName,
  getProcessName,
  getThreadName,
  ThreadInfo,
} from './thread_and_process_info';
import {
  getThreadState,
  getThreadStateFromConstraints,
  goToSchedSlice,
  ThreadState,
  ThreadStateRef,
} from './thread_state';
import {Timestamp} from './widgets/timestamp';

interface ThreadStateTabConfig {
  // Id into |thread_state| sql table.
  readonly id: ThreadStateSqlId;
}

interface RelatedThreadStates {
  prev?: ThreadState;
  next?: ThreadState;
  waker?: ThreadState;
  wakee?: ThreadState[];
}

export class ThreadStateTab extends BottomTab<ThreadStateTabConfig> {
  static readonly kind = 'dev.perfetto.ThreadStateTab';

  state?: ThreadState;
  relatedStates?: RelatedThreadStates;
  loaded: boolean = false;

  static create(args: NewBottomTabArgs): ThreadStateTab {
    return new ThreadStateTab(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);

    this.load().then(() => {
      this.loaded = true;
      raf.scheduleFullRedraw();
    });
  }

  async load() {
    this.state = await getThreadState(this.engine, this.config.id);

    if (!this.state) {
      return;
    }

    const relatedStates: RelatedThreadStates = {};
    relatedStates.prev = (await getThreadStateFromConstraints(this.engine, {
      filters: [
        `ts + dur = ${this.state.ts}`,
        `utid = ${this.state.thread?.utid}`,
      ],
      limit: 1,
    }))[0];
    relatedStates.next = (await getThreadStateFromConstraints(this.engine, {
      filters: [
        `ts = ${this.state.ts + this.state.dur}`,
        `utid = ${this.state.thread?.utid}`,
      ],
      limit: 1,
    }))[0];
    if (this.state.wakerThread?.utid !== undefined) {
      relatedStates.waker = (await getThreadStateFromConstraints(this.engine, {
        filters: [
          `utid = ${this.state.wakerThread?.utid}`,
          `ts <= ${this.state.ts}`,
          `ts + dur >= ${this.state.ts}`,
        ],
      }))[0];
    }
    relatedStates.wakee = await getThreadStateFromConstraints(this.engine, {
      filters: [
        `waker_utid = ${this.state.thread?.utid}`,
        `state = 'R'`,
        `ts >= ${this.state.ts}`,
        `ts <= ${this.state.ts + this.state.dur}`,
      ],
    });

    this.relatedStates = relatedStates;
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
              ),
          m(Section,
            {title: 'Related thread states'},
            this.renderRelatedThreadStates())),
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
          right: m(Timestamp, {ts: state.ts}),
        }),
        m(TreeNode, {
          left: 'Duration',
          right: m(DurationWidget, {dur: state.dur}),
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
      ts: time): m.Children {
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

  private renderRelatedThreadStates(): m.Children {
    if (this.state === undefined || this.relatedStates === undefined) {
      return 'Loading';
    }
    const startTs = this.state.ts;
    const renderRef = (state: ThreadState, name?: string) => m(ThreadStateRef, {
      id: state.threadStateSqlId,
      ts: state.ts,
      dur: state.dur,
      utid: state.thread!.utid,
      name,
    });

    const sliceColumns = {ts: 'ts', dur: 'dur', name: 'name'};
    const sliceColumnNames = [
      'id',
      'utid',
      'ts',
      'dur',
      'name',
      'table_name',
    ];

    const sliceLiteColumns = {ts: 'ts', dur: 'dur', name: 'thread_name'};
    const sliceLiteColumnNames = [
      'id',
      'utid',
      'ts',
      'dur',
      'thread_name',
      'process_name',
      'table_name',
    ];

    const nameForNextOrPrev = (state: ThreadState) =>
        `${state.state} for ${Duration.humanise(state.dur)}`;
    return [m(
        Tree,
        this.relatedStates.waker && m(TreeNode, {
          left: 'Waker',
          right: renderRef(
              this.relatedStates.waker,
              getFullThreadName(this.relatedStates.waker.thread)),
        }),
        this.relatedStates.prev && m(TreeNode, {
          left: 'Previous state',
          right: renderRef(
              this.relatedStates.prev,
              nameForNextOrPrev(this.relatedStates.prev)),
        }),
        this.relatedStates.next && m(TreeNode, {
          left: 'Next state',
          right: renderRef(
              this.relatedStates.next,
              nameForNextOrPrev(this.relatedStates.next)),
        }),
        this.relatedStates.wakee && this.relatedStates.wakee.length > 0 &&
            m(TreeNode,
              {
                left: 'Woken threads',
              },
              this.relatedStates.wakee.map(
                  (state) => m(TreeNode, ({
                                 left: m(Timestamp, {
                                   ts: state.ts,
                                   display: `Start+${
                                       Duration.humanise(state.ts - startTs)}`,
                                 }),
                                 right: renderRef(
                                     state, getFullThreadName(state.thread)),
                  })))),
      ), m(Button,
           {
          label: 'Critical path lite',
          onclick: () => runQuery(`INCLUDE PERFETTO MODULE experimental.thread_executing_span;`, this.engine)
              .then(() => addDebugSliceTrack(
              this.engine,
                  {
                    sqlSource:
                  `
                    SELECT
                      cr.id,
                      cr.utid,
                      cr.ts,
                      cr.dur,
                      thread.name AS thread_name,
                      process.name AS process_name,
                      'thread_state' AS table_name
                    FROM
                      experimental_thread_executing_span_critical_path(
                        ${this.state?.thread?.utid},
                        trace_bounds.start_ts,
                        trace_bounds.end_ts) cr,
                      trace_bounds
                    JOIN thread USING(utid)
                    JOIN process USING(upid)
                  `,
                  columns: sliceLiteColumnNames,
                  },
               `${this.state?.thread?.name}`,
                  sliceLiteColumns,
                  sliceLiteColumnNames)),
      },
      ), m(Button,
           {
          label: 'Critical path',
          onclick: () => runQuery(`INCLUDE PERFETTO MODULE experimental.thread_executing_span;`, this.engine)
              .then(() => addDebugSliceTrack(
              this.engine,
                  {
                    sqlSource:
                  `
                    SELECT cr.id, cr.utid, cr.ts, cr.dur, cr.name, cr.table_name
                      FROM
                        experimental_thread_executing_span_critical_path_stack(
                          ${this.state?.thread?.utid},
                          trace_bounds.start_ts,
                          trace_bounds.end_ts) cr,
                        trace_bounds WHERE name IS NOT NULL
                  `,
                  columns: sliceColumnNames,
                  },
               `${this.state?.thread?.name}`,
                  sliceColumns,
                  sliceColumnNames)),
      },
      )];
  }


  isLoading() {
    return this.state === undefined || this.relatedStates === undefined;
  }
}

bottomTabRegistry.register(ThreadStateTab);
