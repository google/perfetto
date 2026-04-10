// Copyright (C) 2026 The Android Open Source Project
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

import {Icons} from '../../base/semantic_icons';
import {DurationWidget} from '../../components/widgets/duration';
import {Trace} from '../../public/trace';
import {Anchor} from '../../widgets/anchor';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {DetailsShell} from '../../widgets/details_shell';
import {EmptyState} from '../../widgets/empty_state';
import {Grid, GridCell, GridColumn, GridHeaderCell} from '../../widgets/grid';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Spinner} from '../../widgets/spinner';
import {Tree, TreeNode} from '../../widgets/tree';

import {LockContentionDetails} from './android_lock_contention_event_source';

export interface AndroidLockContentionTabAttrs {
  trace: Trace;
  row: LockContentionDetails | null;
  isPending: boolean;
  goToSlice: (id: number) => void;
  goToTrack: (uri: string) => void;
}

export class AndroidLockContentionTab
  implements m.ClassComponent<AndroidLockContentionTabAttrs>
{
  view({attrs}: m.Vnode<AndroidLockContentionTabAttrs>): m.Children {
    const {row, isPending} = attrs;

    if (isPending) {
      return m(
        DetailsShell,
        {title: 'Android Lock Contention'},
        m(
          'div',
          {
            style: {
              display: 'flex',
              justifyContent: 'center',
              padding: '20px',
            },
          },
          m(Spinner, {}),
        ),
      );
    }

    if (!row) {
      return m(
        DetailsShell,
        {title: 'Android Lock Contention'},
        m(EmptyState, {
          title: 'No monitor contention slice selected',
          description: 'Select a monitor contention slice to see details.',
          icon: Icons.Android,
        }),
      );
    }

    return m(
      DetailsShell,
      {
        title: 'Android Lock Contention',
        description: row.lockName,
      },
      this.renderContent(attrs, row),
    );
  }

  private renderContent(
    attrs: AndroidLockContentionTabAttrs,
    row: LockContentionDetails,
  ): m.Children {
    const trace = attrs.trace;
    return [
      row.parentId !== null &&
        m(
          Callout,
          {
            intent: Intent.Warning,
            icon: 'warning',
          },
          m('strong', 'Nested Contention Warning: '),
          'The thread holding this lock is currently blocked by another lock! ',
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () => attrs.goToSlice(row.parentId!),
            },
            'Go to Root Cause',
          ),
        ),

      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Blocked Thread (Victim)'},
            row.binderReplyId !== null &&
              m(
                Callout,
                {
                  intent: Intent.Primary,
                  icon: 'info',
                  style: {marginBottom: '8px'},
                },
                m('strong', 'Binder IPC (Inbound): '),
                'This thread is blocked while handling an incoming Binder transaction. ',
                m(
                  Anchor,
                  {
                    icon: Icons.GoTo,
                    onclick: () => attrs.goToSlice(row.binderReplyId!),
                  },
                  'View Transaction',
                ),
              ),
            m(
              Tree,
              {},
              m(TreeNode, {
                left: 'Thread',
                right: m(
                  'span',
                  `${row.blockedThreadName} [${row.blockedThreadTid ?? '-'}] `,
                  m(Anchor, {
                    icon: Icons.GoTo,
                    onclick: () => attrs.goToSlice(row.id),
                    title: 'Go to blocked thread slice',
                  }),
                ),
              }),
              m(TreeNode, {
                left: 'Main Thread',
                right: row.isBlockedThreadMain ? 'Yes' : 'No',
              }),
              m(TreeNode, {left: 'Method', right: row.blockedMethod}),
              m(TreeNode, {left: 'Location', right: row.blockedSrc}),
            ),
          ),
        ),
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Contention Details'},
            m(
              Tree,
              {},
              m(TreeNode, {left: 'Lock', right: row.lockName}),
              m(TreeNode, {
                left: 'Duration',
                right:
                  row.dur !== null
                    ? m(DurationWidget, {dur: row.dur, trace: trace})
                    : '-',
              }),
              row.waiters.length === 0
                ? m(TreeNode, {
                    left: 'Other Waiters',
                    right: row.waiterCount.toString(),
                  })
                : m(
                    TreeNode,
                    {
                      left: 'Other Waiters',
                      summary: row.waiterCount.toString(),
                    },
                    row.waiters.map((w) =>
                      m(TreeNode, {
                        left: 'Thread',
                        right: m(
                          'span',
                          `${w.threadName} [${w.tid ?? '-'}] `,
                          m(Anchor, {
                            icon: Icons.GoTo,
                            onclick: () => attrs.goToSlice(w.eventId),
                            title: 'Go to contention slice for this waiter',
                          }),
                        ),
                      }),
                    ),
                  ),
              m(TreeNode, {
                left: 'Monotonic Duration',
                right:
                  row.monotonicDur !== null
                    ? m(DurationWidget, {dur: row.monotonicDur, trace})
                    : '-',
              }),
            ),
          ),
        ),
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Blocking Thread (Culprit)'},
            row.blockingBinderTxnId !== null &&
              m(
                Callout,
                {
                  intent: Intent.Primary,
                  icon: 'info',
                  style: {marginBottom: '8px'},
                },
                m('strong', 'Binder IPC (Outbound): '),
                'The blocking thread is currently delayed waiting for an outbound Binder transaction to return. ',
                m(
                  Anchor,
                  {
                    icon: Icons.GoTo,
                    onclick: () => attrs.goToSlice(row.blockingBinderTxnId!),
                  },
                  'View Transaction',
                ),
              ),
            m(
              Tree,
              {},
              m(TreeNode, {
                left: 'Thread',
                right: m(
                  'span',
                  `${row.blockingThreadName} [${row.blockingThreadTid ?? '-'}] `,
                  row.blockingTrackUri &&
                    m(Anchor, {
                      icon: Icons.GoTo,
                      onclick: () => attrs.goToTrack(row.blockingTrackUri!),
                      title: 'Go to blocking thread track',
                    }),
                ),
              }),
              m(TreeNode, {
                left: 'Main Thread',
                right: row.isBlockingThreadMain ? 'Yes' : 'No',
              }),
              m(TreeNode, {left: 'Method', right: row.blockingMethod}),
              m(TreeNode, {left: 'Location', right: row.blockingSrc}),
            ),
          ),
        ),
      ),
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Blocking Thread States'},
            this.renderStatesTable(trace, row),
          ),
        ),
        row.blockedFunctions.length > 0 &&
          m(
            GridLayoutColumn,
            m(
              Section,
              {title: 'Kernel Functions (if blocked)'},
              this.renderFunctionsTable(trace, row),
            ),
          ),
      ),
    ];
  }

  private renderFunctionsTable(
    trace: Trace,
    row: LockContentionDetails,
  ): m.Children {
    const columns: GridColumn[] = [
      {key: 'func', header: m(GridHeaderCell, {}, 'Kernel Function')},
      {key: 'dur', header: m(GridHeaderCell, {}, 'Duration')},
      {key: 'count', header: m(GridHeaderCell, {}, 'Count')},
    ];
    return m(Grid, {
      columns,
      rowData: row.blockedFunctions.map((f) => [
        m(GridCell, {}, f.func),
        m(GridCell, {}, m(DurationWidget, {dur: f.dur, trace})),
        m(GridCell, {}, f.count),
      ]),
    });
  }

  private renderStatesTable(
    trace: Trace,
    row: LockContentionDetails,
  ): m.Children {
    if (row.threadStates.length === 0) {
      return m('div', 'No CPU state data available for blocking thread.');
    }

    const columns: GridColumn[] = [
      {key: 'state', header: m(GridHeaderCell, {}, 'Thread State')},
      {key: 'dur', header: m(GridHeaderCell, {}, 'Duration')},
      {key: 'count', header: m(GridHeaderCell, {}, 'Count')},
    ];
    return m(Grid, {
      columns,
      rowData: row.threadStates.map((s) => [
        m(GridCell, {}, s.state),
        m(GridCell, {}, m(DurationWidget, {dur: s.dur, trace})),
        m(GridCell, {}, s.count),
      ]),
    });
  }
}
