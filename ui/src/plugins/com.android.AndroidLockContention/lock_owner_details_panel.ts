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
import {AsyncLimiter} from '../../base/async_limiter';
import {NUM} from '../../trace_processor/query_result';
import {DurationWidget} from '../../components/widgets/duration';
import type {Trace} from '../../public/trace';
import type {
  ContentWithLoadingFlag,
  TrackEventSelection,
  TrackEventSelectionTab,
} from '../../public/selection';
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

import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Card, CardStack} from '../../widgets/card';
import {Section} from '../../widgets/section';

interface ContentionBreakdownData {
  readonly rows: LockContentionDetails[];
  readonly threadStates: Map<number, ReadonlyArray<ContentionState>>;
  readonly blockedFunctions: Map<
    number,
    ReadonlyArray<ContentionBlockedFunction>
  >;
}

export class LockContentionDetailsTab implements TrackEventSelectionTab {
  readonly id = 'android_lock_contention';
  readonly name = 'Lock Contention';
  readonly priority = 10;

  private currentSelection?: TrackEventSelection;
  private data?: ContentionBreakdownData | null;
  private readonly limiter = new AsyncLimiter();

  constructor(
    private readonly trace: Trace,
    private readonly plugin: AndroidLockContentionPlugin,
  ) {}

  render(selection: TrackEventSelection): ContentWithLoadingFlag | undefined {
    if (this.currentSelection?.eventId !== selection.eventId) {
      this.currentSelection = selection;
      this.data = undefined;
      this.limiter.schedule(async () => {
        this.data = await this.loadData(selection);
      });
    }

    if (!this.data || this.data.rows.length === 0) {
      return undefined;
    }

    return {
      isLoading: false,
      content: m(LockContentionBreakdown, {
        trace: this.trace,
        plugin: this.plugin,
        rows: this.data.rows,
        monitorThreadStates: this.data.threadStates,
        monitorBlockedFunctions: this.data.blockedFunctions,
      }),
    };
  }

  private async loadData(
    selection: TrackEventSelection,
  ): Promise<ContentionBreakdownData | null> {
    const source = new AndroidLockContentionEventSource(this.trace);

    // Check if this is an owner event or a slice matching an owner event
    const query = await this.trace.engine.query(`
      SELECT id FROM __android_lock_contention_owner_events WHERE id = ${selection.eventId}
      UNION ALL
      SELECT oe.id
      FROM android_all_lock_contentions c
      JOIN __android_lock_contention_owner_events oe
        ON oe.owner_tid = c.owner_tid
        AND oe.ts <= c.ts
        AND oe.ts + oe.dur >= c.ts
      WHERE c.id = ${selection.eventId}
      LIMIT 1
    `);

    if (query.numRows() > 0) {
      const ownerEventId = query.firstRow({id: NUM}).id;
      const res = await source.fetchAllDetails(ownerEventId);
      return {
        rows: res.details,
        threadStates: res.threadStates,
        blockedFunctions: res.blockedFunctions,
      };
    }

    // If it is a contention slice without an owner event in this window:
    const singleDetails = await source.fetchDetails(
      selection.eventId,
      selection.trackUri,
    );
    if (singleDetails !== null) {
      const threadStates = new Map<number, ReadonlyArray<ContentionState>>();
      const blockedFunctions = new Map<
        number,
        ReadonlyArray<ContentionBlockedFunction>
      >();
      if (singleDetails.isMonitor) {
        threadStates.set(
          singleDetails.id,
          await source.fetchThreadStates(singleDetails.id),
        );
        blockedFunctions.set(
          singleDetails.id,
          await source.fetchBlockedFunctions(singleDetails.id),
        );
      }
      return {
        rows: [singleDetails],
        threadStates,
        blockedFunctions,
      };
    }

    return null;
  }
}

export interface LockContentionBreakdownAttrs {
  readonly trace: Trace;
  readonly plugin: AndroidLockContentionPlugin;
  readonly rows: LockContentionDetails[];
  readonly monitorThreadStates: Map<number, ReadonlyArray<ContentionState>>;
  readonly monitorBlockedFunctions: Map<
    number,
    ReadonlyArray<ContentionBlockedFunction>
  >;
}

export class LockContentionBreakdown implements m.ClassComponent<LockContentionBreakdownAttrs> {
  view({attrs}: m.Vnode<LockContentionBreakdownAttrs>) {
    const {trace, plugin, rows, monitorThreadStates, monitorBlockedFunctions} =
      attrs;

    const ownerTid = rows.length > 0 ? rows[0].blockingThreadTid : undefined;
    const customTrackUri =
      ownerTid !== undefined && ownerTid !== null
        ? `com.android.AndroidLockContention#OwnerEvents_${ownerTid}`
        : undefined;
    const isCustomPinned = customTrackUri
      ? plugin.pinningManager.isTrackPinned(customTrackUri)
      : false;

    const threadTrackUri = rows.length > 0 ? rows[0].ownerTrackUri : undefined;
    const isThreadPinned = threadTrackUri
      ? plugin.pinningManager.isTrackPinned(threadTrackUri)
      : false;

    const artRows = rows.filter((r) => !r.isMonitor);
    const monitorRows = rows.filter((r) => r.isMonitor);

    return m(
      'div',
      {className: 'pf-lock-owner-panel'},
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
                  plugin.pinningManager.unpinTracks([customTrackUri]);
                } else {
                  plugin.pinningManager.pinTracks([customTrackUri]);
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
                  plugin.pinningManager.unpinTracks([threadTrackUri]);
                } else {
                  plugin.pinningManager.pinTracks([threadTrackUri]);
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
            trace,
            plugin,
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
                trace,
                plugin,
                row,
                threadStates: monitorThreadStates.get(row.id) ?? [],
                blockedFunctions: monitorBlockedFunctions.get(row.id) ?? [],
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

class MonitorContentionCard implements m.ClassComponent<MonitorContentionCardAttrs> {
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
