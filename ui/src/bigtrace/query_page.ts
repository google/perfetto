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
import {Trace} from '../public/trace';
import {Button, ButtonVariant} from '../widgets/button';
import {TextInput} from '../widgets/text_input';
import {Editor} from '../widgets/editor';
import {DataGrid, renderCell} from '../components/widgets/datagrid/datagrid';
import {SchemaRegistry, ColumnSchema, CellRenderer} from '../components/widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from '../components/widgets/datagrid/in_memory_data_source';
import {Row as DataGridRow} from '../trace_processor/query_result';
import {SplitPanel} from '../widgets/split_panel';
import {EmptyState} from '../widgets/empty_state';
import {Callout} from '../widgets/callout';
import {Intent} from '../widgets/common';
import {Anchor} from '../widgets/anchor';
import {Icons} from '../base/semantic_icons';
import {QueryResponse, runQueryForQueryTable} from '../components/query_table/queries';
import {Box} from '../widgets/box';
import {Stack, StackAuto} from '../widgets/stack';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {CopyToClipboardButton} from '../widgets/copy_to_clipboard_button';
import {getSliceId, isSliceish} from '../components/query_table/query_table';
import {DataSource} from '../components/widgets/datagrid/data_source';
import {recentQueriesStorage} from './recent_queries_storage';
import {bigTraceSettingsManager} from './bigtrace_settings_manager';

class HttpDataSource {
  private static readonly BRUSH_API_URL =
    'https://brush-googleapis.corp.google.com/v1/bigtrace/query';
  private static readonly DEFAULT_LIMIT = 10000;

  private baseQuery: string;
  private traceAddress: string;
  private limit: number;
  private traceLimit: number;
  private cachedData: DataGridRow[] | null = null;
  private fetchPromise: Promise<DataGridRow[]> | null = null;

  constructor(
    baseQuery: string,
    traceAddress = 'android_telemetry.field_trace_summaries_prod.last30days',
    limit = HttpDataSource.DEFAULT_LIMIT,
    traceLimit: number,
  ) {
    this.baseQuery = baseQuery;
    this.traceAddress = traceAddress;
    this.limit = limit;
    this.traceLimit = traceLimit;
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
    const url = HttpDataSource.BRUSH_API_URL;

    const data = {
      trace_address: this.traceAddress,
      limit: this.limit,
      trace_limit: this.traceLimit,
      perfetto_sql: this.baseQuery,
    };

    try {
      const response = await fetch(url, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(data),
        credentials: 'include',
        mode: 'cors',
      });

      if (!response.ok) {
        if (response.status === 403) {
            throw new Error(`HTTP error! status: ${response.status}. This might be an authentication issue. Please make sure you are logged in to the correct Google account.`);
        }
        throw new Error(`HTTP error! status: ${response.status}`);
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
      console.error('Brush query error:', error);
      throw error;
    }
  }

  async query(
    forceRefresh = false,
  ): Promise<DataGridRow[]> {
    return this.fetchData(forceRefresh);
  }

  clearCache(): void {
    this.cachedData = null;
    this.fetchPromise = null;
  }
}

interface QueryPageAttrs {
  trace?: Trace;
  useBrushBackend?: boolean;
  initialQuery?: string;
}

const DEFAULT_SQL = `SELECT 
  COUNT(*) 
FROM slice 
WHERE name GLOB '*kswapd0*' 
LIMIT 100;`;

export class QueryPage implements m.ClassComponent<QueryPageAttrs> {
  private sqlQuery = DEFAULT_SQL;
  private trace?: Trace;
  private useBrushBackend = false;
  private limit = 100;

  private queryResult?: QueryResponse;
  private isLoading = false;
  private dataSource?: DataSource;

  oninit({attrs}: m.Vnode<QueryPageAttrs>) {
    this.trace = attrs.trace;
    this.useBrushBackend = attrs.useBrushBackend || false;
    if (attrs.initialQuery) {
      this.sqlQuery = attrs.initialQuery;
    }
  }

  view() {
    const editorPanel = m('.pf-query-page__editor-panel', [
        m(Box, {className: 'pf-query-page__toolbar'}, [
            m(Stack, {orientation: 'horizontal'}, [
              m(Button, {
                label: 'Run Query',
                icon: 'play_arrow',
                loading: this.isLoading,
                intent: this.isLoading ? Intent.None : Intent.Primary,
                variant: ButtonVariant.Filled,
                onclick: () => {
                  this.runQuery(this.sqlQuery);
                },
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
                m(TextInput as any, {
                    type: 'number',
                    value: this.limit,
                    placeholder: 'Limit',
                    onchange: (value: string) => {
                        const newLimit = parseInt(value, 10);
                        if (!isNaN(newLimit) && newLimit > 0) {
                            this.limit = newLimit;
                        }
                    },
                }),
              ]
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
            ? this.renderQueryResult(this.trace!, this.queryResult, this.dataSource)
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

  private async runQuery(query: string) {
    if (!query) return;

    recentQueriesStorage.saveQuery(query);

    this.isLoading = true;
    this.queryResult = undefined;
    m.redraw();

    if (this.useBrushBackend) {
      const traceLimitSetting = bigTraceSettingsManager.get('traceLimit');
      const traceLimit = traceLimitSetting ? traceLimitSetting.get() as number : 1_000_000;
      const dataSource = new HttpDataSource(query, undefined, this.limit, traceLimit);
      try {
        const data = await dataSource.query();
        this.queryResult = {
          rows: data,
          columns: data.length > 0 ? Object.keys(data[0]) : [],
          durationMs: 0,
          error: undefined,
          totalRowCount: data.length,
          statementWithOutputCount: 1,
          statementCount: 1,
          lastStatementSql: query,
          query,
        };
      } catch (e) {
        const error = e instanceof Error ? e.message : String(e);
        this.queryResult = {
          rows: [],
          columns: [],
          durationMs: 0,
          error,
          totalRowCount: 0,
          statementWithOutputCount: 0,
          statementCount: 1,
          lastStatementSql: query,
          query,
        };
      }
    } else {
        if (!this.trace) return;
        this.queryResult = await runQueryForQueryTable(query, this.trace.engine);
    }


    if (this.queryResult.rows) {
        this.dataSource = new InMemoryDataSource(this.queryResult.rows);
    }


    this.isLoading = false;
    m.redraw();
  }

  private renderQueryResult(
    trace: Trace,
    queryResult: QueryResponse,
    dataSource: DataSource,
  ) {
    const queryTimeString = `${queryResult.durationMs.toFixed(1)} ms`;
    if (queryResult.error) {
      return m(
        '.pf-query-page__query-error',
        `SQL error: ${queryResult.error}`,
      );
    } else {
      return [
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
            const cellRenderer: CellRenderer | undefined =
              column === 'id'
                ? (value, row) => {
                    const sliceId = getSliceId(row);
                    const cell = renderCell(value, column);
                    if (sliceId !== undefined && isSliceish(row)) {
                      return m(
                        Anchor,
                        {
                          title: 'Go to slice on the timeline',
                          icon: Icons.UpdateSelection,
                          onclick: () => {
                            // Navigate to the timeline page
                            trace.navigate('#!/viewer');
                            trace.selection.selectSqlEvent('slice', sliceId, {
                              switchToCurrentSelectionTab: false,
                              scrollToSelection: true,
                            });
                          },
                        },
                        cell,
                      );
                    } else {
                      return renderCell(value, column);
                    }
                  }
                : undefined;
            columnSchema[column] = {cellRenderer};
          }
          const schema: SchemaRegistry = {data: columnSchema};

          return m(DataGrid, {
            schema,
            rootSchema: 'data',
            enablePivotControls: false, // In-memory datasource does not support pivoting
            initialColumns: queryResult.columns.map((col) => ({
              id: col,
              field: col,
            })),
            className: 'pf-query-page__results',
            data: dataSource,
            showExportButton: true,
            emptyStateMessage: 'Query returned no rows',
            toolbarItemsLeft: m(
              'span.pf-query-page__results-summary',
              `Returned ${queryResult.totalRowCount.toLocaleString()} rows in ${queryTimeString}`,
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
    }
  }
}
