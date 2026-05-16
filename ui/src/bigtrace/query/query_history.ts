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
import {classNames} from '../../base/classnames';
import {Icons} from '../../base/semantic_icons';
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Stack} from '../../widgets/stack';
import {queryHistoryStorage} from './query_history_storage';
import {
  formatCompact,
  queryStore,
  QueryExecution,
  statusDisplayLabel,
} from './query_store';
import {Tabs, TabsTab} from '../../widgets/tabs';

import {formatDate} from '../../base/time';
import {Spinner} from '../../widgets/spinner';
import {EmptyState} from '../../widgets/empty_state';
import {showModal} from '../../widgets/modal';

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

// Refresh signal fires before the backend insert; wait out the round-trip.
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

// SQL block clamped to ~4 lines with a fade-out mask; click to expand.
// Used by both the sidebar history row and the delete-confirm modal.
// The sidebar inherits its frame from `.pf-query-history__item pre`; the
// modal uses `standalone: true` for the `--standalone` CSS class.
// Expand state is a Mithril instance field so it survives redraws.
interface ClampedQueryAttrs {
  readonly queryText: string;
  readonly standalone?: boolean;
  readonly onExpand?: () => void;
}

class ClampedQuery implements m.ClassComponent<ClampedQueryAttrs> {
  private expanded = false;

  view({attrs}: m.Vnode<ClampedQueryAttrs>): m.Children {
    const {queryText, standalone, onExpand} = attrs;
    if (queryText === '') {
      return m(
        'span.pf-query-history__item-query.pf-query-history__item-query--empty',
        '(no query text)',
      );
    }
    return m(
      'pre.pf-query-history__item-query',
      {
        className: classNames(
          this.expanded && 'pf-query-history__item-query--expanded',
          standalone && 'pf-query-history__item-query--standalone',
        ),
        onclick: () => {
          this.expanded = !this.expanded;
          if (this.expanded) {
            onExpand?.();
          }
        },
      },
      queryText,
    );
  }
}

// UUIDs whose full SQL has already been fetched via the per-uuid endpoint.
const fetchedFullSql = new Set<string>();

// Returns an onExpand callback that fetches the full SQL on first expand,
// or undefined if already fetched / no uuid.
function makeFullSqlExpander(
  uuid: string | undefined,
  currentText: string,
): (() => void) | undefined {
  if (!uuid || fetchedFullSql.has(uuid)) return undefined;
  return () => {
    fetchedFullSql.add(uuid);
    void queryHistoryStorage
      .fetchFullSql(uuid)
      .then((full) => {
        if (full && full !== currentText) {
          queryStore.update(uuid, {perfettoSql: full});
          m.redraw();
        }
      })
      .catch((e) => {
        fetchedFullSql.delete(uuid);
        console.error('Failed to fetch full SQL:', e);
      });
  };
}

// Module-level: survives sidebar toggles so we don't re-fetch on every show.
class HistoryStore {
  history: QueryExecution[] = [];
  isLoading = true;
  error: string | null = null;
  // Default to Ephemeral, matching the Persistent toggle's default-off.
  activeTabKey = 'standard';
  private lastRefreshSignal = -1;
  private debounceTimer?: number;
  private hasEverLoaded = false;

  // No-op if signal unchanged; immediate fetch on first call;
  // debounced on subsequent bumps.
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

  // Bypass signal/debounce: explicit Refresh button + post-Delete.
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

// Point the History sidebar at the tab matching the impending run.
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

    // Span-wrap titles so hover tooltips explain "Ephemeral"/"Persistent".
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
      // Compact for the narrow sidebar; hover reveals the full UTC timestamp.
      const localString = dateObj ? formatCompactDate(dateObj) : 'N/A';
      const utcString =
        startTime !== undefined
          ? formatDate(new Date(startTime), {printTimezone: false})
          : 'N/A';

      const buttonsRow = m(
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
            title: 'Open',
          }),

          m(Button, {
            onclick: async () => {
              if (!uuid) return;
              let confirmed = false;
              await showModal({
                title: 'Delete query from history?',
                content: () =>
                  m('div', [
                    startTime !== undefined &&
                      m(
                        'div',
                        {
                          style: {marginBottom: '8px', opacity: '0.7'},
                          title: `UTC: ${utcString}`,
                        },
                        localString,
                      ),
                    m(ClampedQuery, {
                      queryText: entry.perfettoSql || '',
                      standalone: true,
                      onExpand: makeFullSqlExpander(
                        uuid,
                        entry.perfettoSql || '',
                      ),
                    }),
                  ]),
                buttons: [
                  {text: 'Cancel'},
                  {
                    text: 'Delete',
                    primary: true,
                    action: () => {
                      confirmed = true;
                    },
                  },
                ],
              });
              if (!confirmed) return;
              await queryHistoryStorage.deleteQuery(uuid);
              historyStore.refreshNow();
            },
            icon: Icons.Delete,
            // Red hover so destructive intent reads before click.
            intent: Intent.Danger,
            title: 'Delete query',
          }),
        ],
      );

      return m(
        '.pf-query-history__item',
        {key: `${uuid}-${index}`},
        m('.pf-query-history__item-meta', [
          buttonsRow,
          m('div.pf-query-history__item-header', [
            m(
              'span.pf-query-history__item-status',
              {
                class: `pf-status-${entry.status.toLowerCase().replace(/_/g, '-')}`,
              },
              statusDisplayLabel(entry.status),
            ),
            m(
              'span.pf-query-history__item-date',
              {title: `UTC: ${utcString}`},
              localString,
            ),
          ]),
        ]),
        // Separate section (materialized only): a banded strip between the
        // meta header and the SQL pre, with its own background and borders so
        // it reads as a distinct section, not a row inside the header card.
        isMaterialized &&
          m(
            'div.pf-query-history__item-details',
            {
              className:
                rows === 0
                  ? 'pf-query-history__item-details--empty'
                  : undefined,
            },
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
                    : entry.tableName || 'View Table',
              },
              entry.tableName || '—',
            ),
            m(
              'span.pf-query-history__item-rows-value',
              `${formatCompact(rows)} ${rows === 1 ? 'row' : 'rows'}`,
            ),
          ),
        // Sidebar uses the .pf-query-history__item pre rule for the
        // monospace look; modal callers opt in via `{standalone: true}`.
        // /query_executions clips perfettoSql; first expand fetches the
        // full text via the per-uuid endpoint.
        m(ClampedQuery, {
          queryText,
          onExpand: makeFullSqlExpander(uuid, queryText),
        }),
      );
    });
  }
}
