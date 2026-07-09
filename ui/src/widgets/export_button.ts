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
import {download} from '../base/download_utils';
import {Icons} from '../base/semantic_icons';
import {Button} from './button';
import {ActionButtonHelper} from './action_button_helper';
import {copyToClipboard} from '../base/clipboard';
import {MenuItem, PopupMenu} from './menu';

export type ExportFormat = 'tsv' | 'json' | 'markdown';

export interface ExportButtonAttrs {
  readonly onExportData: (format: ExportFormat) => Promise<string>;

  // Base name used for downloaded files. Defaults to 'data_export'.
  readonly fileBaseName?: string;
}

/**
 * Export button component with dropdown menu for copying data to clipboard
 * or downloading it in different formats (TSV, Markdown, JSON).
 * Maintains its own ActionButtonHelper state to show "Copied" feedback.
 */
export class ExportButton implements m.ClassComponent<ExportButtonAttrs> {
  private helper = new ActionButtonHelper();

  private async copyToClipboardWithHelper(content: Promise<string>) {
    await this.helper.execute(async () => await copyToClipboard(await content));
  }

  view({attrs}: m.CVnode<ExportButtonAttrs>) {
    const {onExportData, fileBaseName} = attrs;
    const loading = this.helper.state === 'working';
    const icon = this.helper.state === 'done' ? Icons.Check : Icons.Download;
    const baseName = fileBaseName ?? 'data_export';

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon,
          loading,
          label: 'Export',
          title: 'Export data',
        }),
      },
      [
        m(MenuItem, {label: 'Copy to clipboard'}, [
          m(MenuItem, {
            label: 'Tab Separated Values',
            icon: 'tsv',
            title: 'Tab-separated values - paste into spreadsheets',
            onclick: async () => {
              await this.copyToClipboardWithHelper(onExportData('tsv'));
            },
          }),
          m(MenuItem, {
            label: 'Markdown Table',
            icon: 'table',
            title: 'Markdown table format',
            onclick: async () => {
              await this.copyToClipboardWithHelper(onExportData('markdown'));
            },
          }),
          m(MenuItem, {
            label: 'JSON',
            icon: 'data_object',
            title: 'JSON array of objects',
            onclick: async () => {
              await this.copyToClipboardWithHelper(onExportData('json'));
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
                fileName: `${baseName}.tsv`,
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
                fileName: `${baseName}.md`,
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
                fileName: `${baseName}.json`,
              });
            },
          }),
        ]),
      ],
    );
  }
}
