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
import {download} from '../../base/download_utils';
import {stringifyJsonWithBigints} from '../../base/json_utils';
import {Icons} from '../../base/semantic_icons';
import {Button} from '../../widgets/button';
import {CopyButtonHelper} from '../../widgets/copy_to_clipboard_button';
import {MenuDivider, MenuItem, MenuTitle, PopupMenu} from '../../widgets/menu';
import {
  formatAsDelimited,
  formatAsMarkdownTable,
  QueryResponse,
} from './queries';

interface QueryTableButtonsAttrs {
  readonly query: string;
  readonly resp: QueryResponse | undefined;
}

export class QueryTableButtons
  implements m.ClassComponent<QueryTableButtonsAttrs>
{
  private copyHelper = new CopyButtonHelper();

  view({attrs}: m.CVnode<QueryTableButtonsAttrs>) {
    const {query, resp} = attrs;

    return [
      this.renderCopyMenu(query, resp),
      resp && resp.error === undefined && this.renderDownloadMenu(query, resp),
    ];
  }

  private renderCopyMenu(query: string, resp: QueryResponse | undefined) {
    const label = this.copyHelper.state === 'copied' ? 'Copied' : 'Copy';
    const loading = this.copyHelper.state === 'working';
    const icon = this.copyHelper.state === 'copied' ? Icons.Check : Icons.Copy;

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          label,
          icon,
          loading,
        }),
      },
      m(MenuItem, {
        label: 'Query',
        icon: 'database_search',
        onclick: async () => this.copyHelper.copy(query),
      }),
      resp &&
        resp.error === undefined && [
          m(MenuDivider),
          m(MenuTitle, {label: 'Results'}),
          m(MenuItem, {
            label: 'TSV',
            icon: 'tsv',
            onclick: async () => {
              const content = formatAsDelimited(resp);
              await this.copyHelper.copy(content);
            },
          }),
          m(MenuItem, {
            label: 'Markdown',
            icon: 'table',
            onclick: async () => {
              const content = formatAsMarkdownTable(resp);
              await this.copyHelper.copy(content);
            },
          }),
          m(MenuItem, {
            label: 'JSON',
            icon: 'data_object',
            onclick: async () => {
              const content = stringifyJsonWithBigints(resp.rows, 2);
              await this.copyHelper.copy(content);
            },
          }),
        ],
    );
  }

  private renderDownloadMenu(query: string, resp: QueryResponse) {
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
          label: 'Query',
          icon: 'database_search',
          onclick: () => {
            download({
              content: query,
              mimeType: 'text/x-sql',
              fileName: 'query.sql',
            });
          },
        }),
        m(MenuDivider),
        m(MenuTitle, {label: 'Results'}),
        m(MenuItem, {
          label: 'TSV',
          icon: 'tsv',
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
          icon: 'table',
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
          icon: 'data_object',
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
