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
import {DetailsShell} from '../../widgets/details_shell';
import {DurationWidget} from '../../components/widgets/duration';
import type {Trace} from '../../public/trace';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import {
  AndroidLockContentionEventSource,
  type LockContentionDetails,
  type ContentionState,
  type ContentionBlockedFunction,
} from './android_lock_contention_event_source';
import type AndroidLockContentionPlugin from './index';
import {translateState} from '../../components/sql_utils/thread_state';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {
  Grid,
  GridCell,
  GridHeaderCell,
  type GridColumn,
} from '../../widgets/grid';
import {Checkbox} from '../../widgets/checkbox';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {Tooltip} from '../../widgets/tooltip';

import {Section} from '../../widgets/section';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Card, CardStack} from '../../widgets/card';

export class LockOwnerDetailsPanel implements TrackEventDetailsPanel {
  private mergedDetails?: LockContentionDetails[];
  private monitorThreadStates = new Map<
    number,
    ReadonlyArray<ContentionState>
  >();
  private monitorBlockedFunctions = new Map<
    number,
    ReadonlyArray<ContentionBlockedFunction>
  >();

  constructor(
    private readonly trace: Trace,
    private readonly eventId: number,
    private readonly plugin: AndroidLockContentionPlugin,
  ) {}

  async load() {
    const source = new AndroidLockContentionEventSource(this.trace);
    this.mergedDetails = await source.fetchMergedDetails(this.eventId);

    const selectedRow = this.mergedDetails.find((r) => r.id === this.eventId);
    if (selectedRow) {
      this.plugin.highlightedTargetIds.add(this.eventId);
      this.plugin.currentBlockedSlice = {
        id: selectedRow.id,
        trackUri: selectedRow.trackUri,
      };
    }

    for (const row of this.mergedDetails) {
      if (row.isMonitor) {
        const states = await source.fetchThreadStates(row.id);
        const funcs = await source.fetchBlockedFunctions(row.id);
        this.monitorThreadStates.set(row.id, states);
        this.monitorBlockedFunctions.set(row.id, funcs);
      }
    }
  }

  render() {
    if (this.mergedDetails === undefined) {
      return m(DetailsShell, {title: 'Lock Owner', description: 'Loading...'});
    }

    const rows = this.mergedDetails;
    const ownerTid = rows.length > 0 ? rows[0].blockingThreadTid : undefined;
    const customTrackUri =
      ownerTid !== undefined && ownerTid !== null
        ? `com.android.AndroidLockContention#OwnerEvents_${ownerTid}`
        : undefined;
    const isCustomPinned = customTrackUri
      ? this.plugin.pinningManager.isTrackPinned(customTrackUri)
      : false;

    const threadTrackUri = rows.length > 0 ? rows[0].ownerTrackUri : undefined;
    const isThreadPinned = threadTrackUri
      ? this.plugin.pinningManager.isTrackPinned(threadTrackUri)
      : false;

    const artRows = rows.filter((r) => !r.isMonitor);
    const monitorRows = rows.filter((r) => r.isMonitor);

    const uniqueBlockedThreads = new Set(
      rows.map((r) => r.blockedThreadTid ?? r.blockedThreadName),
    );
    return m(
      DetailsShell,
      {
        title: 'Lock Owner Contention Breakdown',
        description: `Owner is blocking ${uniqueBlockedThreads.size} threads`,
      },
      m(
        'div',
        {className: 'pf-lock-owner-panel__note'},
        'Press [ and ] to navigate between custom track and original slices.',
      ),

      m(
        'div',
        {className: 'pf-lock-owner-panel__toolbar'},
        m(
          'label',
          {className: 'pf-lock-owner-panel__checkbox-label'},
          m(Checkbox, {
            checked: isCustomPinned,
            onchange: () => {
              if (customTrackUri) {
                if (isCustomPinned) {
                  this.plugin.pinningManager.unpinTracks([customTrackUri]);
                } else {
                  this.plugin.pinningManager.pinTracks([customTrackUri]);
                }
              }
            },
          }),
          'Pin Lock Owner Track',
        ),
        m(
          'label',
          {className: 'pf-lock-owner-panel__checkbox-label'},
          m(Checkbox, {
            checked: isThreadPinned,
            onchange: () => {
              if (threadTrackUri) {
                if (isThreadPinned) {
                  this.plugin.pinningManager.unpinTracks([threadTrackUri]);
                } else {
                  this.plugin.pinningManager.pinTracks([threadTrackUri]);
                }
              }
            },
            disabled: !threadTrackUri,
          }),
          'Pin Thread Track',
        ),
      ),

      artRows.length > 0 &&
        m(
          'div',
          {className: 'pf-lock-owner-panel__section'},
          m('h3', 'ART Lock Contentions'),
          m(ArtContentionsGrid, {
            trace: this.trace,
            plugin: this.plugin,
            rows: artRows,
          }),
        ),

      monitorRows.length > 0 &&
        m(
          'div',
          {className: 'pf-lock-owner-panel__section'},
          m(
            'h3',
            {className: 'pf-lock-owner-panel__title'},
            'Monitor Contentions',
          ),
          m(
            CardStack,
            {className: 'pf-lock-owner-panel__card-stack'},
            monitorRows.map((row) =>
              m(MonitorContentionCard, {
                trace: this.trace,
                plugin: this.plugin,
                row,
                threadStates: this.monitorThreadStates.get(row.id) ?? [],
                blockedFunctions:
                  this.monitorBlockedFunctions.get(row.id) ?? [],
              }),
            ),
          ),
        ),
    );
  }
}

function renderLockName(lockName: string): m.Children {
  return lockName && lockName !== 'Unknown Lock'
    ? lockName
    : m(
        Tooltip,
        {
          trigger: m(
            'span',
            {
              className: 'pf-lock-owner-panel__unknown-text',
            },
            'Unknown',
          ),
        },
        "To see lock names, emit '<name>_lock_acquire' and '<name>_lock_held' trace events around the lock contention.",
      );
}

interface ArtContentionsGridAttrs {
  readonly trace: Trace;
  readonly plugin: AndroidLockContentionPlugin;
  readonly rows: LockContentionDetails[];
}

class ArtContentionsGrid implements m.ClassComponent<ArtContentionsGridAttrs> {
  view({attrs}: m.Vnode<ArtContentionsGridAttrs>) {
    const {trace, plugin, rows} = attrs;
    const columns: GridColumn[] = [
      {key: 'arrow', header: m(GridHeaderCell, {}, 'Show Flow')},
      {key: 'pin', header: m(GridHeaderCell, {}, 'Pin Track')},
      {key: 'thread', header: m(GridHeaderCell, {}, 'Blocked Thread (Victim)')},
      {key: 'lock', header: m(GridHeaderCell, {}, 'Lock Object')},
      {key: 'dur', header: m(GridHeaderCell, {}, 'Duration')},
      {key: 'nav', header: m(GridHeaderCell, {}, 'Go to Event')},
    ];

    return m(Grid, {
      columns,
      rowData: rows.map((row) => {
        const isPinned = row.trackUri
          ? plugin.pinningManager.isTrackPinned(row.trackUri)
          : false;
        const isSelected = plugin.highlightedTargetIds.has(row.id);
        const className = isSelected
          ? 'pf-lock-owner-panel__selected-row'
          : undefined;
        const dur = row.dur;

        return [
          m(
            GridCell,
            {className},
            m(Checkbox, {
              checked: isSelected,
              onchange: () => {
                if (isSelected) {
                  plugin.highlightedTargetIds.delete(row.id);
                } else {
                  plugin.highlightedTargetIds.add(row.id);
                  plugin.currentBlockedSlice = {
                    id: row.id,
                    trackUri: row.trackUri,
                  };
                }
              },
            }),
          ),
          m(
            GridCell,
            {className},
            m(Checkbox, {
              checked: isPinned,
              onchange: () => {
                if (row.trackUri) {
                  if (isPinned) {
                    plugin.pinningManager.unpinTracks([row.trackUri]);
                  } else {
                    plugin.pinningManager.pinTracks([row.trackUri]);
                  }
                }
              },
            }),
          ),
          m(
            GridCell,
            {className},
            `${row.blockedThreadName} [${row.blockedThreadTid ?? '-'}]`,
          ),
          m(GridCell, {className}, renderLockName(row.lockName)),
          m(
            GridCell,
            {className},
            dur !== undefined ? m(DurationWidget, {dur, trace: trace}) : '-',
          ),
          m(
            GridCell,
            {className},
            row.trackUri
              ? m(Anchor, {
                  icon: Icons.GoTo,
                  onclick: () => {
                    plugin.selectAndNavigate(trace, row.id, row.trackUri);
                  },
                  title: 'Go to event slice',
                })
              : '-',
          ),
        ];
      }),
    });
  }
}

interface MonitorContentionCardAttrs {
  readonly trace: Trace;
  readonly plugin: AndroidLockContentionPlugin;
  readonly row: LockContentionDetails;
  readonly threadStates: ReadonlyArray<ContentionState>;
  readonly blockedFunctions: ReadonlyArray<ContentionBlockedFunction>;
}

class MonitorContentionCard
  implements m.ClassComponent<MonitorContentionCardAttrs>
{
  view({attrs}: m.Vnode<MonitorContentionCardAttrs>) {
    const {trace, plugin, row, threadStates, blockedFunctions} = attrs;
    const dur = row.dur;
    const isSelected = plugin.highlightedTargetIds.has(row.id);
    const isPinned = row.trackUri
      ? plugin.pinningManager.isTrackPinned(row.trackUri)
      : false;

    return m(
      Card,
      {
        style: 'border: 1px solid #007acc;',
      },
      m(
        'div',
        {
          className: 'pf-lock-owner-panel__card-header',
        },
        m(
          'div',
          {className: 'pf-lock-owner-panel__card-header-left'},
          m(
            'label',
            {className: 'pf-lock-owner-panel__checkbox-label'},
            m(Checkbox, {
              checked: isSelected,
              onchange: () => {
                if (isSelected) {
                  plugin.highlightedTargetIds.delete(row.id);
                } else {
                  plugin.highlightedTargetIds.add(row.id);
                }
              },
            }),
            'Show Flow',
          ),
          m(
            'label',
            {className: 'pf-lock-owner-panel__checkbox-label'},
            m(Checkbox, {
              checked: isPinned,
              onchange: () => {
                if (row.trackUri) {
                  if (isPinned) {
                    plugin.pinningManager.unpinTracks([row.trackUri]);
                  } else {
                    plugin.pinningManager.pinTracks([row.trackUri]);
                  }
                }
              },
              disabled: !row.trackUri,
            }),
            'Pin Track',
          ),
          m(
            'span',
            m('strong', 'Thread: '),
            `${row.blockedThreadName} [${row.blockedThreadTid ?? '-'}]`,
          ),
          m('span', '|'),
          m(
            'span',
            m('strong', 'Dur: '),
            dur !== undefined ? m(DurationWidget, {dur, trace: trace}) : '-',
          ),
          m('span', '|'),
          m('span', m('strong', 'Lock Name: '), renderLockName(row.lockName)),
        ),
      ),
      m(
        'div',
        {
          className: 'pf-lock-owner-panel__card-content',
        },
        this.renderMonitorDetails(
          row,
          threadStates,
          blockedFunctions,
          trace,
          plugin,
        ),
      ),
    );
  }

  private renderMonitorDetails(
    row: LockContentionDetails,
    threadStates: ReadonlyArray<ContentionState>,
    blockedFunctions: ReadonlyArray<ContentionBlockedFunction>,
    trace: Trace,
    plugin: AndroidLockContentionPlugin,
  ): m.Children {
    return [
      row.parentId !== undefined &&
        m(
          Callout,
          {
            intent: Intent.Warning,
            icon: 'warning',
            className: 'pf-lock-owner-panel__callout',
          },
          m('strong', 'Nested Contention Warning: '),
          'The thread holding this lock is currently blocked by another lock! ',
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () =>
                plugin.selectAndNavigate(trace, row.parentId!, undefined, true),
            },
            'Go to Root Cause',
          ),
        ),
      m(
        GridLayout,
        {},
        m(GridLayoutColumn, {}, this.renderVictimColumn(row, trace, plugin)),
        m(GridLayoutColumn, {}, this.renderContentionColumn(row, trace)),
        m(GridLayoutColumn, {}, this.renderCulpritColumn(row, trace, plugin)),
      ),
      this.renderSummaryTables(threadStates, blockedFunctions, trace),
    ];
  }

  private renderVictimColumn(
    row: LockContentionDetails,
    trace: Trace,
    plugin: AndroidLockContentionPlugin,
  ): m.Children {
    return m(
      Section,
      {title: 'Blocked Thread (Victim)'},
      row.binderReplyId !== undefined &&
        m(
          Callout,
          {
            intent: Intent.Primary,
            icon: 'info',
            className: 'pf-lock-owner-panel__callout',
          },
          m('strong', 'Binder IPC (Inbound): '),
          'This thread is blocked while handling an incoming Binder transaction. ',
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () =>
                plugin.selectAndNavigate(
                  trace,
                  row.binderReplyId!,
                  undefined,
                  true,
                ),
            },
            'View Transaction',
          ),
        ),

      m(
        'div',
        {className: 'pf-lock-owner-panel__details-col'},
        m(
          'div',
          m('strong', 'Thread: '),
          `${row.blockedThreadName} [${row.blockedThreadTid ?? '-'}] `,
          row.trackUri &&
            m(Anchor, {
              icon: Icons.GoTo,
              onclick: () =>
                plugin.selectAndNavigate(trace, row.id, row.trackUri),
              title: 'Go to blocked thread slice',
            }),
        ),
        m(
          'div',
          m('strong', 'Main Thread: '),
          row.isBlockedThreadMain ? 'Yes' : 'No',
        ),
        row.blockedMethod &&
          m(
            'div',
            m('strong', 'Method: '),
            m(
              'span',
              {className: 'pf-lock-owner-panel__monospace'},
              row.blockedMethod,
            ),
          ),
        row.blockedSrc &&
          m(
            'div',
            m('strong', 'Source: '),
            m(
              'span',
              {className: 'pf-lock-owner-panel__monospace'},
              row.blockedSrc,
            ),
          ),
      ),
    );
  }

  private renderContentionColumn(
    row: LockContentionDetails,
    trace: Trace,
  ): m.Children {
    return m(
      Section,
      {title: 'Contention Details'},
      m(
        'div',
        {className: 'pf-lock-owner-panel__details-col'},
        m('div', m('strong', 'Lock Name: '), renderLockName(row.lockName)),
        m(
          'div',
          m('strong', 'Duration: '),
          row.dur !== undefined
            ? m(DurationWidget, {dur: row.dur, trace: trace})
            : '-',
        ),
        m('div', m('strong', 'Other Waiters: '), row.waiterCount.toString()),
      ),
    );
  }

  private renderCulpritColumn(
    row: LockContentionDetails,
    trace: Trace,
    plugin: AndroidLockContentionPlugin,
  ): m.Children {
    return m(
      Section,
      {title: 'Blocking Thread (Culprit)'},
      row.blockingBinderTxnId !== undefined &&
        m(
          Callout,
          {
            intent: Intent.Primary,
            icon: 'info',
            className: 'pf-lock-owner-panel__callout',
          },
          m('strong', 'Binder IPC (Outbound): '),
          'The blocking thread is currently delayed waiting for an outbound Binder transaction to return. ',
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () =>
                plugin.selectAndNavigate(
                  trace,
                  row.blockingBinderTxnId!,
                  undefined,
                  true,
                ),
            },
            'View Transaction',
          ),
        ),

      m(
        'div',
        {className: 'pf-lock-owner-panel__details-col'},
        m(
          'div',
          m('strong', 'Thread: '),
          `${row.blockingThreadName} [${row.blockingThreadTid ?? '-'}] `,
          row.blockingTrackUri &&
            m(Anchor, {
              icon: Icons.GoTo,
              onclick: () =>
                plugin.pinningManager.pinTracks([row.blockingTrackUri!]),
              title: 'Pin blocking thread track',
            }),
        ),
        m(
          'div',
          m('strong', 'Main Thread: '),
          row.isBlockingThreadMain ? 'Yes' : 'No',
        ),
        row.blockingMethod &&
          m(
            'div',
            m('strong', 'Method: '),
            m(
              'span',
              {className: 'pf-lock-owner-panel__monospace'},
              row.blockingMethod,
            ),
          ),
        row.blockingSrc &&
          m(
            'div',
            m('strong', 'Source: '),
            m(
              'span',
              {className: 'pf-lock-owner-panel__monospace'},
              row.blockingSrc,
            ),
          ),
      ),
    );
  }

  private renderSummaryTables(
    threadStates: ReadonlyArray<ContentionState>,
    blockedFunctions: ReadonlyArray<ContentionBlockedFunction>,
    trace: Trace,
  ): m.Children {
    return m(
      'div',
      {className: 'pf-lock-owner-panel__margin-top'},
      m(
        GridLayout,
        {},
        m(
          GridLayoutColumn,
          {},
          m(
            Section,
            {title: 'Blocking Thread States'},
            this.renderStatesTable(threadStates, trace),
          ),
        ),
        blockedFunctions.length > 0 &&
          m(
            GridLayoutColumn,
            {},
            m(
              Section,
              {title: 'Kernel Functions (if blocked)'},
              this.renderFunctionsTable(blockedFunctions, trace),
            ),
          ),
      ),
    );
  }

  private renderStatesTable(
    states: ReadonlyArray<ContentionState>,
    trace: Trace,
  ): m.Children {
    if (states.length === 0) {
      return m('div', 'No CPU state data available for blocking thread.');
    }

    const columns: GridColumn[] = [
      {key: 'state', header: m(GridHeaderCell, {}, 'Blocking Thread State')},
      {key: 'dur', header: m(GridHeaderCell, {}, 'Total Duration')},
      {key: 'count', header: m(GridHeaderCell, {}, 'Occurrences')},
    ];
    return m(Grid, {
      columns,
      rowData: states.map((s) => [
        m(GridCell, {}, translateState(s.state)),
        m(GridCell, {}, m(DurationWidget, {dur: s.dur, trace: trace})),
        m(GridCell, {}, s.count),
      ]),
    });
  }

  private renderFunctionsTable(
    functions: ReadonlyArray<ContentionBlockedFunction>,
    trace: Trace,
  ): m.Children {
    const columns: GridColumn[] = [
      {key: 'func', header: m(GridHeaderCell, {}, 'Blocked Kernel Function')},
      {key: 'dur', header: m(GridHeaderCell, {}, 'Total Duration')},
      {key: 'count', header: m(GridHeaderCell, {}, 'Occurrences')},
    ];
    return m(Grid, {
      columns,
      rowData: functions.map((f) => [
        m(GridCell, {}, f.func),
        m(GridCell, {}, m(DurationWidget, {dur: f.dur, trace: trace})),
        m(GridCell, {}, f.count),
      ]),
    });
  }
}
