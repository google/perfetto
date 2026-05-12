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
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Stack} from '../../widgets/stack';
import {queryHistoryStorage} from './query_history_storage';
import {queryStore, QueryExecution} from './query_store';
import {Tabs, TabsTab} from '../../widgets/tabs';

import {formatDate} from '../../base/time';
import {Spinner} from '../../widgets/spinner';
import {EmptyState} from '../../widgets/empty_state';

// Open-an-existing-history-entry callback.
type OpenQueryFn = (
  query: string,
  uuid: string,
  materialize: boolean,
  forceNew?: boolean,
  limit?: number,
  startTime?: number,
) => void;

interface QueryHistoryComponentAttrs {
  readonly className?: string;
  openQuery: OpenQueryFn;
  readonly refreshSignal?: number;
}

// Round-trip debounce: `runner.run()` bumps the refresh signal *before*
// submitting to the backend (so the new history row doesn't exist
// yet at signal-fire time). Wait long enough for the round-trip and
// the backend's IN_PROGRESS insert to land before refetching.
const HISTORY_REFRESH_DEBOUNCE_MS = 1000;

const MONTH_NAMES = [
  'January',
  'February',
  'March',
  'April',
  'May',
  'June',
  'July',
  'August',
  'September',
  'October',
  'November',
  'December',
] as const;

// Sidebar history-row date format: "May 9, 2026, 6:01 PM".
function formatCompactDate(d: Date): string {
  const month = MONTH_NAMES[d.getMonth()];
  const day = d.getDate();
  const year = d.getFullYear();
  let h = d.getHours();
  const m12 = h >= 12 ? 'PM' : 'AM';
  h = h % 12 || 12;
  const mm = String(d.getMinutes()).padStart(2, '0');
  return `${month} ${day}, ${year}, ${h}:${mm} ${m12}`;
}

// Module-level state. Survives `QueryHistoryComponent` mount/unmount
// cycles (e.g. toggling the right sidebar) so we don't re-fetch the
// history list on every show. Refetches are signal-gated by the
// runner via `refreshSignal` and debounced via
// `HISTORY_REFRESH_DEBOUNCE_MS` to cover the round-trip between the
// signal fire and the backend's row insert.
class HistoryStore {
  history: QueryExecution[] = [];
  isLoading = true;
  error: string | null = null;
  // Default to Ephemeral, matching the Persistent toggle's default-off.
  activeTabKey = 'standard';
  private lastRefreshSignal = -1;
  private debounceTimer?: number;
  private hasEverLoaded = false;

  // Caller uses this on every render: it's a no-op when the signal
  // hasn't changed (so sidebar toggles don't refetch), an immediate
  // fetch on the very first call (so the page mount doesn't sit on
  // the loading spinner for a debounce period), and a debounced
  // fetch on subsequent signal bumps (round-trip delay).
  requestRefresh(refreshSignal: number): void {
    if (refreshSignal === this.lastRefreshSignal) return;
    this.lastRefreshSignal = refreshSignal;
    if (!this.hasEverLoaded) {
      this.load();
      return;
    }
    if (this.debounceTimer !== undefined) {
      window.clearTimeout(this.debounceTimer);
    }
    this.debounceTimer = window.setTimeout(
      () => this.load(),
      HISTORY_REFRESH_DEBOUNCE_MS,
    );
  }

  // Bypass the signal check + debounce. Used by the explicit Refresh
  // button and after a Delete (where the user expects an immediate
  // update).
  refreshNow(): void {
    if (this.debounceTimer !== undefined) {
      window.clearTimeout(this.debounceTimer);
      this.debounceTimer = undefined;
    }
    this.load();
  }

  private async load(): Promise<void> {
    this.hasEverLoaded = true;
    this.isLoading = true;
    this.error = null;
    m.redraw();
    try {
      const list = await queryHistoryStorage.getAllHistory();
      this.history = list.map((entry) =>
        queryStore.getOrCreate(entry.uuid, entry),
      );
    } catch (e) {
      this.error = e instanceof Error ? e.message : String(e);
    } finally {
      this.isLoading = false;
      m.redraw();
    }
  }
}

const historyStore = new HistoryStore();

// Steer the History sidebar to the tab matching the run the user is
// about to do. Keys: 'materialized' / 'standard'.
export function setHistoryActiveTab(materialize: boolean): void {
  const key = materialize ? 'materialized' : 'standard';
  if (historyStore.activeTabKey === key) return;
  historyStore.activeTabKey = key;
  m.redraw();
}

export class QueryHistoryComponent
  implements m.ClassComponent<QueryHistoryComponentAttrs>
{
  oninit(vnode: m.CVnode<QueryHistoryComponentAttrs>) {
    historyStore.requestRefresh(vnode.attrs.refreshSignal ?? 0);
  }

  onbeforeupdate(vnode: m.CVnode<QueryHistoryComponentAttrs>) {
    historyStore.requestRefresh(vnode.attrs.refreshSignal ?? 0);
    return true;
  }

  view({attrs}: m.CVnode<QueryHistoryComponentAttrs>) {
    const {openQuery, ...rest} = attrs;

    if (historyStore.isLoading && historyStore.history.length === 0) {
      return m(
        EmptyState,
        {
          title: 'Loading history...',
          icon: 'hourglass_empty',
          fillHeight: true,
        },
        m(Spinner),
      );
    }

    if (historyStore.error) {
      return m(EmptyState, {
        title: `Failed to load history: ${historyStore.error}`,
        icon: 'error',
        fillHeight: true,
      });
    }

    const standardQueries = historyStore.history.filter((h) => !h.materialized);
    const materializedQueries = historyStore.history.filter(
      (h) => h.materialized,
    );

    // Tab titles wrap in a span so hover tooltips can explain what
    // each category holds (the labels alone are jargon).
    const tabs: TabsTab[] = [
      {
        key: 'standard',
        title: m(
          'span',
          {
            title:
              'Queries run with Persistent OFF — results were shown ' +
              'inline at run time and not saved. Reopen here to see the ' +
              'SQL again or rerun.',
          },
          `Ephemeral (${standardQueries.length})`,
        ),
        content: this.renderHistoryList(standardQueries, false, openQuery),
      },
      {
        key: 'materialized',
        title: m(
          'span',
          {
            title:
              'Queries run with Persistent ON — results saved to a ' +
              'temporary backend table you can reopen and browse here.',
          },
          `Persistent (${materializedQueries.length})`,
        ),
        content: this.renderHistoryList(materializedQueries, true, openQuery),
      },
    ];

    return m(
      '.pf-query-history',
      rest,
      m(Tabs, {
        tabs: tabs,
        activeTabKey: historyStore.activeTabKey,
        onTabChange: (key) => {
          historyStore.activeTabKey = key;
          m.redraw();
        },
        rightContent: m(Button, {
          icon: 'refresh',
          title: 'Refresh history',
          onclick: () => historyStore.refreshNow(),
        }),
      }),
    );
  }

  private renderHistoryList(
    queries: QueryExecution[],
    isMaterialized: boolean,
    openQuery?: OpenQueryFn,
  ): m.Children {
    if (queries.length === 0) {
      return m(
        EmptyState,
        {
          title: isMaterialized
            ? 'No persistent queries yet'
            : 'No ephemeral queries yet',
          icon: 'search',
          fillHeight: true,
        },
        m(
          'div',
          {style: {marginTop: '8px', opacity: 0.7}},
          isMaterialized
            ? 'Run a query with Persistent on to see it here.'
            : 'Run a query with Persistent off to see it here.',
        ),
      );
    }

    return queries.map((entry, index) => {
      const queryText = entry.perfettoSql || '';
      const uuid = entry.uuid;
      const startTime = entry.startTime;
      const rows = entry.processedRows;
      const link = entry.tableLink;
      const dateObj = startTime !== undefined ? new Date(startTime) : null;
      // Compact date for the narrow sidebar — full toLocaleString() like
      // "5/4/2026, 3:47:46 PM" wrapped onto 4 lines once the sidebar
      // shrunk. Drop seconds always; drop year when it matches the
      // current year. Hover reveals the full UTC timestamp.
      const localString = dateObj ? formatCompactDate(dateObj) : 'N/A';
      const utcString =
        startTime !== undefined
          ? formatDate(new Date(startTime), {printTimezone: false})
          : 'N/A';

      return m(
        '.pf-query-history__item',
        {key: `${uuid}-${index}`},
        m(
          Stack,
          {
            className: 'pf-query-history__item-buttons',
            orientation: 'horizontal',
          },
          [
            m(Button, {
              onclick: () => {
                if (openQuery && uuid) {
                  openQuery(
                    queryText,
                    uuid,
                    isMaterialized,
                    false,
                    entry.limit,
                    startTime,
                  );
                }
              },
              icon: Icons.ChangeTab,
              title: 'Open query (switches to tab if already open)',
            }),

            m(Button, {
              onclick: async () => {
                if (!uuid) return;
                // Confirm — the trash icon is 4px from Open and the
                // delete is irreversible.
                const oneLine = queryText
                  .split('\n')
                  .map((s) => s.trim())
                  .filter((s) => s.length > 0)[0];
                const preview =
                  oneLine && oneLine.length > 60
                    ? oneLine.slice(0, 59) + '…'
                    : oneLine ?? uuid;
                if (
                  !window.confirm(
                    `Delete this query from history?\n\n${preview}`,
                  )
                ) {
                  return;
                }
                await queryHistoryStorage.deleteQuery(uuid);
                historyStore.refreshNow();
              },
              icon: Icons.Delete,
              // Danger intent — hover state goes red so the
              // destructive nature is visible BEFORE the click. Was
              // visually identical to the gray Open button right next
              // to it. Confirm dialog still backs it up.
              intent: Intent.Danger,
              title: 'Delete query',
            }),
          ],
        ),
        m('.pf-query-history__item-meta', [
          // Row 1: status pill (left) + start timestamp (right).
          // Pairing them keeps the high-signal "what / when" together;
          // the status colour + colored left bar do the visual lift.
          m('div.pf-query-history__item-header', [
            m(
              'span.pf-query-history__item-status',
              {
                class: `pf-status-${entry.status.toLowerCase().replace(/_/g, '-')}`,
                // The colored left bar + the colored text already make
                // it clear this is a status label; the literal "Status:"
                // prefix would be noise.
                title: `Status: ${entry.status}`,
              },
              // Display: "IN_PROGRESS" → "IN PROGRESS". The transient
              // "UNKNOWN" state (newly-submitted query whose first
              // status poll hasn't returned yet, or a legacy row with
              // a null status) reads as alarming user-facing — show
              // it as "STARTING" instead so the badge matches the
              // body's "Loading query status…" message.
              entry.status === 'UNKNOWN'
                ? 'STARTING'
                : entry.status.replace(/_/g, ' '),
            ),
            m(
              'span.pf-query-history__item-date',
              {title: `UTC: ${utcString}`},
              localString,
            ),
          ]),
          // Row 2 (materialized only): table link (left) + rows count
          // (right). Table + rows are the "what got produced" pair —
          // grouping them lets the eye take in the result summary at
          // a glance, and saves a row of vertical space vs the
          // previous status/rows + date + table layout.
          isMaterialized &&
            m('div.pf-query-history__item-details', [
              m('span.pf-query-history__item-table-row', [
                m('span', 'Table:'),
                m(
                  'a.pf-query-history__item-table-link',
                  {
                    class:
                      rows === 0 || link === undefined || link === ''
                        ? 'pf-query-history__item-table-link--disabled'
                        : 'pf-query-history__item-table-link--active',
                    href: link || '#',
                    target: '_blank',
                    title:
                      rows === 0
                        ? 'No table created for empty results'
                        : 'View Table',
                  },
                  entry.tableName || 'N/A',
                ),
              ]),
              m(
                'span.pf-query-history__item-rows',
                {
                  // Dim the row count when it's zero so empty results
                  // recede; non-zero counts stay at full opacity. The
                  // colored left bar + status pill already do the
                  // "succeeded" signalling.
                  className:
                    rows === 0
                      ? 'pf-query-history__item-rows--empty'
                      : undefined,
                },
                m('span.pf-query-history__item-rows-label', 'Rows:'),
                m(
                  'span.pf-query-history__item-rows-value',
                  rows.toLocaleString(),
                ),
              ),
            ]),
        ]),
        // Clamp the query preview to ~4 lines so a long INCLUDE +
        // multi-column SELECT doesn't dominate the sidebar (each row
        // was eating ~half the viewport for any non-trivial query).
        // Click toggles a `--expanded` class for full text. The
        // collapsed state shows a gradient fade-out at the bottom edge
        // so it reads as truncated rather than fixed-height.
        //
        // Empty queryText (a non-UI client submitted `{}`, or a
        // legacy/corrupt history row) renders an italic placeholder
        // instead of an empty <pre> — the empty <pre> made the card
        // visibly shorter than its peers, reading as a broken render.
        queryText === '' &&
          m(
            'span.pf-query-history__item-query',
            {
              style: {
                fontStyle: 'italic',
                opacity: 0.5,
              },
            },
            '(no query text)',
          ),
        queryText !== '' &&
          m(
            'pre.pf-query-history__item-query',
            {
              style: {
                maxHeight: '4.5em',
                overflow: 'hidden',
                cursor: 'pointer',
                position: 'relative',
                maskImage:
                  'linear-gradient(to bottom, black 70%, transparent 100%)',
                WebkitMaskImage:
                  'linear-gradient(to bottom, black 70%, transparent 100%)',
              },
              title: 'Click to toggle full query',
              onclick: (e: Event) => {
                const el = e.currentTarget as HTMLElement;
                const expanded = el.classList.toggle(
                  'pf-query-history__item-query--expanded',
                );
                if (expanded) {
                  el.style.maxHeight = '';
                  el.style.maskImage = '';
                  el.style.webkitMaskImage = '';
                } else {
                  el.style.maxHeight = '4.5em';
                  el.style.maskImage =
                    'linear-gradient(to bottom, black 70%, transparent 100%)';
                  el.style.webkitMaskImage =
                    'linear-gradient(to bottom, black 70%, transparent 100%)';
                }
              },
            },
            queryText,
          ),
      );
    });
  }
}
