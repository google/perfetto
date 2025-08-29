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

/*
TODO(stevegolton):
- Add debug track button....
*/

import m from 'mithril';
import {findRef, toHTMLElement} from '../../base/dom_utils';
import {download} from '../../base/download_utils';
import {stringifyJsonWithBigints} from '../../base/json_utils';
import {assertExists} from '../../base/logging';
import {Icons} from '../../base/semantic_icons';
import {
  formatAsDelimited,
  formatAsMarkdownTable,
  QueryResponse,
} from '../../components/query_table/queries';
import {DataGridDataSource} from '../../components/widgets/data_grid/common';
import {DataGrid} from '../../components/widgets/data_grid/data_grid';
import {InMemoryDataSource} from '../../components/widgets/data_grid/in_memory_data_source';
import {QueryHistoryComponent} from '../../components/widgets/query_history';
import {Trace} from '../../public/trace';
import {Box} from '../../widgets/box';
import {Button, ButtonVariant} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {Editor} from '../../widgets/editor';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {ResizeHandle} from '../../widgets/resize_handle';
import {Stack, StackAuto} from '../../widgets/stack';
import {Icon} from '../../widgets/icon';
import {globals} from '../../frontend/globals';

class CopyHelper {
  private _copied = false;
  private timeoutId: ReturnType<typeof setTimeout> | undefined;
  private readonly timeout: number;

  constructor(timeout = 2000) {
    this.timeout = timeout;
  }

  get copied(): boolean {
    return this._copied;
  }

  async copy(text: string) {
    await navigator.clipboard.writeText(text);
    this._copied = true;
    m.redraw();

    clearTimeout(this.timeoutId);
    this.timeoutId = setTimeout(() => {
      this._copied = false;
      m.redraw();
    }, this.timeout);
  }
}

export interface QueryPageAttrs {
  readonly trace: Trace;
  readonly editorText: string;
  readonly executedQuery?: string;
  readonly queryResult?: QueryResponse;

  onEditorContentUpdate?(content: string): void;

  onExecute?(query: string): void;
}

export class QueryPage implements m.ClassComponent<QueryPageAttrs> {
  private dataSource?: DataGridDataSource;
  private editorHeight: number = 0;
  private editorElement?: HTMLElement;
  private dataGridCopyHelper = new CopyHelper();

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
          m(CopyToClipboardButton, {
            textToCopy: attrs.editorText,
            title: 'Copy query to clipboard',
            label: 'Copy Query',
          }),
        ]),
      ]),
      globals.isInternalUser &&
        m(
          Box,
          m(Callout, {icon: 'star', intent: Intent.None}, [
            'Try out the ',
            m(
              'a',
              {
                href: 'http://go/perfetto-sql-agent',
                target: '_blank',
              },
              'Perfetto SQL Agent',
            ),
            ' to generate SQL queries and ',
            m(
              'a',
              {
                href: 'http://go/perfetto-llm-user-guide#report-issues',
                target: '_blank',
              },
              'give feedback',
            ),
            '!',
          ]),
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
        this.renderQueryResult(attrs.queryResult, this.dataSource),
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
    queryResult: QueryResponse,
    dataSource: DataGridDataSource,
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
        m(DataGrid, {
          className: 'pf-query-page__results',
          data: dataSource,
          columns: queryResult.columns.map((c) => ({name: c})),
          toolbarItemsLeft: m(
            'span.pf-query-page__elapsed-time',
            {title: `This query returned in ${queryTimeString}`},
            [m(Icon, {icon: 'timer'}), ' ', queryTimeString],
          ),
          toolbarItemsRight: [
            this.renderCopyButton(queryResult),
            this.renderDownloadButton(queryResult),
          ],
        }),
      ];
    }
  }

  private renderCopyButton(resp: QueryResponse) {
    const helper = this.dataGridCopyHelper;
    const label = helper.copied ? 'Copied' : 'Copy';
    const icon = helper.copied ? Icons.Check : Icons.Copy;
    const intent = helper.copied ? Intent.Success : Intent.None;

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon,
          intent,
          title: 'Copy results to clipboard',
          label,
        }),
      },
      [
        m(MenuItem, {
          label: 'TSV',
          onclick: async () => {
            const content = formatAsDelimited(resp);
            await helper.copy(content);
          },
        }),
        m(MenuItem, {
          label: 'Markdown',
          onclick: async () => {
            const content = formatAsMarkdownTable(resp);
            await helper.copy(content);
          },
        }),
        m(MenuItem, {
          label: 'JSON',
          onclick: async () => {
            const content = stringifyJsonWithBigints(resp.rows);
            await helper.copy(content);
          },
        }),
      ],
    );
  }

  private renderDownloadButton(resp: QueryResponse) {
    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: Icons.Download,
          title: 'Download data',
          label: 'Download',
        }),
      },
      [
        m(MenuItem, {
          label: 'TSV',
          onclick: () => {
            const content = formatAsDelimited(resp);
            download({
              content,
              mimeType: 'text/tab-separated-values',
              fileName: 'query_result.tsv',
            });
          },
        }),
        m(MenuItem, {
          label: 'Markdown',
          onclick: () => {
            const content = formatAsMarkdownTable(resp);
            download({
              content,
              mimeType: 'text/markdown',
              fileName: 'query_result.md',
            });
          },
        }),
        m(MenuItem, {
          label: 'JSON',
          onclick: () => {
            const content = stringifyJsonWithBigints(resp.rows, 2);
            download({
              content,
              mimeType: 'text/json',
              fileName: 'query_result.json',
            });
          },
        }),
      ],
    );
  }
}

interface CopyToClipboardButtonAttrs {
  readonly textToCopy: string;
  readonly title?: string;
  readonly label?: string;
}

function CopyToClipboardButton() {
  const helper = new CopyHelper();

  return {
    view({attrs}: m.Vnode<CopyToClipboardButtonAttrs>): m.Children {
      const label = helper.copied ? 'Copied' : attrs.label;
      return m(Button, {
        title: attrs.title ?? 'Copy to clipboard',
        icon: helper.copied ? Icons.Check : Icons.Copy,
        intent: helper.copied ? Intent.Success : Intent.None,
        label,
        onclick: async () => {
          await helper.copy(attrs.textToCopy);
        },
      });
    },
  };
}
