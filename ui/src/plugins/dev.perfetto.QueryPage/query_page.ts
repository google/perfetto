// Copyright (C) 2025 The Android Open Source Project
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
import {findRef, toHTMLElement} from '../../base/dom_utils';
import {assertExists} from '../../base/logging';
import {Icons} from '../../base/semantic_icons';
import {QueryResponse} from '../../components/query_table/queries';
import {DataGrid, renderCell} from '../../components/widgets/datagrid/datagrid';
import {
  CellRenderer,
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {QueryHistoryComponent} from '../../components/widgets/query_history';
import {Trace} from '../../public/trace';
import {Box} from '../../widgets/box';
import {Button, ButtonVariant} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {Editor} from '../../widgets/editor';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {ResizeHandle} from '../../widgets/resize_handle';
import {Stack, StackAuto} from '../../widgets/stack';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {Anchor} from '../../widgets/anchor';
import {getSliceId, isSliceish} from '../../components/query_table/query_table';
import {DataSource} from '../../components/widgets/datagrid/data_source';
import {PopupMenu} from '../../widgets/menu';
import {PopupPosition} from '../../widgets/popup';
import {AddDebugTrackMenu} from '../../components/tracks/add_debug_track_menu';

const HIDE_PERFETTO_SQL_AGENT_BANNER_KEY = 'hidePerfettoSqlAgentBanner';

export interface QueryPageAttrs {
  readonly trace: Trace;
  readonly editorText: string;
  readonly executedQuery?: string;
  readonly queryResult?: QueryResponse;

  onEditorContentUpdate?(content: string): void;

  onExecute?(query: string): void;
}

export class QueryPage implements m.ClassComponent<QueryPageAttrs> {
  private dataSource?: DataSource;
  private editorHeight: number = 0;
  private editorElement?: HTMLElement;

  oncreate({dom}: m.VnodeDOM<QueryPageAttrs>) {
    this.editorElement = toHTMLElement(assertExists(findRef(dom, 'editor')));
    this.editorElement.style.height = '200px';
  }

  onbeforeupdate(
    vnode: m.Vnode<QueryPageAttrs>,
    oldVnode: m.Vnode<QueryPageAttrs>,
  ) {
    // Update the datasource if present
    if (vnode.attrs.queryResult !== oldVnode.attrs.queryResult) {
      if (vnode.attrs.queryResult) {
        this.dataSource = new InMemoryDataSource(vnode.attrs.queryResult.rows);
      } else {
        this.dataSource = undefined;
      }
    }
  }

  view({attrs}: m.CVnode<QueryPageAttrs>) {
    return m(
      '.pf-query-page',
      m(Box, {className: 'pf-query-page__toolbar'}, [
        m(Stack, {orientation: 'horizontal'}, [
          m(Button, {
            label: 'Run Query',
            icon: 'play_arrow',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            onclick: () => {
              attrs.onExecute?.(attrs.editorText);
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
          m(StackAuto), // The spacer pushes the following buttons to the right.
          attrs.trace.isInternalUser &&
            m(Button, {
              icon: 'wand_stars',
              title:
                'Generate SQL queries with the Perfetto SQL Agent! Give feedback: go/perfetto-llm-bug',
              label: 'Generate SQL Queries with AI',
              onclick: () => {
                window.open('http://go/perfetto-sql-agent', '_blank');
              },
            }),
          m(CopyToClipboardButton, {
            textToCopy: attrs.editorText,
            title: 'Copy query to clipboard',
            label: 'Copy Query',
          }),
        ]),
      ]),
      this.shouldDisplayPerfettoSqlAgentBanner(attrs) &&
        m(
          Box,
          m(
            Callout,
            {
              icon: 'wand_stars',
              dismissible: true,
              onDismiss: () => {
                this.hidePerfettoSqlAgentBanner();
              },
            },
            [
              'Try out the ',
              m(
                Anchor,
                {
                  href: 'http://go/perfetto-sql-agent',
                  target: '_blank',
                  icon: Icons.ExternalLink,
                },
                'Perfetto SQL Agent',
              ),
              ' to generate SQL queries and ',
              m(
                Anchor,
                {
                  href: 'http://go/perfetto-llm-user-guide#report-issues',
                  target: '_blank',
                  icon: Icons.ExternalLink,
                },
                'give feedback',
              ),
              '!',
            ],
          ),
        ),
      attrs.editorText.includes('"') &&
        m(
          Box,
          m(
            Callout,
            {icon: 'warning', intent: Intent.None},
            `" (double quote) character observed in query; if this is being used to ` +
              `define a string, please use ' (single quote) instead. Using double quotes ` +
              `can cause subtle problems which are very hard to debug.`,
          ),
        ),
      m(Editor, {
        ref: 'editor',
        language: 'perfetto-sql',
        text: attrs.editorText,
        onUpdate: attrs.onEditorContentUpdate,
        onExecute: attrs.onExecute,
      }),
      m(ResizeHandle, {
        onResize: (deltaPx: number) => {
          this.editorHeight += deltaPx;
          this.editorElement!.style.height = `${this.editorHeight}px`;
        },
        onResizeStart: () => {
          this.editorHeight = this.editorElement!.clientHeight;
        },
      }),
      this.dataSource &&
        attrs.queryResult &&
        this.renderQueryResult(attrs.trace, attrs.queryResult, this.dataSource),
      m(QueryHistoryComponent, {
        className: 'pf-query-page__history',
        trace: attrs.trace,
        runQuery: (query: string) => {
          attrs.onExecute?.(query);
        },
        setQuery: (query: string) => {
          attrs.onEditorContentUpdate?.(query);
        },
      }),
    );
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
          const lastStatement = queryResult.lastStatementSql;

          return m(DataGrid, {
            schema,
            rootSchema: 'data',
            initialColumns: queryResult.columns.map((col) => ({field: col})),
            className: 'pf-query-page__results',
            data: dataSource,
            showExportButton: true,
            toolbarItemsLeft: m(
              'span.pf-query-page__results-summary',
              `Returned ${queryResult.totalRowCount.toLocaleString()} rows in ${queryTimeString}`,
            ),
            toolbarItemsRight: [
              m(
                PopupMenu,
                {
                  trigger: m(Button, {label: 'Add debug track'}),
                  position: PopupPosition.Top,
                },
                m(AddDebugTrackMenu, {
                  trace,
                  query: lastStatement,
                  availableColumns: queryResult.columns,
                  onAdd: () => {
                    // Navigate to the tracks page
                    trace.navigate('#!/viewer');
                  },
                }),
              ),
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

  private shouldDisplayPerfettoSqlAgentBanner(attrs: QueryPageAttrs) {
    return (
      attrs.trace.isInternalUser &&
      localStorage.getItem(HIDE_PERFETTO_SQL_AGENT_BANNER_KEY) !== 'true'
    );
  }

  private hidePerfettoSqlAgentBanner() {
    localStorage.setItem(HIDE_PERFETTO_SQL_AGENT_BANNER_KEY, 'true');
  }
}
