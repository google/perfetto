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
import {Time, time} from '../base/time';
import {raf} from '../core/raf_scheduler';
import {Anchor} from '../widgets/anchor';
import {Button} from '../widgets/button';
import {DetailsShell} from '../widgets/details_shell';
import {GridLayout} from '../widgets/grid_layout';
import {Section} from '../widgets/section';
import {SqlRef} from '../widgets/sql_ref';
import {Tree, TreeNode} from '../widgets/tree';
import {Intent} from '../widgets/common';
import {BottomTab, NewBottomTabArgs} from './bottom_tab';
import {
  SchedSqlId,
  ThreadStateSqlId,
} from '../trace_processor/sql_utils/core_types';
import {
  getThreadState,
  getThreadStateFromConstraints,
  ThreadState,
} from '../trace_processor/sql_utils/thread_state';
import {DurationWidget, renderDuration} from './widgets/duration';
import {Timestamp} from './widgets/timestamp';
import {globals} from './globals';
import {getProcessName} from '../trace_processor/sql_utils/process';
import {
  ThreadInfo,
  getFullThreadName,
  getThreadName,
} from '../trace_processor/sql_utils/thread';
import {ThreadStateRef} from './widgets/thread_state';
import {
  CRITICAL_PATH_CMD,
  CRITICAL_PATH_LITE_CMD,
} from '../public/exposed_commands';
import {goToSchedSlice} from './widgets/sched';

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

  static create(args: NewBottomTabArgs<ThreadStateTabConfig>): ThreadStateTab {
    return new ThreadStateTab(args);
  }

  constructor(args: NewBottomTabArgs<ThreadStateTabConfig>) {
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
    relatedStates.prev = (
      await getThreadStateFromConstraints(this.engine, {
        filters: [
          `ts + dur = ${this.state.ts}`,
          `utid = ${this.state.thread?.utid}`,
        ],
        limit: 1,
      })
    )[0];
    relatedStates.next = (
      await getThreadStateFromConstraints(this.engine, {
        filters: [
          `ts = ${this.state.ts + this.state.dur}`,
          `utid = ${this.state.thread?.utid}`,
        ],
        limit: 1,
      })
    )[0];
    if (this.state.wakerThread?.utid !== undefined) {
      relatedStates.waker = (
        await getThreadStateFromConstraints(this.engine, {
          filters: [
            `utid = ${this.state.wakerThread?.utid}`,
            `ts <= ${this.state.ts}`,
            `ts + dur >= ${this.state.ts}`,
          ],
        })
      )[0];
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
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          this.state && this.renderTree(this.state),
        ),
        m(
          Section,
          {title: 'Related thread states'},
          this.renderRelatedThreadStates(),
        ),
      ),
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
          state.state,
          state.cpu,
          state.schedSqlId,
          state.ts,
        ),
      }),
      state.blockedFunction &&
        m(TreeNode, {
          left: 'Blocked function',
          right: state.blockedFunction,
        }),
      process &&
        m(TreeNode, {
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
    state: string,
    cpu: number | undefined,
    id: SchedSqlId | undefined,
    ts: time,
  ): m.Children {
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
      `${state} on CPU ${cpu}`,
    );
  }

  private renderWakerThread(wakerThread: ThreadInfo) {
    return m(
      TreeNode,
      {left: 'Waker'},
      m(TreeNode, {
        left: 'Process',
        right: getProcessName(wakerThread.process),
      }),
      m(TreeNode, {left: 'Thread', right: getThreadName(wakerThread)}),
    );
  }

  private renderRelatedThreadStates(): m.Children {
    if (this.state === undefined || this.relatedStates === undefined) {
      return 'Loading';
    }
    const startTs = this.state.ts;
    const renderRef = (state: ThreadState, name?: string) =>
      m(ThreadStateRef, {
        id: state.threadStateSqlId,
        ts: state.ts,
        dur: state.dur,
        utid: state.thread!.utid,
        name,
      });

    const nameForNextOrPrev = (state: ThreadState) =>
      `${state.state} for ${renderDuration(state.dur)}`;

    return [
      m(
        Tree,
        this.relatedStates.waker &&
          m(TreeNode, {
            left: 'Waker',
            right: renderRef(
              this.relatedStates.waker,
              getFullThreadName(this.relatedStates.waker.thread),
            ),
          }),
        this.relatedStates.prev &&
          m(TreeNode, {
            left: 'Previous state',
            right: renderRef(
              this.relatedStates.prev,
              nameForNextOrPrev(this.relatedStates.prev),
            ),
          }),
        this.relatedStates.next &&
          m(TreeNode, {
            left: 'Next state',
            right: renderRef(
              this.relatedStates.next,
              nameForNextOrPrev(this.relatedStates.next),
            ),
          }),
        this.relatedStates.wakee &&
          this.relatedStates.wakee.length > 0 &&
          m(
            TreeNode,
            {
              left: 'Woken threads',
            },
            this.relatedStates.wakee.map((state) =>
              m(TreeNode, {
                left: m(Timestamp, {
                  ts: state.ts,
                  display: [
                    'Start+',
                    m(DurationWidget, {dur: Time.sub(state.ts, startTs)}),
                  ],
                }),
                right: renderRef(state, getFullThreadName(state.thread)),
              }),
            ),
          ),
      ),
      globals.commandManager.hasCommand(CRITICAL_PATH_LITE_CMD) &&
        m(Button, {
          label: 'Critical path lite',
          intent: Intent.Primary,
          onclick: () => {
            globals.commandManager.runCommand(
              CRITICAL_PATH_LITE_CMD,
              this.state?.thread?.utid,
            );
          },
        }),
      globals.commandManager.hasCommand(CRITICAL_PATH_CMD) &&
        m(Button, {
          label: 'Critical path',
          intent: Intent.Primary,
          onclick: () => {
            globals.commandManager.runCommand(
              CRITICAL_PATH_CMD,
              this.state?.thread?.utid,
            );
          },
        }),
    ];
  }

  isLoading() {
    return this.state === undefined || this.relatedStates === undefined;
  }
}
