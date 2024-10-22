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
import {Anchor} from '../../widgets/anchor';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {Tree, TreeNode} from '../../widgets/tree';
import {Intent} from '../../widgets/common';
import {SchedSqlId} from '../../trace_processor/sql_utils/core_types';
import {
  getThreadState,
  getThreadStateFromConstraints,
  ThreadState,
} from '../../trace_processor/sql_utils/thread_state';
import {DurationWidget, renderDuration} from '../../frontend/widgets/duration';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {getProcessName} from '../../trace_processor/sql_utils/process';
import {
  getFullThreadName,
  getThreadName,
} from '../../trace_processor/sql_utils/thread';
import {ThreadStateRef} from '../../frontend/widgets/thread_state';
import {
  CRITICAL_PATH_CMD,
  CRITICAL_PATH_LITE_CMD,
} from '../../public/exposed_commands';
import {goToSchedSlice} from '../../frontend/widgets/sched';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';

interface RelatedThreadStates {
  prev?: ThreadState;
  next?: ThreadState;
  waker?: ThreadState;
  wakerInterruptCtx?: boolean;
  wakee?: ThreadState[];
}

export class ThreadStateDetailsPanel implements TrackEventDetailsPanel {
  private state?: ThreadState;
  private relatedStates?: RelatedThreadStates;

  constructor(
    private readonly trace: Trace,
    private readonly id: number,
  ) {}

  async load() {
    const id = this.id;
    this.state = await getThreadState(this.trace.engine, id);

    if (!this.state) {
      return;
    }

    const relatedStates: RelatedThreadStates = {};
    relatedStates.prev = (
      await getThreadStateFromConstraints(this.trace.engine, {
        filters: [
          `ts + dur = ${this.state.ts}`,
          `utid = ${this.state.thread?.utid}`,
        ],
        limit: 1,
      })
    )[0];
    relatedStates.next = (
      await getThreadStateFromConstraints(this.trace.engine, {
        filters: [
          `ts = ${this.state.ts + this.state.dur}`,
          `utid = ${this.state.thread?.utid}`,
        ],
        limit: 1,
      })
    )[0];
    if (this.state.wakerId !== undefined) {
      relatedStates.waker = await getThreadState(
        this.trace.engine,
        this.state.wakerId,
      );
    }
    // note: this might be valid even if there is no |waker| slice, in the case
    // of an interrupt wakeup while in the idle process (which is omitted from
    // the thread_state table).
    relatedStates.wakerInterruptCtx = this.state.wakerInterruptCtx;

    relatedStates.wakee = await getThreadStateFromConstraints(
      this.trace.engine,
      {
        filters: [
          `waker_id = ${id}`,
          `(irq_context is null or irq_context = 0)`,
        ],
      },
    );
    this.relatedStates = relatedStates;
  }

  render() {
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
    if (!this.state) {
      return 'Loading';
    }
    return this.id;
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
        right: this.renderState(state.state, state.cpu, state.schedSqlId),
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
        onclick: () => goToSchedSlice(id),
      },
      `${state} on CPU ${cpu}`,
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
        name,
      });

    const nameForNextOrPrev = (state: ThreadState) =>
      `${state.state} for ${renderDuration(state.dur)}`;

    const renderWaker = (related: RelatedThreadStates) => {
      // Could be absent if:
      // * this thread state wasn't woken up (e.g. it is a running slice).
      // * the wakeup is from an interrupt during the idle process (which
      //   isn't populated in thread_state).
      // * at the start of the trace, before all per-cpu scheduling is known.
      const hasWakerId = related.waker !== undefined;
      // Interrupt context for the wakeups is absent from older traces.
      const hasInterruptCtx = related.wakerInterruptCtx !== undefined;

      if (!hasWakerId && !hasInterruptCtx) {
        return null;
      }
      if (related.wakerInterruptCtx) {
        return m(TreeNode, {
          left: 'Woken by',
          right: `Interrupt`,
        });
      }
      return (
        related.waker &&
        m(TreeNode, {
          left: hasInterruptCtx ? 'Woken by' : 'Woken by (maybe interrupt)',
          right: renderRef(
            related.waker,
            getFullThreadName(related.waker.thread),
          ),
        })
      );
    };

    const renderWakees = (related: RelatedThreadStates) => {
      if (related.wakee === undefined || related.wakee.length == 0) {
        return null;
      }
      const hasInterruptCtx = related.wakee[0].wakerInterruptCtx !== undefined;
      return m(
        TreeNode,
        {
          left: hasInterruptCtx
            ? 'Woken threads'
            : 'Woken threads (maybe interrupt)',
        },
        related.wakee.map((state) =>
          m(TreeNode, {
            left: m(Timestamp, {
              ts: state.ts,
              display: `+${renderDuration(state.ts - startTs)}`,
            }),
            right: renderRef(state, getFullThreadName(state.thread)),
          }),
        ),
      );
    };

    return [
      m(
        Tree,
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
        renderWaker(this.relatedStates),
        renderWakees(this.relatedStates),
      ),
      this.trace.commands.hasCommand(CRITICAL_PATH_LITE_CMD) &&
        m(Button, {
          label: 'Critical path lite',
          intent: Intent.Primary,
          onclick: () => {
            this.trace.commands.runCommand(
              CRITICAL_PATH_LITE_CMD,
              this.state?.thread?.utid,
            );
          },
        }),
      this.trace.commands.hasCommand(CRITICAL_PATH_CMD) &&
        m(Button, {
          label: 'Critical path',
          intent: Intent.Primary,
          onclick: () => {
            this.trace.commands.runCommand(
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
