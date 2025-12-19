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
import {download} from '../../../base/download_utils';
import {Icons} from '../../../base/semantic_icons';
import {Button} from '../../../widgets/button';
import {CopyButtonHelper} from '../../../widgets/copy_to_clipboard_button';
import {MenuItem, PopupMenu} from '../../../widgets/menu';

export type ExportFormat = 'tsv' | 'json' | 'markdown';

export interface DataGridExportButtonAttrs {
  readonly onExportData: (format: ExportFormat) => Promise<string>;
}

/**
 * DataGrid copy button component with dropdown menu for copying data to clipboard
 * in different formats (TSV, Markdown, JSON).
 * Maintains its own CopyButtonHelper state to show "Copied" feedback.
 */
export class DataGridExportButton
  implements m.ClassComponent<DataGridExportButtonAttrs>
{
  private helper = new CopyButtonHelper();

  view({attrs}: m.CVnode<DataGridExportButtonAttrs>) {
    const {onExportData} = attrs;
    const loading = this.helper.state === 'working';
    const icon = this.helper.state === 'copied' ? Icons.Check : Icons.Download;

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon,
          loading,
          label: 'Export',
          title: 'Export table data',
        }),
      },
      [
        m(MenuItem, {label: 'Copy to clipboard'}, [
          m(MenuItem, {
            label: 'Tab Separated Values',
            icon: 'tsv',
            title: 'Tab-separated values - paste into spreadsheets',
            onclick: async () => {
              const content = await onExportData('tsv');
              await this.helper.copy(content);
            },
          }),
          m(MenuItem, {
            label: 'Markdown Table',
            icon: 'table',
            title: 'Markdown table format',
            onclick: async () => {
              const content = await onExportData('markdown');
              await this.helper.copy(content);
            },
          }),
          m(MenuItem, {
            label: 'JSON',
            icon: 'data_object',
            title: 'JSON array of objects',
            onclick: async () => {
              const content = await onExportData('json');
              await this.helper.copy(content);
            },
          }),
        ]),

        m(MenuItem, {label: 'Download'}, [
          m(MenuItem, {
            label: 'Tab Separated Values (.tsv)',
            icon: 'tsv',
            title: 'Tab-separated values - opens in Excel/Sheets',
            onclick: async () => {
              const content = await onExportData('tsv');
              download({
                content,
                mimeType: 'text/tab-separated-values',
                fileName: 'data_export.tsv',
              });
            },
          }),
          m(MenuItem, {
            label: 'Markdown Table (.md)',
            icon: 'table',
            title: 'Markdown table format - paste into docs',
            onclick: async () => {
              const content = await onExportData('markdown');
              download({
                content,
                mimeType: 'text/markdown',
                fileName: 'data_export.md',
              });
            },
          }),
          m(MenuItem, {
            label: 'JSON (.json)',
            icon: 'data_object',
            title: 'JSON array - use in scripts/tools',
            onclick: async () => {
              const content = await onExportData('json');
              download({
                content,
                mimeType: 'application/json',
                fileName: 'data_export.json',
              });
            },
          }),
        ]),
      ],
    );
  }
}
