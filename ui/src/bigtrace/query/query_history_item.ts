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
import {formatDate} from '../../base/time';
import {showModal} from '../../widgets/modal';
import {historyStore, formatCompactDate} from './history_store';

// Open-an-existing-history-entry callback.
export type OpenQueryFn = (
  query: string,
  uuid: string,
  materialize: boolean,
  forceNew?: boolean,
  limit?: number,
  startTime?: number,
) => void;

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
// Capped to avoid unbounded growth over long sessions.
const FETCHED_SQL_MAX = 500;
const fetchedFullSql = new Set<string>();

// Returns an onExpand callback that fetches the full SQL on first expand,
// or undefined if already fetched / no uuid.
function makeFullSqlExpander(
  uuid: string | undefined,
  currentText: string,
): (() => void) | undefined {
  if (!uuid || fetchedFullSql.has(uuid)) return undefined;
  return () => {
    if (fetchedFullSql.size >= FETCHED_SQL_MAX) {
      // Evict oldest entry (Set iteration order = insertion order).
      const first = fetchedFullSql.values().next().value;
      if (first !== undefined) fetchedFullSql.delete(first);
    }
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

export function renderHistoryItem(
  entry: QueryExecution,
  index: number,
  isMaterialized: boolean,
  openQuery?: OpenQueryFn,
): m.Children {
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
                  onExpand: makeFullSqlExpander(uuid, entry.perfettoSql || ''),
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
            rows === 0 ? 'pf-query-history__item-details--empty' : undefined,
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
}
