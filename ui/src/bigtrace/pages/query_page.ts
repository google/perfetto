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
import {Button, ButtonVariant} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {Editor} from '../../widgets/editor';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {
  SchemaRegistry,
  ColumnSchema,
} from '../../components/widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {Row as DataGridRow} from '../../trace_processor/query_result';
import {SplitPanel} from '../../widgets/split_panel';
import {EmptyState} from '../../widgets/empty_state';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {Box} from '../../widgets/box';
import {Stack, StackAuto} from '../../widgets/stack';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {DataSource} from '../../components/widgets/datagrid/data_source';
import {recentQueriesStorage} from '../query/recent_queries_storage';
import {SettingFilter} from '../settings/settings_types';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {endpointStorage} from '../settings/endpoint_storage';
import {Tabs} from '../../widgets/tabs';
import {linkify} from '../../widgets/anchor';

export interface QueryResponse {
  query: string;
  error?: string;
  totalRowCount: number;
  columns: string[];
  rows: DataGridRow[];
  statementCount: number;
  statementWithOutputCount: number;
  lastStatementSql: string;
}

class HttpDataSource {
  private static readonly DEFAULT_LIMIT = 1000000;

  private endpoint: string;
  private baseQuery: string;
  private limit: number;
  private settings: SettingFilter[];
  private cachedData: DataGridRow[] | null = null;
  private fetchPromise: Promise<DataGridRow[]> | null = null;
  private abortController: AbortController | null = null;

  constructor(
    endpoint: string,
    baseQuery: string,
    limit = HttpDataSource.DEFAULT_LIMIT,
    settings: SettingFilter[],
  ) {
    this.endpoint = endpoint;
    this.baseQuery = baseQuery;
    this.limit = limit;
    this.settings = settings;
  }

  private async fetchData(forceRefresh = false): Promise<DataGridRow[]> {
    if (forceRefresh) {
      this.cachedData = null;
      this.fetchPromise = null;
    }

    if (this.cachedData !== null) {
      return this.cachedData;
    }

    if (this.fetchPromise !== null) {
      return this.fetchPromise;
    }

    this.fetchPromise = this.performFetch();
    try {
      this.cachedData = await this.fetchPromise;
      return this.cachedData;
    } finally {
      this.fetchPromise = null;
    }
  }

  private async performFetch(): Promise<DataGridRow[]> {
    const url = `${this.endpoint}/execute_bigtrace_query`;

    const serializedSettings = this.settings.map((s) => ({
      setting_id: s.settingId,
      values: s.values,
      category: s.category,
    }));

    const data = {
      limit: this.limit,
      perfetto_sql: this.baseQuery,
      settings: serializedSettings,
    };

    this.abortController = new AbortController();

    try {
      const response = await fetch(url, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(data),
        credentials: 'include',
        mode: 'cors',
        signal: this.abortController.signal,
      });

      if (!response.ok) {
        let errorText = '';
        try {
          errorText = await response.text();
        } catch (e) {
          errorText = 'Could not read error body';
        }
        if (response.status === 403) {
          throw new Error(
            `HTTP error! status: ${response.status}. This might be an authentication issue. Please make sure you are logged in to the correct Google account. Backend says: ${errorText}`,
          );
        }
        throw new Error(
          `HTTP error! status: ${response.status}, backend says: ${errorText}`,
        );
      }

      const result = await response.json();

      if (
        result.columnNames !== undefined &&
        result.columnNames !== null &&
        result.rows !== undefined &&
        result.rows !== null
      ) {
        return result.rows.map(
          (row: {values: Array<string | number | null>}) => {
            const rowObject: DataGridRow = {};
            result.columnNames.forEach((header: string, index: number) => {
              if (header === null) return;
              const value = row.values[index];
              const numValue = Number(value);
              rowObject[header] =
                value === null || value === 'NULL' || isNaN(numValue)
                  ? value
                  : numValue;
            });
            return rowObject;
          },
        );
      }

      return [];
    } catch (error) {
      if (error instanceof DOMException && error.name === 'AbortError') {
        throw new Error('Query was cancelled.');
      }
      throw error;
    }
  }

  abort(): void {
    this.abortController?.abort();
  }

  async query(forceRefresh = false): Promise<DataGridRow[]> {
    return this.fetchData(forceRefresh);
  }

  clearCache(): void {
    this.cachedData = null;
    this.fetchPromise = null;
  }
}

interface QueryPageAttrs {
  useBrushBackend?: boolean;
  initialQuery?: string;
}

const DEFAULT_SQL = `SELECT
  COUNT(*) as slice_count
FROM slice;`;

class QuerySessionState {
  sqlQuery = DEFAULT_SQL;
  limit = 100;
  queryResult?: QueryResponse;
  isLoading = false;
  dataSource?: DataSource;
  querySettings: SettingFilter[] = [];
  activeHttpDataSource?: HttpDataSource;
}

const sessionState = new QuerySessionState();

export class QueryPage implements m.ClassComponent<QueryPageAttrs> {
  private useBrushBackend = false;

  get sqlQuery() {
    return sessionState.sqlQuery;
  }
  set sqlQuery(v: string) {
    sessionState.sqlQuery = v;
  }

  get limit() {
    return sessionState.limit;
  }
  set limit(v: number) {
    sessionState.limit = v;
  }

  get queryResult() {
    return sessionState.queryResult;
  }
  set queryResult(v: QueryResponse | undefined) {
    sessionState.queryResult = v;
  }

  get isLoading() {
    return sessionState.isLoading;
  }
  set isLoading(v: boolean) {
    sessionState.isLoading = v;
  }

  get dataSource() {
    return sessionState.dataSource;
  }
  set dataSource(v: DataSource | undefined) {
    sessionState.dataSource = v;
  }

  oninit({attrs}: m.Vnode<QueryPageAttrs>) {
    this.useBrushBackend = attrs.useBrushBackend || false;
    if (attrs.initialQuery) {
      this.sqlQuery = attrs.initialQuery;
    }
    if (this.useBrushBackend) {
      bigTraceSettingsStorage.loadSettings();
    }
  }

  view() {
    const editorPanel = m('.pf-query-page__editor-panel', [
      m(Box, {className: 'pf-query-page__toolbar'}, [
        m(Stack, {orientation: 'horizontal'}, [
          this.isLoading
            ? m(Button, {
                label: 'Cancel',
                icon: 'stop',
                intent: Intent.Warning,
                variant: ButtonVariant.Filled,
                onclick: () => this.cancelQuery(),
              })
            : m(Button, {
                label: 'Run Query',
                icon: 'play_arrow',
                intent: Intent.Primary,
                variant: ButtonVariant.Filled,
                onclick: () => this.runQuery(this.sqlQuery),
              }),
          m(
            Stack,
            {
              orientation: 'horizontal',
              className: 'pf-query-page__hotkeys',
            },
            'or press',
            m(HotkeyGlyphs, {hotkey: 'Mod+Enter'}),
          ),
          m(StackAuto),
          this.useBrushBackend && [
            m('span', 'Result limit:'),
            m(TextInput, {
              type: 'number',
              value: String(this.limit),
              placeholder: 'Limit',
              onChange: (value: string) => {
                const newLimit = parseInt(value, 10);
                if (!isNaN(newLimit) && newLimit > 0) {
                  this.limit = newLimit;
                }
              },
            }),
          ],
        ]),
      ]),
      this.sqlQuery.includes('"') &&
        m(
          Callout,
          {icon: 'warning', intent: Intent.None},
          `" (double quote) character observed in query; if this is being used to ` +
            `define a string, please use ' (single quote) instead. Using double quotes ` +
            `can cause subtle problems which are very hard to debug.`,
        ),
      m(Editor, {
        text: this.sqlQuery,
        language: 'perfetto-sql',
        onUpdate: (text: string) => {
          this.sqlQuery = text;
        },
        onExecute: (query: string) => this.runQuery(query),
      }),
    ]);

    const resultsPanel = m(
      '.pf-query-page__results-panel',
      this.dataSource && this.queryResult
        ? this.renderQueryResult(this.queryResult, this.dataSource)
        : this.isLoading
          ? m(EmptyState, {
              title: 'Running query...',
              icon: 'hourglass_empty',
              fillHeight: true,
            })
          : m(EmptyState, {
              title: 'Run a query to see results',
              icon: 'search',
              fillHeight: true,
            }),
    );

    return m(
      '.pf-query-page',
      m(SplitPanel, {
        direction: 'vertical',
        initialSplit: {percent: 35},
        minSize: 100,
        firstPanel: editorPanel,
        secondPanel: resultsPanel,
      }),
    );
  }

  private cancelQuery() {
    sessionState.activeHttpDataSource?.abort();
    sessionState.activeHttpDataSource = undefined;
    this.isLoading = false;
    m.redraw();
  }

  private async runQuery(query: string) {
    if (!query) return;

    // Abort any in-flight query before starting a new one.
    sessionState.activeHttpDataSource?.abort();

    recentQueriesStorage.saveQuery(query);

    this.isLoading = true;
    this.queryResult = undefined;
    m.redraw();

    if (this.useBrushBackend) {
      const endpointSetting = endpointStorage.get('bigtraceEndpoint');
      const endpoint = endpointSetting ? (endpointSetting.get() as string) : '';

      await bigTraceSettingsStorage.loadSettings();

      const settings = bigTraceSettingsStorage.buildSettingFilters();
      sessionState.querySettings = settings;

      const httpDataSource = new HttpDataSource(
        endpoint,
        query,
        this.limit,
        settings,
      );
      sessionState.activeHttpDataSource = httpDataSource;
      try {
        const data = await httpDataSource.query();
        this.queryResult = {
          rows: data,
          columns: data.length > 0 ? Object.keys(data[0]) : [],
          error: undefined,
          totalRowCount: data.length,
          statementWithOutputCount: 1,
          statementCount: 1,
          lastStatementSql: query,
          query,
        };
      } catch (e) {
        // Don't show an error for user-initiated cancellation.
        if (e instanceof Error && e.message === 'Query was cancelled.') {
          return;
        }
        const error = e instanceof Error ? e.message : String(e);
        this.queryResult = {
          rows: [],
          columns: [],
          error,
          totalRowCount: 0,
          statementWithOutputCount: 0,
          statementCount: 1,
          lastStatementSql: query,
          query,
        };
      } finally {
        sessionState.activeHttpDataSource = undefined;
      }
    } else {
      throw new Error(
        'Local query execution is unsupported in bigtrace context.',
      );
    }

    if (this.queryResult !== undefined) {
      this.dataSource = new InMemoryDataSource(this.queryResult.rows);
    }

    this.isLoading = false;
    m.redraw();
  }

  private renderQueryResult(
    queryResult: QueryResponse,
    dataSource: DataSource,
  ) {
    if (queryResult.error) {
      return m(
        '.pf-query-page__query-error',
        `SQL error: ${queryResult.error}`,
      );
    } else {
      const tableContent = [
        queryResult.statementWithOutputCount > 1 &&
          m(Box, [
            m(Callout, {icon: 'warning', intent: Intent.None}, [
              `${queryResult.statementWithOutputCount} out of ${queryResult.statementCount} `,
              'statements returned a result. ',
              'Only the results for the last statement are displayed.',
            ]),
          ]),
        (() => {
          // Build schema directly
          const columnSchema: ColumnSchema = {};
          for (const column of queryResult.columns) {
            if (column === 'link') {
              columnSchema[column] = {
                cellRenderer: (value) => {
                  if (value === null || value === undefined) {
                    return '';
                  }
                  return linkify(String(value));
                },
              };
            } else {
              columnSchema[column] = {cellRenderer: undefined};
            }
          }
          const schema: SchemaRegistry = {data: columnSchema};

          return m(DataGrid, {
            schema,
            rootSchema: 'data',
            enablePivotControls: false, // In-memory datasource does not support pivoting
            initialColumns: queryResult.columns
              .filter((col) => {
                if (!col.startsWith('_')) return true;
                if (col === '_trace_id') return true;
                const settingId = col.substring(1);
                return sessionState.querySettings.some(
                  (s) =>
                    s.settingId === settingId &&
                    s.category === 'TRACE_METADATA',
                );
              })
              .map((col) => ({
                id: col,
                field: col,
              })),
            className: 'pf-query-page__results',
            data: dataSource,
            showExportButton: true,
            emptyStateMessage: 'Query returned no rows',
            toolbarItemsLeft: m(
              'span.pf-query-page__results-summary',
              `Returned ${queryResult.totalRowCount.toLocaleString()} rows`,
            ),
            toolbarItemsRight: [
              m(CopyToClipboardButton, {
                textToCopy: queryResult.query,
                title: 'Copy executed query to clipboard',
                label: 'Copy Query',
              }),
            ],
          });
        })(),
      ];

      return m(Tabs, {
        tabs: [
          {
            key: 'table',
            title: 'Table',
            content: tableContent,
          },
          {
            key: 'chart',
            title: 'Chart',
            content: m(EmptyState, {
              title: 'Charts are coming soon',
              icon: 'bar_chart',
            }),
          },
        ],
      });
    }
  }
}
