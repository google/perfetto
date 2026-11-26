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
import type {ColumnDef} from '../graph_utils';
import {ColumnPicker} from '../widgets/column_picker';
import {Row} from './row';

export interface TreeColumnsRowAttrs {
  readonly idColumn: string;
  readonly parentIdColumn: string;
  readonly columns: ColumnDef[];
  readonly onIdChange: (value: string) => void;
  readonly onParentIdChange: (value: string) => void;
}

export class TreeColumnsRow implements m.ClassComponent<TreeColumnsRowAttrs> {
  view({attrs}: m.CVnode<TreeColumnsRowAttrs>): m.Children {
    return [
      m('.pf-spag-section-label', 'Tree columns'),
      m(Row, [
        m(ColumnPicker, {
          value: attrs.idColumn,
          columns: attrs.columns,
          placeholder: 'id',
          onSelect: attrs.onIdChange,
        }),
        m('span.pf-spag-operator', '→'),
        m(ColumnPicker, {
          value: attrs.parentIdColumn,
          columns: attrs.columns,
          placeholder: 'parent_id',
          onSelect: attrs.onParentIdChange,
        }),
      ]),
    ];
  }
}
