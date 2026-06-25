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

import './column_picker.scss';
import m from 'mithril';
import {type ComboboxSuggestion, Combobox} from '../../../widgets/combobox';
import {perfettoSqlTypeIcon} from '../../../trace_processor/perfetto_sql_type';
import {classNames} from '../../../base/classnames';
import type {ColumnDef} from '../graph_utils';

// Column picker built on Combobox, adding type icons to suggestions.

export interface ColumnPickerAttrs {
  readonly value: string;
  readonly columns: ColumnDef[];
  readonly onSelect: (value: string) => void;
  readonly placeholder?: string;
  readonly className?: string;
}

export class ColumnPicker implements m.ClassComponent<ColumnPickerAttrs> {
  view({attrs}: m.CVnode<ColumnPickerAttrs>) {
    const suggestions: ComboboxSuggestion[] = attrs.columns.map((col) => ({
      value: col.name,
      icon: perfettoSqlTypeIcon(col.type),
    }));

    const isUnknown =
      attrs.value !== '' &&
      attrs.columns.length > 0 &&
      !attrs.columns.some((c) => c.name === attrs.value);

    return m(Combobox, {
      value: attrs.value,
      suggestions,
      onChange: attrs.onSelect,
      placeholder: attrs.placeholder ?? 'column',
      className: classNames(
        isUnknown && 'pf-column-picker--unknown',
        attrs.className,
      ),
    });
  }
}
