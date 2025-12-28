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
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {
  SIMPLE_TYPE_KINDS,
  isIdType,
  perfettoSqlTypeToString,
} from '../../../../trace_processor/perfetto_sql_type';
import {ColumnInfo} from '../column_info';

/**
 * Renders a type selector for a column
 * @param col The column information
 * @param index The column index
 * @param onColumnType Optional callback when type is changed
 * @returns A mithril child with the type selector UI
 */
export function renderTypeSelector(
  col: ColumnInfo,
  index: number,
  onColumnType?: (index: number, type: string) => void,
): m.Child {
  // If no callback provided, don't render type selector
  if (!onColumnType) {
    return null;
  }

  const currentType = col.type ?? 'UNKNOWN';
  const originalType = col.column.type;

  // Build the list of type options
  const typeOptions: {label: string; value: string}[] = [];

  // If the original type is an ID type, include it as an option
  if (originalType !== undefined && isIdType(originalType)) {
    const idTypeStr = perfettoSqlTypeToString(originalType);
    typeOptions.push({label: idTypeStr, value: idTypeStr});
  }

  // Add all simple types
  for (const type of SIMPLE_TYPE_KINDS) {
    typeOptions.push({label: type.toUpperCase(), value: type.toUpperCase()});
  }

  const handleTypeChange = (newType: string) => {
    onColumnType(index, newType);
  };

  return m(
    PopupMenu,
    {
      trigger: m('.pf-column-type', currentType),
    },
    typeOptions.map((opt) =>
      m(MenuItem, {
        label: opt.label,
        active: currentType === opt.value,
        onclick: () => handleTypeChange(opt.value),
      }),
    ),
  );
}
