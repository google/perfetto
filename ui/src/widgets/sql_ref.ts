// Copyright (C) 2023 The Android Open Source Project
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
import {copyToClipboard} from '../base/clipboard';
import {Icons} from '../base/semantic_icons';
import {Anchor} from './anchor';
import {MenuItem, PopupMenu2} from './menu';

// This widget provides common styling and popup menu options for a SQL row,
// given a table name and an ID.
export interface SqlRefAttrs {
  // The name of the table our row lives in.
  table: string;
  // The ID of our row.
  // If not provided, `table[Unknown]` is shown with no popup menu.
  id?: number;
}

export class SqlRef implements m.ClassComponent<SqlRefAttrs> {
  view({attrs}: m.CVnode<SqlRefAttrs>) {
    const {table, id} = attrs;
    if (id !== undefined) {
      return m(
        PopupMenu2,
        {
          trigger: m(Anchor, {icon: Icons.ContextMenu}, `${table}[${id}]`),
        },
        m(MenuItem, {
          label: 'Copy ID',
          icon: 'content_copy',
          onclick: () => copyToClipboard(`${id}`),
        }),
        m(MenuItem, {
          label: 'Copy SQL query',
          icon: 'file_copy',
          onclick: () =>
            copyToClipboard(`select * from ${table} where id=${id}`),
        }),
      );
    } else {
      return `${table}[Unknown]`;
    }
  }
}
