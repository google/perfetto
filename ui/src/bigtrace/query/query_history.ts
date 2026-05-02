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
import {Stack} from '../../widgets/stack';
import {queryHistoryStorage} from './query_history_storage';
import {queryStore, QueryExecution} from './query_store';
import {Tabs, TabsTab} from '../../widgets/tabs';

import {formatDate} from '../../base/time';
import {Spinner} from '../../widgets/spinner';
import {EmptyState} from '../../widgets/empty_state';

interface QueryHistoryComponentAttrs {
  readonly className?: string;
  setQuery: (query: string) => void;
  openQuery: (
    query: string,
    uuid: string,
    materialize: boolean,
    forceNew?: boolean,
    limit?: number,
    startTime?: string,
  ) => void;
  readonly refreshSignal?: number;
}

export class QueryHistoryComponent
  implements m.ClassComponent<QueryHistoryComponentAttrs>
{
  private history: QueryExecution[] = [];

  private lastRefreshSignal = 0;
  private refreshTimeout?: number;

  onbeforeupdate(vnode: m.CVnode<QueryHistoryComponentAttrs>) {
    if (vnode.attrs.refreshSignal !== this.lastRefreshSignal) {
      this.lastRefreshSignal =
        vnode.attrs.refreshSignal !== undefined ? vnode.attrs.refreshSignal : 0;
      if (this.refreshTimeout !== undefined) {
        window.clearTimeout(this.refreshTimeout);
      }
      this.refreshTimeout = window.setTimeout(() => {
        this.loadHistory();
        this.refreshTimeout = undefined;
      }, 1000);
    }
    return true;
  }
  private isLoading = true;
  private error: string | null = null;
  private activeTabKey = 'materialized';

  oninit(_vnode: m.CVnode<QueryHistoryComponentAttrs>) {
    this.loadHistory();
  }

  async loadHistory() {
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

  view({attrs}: m.CVnode<QueryHistoryComponentAttrs>) {
    const {openQuery, ...rest} = attrs;

    if (this.isLoading) {
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

    if (this.error) {
      return m(EmptyState, {
        title: `Failed to load history: ${this.error}`,
        icon: 'error',
        fillHeight: true,
      });
    }

    const standardQueries = this.history.filter((h) => !h.materialized);
    const materializedQueries = this.history.filter((h) => h.materialized);

    const tabs: TabsTab[] = [
      {
        key: 'standard',
        title: `Ephemeral (${standardQueries.length})`,
        content: this.renderHistoryList(standardQueries, false, openQuery),
      },
      {
        key: 'materialized',
        title: `Persistent (${materializedQueries.length})`,
        content: this.renderHistoryList(materializedQueries, true, openQuery),
      },
    ];

    return m('.pf-query-history', {...rest, style: {position: 'relative'}}, [
      m(
        'div',
        {style: {position: 'absolute', top: '5px', right: '5px', zIndex: 10}},
        [
          m(Button, {
            icon: 'refresh',
            title: 'Refresh history',
            onclick: () => this.loadHistory(),
          }),
        ],
      ),
      m(Tabs, {
        tabs: tabs,
        activeTabKey: this.activeTabKey,
        onTabChange: (key) => {
          this.activeTabKey = key;
          m.redraw();
        },
      }),
    ]);
  }

  private renderHistoryList(
    queries: QueryExecution[],
    isMaterialized: boolean,
    openQuery?: (
      query: string,
      uuid: string,
      materialize: boolean,
      forceNew?: boolean,
      limit?: number,
    ) => void,
  ): m.Children {
    if (queries.length === 0) {
      return m(EmptyState, {
        title: 'No history found',
        icon: 'search',
        fillHeight: true,
      });
    }

    return queries.map((entry, index) => {
      const queryText = entry.perfettoSql || '';
      const uuid = entry.uuid;
      const startTime = entry.startTime;
      const rows = entry.processedRows;
      const link = entry.tableLink;
      const dateObj = startTime !== undefined ? new Date(startTime) : null;
      const localString = dateObj ? dateObj.toLocaleString() : 'N/A';
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
                  (
                    openQuery as (
                      q: string,
                      u: string,
                      m: boolean,
                      f?: boolean,
                      l?: number,
                      s?: string,
                    ) => void
                  )(
                    queryText,
                    uuid,
                    isMaterialized,
                    false,
                    entry.limit,
                    startTime !== undefined ? String(startTime) : undefined,
                  );
                }
              },
              icon: Icons.ChangeTab,
              title: 'Open query (switches to tab if already open)',
            }),

            m(Button, {
              onclick: async () => {
                if (uuid) {
                  await queryHistoryStorage.deleteQuery(uuid);
                  this.loadHistory();
                }
              },
              icon: Icons.Delete,
              title: 'Delete query',
            }),
          ],
        ),
        m('.pf-query-history__item-meta', [
          m(
            'div',
            {
              style: {
                display: 'flex',
                justifyContent: 'space-between',
                width: '100%',
              },
            },
            [
              m(
                'span',
                {title: `UTC: ${utcString}`},
                `Started: ${localString}`,
              ),
              m(
                'span.pf-query-history__item-status',
                {
                  class: `pf-status-${entry.status.toLowerCase()}`,
                },
                `Status: ${entry.status}`,
              ),
              isMaterialized && m('span', `Rows: ${rows}`),
            ],
          ),
          isMaterialized &&
            m('div', {style: {marginTop: '4px'}}, [
              m(
                'span',
                {
                  style: {
                    display: 'inline-flex',
                    alignItems: 'center',
                    gap: '2px',
                    maxWidth: '100%',
                  },
                },
                [
                  m('span', 'Table:'),
                  m(
                    'a',
                    {
                      href: link || '#',
                      target: '_blank',
                      style: {
                        overflow: 'hidden',
                        textOverflow: 'ellipsis',
                        whiteSpace: 'nowrap',
                        flex: 1,
                        color:
                          rows === 0 || link === undefined || link === ''
                            ? 'var(--pf-color-foreground-secondary, #9aa0a6)'
                            : 'var(--pf-color-primary, #1a73e8)',
                        pointerEvents:
                          rows === 0 || link === undefined || link === ''
                            ? 'none'
                            : 'auto',
                        cursor:
                          rows === 0 || link === undefined || link === ''
                            ? 'default'
                            : 'pointer',
                        textDecoration: 'none',
                      },
                      title:
                        rows === 0
                          ? 'No table created for empty results'
                          : 'View Table',
                    },
                    entry.tableName || 'N/A',
                  ),
                ],
              ),
            ]),
        ]),
        m('pre', queryText),
      );
    });
  }
}
