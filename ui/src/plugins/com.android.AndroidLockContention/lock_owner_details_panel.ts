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
import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {
  AndroidLockContentionEventSource,
  LockContentionDetails,
} from './android_lock_contention_event_source';
import AndroidLockContentionPlugin from './index';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {Grid, GridCell, GridHeaderCell, GridColumn} from '../../widgets/grid';
import {Checkbox} from '../../widgets/checkbox';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';

import {Section} from '../../widgets/section';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Card, CardStack} from '../../widgets/card';

const DETAILS_SQL = `
INCLUDE PERFETTO MODULE intervals.intersect;

-- Extract all unique utids that act as lock owners
CREATE OR REPLACE PERFETTO TABLE _all_lock_blocking_utids AS
SELECT DISTINCT owner_utid AS utid
FROM android_all_lock_contentions
WHERE owner_utid IS NOT NULL;
-- Filter thread_state to only include these utids
CREATE OR REPLACE PERFETTO VIEW _all_lock_blocking_thread_state AS
SELECT
  id,
  utid AS owner_utid,
  ts,
  dur,
  state,
  blocked_function
FROM thread_state
WHERE utid IN (SELECT utid FROM _all_lock_blocking_utids)
  AND dur >= 0;
-- Filter contentions to only include valid durations for interval intersect
CREATE OR REPLACE PERFETTO VIEW _android_all_lock_contentions_valid_dur AS
SELECT * FROM android_all_lock_contentions
WHERE dur >= 0;
-- Use interval intersect to get thread states during all lock contentions
CREATE OR REPLACE PERFETTO VIEW _android_all_lock_contention_thread_state_intersect AS
SELECT * FROM _interval_intersect_with_col_names!(
  _android_all_lock_contentions_valid_dur, id, ts, dur,
  _all_lock_blocking_thread_state, id, ts, dur,
  (owner_utid)
);
-- Unified view of thread states for all lock contentions
CREATE OR REPLACE PERFETTO VIEW android_all_lock_contention_thread_state AS
SELECT
  ii.id_0 AS id,
  ii.ts,
  ii.dur,
  ii.owner_utid,
  bts.blocked_function,
  bts.state
FROM _android_all_lock_contention_thread_state_intersect ii
JOIN _all_lock_blocking_thread_state bts ON ii.id_1 = bts.id;
-- Aggregated thread_states for all lock contentions
CREATE OR REPLACE PERFETTO VIEW android_all_lock_contention_thread_state_by_txn AS
SELECT
  id,
  state AS thread_state,
  SUM(dur) AS thread_state_dur,
  COUNT(1) AS thread_state_count
FROM android_all_lock_contention_thread_state
GROUP BY id, state;
-- Aggregated blocked_functions for all lock contentions
CREATE OR REPLACE PERFETTO VIEW android_all_lock_contention_blocked_functions_by_txn AS
SELECT
  id,
  blocked_function,
  SUM(dur) AS blocked_function_dur,
  COUNT(1) AS blocked_function_count
FROM android_all_lock_contention_thread_state
WHERE blocked_function IS NOT NULL
GROUP BY id, blocked_function;
`;

export class LockOwnerDetailsPanel implements TrackEventDetailsPanel {
  private mergedDetails?: LockContentionDetails[];
  private selectedMonitorEventId?: number;

  constructor(
    private readonly trace: Trace,
    private readonly eventId: number,
    private readonly plugin: AndroidLockContentionPlugin,
  ) {}

  async load() {
    await this.trace.engine.query(DETAILS_SQL);
    const source = new AndroidLockContentionEventSource(this.trace);
    this.mergedDetails = await source.fetchMergedDetails(this.eventId);
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

    return m(
      DetailsShell,
      {
        title: 'Lock Owner Contention Breakdown',
        description: `Owner is blocking ${rows.length} threads`,
      },
      m(
        'div',
        {style: 'padding: 10px; color: #666; font-style: italic;'},
        'Press [ and ] to navigate between custom track and original slices.',
      ),

      m(
        'div',
        {
          style:
            'padding: 10px; display: flex; gap: 20px; background-color: rgba(0,0,0,0.05); border-bottom: 1px solid #ddd;',
        },
        m(
          'label',
          {style: 'display: flex; align-items: center; gap: 5px;'},
          m(Checkbox, {
            checked: isCustomPinned,
            onchange: () => {
              if (customTrackUri) {
                if (isCustomPinned) {
                  this.plugin.pinningManager.unpinTracks([customTrackUri]);
                } else {
                  this.plugin.pinningManager.pinTracks([customTrackUri]);
                }
                this.plugin.pinningManager.applyPinning(this.trace);
              }
            },
          }),
          'Pin Lock Owner Track',
        ),
        m(
          'label',
          {style: 'display: flex; align-items: center; gap: 5px;'},
          m(Checkbox, {
            checked: isThreadPinned,
            onchange: () => {
              if (threadTrackUri) {
                if (isThreadPinned) {
                  this.plugin.pinningManager.unpinTracks([threadTrackUri]);
                } else {
                  this.plugin.pinningManager.pinTracks([threadTrackUri]);
                }
                this.plugin.pinningManager.applyPinning(this.trace);
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
          {style: 'padding: 10px;'},
          m('h3', 'ART Lock Contentions'),
          this.renderArtContentions(artRows),
        ),

      monitorRows.length > 0 &&
        m(
          'div',
          {style: 'padding: 10px;'},
          m('h3', 'Monitor Contentions'),
          this.renderMonitorContentions(monitorRows),
        ),
    );
  }

  private renderArtContentions(rows: LockContentionDetails[]): m.Children {
    const columns: GridColumn[] = [
      {key: 'arrow', widthPx: 40, header: m(GridHeaderCell, {}, 'Show')},
      {key: 'pin', widthPx: 40, header: m(GridHeaderCell, {}, 'Pin')},
      {key: 'thread', header: m(GridHeaderCell, {}, 'Blocked Thread')},
      {key: 'lock', header: m(GridHeaderCell, {}, 'Lock')},
      {key: 'dur', header: m(GridHeaderCell, {}, 'Duration')},
      {key: 'nav', widthPx: 40, header: m(GridHeaderCell, {}, '')},
    ];

    return m(Grid, {
      columns,
      rowData: rows.map((row) => {
        const isPinned = row.trackUri
          ? this.plugin.pinningManager.isTrackPinned(row.trackUri)
          : false;
        const isSelected = this.plugin.highlightedTargetIds.has(row.id);
        const style = isSelected
          ? 'background-color: rgba(0, 0, 255, 0.1);'
          : '';

        return [
          m(
            GridCell,
            {style},
            m(Checkbox, {
              checked: isSelected,
              onchange: () => {
                if (isSelected) {
                  this.plugin.highlightedTargetIds.delete(row.id);
                } else {
                  this.plugin.highlightedTargetIds.add(row.id);
                }
                this.trace.raf.scheduleCanvasRedraw();
              },
            }),
          ),
          m(
            GridCell,
            {style},
            m(Checkbox, {
              checked: isPinned,
              onchange: () => {
                if (row.trackUri) {
                  if (isPinned) {
                    this.plugin.pinningManager.unpinTracks([row.trackUri]);
                  } else {
                    this.plugin.pinningManager.pinTracks([row.trackUri]);
                  }
                  this.plugin.pinningManager.applyPinning(this.trace);
                }
              },
            }),
          ),
          m(
            GridCell,
            {style},
            `${row.blockedThreadName} [${row.blockedThreadTid ?? '-'}]`,
          ),
          m(GridCell, {style}, row.lockName ?? '-'),
          m(
            GridCell,
            {style},
            row.dur !== null
              ? m(DurationWidget, {dur: row.dur, trace: this.trace})
              : '-',
          ),
          m(
            GridCell,
            {style},
            row.trackUri
              ? m(Anchor, {
                  icon: Icons.GoTo,
                  onclick: () => {
                    const selection = this.trace.selection.selection;
                    if (selection !== undefined) {
                      this.plugin.navigation.push(selection);
                    }
                    this.trace.selection.selectTrackEvent(
                      row.trackUri!,
                      row.id,
                      {
                        scrollToSelection: true,
                        switchToCurrentSelectionTab: false,
                      },
                    );
                  },
                  title: 'Go to event slice',
                })
              : '-',
          ),
        ];
      }),
    });
  }

  private renderMonitorContentions(rows: LockContentionDetails[]): m.Children {
    return m(
      CardStack,
      {style: 'gap: 5px;'},
      rows.map((row) => {
        const isSelected = this.selectedMonitorEventId === row.id;
        return m(
          Card,
          {
            style: isSelected ? 'border: 1px solid #007acc;' : '',
          },
          m(
            'div',
            {
              style:
                'display: flex; justify-content: space-between; align-items: center; cursor: pointer; padding: 8px;',
              onclick: async () => {
                if (isSelected) {
                  this.selectedMonitorEventId = undefined;
                  this.plugin.highlightedTargetIds.delete(row.id);
                } else {
                  this.selectedMonitorEventId = row.id;
                  this.plugin.highlightedTargetIds.add(row.id);
                  if (
                    row.threadStates.length === 0 &&
                    row.blockedFunctions.length === 0
                  ) {
                    const source = new AndroidLockContentionEventSource(
                      this.trace,
                    );
                    row.threadStates = await source.fetchThreadStates(row.id);
                    row.blockedFunctions = await source.fetchBlockedFunctions(
                      row.id,
                    );
                    m.redraw();
                  }
                }
                this.trace.raf.scheduleCanvasRedraw();
              },
            },
            m(
              'div',
              {style: 'display: flex; gap: 10px; align-items: center;'},
              m(
                'span',
                {style: 'font-weight: bold;'},
                `${row.blockedThreadName} [${row.blockedThreadTid ?? '-'}]`,
              ),
              m('span', '|'),
              row.dur !== null
                ? m(DurationWidget, {dur: row.dur, trace: this.trace})
                : '-',
              m('span', '|'),
              m('span', {style: 'color: #666;'}, row.lockName),
            ),
            m(
              'span',
              {style: 'color: #007acc; font-weight: bold;'},
              isSelected ? 'Collapse ▲' : 'Expand Details ▼',
            ),
          ),
          isSelected &&
            m(
              'div',
              {
                style:
                  'margin-top: 10px; border-top: 1px solid #eee; padding: 8px;',
              },
              this.renderMonitorDetails(row),
            ),
        );
      }),
    );
  }

  private renderMonitorDetails(row: LockContentionDetails): m.Children {
    return [
      row.parentId !== null &&
        m(
          Callout,
          {
            intent: Intent.Warning,
            icon: 'warning',
            style: {marginBottom: '8px'},
          },
          m('strong', 'Nested Contention Warning: '),
          'The thread holding this lock is currently blocked by another lock! ',
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () => {
                const selection = this.trace.selection.selection;
                if (selection !== undefined) {
                  this.plugin.navigation.push(selection);
                }
                this.trace.selection.selectSqlEvent('slice', row.parentId!, {
                  scrollToSelection: true,
                  switchToCurrentSelectionTab: false,
                });
              },
            },
            'Go to Root Cause',
          ),
        ),
      m(
        GridLayout,
        {},
        m(
          GridLayoutColumn,
          {},
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
                    onclick: () => {
                      const selection = this.trace.selection.selection;
                      if (selection !== undefined) {
                        this.plugin.navigation.push(selection);
                      }
                      this.trace.selection.selectSqlEvent(
                        'slice',
                        row.binderReplyId!,
                        {
                          scrollToSelection: true,
                          switchToCurrentSelectionTab: false,
                        },
                      );
                    },
                  },
                  'View Transaction',
                ),
              ),

            m(
              'div',
              {style: 'display: flex; flex-direction: column; gap: 5px;'},
              m(
                'div',
                m('strong', 'Thread: '),
                `${row.blockedThreadName} [${row.blockedThreadTid ?? '-'}] `,
                row.trackUri &&
                  m(Anchor, {
                    icon: Icons.GoTo,
                    onclick: () => {
                      const selection = this.trace.selection.selection;
                      if (selection !== undefined) {
                        this.plugin.navigation.push(selection);
                      }
                      this.trace.selection.selectTrackEvent(
                        row.trackUri!,
                        row.id,
                        {scrollToSelection: true},
                      );
                    },
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
                    {style: 'font-family: monospace;'},
                    row.blockedMethod,
                  ),
                ),
            ),
          ),
        ),
        m(
          GridLayoutColumn,
          {},
          m(
            Section,
            {title: 'Contention Details'},
            m(
              'div',
              {style: 'display: flex; flex-direction: column; gap: 5px;'},
              m('div', m('strong', 'Lock: '), row.lockName),
              m(
                'div',
                m('strong', 'Duration: '),
                row.dur !== null
                  ? m(DurationWidget, {dur: row.dur, trace: this.trace})
                  : '-',
              ),
              m(
                'div',
                m('strong', 'Other Waiters: '),
                row.waiterCount.toString(),
              ),
            ),
          ),
        ),
        m(
          GridLayoutColumn,
          {},
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
                    onclick: () => {
                      const selection = this.trace.selection.selection;
                      if (selection !== undefined) {
                        this.plugin.navigation.push(selection);
                      }
                      this.trace.selection.selectSqlEvent(
                        'slice',
                        row.blockingBinderTxnId!,
                        {
                          scrollToSelection: true,
                          switchToCurrentSelectionTab: false,
                        },
                      );
                    },
                  },
                  'View Transaction',
                ),
              ),

            m(
              'div',
              {style: 'display: flex; flex-direction: column; gap: 5px;'},
              m(
                'div',
                m('strong', 'Thread: '),
                `${row.blockingThreadName} [${row.blockingThreadTid ?? '-'}] `,
                row.blockingTrackUri &&
                  m(Anchor, {
                    icon: Icons.GoTo,
                    onclick: () =>
                      this.plugin.pinningManager.pinTracks([
                        row.blockingTrackUri!,
                      ]),
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
                    {style: 'font-family: monospace;'},
                    row.blockingMethod,
                  ),
                ),
            ),
          ),
        ),
      ),
      m(
        'div',
        {style: 'margin-top: 10px;'},
        m(
          GridLayout,
          {},
          m(
            GridLayoutColumn,
            {},
            m(
              Section,
              {title: 'Blocking Thread States'},
              this.renderStatesTable(row),
            ),
          ),
          row.blockedFunctions.length > 0 &&
            m(
              GridLayoutColumn,
              {},
              m(
                Section,
                {title: 'Kernel Functions (if blocked)'},
                this.renderFunctionsTable(row),
              ),
            ),
        ),
      ),
    ];
  }

  private renderStatesTable(row: LockContentionDetails): m.Children {
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
        m(GridCell, {}, m(DurationWidget, {dur: s.dur, trace: this.trace})),
        m(GridCell, {}, s.count),
      ]),
    });
  }

  private renderFunctionsTable(row: LockContentionDetails): m.Children {
    const columns: GridColumn[] = [
      {key: 'func', header: m(GridHeaderCell, {}, 'Kernel Function')},
      {key: 'dur', header: m(GridHeaderCell, {}, 'Duration')},
      {key: 'count', header: m(GridHeaderCell, {}, 'Count')},
    ];
    return m(Grid, {
      columns,
      rowData: row.blockedFunctions.map((f) => [
        m(GridCell, {}, f.func),
        m(GridCell, {}, m(DurationWidget, {dur: f.dur, trace: this.trace})),
        m(GridCell, {}, f.count),
      ]),
    });
  }
}
