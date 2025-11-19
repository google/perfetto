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
import {Icons} from '../../../base/semantic_icons';
import {Button} from '../../../widgets/button';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {download} from '../../../base/download_utils';
import {DataGridApi} from './data_grid';
import {CopyButtonHelper} from '../../../widgets/copy_to_clipboard_button';

export interface CopyButtonAttrs {
  readonly api: DataGridApi;
}

/**
 * Copy button component with dropdown menu for copying data to clipboard
 * in different formats (TSV, Markdown, JSON).
 * Maintains its own CopyButtonHelper state to show "Copied" feedback.
 */
export class CopyButton implements m.ClassComponent<CopyButtonAttrs> {
  private helper = new CopyButtonHelper();

  view({attrs}: m.CVnode<CopyButtonAttrs>) {
    const {api} = attrs;
    const label = this.helper.state === 'copied' ? 'Copied' : 'Copy';
    const loading = this.helper.state === 'working';
    const icon = this.helper.state === 'copied' ? Icons.Check : Icons.Copy;

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon,
          label,
          loading,
          title: 'Copy filtered data to clipboard',
        }),
      },
      [
        m(MenuItem, {
          label: 'TSV',
          icon: 'tsv',
          onclick: async () => {
            const content = await api.exportData('tsv');
            await this.helper.copy(content);
          },
        }),
        m(MenuItem, {
          label: 'Markdown',
          icon: 'table',
          onclick: async () => {
            const content = await api.exportData('markdown');
            await this.helper.copy(content);
          },
        }),
        m(MenuItem, {
          label: 'JSON',
          icon: 'data_object',
          onclick: async () => {
            const content = await api.exportData('json');
            await this.helper.copy(content);
          },
        }),
      ],
    );
  }
}

export interface DownloadButtonAttrs {
  readonly api: DataGridApi;
}

/**
 * Download button component with dropdown menu for downloading data
 * in different formats (TSV, Markdown, JSON).
 */
export class DownloadButton implements m.ClassComponent<DownloadButtonAttrs> {
  view({attrs}: m.CVnode<DownloadButtonAttrs>) {
    const {api} = attrs;

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: Icons.Download,
          label: 'Download',
          title: 'Download filtered data',
        }),
      },
      [
        m(MenuItem, {
          label: 'TSV',
          icon: 'tsv',
          onclick: async () => {
            const content = await api.exportData('tsv');
            download({
              content,
              mimeType: 'text/tab-separated-values',
              fileName: 'data_export.tsv',
            });
          },
        }),
        m(MenuItem, {
          label: 'Markdown',
          icon: 'table',
          onclick: async () => {
            const content = await api.exportData('markdown');
            download({
              content,
              mimeType: 'text/markdown',
              fileName: 'data_export.md',
            });
          },
        }),
        m(MenuItem, {
          label: 'JSON',
          icon: 'data_object',
          onclick: async () => {
            const content = await api.exportData('json');
            download({
              content,
              mimeType: 'application/json',
              fileName: 'data_export.json',
            });
          },
        }),
      ],
    );
  }
}
