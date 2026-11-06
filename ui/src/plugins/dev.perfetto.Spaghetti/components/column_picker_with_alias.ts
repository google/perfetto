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
import {AliasTag} from './alias_tag';

export interface ColumnPickerWithAliasAttrs {
  readonly value: string;
  readonly columns: ColumnDef[];
  readonly onSelect: (value: string) => void;
  readonly placeholder?: string;
  readonly className?: string;
  readonly alias?: string;
  readonly aliasPlaceholder?: string;
  readonly onAliasChange?: (alias: string) => void;
}

export class ColumnPickerWithAlias implements m.ClassComponent<ColumnPickerWithAliasAttrs> {
  view({attrs}: m.CVnode<ColumnPickerWithAliasAttrs>): m.Children {
    const {
      value,
      columns,
      onSelect,
      placeholder,
      className,
      alias,
      aliasPlaceholder,
      onAliasChange,
    } = attrs;

    return [
      m(ColumnPicker, {
        value,
        columns,
        onSelect,
        placeholder,
        className,
      }),
      onAliasChange !== undefined &&
        m(AliasTag, {
          alias: alias ?? '',
          placeholder: aliasPlaceholder || value || 'alias',
          onChange: onAliasChange,
        }),
    ];
  }
}
