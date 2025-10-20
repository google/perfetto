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
import {Button, ButtonBar, ButtonVariant} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {Tree, TreeNode} from '../../widgets/tree';
import {Intent} from '../../widgets/common';
import {SchedSqlId} from '../../components/sql_utils/core_types';
import {
  getThreadState,
  getThreadStateFromConstraints,
  ThreadState,
} from '../../components/sql_utils/thread_state';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import {getProcessName} from '../../components/sql_utils/process';
import {
  getFullThreadName,
  getThreadName,
} from '../../components/sql_utils/thread';
import {ThreadStateRef} from '../../components/widgets/thread_state';
import {CRITICAL_PATH_LITE_CMD} from '../../public/exposed_commands';
import {goToSchedSlice} from '../../components/widgets/sched';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {formatDuration} from '../../components/time_utils';
import {Stack} from '../../widgets/stack';

interface RelatedThreadStates {
  prev?: ThreadState;
  next?: ThreadState;
  waker?: ThreadState;
  wakerInterruptCtx?: boolean;
  wakee?: ThreadState[];
}

export class ThreadStateDetailsPanel implements TrackEventDetailsPanel {
  private threadState?: ThreadState;
  private relatedStates?: RelatedThreadStates;

  constructor(
    private readonly trace: Trace,
    private readonly id: number,
  ) {}

  async load() {
    const id = this.id;
    this.threadState = await getThreadState(this.trace.engine, id);

    if (!this.threadState) {
      return;
    }

    const relatedStates: RelatedThreadStates = {};

    const prevRelatedStates = await getThreadStateFromConstraints(
      this.trace.engine,
      {
        filters: [
          `ts + dur = ${this.threadState.ts}`,
          `utid = ${this.threadState.thread?.utid}`,
        ],
        limit: 1,
      },
    );

    if (prevRelatedStates.length > 0) {
      relatedStates.prev = prevRelatedStates[0];
    }

    const nextRelatedStates = await getThreadStateFromConstraints(
      this.trace.engine,
      {
        filters: [
          `ts = ${this.threadState.ts + this.threadState.dur}`,
          `utid = ${this.threadState.thread?.utid}`,
        ],
        limit: 1,
      },
    );

    if (nextRelatedStates.length > 0) {
      relatedStates.next = nextRelatedStates[0];
    }

    // note: this might be valid even if there is no |waker| slice, in the case
    // of an interrupt wakeup while in the idle process (which is omitted from
    // the thread_state table).
    relatedStates.wakerInterruptCtx = this.threadState.wakerInterruptCtx;

    if (this.threadState.wakerId !== undefined) {
      relatedStates.waker = await getThreadState(
        this.trace.engine,
        this.threadState.wakerId,
      );
    } else if (
      this.threadState.state == 'Running' &&
      relatedStates.prev?.wakerId !== undefined
    ) {
      // For running slices, extract waker info from the preceding runnable.
      relatedStates.waker = await getThreadState(
        this.trace.engine,
        relatedStates.prev.wakerId,
      );
      relatedStates.wakerInterruptCtx = relatedStates.prev.wakerInterruptCtx;
    }

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
          this.threadState && this.renderTree(this.threadState),
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
    if (!this.threadState) {
      return 'Loading';
    }
    return this.id;
  }

  private renderTree(threadState: ThreadState) {
    const thread = threadState.thread;
    const process = threadState.thread?.process;
    return m(
      Tree,
      m(TreeNode, {
        left: 'Start time',
        right: m(Timestamp, {trace: this.trace, ts: threadState.ts}),
      }),
      m(TreeNode, {
        left: 'Duration',
        right: m(DurationWidget, {trace: this.trace, dur: threadState.dur}),
      }),
      m(TreeNode, {
        left: 'State',
        right: this.renderState(
          threadState.state,
          threadState.cpu,
          threadState.schedSqlId,
        ),
      }),
      threadState.blockedFunction &&
        m(TreeNode, {
          left: 'Blocked function',
          right: threadState.blockedFunction,
        }),
      process &&
        m(TreeNode, {
          left: 'Process',
          right: getProcessName(process),
        }),
      thread && m(TreeNode, {left: 'Thread', right: getThreadName(thread)}),
      threadState.priority !== undefined &&
        m(TreeNode, {
          left: 'Priority',
          right: threadState.priority,
        }),
      m(TreeNode, {
        left: 'SQL ID',
        right: m(SqlRef, {table: 'thread_state', id: threadState.id}),
      }),
    );
  }

  private renderState(
    state: string,
    cpu: number | undefined,
    id: SchedSqlId | undefined,
  ): m.Children {
    if (!state) {
      return '[Unknown]';
    }
    if (id === undefined || cpu === undefined) {
      return state;
    }
    return m(
      Anchor,
      {
        title: 'Go to CPU slice',
        icon: 'call_made',
        onclick: () => goToSchedSlice(this.trace, id),
      },
      `${state} on CPU ${cpu}`,
    );
  }

  private renderRelatedThreadStates(): m.Children {
    if (this.threadState === undefined || this.relatedStates === undefined) {
      return 'Loading';
    }
    const startTs = this.threadState.ts;
    const renderRef = (state: ThreadState, name?: string) =>
      m(ThreadStateRef, {
        trace: this.trace,
        id: state.id,
        name,
      });

    const nameForNextOrPrev = (threadState: ThreadState) =>
      `${threadState.state} for ${formatDuration(this.trace, threadState.dur)}`;

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
              trace: this.trace,
              ts: state.ts,
              display: `+${formatDuration(this.trace, state.ts - startTs)}`,
            }),
            right: renderRef(state, getFullThreadName(state.thread)),
          }),
        ),
      );
    };

    return m(Stack, [
      m(Tree, [
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
      ]),
      this.trace.commands.hasCommand(CRITICAL_PATH_LITE_CMD) &&
        m(ButtonBar, [
          m(Button, {
            label: 'Critical path lite',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            onclick: () => {
              this.trace.commands.runCommand(
                CRITICAL_PATH_LITE_CMD,
                this.threadState?.thread?.utid,
              );
            },
          }),
        ]),
    ]);
  }

  isLoading() {
    return this.threadState === undefined || this.relatedStates === undefined;
  }
}
